"""
PrintMiiOut Flask Web App
Fetches Mii GLB/PNG from mii-unsecure.ariankordi.net then runs
the Blender pipeline to produce a printable STL.
"""
import os
import sys
import shutil
import subprocess
import tempfile
import threading
import uuid
import time

import requests
from dotenv import load_dotenv
import sentry_sdk
from flask import (
    Flask, render_template, request, send_file,
    jsonify, abort, url_for
)

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

# In-memory job store  {job_id: {"status": ..., "file": ..., "error": ...}}
JOBS: dict = {}
JOBS_LOCK = threading.Lock()

# Cleanup old jobs after this many seconds
JOB_TTL = 300


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


def _fetch_mii_glb(params: dict) -> bytes:
    """Fetch the Mii head .glb from the remote API."""
    glb_params = dict(params)
    glb_params.setdefault("resourceType", "very_high")
    glb_params.setdefault("texResolution", "2048")
    url = _build_mii_url("/miis/image.glb", glb_params)
    app.logger.info("Fetching GLB: %s", url)
    r = requests.get(url, timeout=60)
    r.raise_for_status()
    return r.content


def _fetch_mii_png(params: dict) -> bytes:
    """Fetch the Mii face texture .png (mask_only) from the remote API."""
    png_params = dict(params)
    png_params["drawStageMode"] = "mask_only"
    png_params.setdefault("width", "2048")
    # PNG only uses data/nnid/api_id, not GLB-specific params
    for k in ("type", "expression", "shaderType", "view_type", "resourceType", "texResolution"):
        png_params.pop(k, None)
    url = _build_mii_url("/miis/image.png", png_params)
    app.logger.info("Fetching PNG: %s", url)
    r = requests.get(url, timeout=60)
    r.raise_for_status()
    return r.content


def _run_blender_job(job_id: str, glb_path: str, png_path: str,
                     body_type: str, output_stl: str):
    """Run the Blender pipeline in a thread and update JOBS."""
    with JOBS_LOCK:
        JOBS[job_id]["status"] = "running"

    blender = _blender_exe()
    if not blender:
        with JOBS_LOCK:
            JOBS[job_id]["status"] = "error"
            JOBS[job_id]["error"] = "Blender not found. Please install Blender."
        return

    script = os.path.join(os.path.dirname(__file__), "printmiiout.py")
    cmd = [blender, "-b", "--python", script, "--", glb_path]
    if body_type in ("m", "f"):
        cmd.append(body_type)

    env = os.environ.copy()
    # Pass the PNG path so printmiiout.py can pick it up
    # We place it next to the GLB as "image.png" - printmiiout already looks there
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=600,
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr[-3000:] if result.stderr else "Blender exited with error")

        # printmiiout.py writes <input_basename>.stl next to the GLB
        generated_stl = os.path.splitext(glb_path)[0] + ".stl"
        if not os.path.exists(generated_stl):
            raise RuntimeError("STL was not created by Blender script.")
        shutil.move(generated_stl, output_stl)

        with JOBS_LOCK:
            JOBS[job_id]["status"] = "done"
            JOBS[job_id]["file"] = output_stl
            JOBS[job_id]["log"] = result.stdout[-4000:]
    except Exception as exc:
        with JOBS_LOCK:
            JOBS[job_id]["status"] = "error"
            JOBS[job_id]["error"] = str(exc)
    finally:
        # Clean up temp GLB & PNG
        for f in (glb_path, png_path):
            try:
                os.unlink(f)
            except Exception:
                pass


def _detect_mii_gender(raw_bytes: bytes) -> str | None:
    """Detect Mii gender ('m' or 'f') from binary data."""
    try:
        if len(raw_bytes) in (96, 74):
            return 'f' if (raw_bytes[0x18] & 1) != 0 else 'm'
        elif len(raw_bytes) == 76:
            return 'f' if (raw_bytes[0] & 0x40) != 0 else 'm'
        elif len(raw_bytes) == 46:
            return 'f' if raw_bytes[22] == 1 else 'm'
        elif len(raw_bytes) == 88:
            return 'f' if raw_bytes[0x11] == 1 else 'm'
    except Exception:
        pass
    return None


def _start_job(form_params: dict, body_type: str) -> str:
    """Fetch assets, start Blender thread, return job_id."""
    if body_type == "auto":
        import base64
        body_type = "m"  # Default fallback
        data_b64 = form_params.get("data")
        if not data_b64 and form_params.get("nnid"):
            try:
                url = _build_mii_url(f"/mii_data/{form_params['nnid']}", {"api_id": form_params.get("api_id")})
                res = requests.get(url, timeout=10)
                if res.status_code == 200:
                    data_b64 = res.json().get("data")
            except Exception:
                pass
        if data_b64:
            try:
                raw = base64.b64decode(data_b64)
                gen = _detect_mii_gender(raw)
                if gen:
                    body_type = gen
            except Exception:
                pass

    # Fetch GLB
    glb_data = _fetch_mii_glb(form_params)
    png_data = _fetch_mii_png(form_params)

    # Write to temp files
    tmpdir = tempfile.mkdtemp(prefix="printmiiout_")
    glb_path = os.path.join(tmpdir, "mii_head.glb")
    png_path = os.path.join(tmpdir, "image.png")  # printmiiout looks for "image.png" beside GLB
    out_stl  = os.path.join(tmpdir, "output.stl")

    with open(glb_path, "wb") as fh:
        fh.write(glb_data)
    with open(png_path, "wb") as fh:
        fh.write(png_data)

    job_id = str(uuid.uuid4())
    with JOBS_LOCK:
        JOBS[job_id] = {
            "status": "queued",
            "file": None,
            "error": None,
            "created": time.time(),
        }

    t = threading.Thread(
        target=_run_blender_job,
        args=(job_id, glb_path, png_path, body_type, out_stl),
        daemon=True,
    )
    t.start()
    return job_id


