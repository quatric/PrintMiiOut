#!/usr/bin/env python3
"""Builds static/client/labels.json: the sheets of labels the sticker maker
lays its stickers out on.

The sheets are the ones The Print Shop 4 supports (printshop-us.txt and
printshop-a4.txt here, copied from its template lists), less the badges,
transfers, tags and signs that are not stickers. Where each label sits on
its sheet comes from LibreOffice's label database, or failing that from
gLabels' templates; the few that neither has were measured from Avery's own
templates (HAND). Every layout is checked against the size Print Shop gives
and against the paper. After those come every other Avery and Zweckform
sheet of labels either database has, less the cards, tabs and disk labels.

  python tools/labels.py LIBREOFFICE_LABELS_XML GLABELS_TEMPLATE_XML... > static/client/labels.json

LibreOffice: extras/source/labels/labels.xml in github.com/LibreOffice/core
gLabels:     templates/avery-*-templates.xml in github.com/jimevins/glabels-qt
"""
import json
import re
import sys
import xml.etree.ElementTree as ET
from fractions import Fraction
from pathlib import Path

HERE = Path(__file__).resolve().parent / "labels"
PAPER = {"letter": (215.9, 279.4), "a4": (210.0, 297.0)}
IN = 25.4

# Avery's mini-sheets, a quarter of the page, which no printer feeds as
# Letter, and its print-or-write labels, which come on small sheets too.
MINI = re.compile(r"^(2160|2162|2163|5105|5109|5124|5214|5222|54\d\d|2222|5490|6471|724\d\d)$")

# Not stickers: inserts, cards, tags, transfers, signs and odd die cuts.
SKIP = re.compile(
    r"Insert|ID Card|\bID Badge|Photo ID|Fold|Clip|Hanging|Pin Style|Pin Badge|Combi Badge|Invitation|"
    r"Self Laminating|T-shirt|Transfer|Fabric|Transparency|Laminated|Self-Cling|Sign|Tag\b|Topper|Arched|"
    r"Flourish|Star|Bubble|Piggyback|Paper Receipt|Notes|Price|Heavy Duty|Durable|Security|Warning|"
    r"Weatherproof|Kraft|Metallic|Foil|Notarial|Seal|Clear|Neon|Fluoro|Glossy|Textile|Glass|Decal|QR Code|"
    r"A5 |Border Sticker|^L4726",
    re.I,
)

# More not-stickers among the sheets Print Shop does not list, some in German.
EXTRA_SKIP = re.compile(
    r"Card|Karte|Visitenkarten|Diskette|Disk|Divider|Tabs?\b|Index|Binder|Spine|Ordner|Box File|\bCD|DVD|Video|"
    r"Cassette|Tape|Zip|MiniDisc|Mini Disc|Cartridge|Photo|Dia-|Cover|Magnet|Tamper|Contents|Portfolio|Filing|"
    r"Compliment|Eurofolio|Regal|Transparent|Folien",
    re.I,
)

# Look-alikes on the same paper that neither database names: the part
# whose sheet each one shares, per Avery's template compatibility lists.
ALIAS = {
    "letter": {
        "5195": "8195", "8168": "5168",
        # Name badges, eight to a sheet like 5395.
        "41006": "5395", "5140": "5395", "5141": "5395", "5147": "5395", "5153": "5395",
    },
    "a4": {
        "L7962": "L7162", "LR7162": "L7162", "L7963": "L7163", "L7965": "L7165", "L7966": "L7166",
        "L7973": "L7173", "L7173B": "L7173", "LR7167": "L7167", "LR7168": "L7168", "J8169": "L7169",
        "E3210": "J8658", "E3211": "L7651", "E3212": "L7163", "E3213": "L7164", "L7632": "L4732",
        "3663": "3666", "3656": "3652", "3654": "3653", "3483": "C2361",
    },
}

