"""
PrintMiiOut Flask Web App
Serves the page, which builds the STL in the visitor's browser. The server
only reads the form (/resolve: decrypts QR codes, looks up NNIDs, picks the
body); the browser fetches the Mii from mii-unsecure.ariankordi.net itself
and makes the model with the WebAssembly core (core/, static/client/).
"""
import os
import re

import requests
from dotenv import load_dotenv
import sentry_sdk
from flask import (
    Flask, render_template, request,
    jsonify, url_for, Response
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

# One session keeps the connection to the Mii API open between lookups.
_HTTP = requests.Session()

NNID_RE = re.compile(r"^[A-Za-z0-9._-]{1,32}$")


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

> A free web tool that turns a Nintendo Mii into a 3D-printable STL file. It fetches the Mii and builds a head and body right in the visitor's browser (C++ compiled to WebAssembly), then hands over an STL for a slicer.

## Using it

- [The tool]({SITE_URL}/): enter a Nintendo Network ID (NNID) or Pretendo Network ID (PNID), upload a binary .mii / .ffsd file, or upload a Mii QR code image. Building usually takes under half a minute and needs up to 1 GB of memory.
- Glasses print as a flat frame that rests on the face, with temple arms holding it to the head.
- The figure prints as one piece by default, or as snap-together parts: the head, torso, arms and legs (with or without the base) print separately and press together on pins with crush ribs, like building bricks.
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
    figure in snap-together pieces and whether to stand it on a base, as the
    /resolve form asks for them, or an error message for the user."""
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


@app.route("/resolve", methods=["POST"])
def resolve():
    """Where the browser fetches the Mii, which body, height and build to
    give it, and how to print it. Runs nothing; the browser fetches the files
    from the Mii API itself and builds the model."""
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
