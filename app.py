"""
PrintMiiOut Flask Web App
Serves the page, which builds the STL in the visitor's browser. The server
only reads the form (/resolve: decrypts QR codes, looks up NNIDs, picks the
body); the browser fetches the Mii from mii-unsecure.ariankordi.net itself
and makes the model with the WebAssembly core (core/, static/client/).
"""
import math
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


# Switch 2 Mii QR codes are a 3DS one (112 bytes) with 32 more after it: a
# marker byte, then 15 bytes sealed with AES-CCM (the 3DS code's first 8
# bytes and 4 zeros as the nonce, a 16-byte tag) whose first 9 are the rest
# of a 10-byte extension. Its last 8 bytes are the Switch's wider colours
# (nn::mii common colour indices) and glasses type, which the 3DS data has
# no room for. As mii-unsecure.ariankordi.net reads them.
MII_QR_SWITCH2_KEY = bytes([0xA9, 0x3C, 0xB1, 0x2E, 0x94, 0x86, 0x1A, 0xC1,
                            0xD0, 0x74, 0xBB, 0x60, 0x19, 0xFD, 0x0E, 0x92])
MII_QR_SWITCH2_LENGTH = 112 + 32
MII_QR_SWITCH2_MARKER = 0x11


def _decrypt_mii_qr_bytes(qr_bytes: bytes) -> bytes:
    """Decrypt a Mii QR code's payload: 3DS/Wii U/Tomodachi Life (112 bytes
    or more) to its 96-byte Ver3StoreData, or a Switch 2 one (144 bytes) to
    an 88-byte Switch CharInfo carrying its wider colours."""
    from Crypto.Cipher import AES
    if len(qr_bytes) < 0x60:
        return qr_bytes
    nonce = qr_bytes[:8]
    cipher = AES.new(MII_QR_KEY, AES.MODE_CCM, nonce + bytes([0, 0, 0, 0]))
    content = cipher.decrypt(qr_bytes[8:8+0x58])
    ver3 = content[:12] + nonce + content[12:]
    extension = _switch2_qr_extension(qr_bytes)
    if extension is None:
        return ver3
    return _ver3_to_charinfo(ver3, extension[2:])


def _switch2_qr_extension(qr_bytes: bytes) -> bytes | None:
    """The 10-byte extension of a Switch 2 Mii QR code, or None for any
    other code (or one whose extension does not authenticate)."""
    from Crypto.Cipher import AES
    if len(qr_bytes) != MII_QR_SWITCH2_LENGTH or qr_bytes[112] != MII_QR_SWITCH2_MARKER:
        return None
    sealed = qr_bytes[113:]
    cipher = AES.new(MII_QR_SWITCH2_KEY, AES.MODE_CCM, nonce=qr_bytes[:8] + bytes(4), mac_len=16)
    try:
        plain = cipher.decrypt_and_verify(sealed[:-16], sealed[-16:])
    except ValueError:
        return None
    return qr_bytes[112:113] + plain[:9]


