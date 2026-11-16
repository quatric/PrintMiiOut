"""
PrintMiiOut Flask Web App
Fetches Mii GLB/PNG from mii-unsecure.ariankordi.net then runs
the Blender pipeline to produce a printable STL.
"""
import os
import re
import sys
import shutil
import subprocess
import tempfile
import threading
import uuid
import time
import json

from concurrent.futures import ThreadPoolExecutor

import requests
from dotenv import load_dotenv
import sentry_sdk
from flask import (
    Flask, render_template, request, send_file,
    jsonify, abort, url_for, Response
)

import miidata

load_dotenv()

sentry_dsn = os.environ.get("SENTRY_DSN")
if sentry_dsn:
    sentry_sdk.init(
        dsn=sentry_dsn,
        # Add data like request headers and IP for users,
        # see https://docs.sentry.io/platforms/python/data-management/data-collected/ for more info
        send_default_pii=True,
    )

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 2 * 1024 * 1024  # 2 MB upload limit

MII_API_BASE = "https://mii-unsecure.ariankordi.net"

# Disk-backed job store for multi-process Gunicorn workers
JOBS_DIR = "/tmp/printmiiout_jobs"
os.makedirs(JOBS_DIR, exist_ok=True)
JOBS_LOCK = threading.Lock()

# Cleanup old jobs after this many seconds
JOB_TTL = 1800  # 30 minutes — file is cleaned up on download, this is a fallback

# Every submission costs two upstream fetches and a long Blender run, so
# submissions are limited per client and Blender runs are limited per process.
SUBMIT_WINDOW = 10 * 60          # seconds
SUBMIT_PER_CLIENT = 5            # submissions per client per window
MAX_PENDING_JOBS = 8             # queued + running jobs on disk before refusing new ones
BLENDER_CONCURRENCY = int(os.environ.get("BLENDER_CONCURRENCY", "2"))
# Set when the app sits behind a reverse proxy that appends the client to
# X-Forwarded-For; without a proxy that header is client-controlled.
TRUST_PROXY = os.environ.get("TRUST_PROXY", "").lower() in ("1", "true", "yes")

# One session keeps the connection to the Mii API open between requests, and
# the pool lets a submission fetch everything it needs from it at once.
_HTTP = requests.Session()
_HTTP.mount("https://", requests.adapters.HTTPAdapter(pool_maxsize=16))
_FETCH_POOL = ThreadPoolExecutor(max_workers=16)

_SUBMITS: dict[str, list[float]] = {}
_SUBMITS_LOCK = threading.Lock()
_BLENDER_SLOTS = threading.BoundedSemaphore(BLENDER_CONCURRENCY)

NNID_RE = re.compile(r"^[A-Za-z0-9._-]{1,32}$")


def _client_key() -> str:
    if TRUST_PROXY:
        forwarded = request.headers.get("X-Forwarded-For", "")
        last_hop = forwarded.split(",")[-1].strip()
        if last_hop:
            return last_hop
    return request.remote_addr or "unknown"


def _submit_allowed(client: str) -> bool:
    now = time.time()
    with _SUBMITS_LOCK:
        recent = [t for t in _SUBMITS.get(client, []) if now - t < SUBMIT_WINDOW]
        if len(recent) >= SUBMIT_PER_CLIENT:
            _SUBMITS[client] = recent
            return False
        recent.append(now)
        _SUBMITS[client] = recent
        # Drop clients that have gone quiet so the map stays small.
        for key in [k for k, v in _SUBMITS.items() if not v or now - v[-1] >= SUBMIT_WINDOW]:
            if key != client:
                _SUBMITS.pop(key, None)
    return True


def _pending_job_count() -> int:
    count = 0
    try:
        for entry in os.listdir(JOBS_DIR):
            jfile = os.path.join(JOBS_DIR, entry, "job.json")
            try:
                with open(jfile, "r", encoding="utf-8") as fh:
                    if json.load(fh).get("status") in ("queued", "running"):
                        count += 1
            except Exception:
                continue
    except FileNotFoundError:
        pass
    return count


def _valid_job_id(job_id: str) -> bool:
    try:
        return str(uuid.UUID(job_id)) == job_id
    except (ValueError, AttributeError, TypeError):
        return False