# Sheets in neither database, from the margins on Avery's product pages for
# the Presta templates they share: (shape, w, h, corner, columns, rows,
# left, top, across, down), mm.
HAND = {
    # 2" circles (Presta 94501): 0.625" margins all round.
    "22807": ("round", 50.8, 50.8, 0, 3, 4, 15.875, 15.875, 66.675, 65.617),
    # 2" squares (94107): the same.
    "22806": ("rect", 50.8, 50.8, 3.175, 3, 4, 15.875, 15.875, 66.675, 65.617),
    # 1-1/2" squares (94106): 0.5" each side, 0.625" top and bottom.
    "22805": ("rect", 38.1, 38.1, 3.175, 4, 5, 12.7, 15.875, 50.8, 52.388),
    # 2" by 2-5/8" (94235): 0.1875" each side, 0.5" top and bottom.
    "6572": ("rect", 66.675, 50.8, 1.588, 3, 5, 4.763, 12.7, 69.85, 50.8),
    # 1-1/2" by 2-1/2" ovals (94051): 0.37" each side, 0.625" top and bottom.
    "6583": ("oval", 63.5, 38.1, 0, 3, 6, 9.398, 15.875, 66.802, 41.91),
    # 3-3/4" by 4-3/4" (94254): 0.375" each side, 0.625" top and bottom.
    "6878": ("rect", 95.25, 120.65, 3.175, 2, 2, 9.525, 15.875, 101.6, 127.0),
    # Whole sheets of sticker paper, with no cuts.
    "3383": ("sheet", 215.9, 279.4, 0, 1, 1, 0, 0, 215.9, 279.4),
    "53214": ("sheet", 215.9, 279.4, 0, 1, 1, 0, 0, 215.9, 279.4),
    "3270": ("sheet", 215.9, 279.4, 0, 1, 1, 0, 0, 215.9, 279.4),
}


def inches(s: str) -> float:
    """'2-5/8', '1 3/5', '27/32' or '1' inches, in mm."""
    total = Fraction(0)
    for part in re.split(r"[-\s]+", s.strip()):
        if part:
            total += Fraction(part)
    return float(total) * IN


def size(text: str, paper: str):
    """The (smaller, larger) sides in mm and whether it is round, from a
    Print Shop size: '1 x 2-5/8', 'Ø 1-2/3', '1-1/2 Ø', '63,5 x 38,1'."""
    round_ = "Ø" in text
    text = text.replace("Ø", "").strip()
    conv = inches if paper == "letter" else (lambda v: float(v.replace(",", ".")))
    sides = [conv(v) for v in text.split(" x ")] if " x " in text else [conv(text)] * 2
    return tuple(sorted(sides)), round_


def printshop(paper: str):
    name = "printshop-us.txt" if paper == "letter" else "printshop-a4.txt"
    for line in (HERE / name).read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        kind, title, dims = line.split("|")
        parts, desc = title.split(" ", 1)
        if parts == "Avery":  # 'Avery 3421 Multi-Function Label'
            parts, desc = desc.split(" ", 1)
        if SKIP.search(desc) or SKIP.search(parts) or not dims or MINI.match(parts.split("/")[0]):
            continue
        desc = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", desc)  # 'Easy PeelAddress' lost a ®
        yield kind, parts.split("/"), desc, size(dims, paper)


def libreoffice(path):
    """{part: [layout, ...]} from labels.xml, in mm; LibreOffice keeps
    hundredths of a mm: across;down;width;height;left;top;columns;rows;paper."""
    out = {"letter": {}, "a4": {}, "desc": {}}
    root = ET.parse(path).getroot()
    for m in root.iter("manufacturer"):
        paper = {"Avery Letter Size": "letter", "Avery A4": "a4", "Avery Zweckform": "a4"}.get(m.get("name"))
        if not paper:
            continue
        for label in m.iter("label"):
            f = label.findtext("measure").split(";")
            if f[0] != "S":
                continue
            v = [int(x) / 100 for x in f[1:]]
            g = dict(shape="rect", w=v[2], h=v[3], r=None, cols=int(f[7]), rows=int(f[8]), x=v[4], y=v[5], dx=v[0],
                     dy=v[1])
            if abs(v[8] - PAPER[paper][0]) > 1 or abs(v[9] - PAPER[paper][1]) > 1:
                continue
            part, _, desc = label.findtext("name").partition(" ")
            out["desc"].setdefault(part, desc)
            out[paper].setdefault(part, []).append(g)
    return out