def _ver3_to_charinfo(ver3: bytes, colors: bytes) -> bytes:
    """A 96-byte 3DS/Wii U Mii as a Switch nn::mii::CharInfo (88 bytes), its
    colours and glasses type replaced by colors: faceline, hair, eye,
    eyebrow, mouth, beard, glass colour, glass type."""
    import hashlib
    t = ver3
    c = bytearray(88)
    # A create ID made from the Mii, marked as a normal one.
    create_id = bytearray(hashlib.sha256(bytes(t)).digest()[:16])
    create_id[8] = (create_id[8] & 0x3F) | 0x80
    c[0:16] = create_id
    name = t[26:46]
    if not any(name[:2]):
        name = "Mii".encode("utf-16-le").ljust(20, b"\0")
    c[16:36] = name
    c[38] = (t[1] >> 4) & 3        # font region
    c[39] = (t[25] >> 2) & 15      # favourite colour
    c[40] = t[24] & 1              # gender
    c[41] = min(t[46], 127)        # height
    c[42] = min(t[47], 127)        # build
    c[43] = 0 if t[12] & 0x80 else 1  # special unless its create ID is a normal one
    c[44] = (t[1] >> 2) & 3        # region move
    c[45] = (t[48] >> 1) & 15      # faceline type
    c[47] = t[49] & 15             # wrinkles
    c[48] = t[49] >> 4             # makeup
    c[49] = t[50]                  # hair type
    c[51] = (t[51] >> 3) & 1       # hair flip
    c[52] = t[52] & 63             # eye type
    c[54] = (t[53] >> 1) & 15      # eye scale
    c[55] = t[53] >> 5             # eye aspect
    c[56] = t[54] & 31             # eye rotate
    c[57] = ((t[55] & 1) << 3) | (t[54] >> 5)  # eye x
    c[58] = (t[55] >> 1) & 31      # eye y
    c[59] = t[56] & 31             # eyebrow type
    c[61] = t[57] & 15             # eyebrow scale
    c[62] = (t[57] >> 4) & 7       # eyebrow aspect
    c[63] = t[58] & 31             # eyebrow rotate
    c[64] = ((t[59] & 1) << 3) | (t[58] >> 5)  # eyebrow x
    c[65] = (t[59] >> 1) & 31      # eyebrow y
    c[66] = t[60] & 31             # nose type
    c[67] = ((t[61] & 1) << 3) | (t[60] >> 5)  # nose scale
    c[68] = (t[61] >> 1) & 31      # nose y
    c[69] = t[62] & 63             # mouth type
    c[71] = (t[63] >> 1) & 15      # mouth scale
    c[72] = t[63] >> 5             # mouth aspect
    c[73] = t[64] & 31             # mouth y
    c[75] = t[66] & 7              # beard type
    c[76] = t[64] >> 5             # mustache type
    c[77] = ((t[67] & 3) << 2) | (t[66] >> 6)  # mustache scale
    c[78] = (t[67] >> 2) & 31      # mustache y
    c[81] = ((t[69] & 7) << 1) | (t[68] >> 7)  # glass scale
    c[82] = t[69] >> 3             # glass y
    c[83] = t[70] & 1              # mole type
    c[84] = (t[70] >> 1) & 15      # mole scale
    c[85] = ((t[71] & 3) << 3) | (t[70] >> 5)  # mole x
    c[86] = (t[71] >> 2) & 31      # mole y
    # The colours (and glasses type) from the Switch 2 extension.
    (c[46], c[50], c[53], c[60], c[70], c[74], c[80], c[79]) = colors[:8]
    return bytes(c)


def _normalize_mii_data(val: bytes | str) -> str:
    """Normalize any binary Mii file (.miigx, .mii, .ffsd) or base64/hex string to lowercase hex."""
    import base64
    if isinstance(val, bytes):
        raw = val
        if len(raw) in (112, MII_QR_SWITCH2_LENGTH):
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
        if len(raw) in (112, MII_QR_SWITCH2_LENGTH):
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


def _mii_face_url(params: dict) -> str:
    """The Mii's head drawn in colour, facing front on a clear background,
    as sticker sheets print it."""
    face_params = {k: params[k] for k in ("data", "nnid", "api_id", "expression") if params.get(k)}
    face_params.update(type="face_only", width="1024")
    return _build_mii_url("/miis/image.png", face_params)


def _mii_raw(form_params: dict) -> bytes | None:
    """The Mii's data, looking an NNID or PNID up if need be, or None when
    the lookup fails or the data cannot be read."""
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
            return miidata.decode(data)
        except Exception:
            pass
    return None


def _mii_traits(raw: bytes | None) -> dict:
    """The Mii's gender, height and build. Unknown formats and failed
    lookups give no gender and average proportions."""
    if raw:
        try:
            return miidata.traits(raw)
        except Exception:
            pass
    return {"gender": None, "height": miidata.AVERAGE, "build": miidata.AVERAGE}


def _body_for(raw, body_type: str, proportions: str) -> tuple[str, int, int]:
    """The body to build ("m", "f" or "none") and the height and build to
    give it. raw: the Mii's data, or a function that looks it up, called
    only when the choice depends on it."""
    traits = {"gender": None, "height": miidata.AVERAGE, "build": miidata.AVERAGE}
    if body_type == "auto" or proportions == "mii":
        traits = _mii_traits(raw() if callable(raw) else raw)
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


