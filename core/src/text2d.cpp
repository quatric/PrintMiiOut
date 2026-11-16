#include "text2d.hpp"

#include <algorithm>
#include <cmath>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace pmo {

struct Font::Impl {
	std::vector<uint8_t> data;
	stbtt_fontinfo info{};
	double scale = 1;  // font units to em
};

Font::Font() : impl_(new Impl) {}
Font::~Font() = default;

bool Font::load(const uint8_t* data, size_t size) {
	ok_ = false;
	if (!data || size < 12) return false;
	impl_->data.assign(data, data + size);
	int offset = stbtt_GetFontOffsetForIndex(impl_->data.data(), 0);
	if (offset < 0 || !stbtt_InitFont(&impl_->info, impl_->data.data(), offset)) return false;
	impl_->scale = stbtt_ScaleForMappingEmToPixels(&impl_->info, 1.0f);
	ok_ = impl_->scale > 0;
	return ok_;
}

std::u32string decodeUtf8(const std::string& s) {
	std::u32string out;
	size_t i = 0;
	while (i < s.size()) {
		unsigned char c = (unsigned char)s[i];
		int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		if (n < 0 || i + n >= s.size()) {
			i++;
			continue;
		}
		char32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
		bool good = true;
		for (int k = 1; k <= n; k++) {
			unsigned char d = (unsigned char)s[i + k];
			if ((d >> 6) != 2) {
				good = false;
				break;
			}
			cp = (cp << 6) | (d & 0x3F);
		}
		i += good ? n + 1 : 1;
		if (good) out.push_back(cp);
	}
	return out;
}

Line Font::layOut(const std::string& utf8) const {
	Line line;
	if (!ok_) return line;
	const stbtt_fontinfo* info = &impl_->info;
	const double s = impl_->scale;
	double pen = 0;
	int prev = 0;
	bool inked = false;
	for (char32_t cp : decodeUtf8(utf8)) {
		if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) continue;
		int g = stbtt_FindGlyphIndex(info, int(cp));
		if (!g) {
			line.missing++;
			continue;
		}
		if (prev) pen += stbtt_GetGlyphKernAdvance(info, prev, g) * s;
		int adv = 0, lsb = 0;
		stbtt_GetGlyphHMetrics(info, g, &adv, &lsb);
		Glyph glyph;
		glyph.x = pen;
		glyph.advance = adv * s;
		stbtt_vertex* v = nullptr;
		int n = stbtt_GetGlyphShape(info, g, &v);
		Ring ring;
		P2 at;
		auto close = [&]() {
			if (ring.size() >= 3) glyph.rings.push_back(ring);
			ring.clear();
		};
		for (int k = 0; k < n; k++) {
			P2 to(v[k].x * s, v[k].y * s);
			switch (v[k].type) {
			case STBTT_vmove:
				close();
				ring.push_back(to);
				break;
			case STBTT_vline:
				ring.push_back(to);
				break;
			case STBTT_vcurve: {
				P2 c(v[k].cx * s, v[k].cy * s);
				for (int q = 1; q <= 8; q++) {
					double t = q / 8.0, u = 1 - t;
					ring.push_back(at * (u * u) + c * (2 * u * t) + to * (t * t));
				}
				break;
			}
			case STBTT_vcubic: {
				P2 c0(v[k].cx * s, v[k].cy * s), c1(v[k].cx1 * s, v[k].cy1 * s);
				for (int q = 1; q <= 12; q++) {
					double t = q / 12.0, u = 1 - t;
					ring.push_back(at * (u * u * u) + c0 * (3 * u * u * t) + c1 * (3 * u * t * t) + to * (t * t * t));
				}
				break;
			}
			}
			at = to;
		}
		close();
		stbtt_FreeShape(info, v);
		for (const Ring& r : glyph.rings)
			for (const P2& p : r) {
				line.yMin = inked ? std::min(line.yMin, p.y) : p.y;
				line.yMax = inked ? std::max(line.yMax, p.y) : p.y;
				inked = true;
			}
		pen += glyph.advance;
		line.glyphs.push_back(std::move(glyph));
		prev = g;
	}
	line.advance = pen;
	return line;
}

TextPath Font::outline(const std::string& utf8) const {
	TextPath out;
	if (!ok_) return out;
	const stbtt_fontinfo* info = &impl_->info;
	const double s = impl_->scale;
	int ascent = 0, descent = 0, gap = 0;
	stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
	out.ascent = ascent * s;
	out.descent = descent * s;
	double pen = 0;
	int prev = 0;
	for (char32_t cp : decodeUtf8(utf8)) {
		if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) continue;
		int g = stbtt_FindGlyphIndex(info, int(cp));
		if (!g) {
			out.missing++;
			continue;
		}
		if (prev) pen += stbtt_GetGlyphKernAdvance(info, prev, g) * s;
		int adv = 0, lsb = 0;
		stbtt_GetGlyphHMetrics(info, g, &adv, &lsb);
		OutlineGlyph glyph;
		glyph.cp = cp;
		glyph.x = pen;
		glyph.advance = adv * s;
		stbtt_vertex* v = nullptr;
		int n = stbtt_GetGlyphShape(info, g, &v);
		std::vector<float>& path = glyph.path;
		float ax = 0, ay = 0;
		bool open = false, inked = false;
		auto ink = [&](double x, double y) {
			glyph.x0 = inked ? std::min(glyph.x0, x) : x;
			glyph.y0 = inked ? std::min(glyph.y0, y) : y;
			glyph.x1 = inked ? std::max(glyph.x1, x) : x;
			glyph.y1 = inked ? std::max(glyph.y1, y) : y;
			inked = true;
		};
		for (int k = 0; k < n; k++) {
			float x = float(v[k].x * s), y = float(v[k].y * s);
			switch (v[k].type) {
			case STBTT_vmove:
				if (open) path.push_back(3);
				path.insert(path.end(), {0, x, y});
				open = true;
				break;
			case STBTT_vline:
				path.insert(path.end(), {1, x, y});
				break;
			case STBTT_vcurve: {
				// A quadratic through c is the cubic with controls 2/3 of
				// the way from each end to c.
				float cx = float(v[k].cx * s), cy = float(v[k].cy * s);
				path.insert(path.end(), {2, ax + (cx - ax) * 2 / 3, ay + (cy - ay) * 2 / 3, x + (cx - x) * 2 / 3,
				                         y + (cy - y) * 2 / 3, x, y});
				ink(cx, cy);
				break;
			}
			case STBTT_vcubic:
				path.insert(path.end(), {2, float(v[k].cx * s), float(v[k].cy * s), float(v[k].cx1 * s),
				                         float(v[k].cy1 * s), x, y});
				ink(v[k].cx * s, v[k].cy * s);
				ink(v[k].cx1 * s, v[k].cy1 * s);
				break;
			}
			ink(x, y);
			ax = x;
			ay = y;
		}
		if (open) path.push_back(3);
		stbtt_FreeShape(info, v);
		// The control points bound the curve; the font's own box is tighter.
		int bx0, by0, bx1, by1;
		if (inked && stbtt_GetGlyphBox(info, g, &bx0, &by0, &bx1, &by1)) {
			glyph.x0 = bx0 * s;
			glyph.y0 = by0 * s;
			glyph.x1 = bx1 * s;
			glyph.y1 = by1 * s;
		}
		pen += glyph.advance;
		out.glyphs.push_back(std::move(glyph));
		prev = g;
	}
	out.advance = pen;
	return out;
}

}  // namespace pmo
