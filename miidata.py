"""
Reads the traits PrintMiiOut needs (gender, height, build, and the name to
letter a coin or stamp with) out of the Mii data formats the site accepts,
without rendering anything.

Heights and builds run 0 to 127 in every format here, 64 being the middle
of the editor's slider.
"""
import base64

AVERAGE = 64


def decode(text):
    """Mii data as bytes, from hex or base64 text (or bytes, unchanged)."""
    if isinstance(text, (bytes, bytearray)):
        return bytes(text)
    s = "".join(str(text).split())
    if len(s) % 2 == 0 and s and all(c in "0123456789abcdefABCDEF" for c in s):
        return bytes.fromhex(s)
    s = s.replace("-", "+").replace("_", "/")
    return base64.b64decode(s + "=" * (-len(s) % 4))


def _studio_decode(data):
    """Mii Studio's URL data: each byte is offset by 7 and XORed with the
    one before it (FFL.js _studioURLObfuscationDecode)."""
    out = bytearray()
    prev = data[0]
    for b in data[1:]:
        out.append(((b - 7) & 0xFF) ^ prev)
        prev = b
    return bytes(out)


def traits(raw):
    """{"gender": "m" | "f" | None, "height": 0-127, "build": 0-127} for a
    Mii in any of these formats, or the average height and build with no
    gender when the format is not one of them:

      96, 92  3DS / Wii U (FFLStoreData, Ver3StoreData, decrypted QR codes)
      74, 76  Wii (RFLCharData, RFLStoreData), big endian
      88      Switch nn::mii::CharInfo
      68, 48  Switch StoreData and CoreData
      46, 47  Mii Studio, raw and as its URLs carry it
    """
    gender = None
    height = build = AVERAGE
    n = len(raw)
    try:
        if n in (92, 96):
            gender = raw[0x18] & 1
            height, build = raw[0x2E], raw[0x2F]
        elif n in (74, 76):
            gender = (raw[0] >> 6) & 1
            height, build = raw[0x16], raw[0x17]
        elif n == 88:
            gender = raw[0x28]
            height, build = raw[0x29], raw[0x2A]
        elif n in (48, 68):
            gender = raw[4] >> 7
            height, build = raw[1] & 0x7F, raw[2] & 0x7F
        elif n in (46, 47):
            studio = _studio_decode(raw) if n == 47 else raw
            gender = studio[22]
            height, build = studio[30], studio[2]
    except IndexError:
        pass
    if gender not in (0, 1):
        gender = None
    if not (0 <= height <= 127):
        height = AVERAGE
    if not (0 <= build <= 127):
        build = AVERAGE
    return {
        "gender": None if gender is None else ("f" if gender else "m"),
        "height": int(height),
        "build": int(build),
    }


# Where each format keeps the nickname: (offset, bytes, encoding).
_NAME_AT = {
    96: (0x1A, 20, "utf-16-le"),  # 3DS / Wii U
    92: (0x1A, 20, "utf-16-le"),
    74: (0x02, 20, "utf-16-be"),  # Wii
    76: (0x02, 20, "utf-16-be"),
    88: (0x10, 22, "utf-16-le"),  # Switch CharInfo
    68: (0x1C, 20, "utf-16-le"),  # Switch StoreData and CoreData
    48: (0x1C, 20, "utf-16-le"),
}


def name(raw):
    """The Mii's nickname, or "" when the format keeps none (Mii Studio
    data) or it cannot be read."""
    at = _NAME_AT.get(len(raw))
    if not at:
        return ""
    offset, size, encoding = at
    try:
        text = raw[offset:offset + size].decode(encoding, errors="ignore")
    except Exception:
        return ""
    text = text.split("\x00", 1)[0]
    return "".join(c for c in text if c.isprintable()).strip()
