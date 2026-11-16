"""
Cuts the font the coins and stamps are lettered in down to the characters a
Mii's name or a short message is likely to use: Latin, kana, the JIS X 0208
kanji, full-width forms and the symbols Mii names carry (hearts, stars,
notes). The browser fetches it only when there is text to letter.

    pip install fonttools
    python tools/subset_font.py ~/Library/Fonts.old/FOT-RodinNTLGPro-B.otf static/fonts/RodinNTLGPro-B-print.otf

The result stays an OpenType (CFF) font, which stb_truetype reads in the core.
"""
import sys

from fontTools import subset


def jis_kanji():
    """Every kanji in JIS X 0208 (levels 1 and 2), via EUC-JP."""
    chars = []
    for row in range(16, 85):
        for col in range(1, 95):
            try:
                chars.append(bytes([0xA0 + row, 0xA0 + col]).decode("euc_jp"))
            except UnicodeDecodeError:
                pass
    return chars


RANGES = [
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x017F),  # Latin-1 and Latin Extended-A
    (0x0391, 0x03C9),  # Greek
    (0x0401, 0x0451),  # Cyrillic
    (0x2010, 0x203B),  # punctuation
    (0x2160, 0x216B),  # Roman numerals
    (0x2190, 0x21FF),  # arrows
    (0x2460, 0x2473),  # circled numbers
    (0x2500, 0x254B),  # box drawing
    (0x25A0, 0x25FF),  # shapes
    (0x2600, 0x266F),  # stars, hearts, notes
    (0x3000, 0x30FF),  # CJK punctuation, hiragana, katakana
    (0xFF01, 0xFF9F),  # full-width and half-width forms
    (0xFFE0, 0xFFE6),
]


def main(src, dst):
    text = "".join(chr(c) for a, b in RANGES for c in range(a, b + 1)) + "".join(jis_kanji())
    options = subset.Options()
    options.layout_features = ["kern", "palt"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.hinting = False
    font = subset.load_font(src, options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(text=text)
    subsetter.subset(font)
    subset.save_font(font, dst, options)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