def _get_job(job_id: str) -> dict | None:
    # Job IDs are UUIDs the server made; anything else never reaches the filesystem.
    if not _valid_job_id(job_id):
        return None
    path = os.path.join(JOBS_DIR, job_id, "job.json")
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return None


def _save_job(job_id: str, job_data: dict):
    jdir = os.path.join(JOBS_DIR, job_id)
    os.makedirs(jdir, exist_ok=True)
    path = os.path.join(jdir, "job.json")
    with JOBS_LOCK:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(job_data, fh)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

MII_QR_KEY = bytes([0x59, 0xFC, 0x81, 0x7E, 0x64, 0x46, 0xEA, 0x61, 0x90, 0x34, 0x7B, 0x20, 0xE9, 0xBD, 0xCE, 0x52])


def _decrypt_mii_qr_bytes(qr_bytes: bytes) -> bytes:
    """Decrypt 3DS/Wii U/Miitomo Mii QR code payload (typically 112 or 96 bytes)."""
    from Crypto.Cipher import AES
    if len(qr_bytes) < 0x60:
        return qr_bytes
    nonce = qr_bytes[:8]
    cipher = AES.new(MII_QR_KEY, AES.MODE_CCM, nonce + bytes([0, 0, 0, 0]))
    content = cipher.decrypt(qr_bytes[8:8+0x58])
    return content[:12] + nonce + content[12:]


def _normalize_mii_data(val: bytes | str) -> str:
    """Normalize any binary Mii file (.miigx, .mii, .ffsd) or base64/hex string to lowercase hex."""
    import base64
    if isinstance(val, bytes):
        raw = val
        if len(raw) == 112:
            try:
                raw = _decrypt_mii_qr_bytes(raw)
            except Exception:
                pass
        return raw.hex()
    s = val.strip()
    if "data=" in s:
        s = s.split("data=")[-1].split("&")[0]
    if all(c in "0123456789abcdefABCDEF" for c in s) and len(s) % 2 == 0 and len(s) >= 32:
        return s.lower()
    try:
        raw = base64.b64decode(s)
        if len(raw) == 112:
            try:
                raw = _decrypt_mii_qr_bytes(raw)
            except Exception:
                pass
        return raw.hex()
    except Exception:
        return s


def _decode_qr_image(file_stream) -> bytes:
    """Decode binary QR code from an image stream using zxingcpp."""
    import cv2
    import numpy as np
    import zxingcpp

    file_bytes = np.frombuffer(file_stream.read(), np.uint8)
    img = cv2.imdecode(file_bytes, cv2.IMREAD_COLOR)
    if img is None:
        raise ValueError("Could not decode image file. Please upload a valid PNG/JPG.")

    results = zxingcpp.read_barcodes(img)
    if not results:
        raise ValueError("No QR code found in the image.")
    for res in results:
        if res.bytes:
            return bytes(res.bytes)
    raise ValueError("QR code found but contained no binary data.")


def _blender_exe() -> str:
    for candidate in [
        shutil.which("blender"),
        "/opt/homebrew/bin/blender",
        "/Applications/Blender.app/Contents/MacOS/Blender",
    ]:
        if candidate and os.path.exists(candidate):
            return candidate
    return None


def _build_mii_url(endpoint: str, params: dict) -> str:
    """Build a URL for the mii-unsecure API."""
    from urllib.parse import urlencode
    qs = {k: v for k, v in params.items() if v is not None and v != ""}
    return f"{MII_API_BASE}{endpoint}?{urlencode(qs)}"


def _mii_glb_url(params: dict) -> str:
    """The Mii head .glb on the remote API."""
    glb_params = dict(params)
    glb_params.setdefault("resourceType", "very_high")
    glb_params.setdefault("texResolution", "2048")
    return _build_mii_url("/miis/image.glb", glb_params)


def _mii_png_url(params: dict) -> str:
    """The Mii face texture .png (mask_only) on the remote API."""
    png_params = dict(params)
    png_params["drawStageMode"] = "mask_only"
    png_params.setdefault("width", "2048")
    # PNG only uses data/nnid/api_id, not GLB-specific params. It keeps the
    # expression, since it replaces the mask the GLB draws for it.
    for k in ("type", "shaderType", "view_type", "resourceType", "texResolution"):
        png_params.pop(k, None)
    return _build_mii_url("/miis/image.png", png_params)