UNITS = {"in": IN, "mm": 1.0, "cm": 10.0, "pt": IN / 72, "pc": IN / 6}


def length(v):
    if v is None:
        return 0.0
    m = re.fullmatch(r"\s*([-\d.]+)\s*([a-z]*)\s*", v)
    return float(m.group(1)) * UNITS[m.group(2) or "pt"]


def glabels(paths):
    """{part: [layout]} from gLabels templates, one label kind, one layout."""
    out = {"letter": {}, "a4": {}, "desc": {}}
    equiv = {}
    for path in paths:
        for t in ET.parse(path).getroot().iter("Template"):
            if t.get("brand") != "Avery":
                continue
            if t.get("equiv"):  # a look-alike, on the same paper as the one it copies
                equiv[t.get("part")] = t.get("equiv")
                continue
            paper = {"US-Letter": "letter", "A4": "a4"}.get(t.get("size"))
            if not paper:
                continue
            labels = [l for l in t if l.tag.startswith("Label-")]
            if len(labels) != 1:
                continue
            l = labels[0]
            layouts = list(l.iter("Layout"))
            if len(layouts) != 1:
                continue
            shape = l.tag[6:]
            if shape == "rectangle":
                g = dict(shape="rect", w=length(l.get("width")), h=length(l.get("height")), r=length(l.get("round")))
            elif shape == "round":
                d = 2 * length(l.get("radius"))
                g = dict(shape="round", w=d, h=d, r=0)
            elif shape == "ellipse":
                g = dict(shape="oval", w=length(l.get("width")), h=length(l.get("height")), r=0)
            else:
                continue
            L = layouts[0]
            g.update(cols=int(L.get("nx")), rows=int(L.get("ny")), x=length(L.get("x0")), y=length(L.get("y0")),
                     dx=length(L.get("dx")), dy=length(L.get("dy")))
            out[paper].setdefault(t.get("part"), []).append(g)
            out["desc"].setdefault(t.get("part"), (t.get("_description") or t.get("description") or "").capitalize())
    for part, to in equiv.items():
        for paper in out:
            if to in out[paper]:
                out[paper].setdefault(part, out[paper][to])
    out["equiv"] = equiv
    return out


def fits(g, want, paper):
    """Whether layout g has labels of the size Print Shop gives, all on the
    paper, none overlapping."""
    pw, ph = PAPER[paper]
    (a, b), _ = want
    if abs(min(g["w"], g["h"]) - a) > 1.0 or abs(max(g["w"], g["h"]) - b) > 1.0:
        return False
    if g["cols"] > 1 and g["dx"] < g["w"] - 0.05 or g["rows"] > 1 and g["dy"] < g["h"] - 0.05:
        return False
    right = g["x"] + (g["cols"] - 1) * g["dx"] + g["w"]
    bottom = g["y"] + (g["rows"] - 1) * g["dy"] + g["h"]
    return g["x"] >= -0.05 and g["y"] >= -0.05 and right <= pw + 0.1 and bottom <= ph + 0.1


def sheet(g, paper):
    """g tidied: rounded, and one page-sized label made a whole sheet."""
    pw, ph = PAPER[paper]
    if g["shape"] != "sheet" and g["cols"] * g["rows"] == 1 and g["w"] > pw - 20 and g["h"] > ph - 20:
        g["shape"] = "sheet"  # one label the size of the page
    return {k: round(v, 3) if isinstance(v, float) else v for k, v in g.items()}