@app.after_request
def _revalidate_client(resp):
    # The page and the browser-side modules change together; make browsers (and
    # any cache in front) check before reusing an old copy of one of them.
    if request.path.startswith("/static/client/"):
        resp.headers["Cache-Control"] = "no-cache"
    return resp


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

> A free web tool that turns a Nintendo Mii into a 3D-printable STL file: a figure, a bobblehead, an articulated Mii, a coin, a keychain or a name stamp. It fetches the Mii and builds the model right in the visitor's browser (C++ compiled to WebAssembly), then hands over an STL for a slicer. It also draws printable PDFs for a regular printer: sheets of Mii name stickers, and towel and shirt designs.

## Using it

- [The tool]({SITE_URL}/): enter a Nintendo Network ID (NNID) or Pretendo Network ID (PNID), upload a binary .mii / .ffsd file, or upload a Mii QR code image. Building usually takes under half a minute and needs up to 1 GB of memory.
- Glasses print as a flat frame that rests on the face, with temple arms holding it to the head.
- The figure prints as one piece by default, or as snap-together parts: the head, torso, arms and legs (with or without the base) print separately and press together on pins with crush ribs, like building bricks. A "Pin fit" slider loosens or tightens every pin for the visitor's printer.
- The body follows the Mii's own height and build, scaled bone by bone as Nintendo's editor scales it, with arms and legs held thick enough to print. An "Average" option uses the middle height and build instead.
- Besides a figure it makes:
  - the head alone in 3D, standing on a flat cut under the chin, with a keychain loop on top if wanted;
  - a print-in-place bobblehead: the head hangs on a ball-ended post rising from the body, captured inside the head as it prints, so it nods without assembly;
  - a flat articulated Mii, drawn from the front like a paper doll, whose arms and legs swing on joints that print in place (each limb's knob prints already captured in a ring on the body), with no supports or assembly;
  - a coin or token with the face drawn flat (round, rounded square, or cut to the outline of the face, with or without a rim, raised or engraved, with a keychain loop if wanted), lettered with the Mii's name or any text in Rodin NTLG;
  - a keychain after Wii no Ma's: a round disc in a bezel with a ring for the chain, the face drawn in the middle and the name above or below it, built in three heights so two filament swaps colour it (background, face, then lines and rim);
  - a round self-inking-style name stamp for an ink pad: a bullet-shaped grip, a face plug that prints face up and presses into it, and a cap;
  - a square stamp with a domed top showing the design, a face plate, two pins and a cap.
- For a regular printer it draws PDFs:
  - a sheet of name stickers after Wii no Ma's Mii sticker shop (Miiのお名前シール), in six colour schemes, with the face in colour or as a stamp's line art, on sticker paper or fitted to dozens of Avery label sheets (US Letter and A4: round, square, oval, address, shipping and name badge labels);
  - a towel design (a field of dots with the face and name in the corner) and a shirt print (the face in a ring lettered with the name), in Shachihata's stamp ink colours or pink, mirrored for iron-on transfers if wanted.

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


MAKES = ("figure", "head", "bobblehead", "articulated", "coin", "keychain", "stamp", "square_stamp", "stickers", "towel", "shirt")
FLAT_MAKES = ("coin", "keychain", "stamp", "square_stamp")
# Printed on paper rather than a 3D printer: the browser draws a PDF.
PRINT_MAKES = ("stickers", "towel", "shirt")
TEXT_MAX = 32


def _clean_text(text: str) -> str:
    """Lettering as typed, without control or formatting characters, on one
    line and no longer than TEXT_MAX characters."""
    text = "".join(c if c.isprintable() else " " for c in text)
    return " ".join(text.split())[:TEXT_MAX]


def _parse_form() -> tuple[tuple[dict, dict] | None, str | None]:
    """The Mii API params and the choices of what to make and how, as the
    /resolve form asks for them, or an error message for the user."""
    input_type = request.form.get("input_type", "nnid")
    make = request.form.get("make", "figure")
    if make not in MAKES:
        make = "figure"
    body_type   = request.form.get("body_type", "auto")
    if body_type not in ("auto", "m", "f", "none"):
        body_type = "auto"
    if make in ("bobblehead", "articulated") and body_type == "none":
        body_type = "auto"  # the head bobbles on a body; the limbs need one
    proportions = request.form.get("proportions", "mii")
    if proportions not in ("mii", "average"):
        proportions = "mii"
    try:
        fit = float(request.form.get("fit", "0"))
    except ValueError:
        fit = 0.0
    # Millimetres per side, as far as the page's slider goes.
    fit = round(min(max(fit, -0.1), 0.2), 2) if math.isfinite(fit) else 0.0
    try:
        size = float(request.form.get("size", "0"))
    except ValueError:
        size = 0.0
    # Millimetres across (an articulated Mii: how tall); 0 for the usual size.
    # The core keeps each make's size within what it can print.
    size = round(min(max(size, 0.0), 200.0), 1) if math.isfinite(size) else 0.0
    lettering = request.form.get("lettering", "name")
    if lettering not in ("none", "name", "custom"):
        lettering = "name"
    choices = {
        "make": make,
        "body_type": body_type,
        "proportions": proportions,
        "split": make == "figure" and request.form.get("pieces", "whole") == "split",
        "base": request.form.get("base", "on") != "off",
        "mmu": request.form.get("mmu") == "on",
        "hair": {"smooth": 3, "detailed": 1, "faceted": 0}.get(request.form.get("hair"), 3),
        "fit": fit,
        "size": size,
        "shape": request.form.get("shape") if request.form.get("shape") in ("square", "face") else "round",
        "rim": request.form.get("rim", "on") != "off",
        "keychain": request.form.get("keychain") == "on",
        "relief": "engraved" if request.form.get("relief") == "engraved" else "raised",
        "lettering": lettering,
        "text": _clean_text(request.form.get("text", "")) if lettering == "custom" else "",
        "text_pos": "below" if request.form.get("text_pos") == "below" else "above",
        "border": request.form.get("border") if request.form.get("border") in ("line", "dashed") else "none",
    }

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

    return (api_params, choices), None


@app.route("/resolve", methods=["POST"])
def resolve():
    """Where the browser fetches the Mii, which body, height and build to
    give it, and how to print it. Runs nothing; the browser fetches the files
    from the Mii API itself and builds the model."""
    form, error = _parse_form()
    if error:
        return jsonify(error=error), 400
    api_params, c = form
    raw_cache: list = []

    def raw():
        if not raw_cache:
            raw_cache.append(_mii_raw(api_params))
        return raw_cache[0]

    job = {
        "glb": _mii_glb_url(api_params),
        "png": _mii_png_url(api_params),
        "make": c["make"].replace("_", "-"),
        "fit": c["fit"],
    }
    if c["make"] in PRINT_MAKES:
        text = c["text"]
        if c["lettering"] == "name":
            mii = raw()
            text = _clean_text(miidata.name(mii)) if mii else ""
        job.update(face=_mii_face_url(api_params), text=text)
    elif c["make"] in FLAT_MAKES:
        text = c["text"]
        if c["lettering"] == "name":
            mii = raw()
            text = _clean_text(miidata.name(mii)) if mii else ""
        job.update(
            size=c["size"],
            shape=c["shape"],
            rim=c["rim"],
            # The round stamp has a strap hole through its grip already.
            keychain=c["make"] in ("coin", "square_stamp") and c["keychain"],
            relief=c["relief"],
            text=text,
            text_pos=c["text_pos"],
            border=c["border"],
        )
    elif c["make"] == "head":
        # The head alone: no body, and a flat cut under the chin for "base".
        job.update(body=None, height=miidata.AVERAGE, build=miidata.AVERAGE, split=False, base=c["base"],
                   keychain=c["keychain"], mmu=c["mmu"], hair_smoothing=c["hair"])
    else:
        body_type, height, build = _body_for(raw, c["body_type"], c["proportions"])
        job.update(
            body=None if body_type == "none" else body_type,
            height=height,
            build=build,
            split=c["split"],
            base=c["base"],
            # Painted for a multi-material printer: a whole figure or a bobblehead.
            mmu=c["mmu"] and c["make"] in ("figure", "bobblehead") and not c["split"],
            hair_smoothing=c["hair"],
        )
        if c["make"] == "articulated":
            job["size"] = c["size"]
    return jsonify(job)


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