def _fetch_mii_glb(params: dict) -> bytes:
    """Fetch the Mii head .glb from the remote API."""
    url = _mii_glb_url(params)
    app.logger.info("Fetching GLB: %s", url)
    r = _HTTP.get(url, timeout=60)
    r.raise_for_status()
    return r.content


def _fetch_mii_png(params: dict) -> bytes:
    """Fetch the Mii face texture .png (mask_only) from the remote API."""
    url = _mii_png_url(params)
    app.logger.info("Fetching PNG: %s", url)
    r = _HTTP.get(url, timeout=60)
    r.raise_for_status()
    return r.content


def _run_blender_job(job_id: str, glb_path: str, png_path: str,
                     body_type: str, output_stl: str,
                     height: int = 64, build: int = 64,
                     split: bool = False, base: bool = True):
    """Run the Blender pipeline in a thread and update the job file."""
    job = _get_job(job_id) or {}

    # Wait for a Blender slot so a burst of submissions runs a few at a time.
    with _BLENDER_SLOTS:
        job["status"] = "running"
        _save_job(job_id, job)

        blender = _blender_exe()
        if not blender:
            app.logger.error("Blender not found for job %s", job_id)
            job["status"] = "error"
            job["error"] = "The 3D converter is not available right now. Please try again later."
            _save_job(job_id, job)
            return

        script = os.path.join(os.path.dirname(__file__), "printmiiout.py")
        # Factory settings skip the user's preferences and add-ons at startup.
        cmd = [blender, "-b", "--factory-startup", "--python", script, "--", glb_path]
        if body_type in ("m", "f"):
            cmd += [body_type, "--height", str(int(height)), "--build", str(int(build))]
            if split:
                cmd.append("--split")
            if not base:
                cmd.append("--no-base")

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=600,
            )
            if result.returncode != 0:
                raise RuntimeError(result.stderr[-3000:] if result.stderr else "Blender exited with error")

            generated_stl = os.path.splitext(glb_path)[0] + ".stl"
            if not os.path.exists(generated_stl):
                raise RuntimeError("STL was not created by Blender script.")
            shutil.move(generated_stl, output_stl)

            job["status"] = "done"
            job["file"] = output_stl
            job["log"] = result.stdout[-4000:]
            _save_job(job_id, job)
        except Exception as exc:
            # Details (paths, Blender's stderr) stay in the server log.
            app.logger.error("Job %s failed: %s", job_id, exc)
            job["status"] = "error"
            job["error"] = "Converting this Mii failed. Please try again or try another Mii."
            _save_job(job_id, job)
        finally:
            for f in (glb_path, png_path):
                try:
                    os.unlink(f)
                except Exception:
                    pass


def _mii_traits(form_params: dict) -> dict:
    """The Mii's gender, height and build, looking the Mii up if needed.
    Unknown formats and failed lookups give no gender and average proportions."""
    data = form_params.get("data")
    if not data and form_params.get("nnid"):
        try:
            from urllib.parse import quote
            url = _build_mii_url(f"/mii_data/{quote(form_params['nnid'], safe='')}", {"api_id": form_params.get("api_id")})
            res = _HTTP.get(url, timeout=10)
            if res.status_code == 200:
                data = res.json().get("data")
        except Exception:
            pass
    if data:
        try:
            return miidata.traits(miidata.decode(data))
        except Exception:
            pass
    return {"gender": None, "height": miidata.AVERAGE, "build": miidata.AVERAGE}


def _body_for(form_params: dict, body_type: str, proportions: str) -> tuple[str, int, int]:
    """The body to build ("m", "f" or "none") and the height and build to
    give it, looking the Mii up when the choice depends on it."""
    traits = {"gender": None, "height": miidata.AVERAGE, "build": miidata.AVERAGE}
    if body_type == "auto" or proportions == "mii":
        traits = _mii_traits(form_params)
    if body_type == "auto":
        body_type = traits["gender"] or "m"
    if proportions != "mii":
        traits["height"] = traits["build"] = miidata.AVERAGE
    return body_type, traits["height"], traits["build"]