def _cleanup_old_jobs():
    now = time.time()
    with JOBS_LOCK:
        stale = [jid for jid, j in JOBS.items()
                 if now - j.get("created", now) > JOB_TTL]
        for jid in stale:
            fpath = JOBS[jid].get("file")
            if fpath:
                try:
                    shutil.rmtree(os.path.dirname(fpath), ignore_errors=True)
                except Exception:
                    pass
            del JOBS[jid]


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

@app.route("/", methods=["GET"])
def index():
    return render_template("index.html")


@app.route("/submit", methods=["POST"])
def submit():
    _cleanup_old_jobs()

    input_type = request.form.get("input_type", "nnid")
    body_type   = request.form.get("body_type", "auto")

    # Build the API params based on input type
    api_params: dict = {}

    if input_type == "nnid":
        nnid = request.form.get("nnid", "").strip()
        if not nnid:
            return jsonify(error="NNID is required."), 400
        api_params["nnid"] = nnid

    elif input_type == "pnid":
        pnid = request.form.get("pnid", "").strip()
        if not pnid:
            return jsonify(error="PNID is required."), 400
        api_params["nnid"]   = pnid
        api_params["api_id"] = "1"

    elif input_type == "data":
        # Could be hex/base64 text OR a file upload
        data_text = request.form.get("data_text", "").strip()
        data_file = request.files.get("data_file")

        if data_file and data_file.filename:
            raw = data_file.read()
            if len(raw) in (112, 96):
                try:
                    raw = _decrypt_mii_qr_bytes(raw)
                except Exception:
                    pass
            import base64
            api_params["data"] = base64.b64encode(raw).decode()
        elif data_text:
            api_params["data"] = data_text
        else:
            return jsonify(error="No Mii data provided."), 400

    elif input_type == "qr":
        qr_text = request.form.get("qr_text", "").strip()
        qr_file = request.files.get("qr_file")

        raw_bytes = None
        if qr_file and qr_file.filename:
            try:
                raw_bytes = _decode_qr_image(qr_file)
            except Exception as exc:
                return jsonify(error=f"QR Image Error: {str(exc)}"), 400
        elif qr_text:
            import base64
            try:
                if len(qr_text) % 2 == 0 and all(c in "0123456789abcdefABCDEF" for c in qr_text):
                    raw_bytes = bytes.fromhex(qr_text)
                else:
                    raw_bytes = base64.b64decode(qr_text)
            except Exception:
                return jsonify(error="Invalid hex or base64 QR data pasted."), 400
        else:
            return jsonify(error="Please upload a QR code image or paste scanned QR data."), 400

        try:
            decrypted = _decrypt_mii_qr_bytes(raw_bytes)
        except Exception as exc:
            return jsonify(error=f"QR Decryption Error: {str(exc)}"), 400

        import base64
        api_params["data"] = base64.b64encode(decrypted).decode()

    else:
        return jsonify(error="Unknown input type."), 400

    # Expression param for GLB (using default view and shader)
    api_params["expression"] = request.form.get("expression", "normal")

    try:
        job_id = _start_job(api_params, body_type)
    except requests.HTTPError as exc:
        status = exc.response.status_code if exc.response is not None else 500
        msg = exc.response.text[:500] if exc.response is not None else str(exc)
        return jsonify(error=f"Mii API error {status}: {msg}"), 502
    except Exception as exc:
        return jsonify(error=str(exc)), 500

    return jsonify(job_id=job_id)


@app.route("/status/<job_id>")
def status(job_id: str):
    with JOBS_LOCK:
        job = JOBS.get(job_id)
    if job is None:
        return jsonify(error="Job not found."), 404
    return jsonify(
        status=job["status"],
        error=job.get("error"),
    )


@app.route("/download/<job_id>")
def download(job_id: str):
    with JOBS_LOCK:
        job = JOBS.get(job_id)
    if job is None or job["status"] != "done":
        abort(404)
    fpath = job["file"]
    if not fpath or not os.path.exists(fpath):
        abort(404)
    return send_file(
        fpath,
        as_attachment=True,
        download_name="printmiiout.stl",
        mimetype="application/octet-stream",
    )


if __name__ == "__main__":
    app.run(debug=True, port=5000)