def key(g):
    # Layouts within half a millimetre of each other are one sheet.
    return (g["shape"], g["cols"], g["rows"]) + tuple(round(g[k] * 2) for k in ("w", "h", "x", "y", "dx", "dy"))


def kind(desc):
    return "N" if re.search(r"Badge", desc, re.I) else "A" if re.search(r"Address|Shipping|Mailing", desc, re.I) else "O"


def extras(paper, lo, gl, groups):
    """Every other Avery sheet either database has, past Print Shop's list:
    gLabels first, which keeps the shape, then LibreOffice."""
    have = {p for e in groups.values() for p in e["parts"]}
    for db in (gl, lo):
        for part, cands in db[paper].items():
            desc = gl["desc"].get(part) or lo["desc"].get(part) or "Label"
            base = part.split(".")[0]  # gLabels' 3274.1, .2: one sheet's kinds
            if part in have or base in have or SKIP.search(desc) or EXTRA_SKIP.search(desc) or SKIP.search(part) or MINI.match(base):
                continue
            g = cands[0]
            if g["shape"] == "rect" and re.search(r"Round|Circ|Kreis|\brund", desc, re.I):
                g = dict(g, shape="round", r=0)  # LibreOffice keeps no shape
            elif g["shape"] == "rect" and re.search(r"Oval", desc, re.I):
                g = dict(g, shape="oval", r=0)
            if g["r"] is None:
                g = dict(g, r=1.5)
            if not fits(g, ((min(g["w"], g["h"]), max(g["w"], g["h"])), False), paper):
                continue
            g = sheet(dict(g), paper)
            entry = groups.setdefault(key(g), dict(parts=[], desc=desc, kind=kind(desc), **g))
            if base not in entry["parts"]:
                entry["parts"].append(base)
            have.add(part)


def main(lo_path, gl_paths):
    lo, gl = libreoffice(lo_path), glabels(gl_paths)
    out, missing = {}, []
    for paper in PAPER:
        groups = {}
        for kind, parts, desc, want in printshop(paper):
            (a, b), round_ = want
            g = None
            for part in parts:
                if part in HAND:
                    s, w, h, r, nx, ny, x, y, dx, dy = HAND[part]
                    g = dict(shape=s, w=w, h=h, r=r, cols=nx, rows=ny, x=x, y=y, dx=dx, dy=dy)
                    break
                # Its own name, the part it copies, or without the letters
                # (LibreOffice files Zweckform's L4784 as 4784).
                # LibreOffice first, which measures Avery's own templates.
                names = [n for n in (part, ALIAS[paper].get(part), re.sub(r"^[A-Z]+", "", part), gl["equiv"].get(part)) if n]
                cands = [c for db in (lo, gl) for n in names for c in db[paper].get(n, []) if fits(c, want, paper)]
                if cands:
                    g = dict(cands[0])
                    break
            if not g:
                missing.append(f"{paper} {'/'.join(parts)} {desc} {want}")
                continue
            # The shape, which LibreOffice does not keep.
            known = [c for part in parts for c in gl[paper].get(part, []) if fits(c, want, paper)]
            if g["shape"] == "rect" and (round_ or (known and known[0]["shape"] == "round")):
                g["shape"] = "round"
            elif g["shape"] == "rect" and ("Oval" in desc or (known and known[0]["shape"] == "oval")):
                g["shape"] = "oval"
            if g["r"] is None:
                g["r"] = known[0]["r"] if known and known[0]["shape"] == "rect" else (0 if g["shape"] != "rect" else 1.5)
            g = sheet(g, paper)
            entry = groups.setdefault(key(g), dict(parts=[], desc=desc, kind=kind, **g))
            for part in parts:
                if part not in entry["parts"]:
                    entry["parts"].append(part)
        extras(paper, lo, gl, groups)
        out[paper] = list(groups.values())
    for m in missing:
        print("no layout:", m, file=sys.stderr)
    json.dump(out, sys.stdout, ensure_ascii=False, separators=(",", ":"))
    sys.stdout.write("\n")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2:])