def _start_job(form_params: dict, body_type: str, proportions: str = "mii",
               split: bool = False, base: bool = True) -> str:
    """Fetch assets, start Blender thread, return job_id."""
    # The model, the mask and the Mii lookup are independent upstream
    # requests, so they run side by side instead of one after another.
    glb_future = _FETCH_POOL.submit(_fetch_mii_glb, form_params)
    png_future = _FETCH_POOL.submit(_fetch_mii_png, form_params)
    body_type, height, build = _body_for(form_params, body_type, proportions)
    traits = {"height": height, "build": build}
    glb_data = glb_future.result()
    png_data = png_future.result()

    # Write to temp files
    job_id = str(uuid.uuid4())
    jdir = os.path.join(JOBS_DIR, job_id)
    os.makedirs(jdir, exist_ok=True)
    glb_path = os.path.join(jdir, "mii_head.glb")
    png_path = os.path.join(jdir, "image.png")
    out_stl  = os.path.join(jdir, "output.stl")

    with open(glb_path, "wb") as fh:
        fh.write(glb_data)
    with open(png_path, "wb") as fh:
        fh.write(png_data)

    _save_job(job_id, {
        "status": "queued",
        "file": out_stl,
        "error": None,
        "created": time.time(),
    })

    t = threading.Thread(
        target=_run_blender_job,
        args=(job_id, glb_path, png_path, body_type, out_stl, traits["height"], traits["build"],
              split, base),
        daemon=True,
    )
    t.start()
    return job_id


def _cleanup_old_jobs():
    now = time.time()
    if not os.path.exists(JOBS_DIR):
        return
    with JOBS_LOCK:
        for entry in os.listdir(JOBS_DIR):
            jdir = os.path.join(JOBS_DIR, entry)
            jfile = os.path.join(jdir, "job.json")
            if not os.path.isdir(jdir) or not os.path.exists(jfile):
                continue
            try:
                with open(jfile, "r", encoding="utf-8") as fh:
                    job = json.load(fh)
                if now - job.get("created", now) > JOB_TTL:
                    fpath = job.get("file")
                    if fpath and os.path.exists(fpath) and job.get("status") == "done":
                        job["created"] = now
                        with open(jfile, "w", encoding="utf-8") as fh:
                            json.dump(job, fh)
                        continue
                    shutil.rmtree(jdir, ignore_errors=True)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

@app.route("/", methods=["GET"])
def index():
    return render_template("index.html")


SITE_URL = "https://printmiiout.com"

ROBOTS_TXT = (
    "User-agent: *\n"
    "Allow: /\n"
    "Disallow: /submit\n"
    "Disallow: /resolve\n"
    "Disallow: /status/\n"
    "Disallow: /download/\n"
    "Disallow: /decode-qr\n"
    "\n"
    f"Sitemap: {SITE_URL}/sitemap.xml\n"
)

SITEMAP_XML = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
    f"  <url><loc>{SITE_URL}/</loc></url>\n"
    "</urlset>\n"
)

LLMS_TXT = f"""# PrintMiiOut (printmiiout.com)

> A free web tool that turns a Nintendo Mii into a 3D-printable STL file. It fetches the Mii, builds a head and body in Blender and returns an STL for a slicer.

## Using it

- [The tool]({SITE_URL}/): enter a Nintendo Network ID (NNID) or Pretendo Network ID (PNID), upload a binary .mii / .ffsd file, or upload a Mii QR code image. Conversion usually takes under a minute.
- Glasses print as a flat frame that rests on the face, with temple arms holding it to the head.
- The figure prints as one piece by default, or as snap-together parts: the head, torso, arms and legs (with or without the base) print separately and press together on pins with crush ribs, like building bricks.
- The model can also be built in the visitor's own browser (beta): the same pipeline compiled to WebAssembly, with assimp reading the Mii and writing the STL.
- The body follows the Mii's own height and build, scaled bone by bone as Nintendo's editor scales it, with arms and legs held thick enough to print. An "Average" option uses the middle height and build instead.

## Source

- [Code on GitHub](https://github.com/quatric/PrintMiiOut): licensed under the GNU AGPL v3. Fonts, images and Mii artwork keep their own terms.
"""


