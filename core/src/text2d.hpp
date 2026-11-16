// Lettering for coins and stamps: glyph outlines from an OpenType font (the
// site uses Rodin NTLG, read with stb_truetype), laid in a line or round an arc.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "raster.hpp"

namespace pmo {

/** One glyph of a line, its outline in em units with the baseline at y = 0
 *  and its pen position at x = 0. */
struct Glyph {
	std::vector<Ring> rings;
	double x = 0;        // pen position along the line, em
	double advance = 0;  // em
};

/** One glyph as the font draws it, for vector output: its path in em units
 *  (y up, baseline at 0, pen at 0) as a flat list of commands, each a code
 *  then its points: 0 move x y, 1 line x y, 2 cubic x1 y1 x2 y2 x y, 3 close.
 *  Quadratic curves come out as the cubics they equal. */
struct OutlineGlyph {
	char32_t cp = 0;
	double x = 0;        // kerned pen position along the line, em
	double advance = 0;  // em
	double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // ink box, em (all 0 for a space)
	std::vector<float> path;
};

struct TextPath {
	std::vector<OutlineGlyph> glyphs;
	double advance = 0;  // em
	double ascent = 0, descent = 0;  // the font's, em (descent negative)
	int missing = 0;
};

struct Line {
	std::vector<Glyph> glyphs;
	double advance = 0;           // the whole line, em
	double yMin = 0, yMax = 0;    // ink extent, em (0, 0 if nothing has ink)
	int missing = 0;              // characters the font lacks, left out
};

class Font {
public:
	Font();
	~Font();
	/** Reads an OpenType or TrueType font (a copy is kept). False if it is not one. */
	bool load(const uint8_t* data, size_t size);
	bool loaded() const { return ok_; }
	/** The glyphs for UTF-8 text, kerned, in one line. */
	Line layOut(const std::string& utf8) const;
	/** The same glyphs with their curves kept, for printing on paper. */
	TextPath outline(const std::string& utf8) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	bool ok_ = false;
};

/** The code points in UTF-8 text (malformed bytes are skipped). */
std::u32string decodeUtf8(const std::string& s);

}  // namespace pmo