def _text(body, mimetype):
    return Response(body, mimetype=mimetype, headers={"Cache-Control": "public, max-age=3600"})


@app.route("/robots.txt")
def robots_txt():
    return _text(ROBOTS_TXT, "text/plain")


@app.route("/sitemap.xml")
def sitemap_xml():
    return _text(SITEMAP_XML, "application/xml")


@app.route("/llms.txt")
def llms_txt():
    return _text(LLMS_TXT, "text/plain")


def _parse_form() -> tuple[tuple[dict, str, str, bool, bool] | None, str | None]:
    """The Mii API params, body type, proportions, whether to print the
    figure in snap-together pieces and whether to stand it on a base, as a
    /submit or /resolve form asks for them, or an error message for the user."""
    input_type = request.form.get("input_type", "nnid")
    body_type   = request.form.get("body_type", "auto")
    if body_type not in ("auto", "m", "f", "none"):
        body_type = "auto"
    proportions = request.form.get("proportions", "mii")
    if proportions not in ("mii", "average"):
        proportions = "mii"
    split = request.form.get("pieces", "whole") == "split"
    base = request.form.get("base", "on") != "off"

    # Build the API params based on input type
    api_params: dict = {}

    if input_type == "nnid":
        nnid = request.form.get("nnid", "").strip()
        if not nnid:
            return None, "NNID is required."
        if not NNID_RE.fullmatch(nnid):
            return None, "That doesn't look like a valid NNID."
        api_params["nnid"] = nnid

    elif input_type == "pnid":
        pnid = request.form.get("pnid", "").strip()
        if not pnid:
            return None, "PNID is required."
        if not NNID_RE.fullmatch(pnid):
            return None, "That doesn't look like a valid PNID."
        api_params["nnid"]   = pnid
        api_params["api_id"] = "1"

    elif input_type == "data":
        data_text = request.form.get("data_text", "").strip()
        data_file = request.files.get("data_file")

        if data_file and data_file.filename:
            raw = data_file.read()
            api_params["data"] = _normalize_mii_data(raw)
        elif data_text:
            api_params["data"] = _normalize_mii_data(data_text)
        else:
            return None, "No Mii data provided."

    elif input_type == "qr":
        qr_text = request.form.get("qr_text", "").strip()
        qr_file = request.files.get("qr_file")

        raw_bytes = None
        if qr_file and qr_file.filename:
            try:
                raw_bytes = _decode_qr_image(qr_file)
            except Exception as exc:
                return None, f"QR Image Error: {str(exc)}"
        elif qr_text:
            import base64
            try:
                if len(qr_text) % 2 == 0 and all(c in "0123456789abcdefABCDEF" for c in qr_text):
                    raw_bytes = bytes.fromhex(qr_text)
                else:
                    raw_bytes = base64.b64decode(qr_text)
            except Exception:
                return None, "Invalid hex or base64 QR data pasted."
        else:
            return None, "Please upload a QR code image or paste scanned QR data."

        try:
            decrypted = _decrypt_mii_qr_bytes(raw_bytes)
        except Exception as exc:
            return None, f"QR Decryption Error: {str(exc)}"

        import base64
        api_params["data"] = base64.b64encode(decrypted).decode()

    else:
        return None, "Unknown input type."

    # Expression param for GLB (using default view and shader)
    # Names from the swagger docs, or numeric Miitomo expression ids
    expression = request.form.get("expression", "normal")
    if not re.fullmatch(r"[a-z_]{1,32}|\d{1,3}", expression):
        expression = "normal"
    api_params["expression"] = expression

    return (api_params, body_type, proportions, split, base), None


@app.route("/submit", methods=["POST"])
def submit():
    _cleanup_old_jobs()

    if not _submit_allowed(_client_key()):
        return jsonify(error="Too many requests. Please wait a few minutes and try again."), 429
    if _pending_job_count() >= MAX_PENDING_JOBS:
        return jsonify(error="PrintMiiOut is busy right now. Please try again in a few minutes."), 503

    form, error = _parse_form()
    if error:
        return jsonify(error=error), 400
    api_params, body_type, proportions, split, base = form

    try:
        job_id = _start_job(api_params, body_type, proportions, split, base)
    except requests.HTTPError as exc:
        status = exc.response.status_code if exc.response is not None else 500
        app.logger.warning("Mii API error %s for %s input", status, request.form.get("input_type", "nnid"))
        if status == 404:
            return jsonify(error="No Mii was found for that ID."), 400
        if 400 <= status < 500:
            return jsonify(error="The Mii service rejected that input. Please check it and try again."), 400
        return jsonify(error="The Mii service is unavailable right now. Please try again later."), 502
    except Exception as exc:
        app.logger.error("Could not start job: %s", exc)
        return jsonify(error="Could not start the conversion. Please try again."), 400

    return jsonify(job_id=job_id)


@app.route("/resolve", methods=["POST"])
def resolve():
    """For building in the browser: where to fetch the Mii and which body,
    height and build to give it. Takes the same form as /submit and runs
    nothing; the browser fetches the files from the Mii API itself."""
    form, error = _parse_form()
    if error:
        return jsonify(error=error), 400
    api_params, body_type, proportions, split, base = form
    body_type, height, build = _body_for(api_params, body_type, proportions)
    return jsonify(
        glb=_mii_glb_url(api_params),
        png=_mii_png_url(api_params),
        body=None if body_type == "none" else body_type,
        height=height,
        build=build,
        split=split,
        base=base,
    )


@app.route("/status/<job_id>")
def status(job_id: str):
    job = _get_job(job_id)
    if job is None:
        return jsonify(error="Job not found."), 404
    return jsonify(
        status=job["status"],
        error=job.get("error"),
    )


@app.route("/download/<job_id>")
def download(job_id: str):
    job = _get_job(job_id)
    if job is None or job["status"] != "done":
        abort(404)
    fpath = job.get("file")
    if not fpath or not os.path.exists(fpath):
        abort(404)

    jdir = os.path.dirname(fpath)
    from flask import after_this_request

    @after_this_request
    def _cleanup(response):
        try:
            shutil.rmtree(jdir, ignore_errors=True)
        except Exception:
            pass
        return response

    return send_file(
        fpath,
        as_attachment=True,
        download_name="printmiiout.stl",
        mimetype="application/octet-stream",
    )


@app.route("/decode-qr-raw", methods=["POST"])
def decode_qr_raw():
    """Accept raw binary QR bytes already extracted client-side by jsQR.
    Runs AES-CCM decryption and returns the result as base64.
    This is the primary path for the new client-side QR flow."""
    qr_raw = request.files.get("qr_raw")
    if not qr_raw:
        return jsonify(error="No binary data uploaded."), 400
    raw_bytes = qr_raw.read()
    try:
        decrypted = _decrypt_mii_qr_bytes(raw_bytes)
    except Exception as exc:
        return jsonify(error=f"QR decrypt error: {exc}"), 400
    import base64
    return jsonify(
        data=decrypted.hex(),
        hex=decrypted.hex(),
        size=len(decrypted),
    )


@app.route("/decode-qr", methods=["POST"])
def decode_qr():
    """Server-side QR decode helper.
    Accepts a raw binary file upload (field name 'qr_file') and returns
    the decrypted Mii data as hex so the frontend can use it directly.
    Falls back to returning the raw bytes as hex if decryption fails."""
    qr_file = request.files.get("qr_file")
    if not qr_file or not qr_file.filename:
        return jsonify(error="No file uploaded."), 400

    try:
        raw_bytes = _decode_qr_image(qr_file)
    except Exception as exc:
        return jsonify(error=f"QR scan error: {exc}"), 400

    try:
        decrypted = _decrypt_mii_qr_bytes(raw_bytes)
    except Exception as exc:
        return jsonify(error=f"QR decrypt error: {exc}"), 400

    import base64
    return jsonify(
        data=base64.b64encode(decrypted).decode(),
        hex=decrypted.hex(),
        size=len(decrypted),
    )


if __name__ == "__main__":
    # The Werkzeug debugger lets anyone who reaches the page run code, so it
    # is only on when asked for explicitly.
    app.run(debug=os.environ.get("FLASK_DEBUG") == "1", port=5000)
