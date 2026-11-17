#include "trinkets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "fuse.hpp"
#include "raster.hpp"
#include "sdf.hpp"
#include "split.hpp"

namespace pmo {

namespace {

using LogFn = std::function<void(const std::string&)>;

/** Where a design goes, centred on the origin, Y up: a circle or a rounded rectangle. */
struct Area {
	bool round = true;
	double r = 10, hx = 10, hy = 10, corner = 0;
	double sd(double x, double y) const { return round ? std::hypot(x, y) - r : roundRect(x, y, hx, hy, corner); }
	double halfW() const { return round ? r : hx; }
	double halfH() const { return round ? r : hy; }
	Area inset(double d) const {
		Area a = *this;
		a.r -= d;
		a.hx -= d;
		a.hy -= d;
		a.corner = std::max(0.0, corner - d);
		return a;
	}
};

/** The flat design, in millimetres: what prints (ink, lettering, border)
 *  and the head's silhouette, with their signed distances. */
struct Design {
	Mask ink, head;
	Distance2 dInk, dHead;
};

struct Style {
	double line = 0.55;      // outline width, mm
	double minWidth = 0.5;   // thinnest feature, mm
	double borderWidth = 0.7;
};

const double kPi = 3.14159265358979323846;

/** Lays the face, the lettering and the border out in the area. */
Design compose(const FaceSource& face, const Font* font, const TrinketOptions& o, const Area& area, const Style& st,
               const LogFn& log) {
	Design d;
	const double halfW = area.halfW(), halfH = area.halfH();
	const double px = std::clamp(2 * std::max(halfW, halfH) / 1400, 0.02, 0.04);
	d.ink.cover(-halfW - 1, -halfH - 1, halfW + 1, halfH + 1, px, 0);
	d.head.reset(d.ink.x0, d.ink.y0, px, d.ink.w, d.ink.h, 0);
	char buf[200];

	// A border just inside the edge, solid or dashed.
	Area inner = area.inset(0.3);
	if (o.border) {
		const double bw = std::max(st.borderWidth, st.minWidth);
		double perimeter = area.round ? 2 * kPi * area.r : 4 * (area.hx + area.hy);
		int dashes = std::max(8, int(std::lround(perimeter / 3.0)));
		for (int j = 0; j < d.ink.h; j++)
			for (int i = 0; i < d.ink.w; i++) {
				double x = d.ink.cx(i), y = d.ink.cy(j), s = area.sd(x, y);
				if (s > 0 || s < -bw) continue;
				if (o.border == 2) {
					double t;  // how far round the edge, 0 to 1
					if (area.round) {
						t = (std::atan2(y, x) + kPi) / (2 * kPi);
					} else {  // walk the rectangle's sides
						double ax = std::fabs(x) / area.hx, ay = std::fabs(y) / area.hy;
						double along = ax > ay ? (area.hy + y * (x > 0 ? 1 : -1)) + (x > 0 ? 0 : 2 * area.hx + 2 * area.hy)
						                       : (area.hx + x * (y > 0 ? -1 : 1)) + (y > 0 ? 2 * area.hy : 4 * area.hy + 2 * area.hx);
						t = along / perimeter;
					}
					double phase = t * dashes - std::floor(t * dashes);
					if (phase > 0.6) continue;
				}
				d.ink.at(i, j) = 1;
			}
		inner = area.inset(bw + 0.6);
	}

	// Lettering, round the top or in a line above or below the face.
	double faceTop = inner.halfH(), faceBottom = -inner.halfH();
	double arcInner = 1e300, arcHalfAngle = 0;  // the face keeps inside the arc's inner edge
	std::vector<Ring> letters;
	Line line;
	if (font && font->loaded() && !o.text.empty()) line = font->layOut(o.text);
	if (line.missing) {
		std::snprintf(buf, sizeof buf, "Lettering: %d characters are not in the font and were left out", line.missing);
		log(buf);
	}
	bool hasText = line.advance > 0 && line.yMax > line.yMin;
	const double gap = 0.05 * std::min(inner.halfW(), inner.halfH()) + 0.3;
	if (hasText) {
		const double top = line.yMax, bottom = std::min(line.yMin, 0.0);
		const double tall = std::max(top - bottom, 0.7);
		double em;
		if (inner.round && !o.textBelow) {
			const double R = inner.r, band = 0.27 * R, maxAngle = 2.5;
			em = band / tall;
			double rb = R - top * em;
			for (int it = 0; it < 3; it++) {
				rb = R - top * em;
				double angle = line.advance * em / rb;
				if (angle > maxAngle) em *= maxAngle / angle;
			}
			rb = R - top * em;
			for (const Glyph& g : line.glyphs) {
				const double mid = g.x + g.advance / 2;
				const double th = kPi / 2 + (line.advance / 2 - mid) * em / rb;
				const P2 n(std::cos(th), std::sin(th)), t(std::sin(th), -std::cos(th));
				for (const Ring& r : g.rings) {
					Ring out;
					for (const P2& p : r) out.push_back(n * (rb + p.y * em) + t * ((g.x + p.x - mid) * em));
					letters.push_back(out);
				}
			}
			arcInner = rb + bottom * em - gap;
			arcHalfAngle = line.advance * em / rb / 2 + 0.15;
			std::snprintf(buf, sizeof buf, "Lettering: %zu glyphs round the top, %.2f mm high", line.glyphs.size(), em);
			log(buf);
		} else {
			const bool below = o.textBelow;
			double band = inner.round ? 0.27 * inner.r : std::min(0.44 * inner.hx, 0.3 * inner.hy);
			double yBot, yTop, width;
			if (inner.round) {
				yBot = below ? -0.84 * inner.r : 0.84 * inner.r - band;
				yTop = yBot + band;
				double edge = below ? yBot : yTop;
				width = 2 * std::sqrt(std::max(0.0, inner.r * inner.r - edge * edge)) * 0.94;
			} else {
				yBot = below ? -inner.hy : inner.hy - band;
				yTop = yBot + band;
				width = 2 * inner.hx * 0.96;
			}
			em = std::min(band / tall, width / line.advance);
			const double base = (yTop + yBot) / 2 - (top + bottom) / 2 * em, left = -line.advance * em / 2;
			for (const Glyph& g : line.glyphs)
				for (const Ring& r : g.rings) {
					Ring out;
					for (const P2& p : r) out.push_back({left + (g.x + p.x) * em, base + p.y * em});
					letters.push_back(out);
				}
			if (below)
				faceBottom = base + top * em + gap;
			else
				faceTop = base + bottom * em - gap;
			std::snprintf(buf, sizeof buf, "Lettering: %zu glyphs %s the face, %.2f mm high", line.glyphs.size(),
			              below ? "below" : "above", em);
			log(buf);
		}
	}

	// The face: as big as fits. Its box is checked by a squarish oval in
	// it, since a head leaves the corners of its box empty.
	const double fw = face.x1 - face.x0, fh = face.z1 - face.z0;
	if (!(fw > 0 && fh > 0)) throw std::runtime_error("face: nothing to draw");
	auto fits = [&](double s, double yc) {
		const double w = fw * s, h = fh * s;
		if (yc + h / 2 > faceTop + 1e-9 || yc - h / 2 < faceBottom - 1e-9) return false;
		const double slack = inner.round ? 0.02 * inner.r : 0.0;
		for (int q = 0; q < 32; q++) {
			double a = 2 * kPi * q / 32, c = std::cos(a), sn = std::sin(a);
			double x = w / 2 * std::copysign(std::pow(std::fabs(c), 0.6), c);
			double y = yc + h / 2 * std::copysign(std::pow(std::fabs(sn), 0.6), sn);
			if (inner.sd(x, y) > slack) return false;
			if (y > 0 && std::fabs(std::atan2(x, y)) < arcHalfAngle && std::hypot(x, y) > arcInner) return false;
		}
		return true;
	};
	auto place = [&](double s, double& yc) {
		const double h = fh * s;
		double lo = faceBottom + h / 2, hi = faceTop - h / 2;
		if (lo > hi) return false;
		double first = 1e300, last = -1e300;
		for (int k = 0; k <= 40; k++) {
			double y = hi - (hi - lo) * k / 40;
			if (fits(s, y)) {
				first = std::min(first, y);
				last = std::max(last, y);
			}
		}
		if (first > last) return false;
		yc = (first + last) / 2;
		return true;
	};
	double sLo = 0, sHi = 2 * std::max(halfW, halfH) / std::min(fw, fh), yc = 0;
	for (int it = 0; it < 40; it++) {
		double s = (sLo + sHi) / 2, y;
		if (place(s, y))
			sLo = s;
		else
			sHi = s;
	}
	const double s = sLo;
	if (!(s > 0) || !place(s, yc)) throw std::runtime_error("face: no room for the face");
	FaceArt art = face.draw(px / s, st.line / s, st.minWidth / s);
	const double fcx = (face.x0 + face.x1) / 2, fcz = (face.z0 + face.z1) / 2;
	for (int j = 0; j < d.ink.h; j++)
		for (int i = 0; i < d.ink.w; i++) {
			double x = d.ink.cx(i), y = d.ink.cy(j);
			if (inner.sd(x, y) > 0) continue;
			int ai, aj;
			if (!art.ink.cell(fcx + x / s, fcz + (y - yc) / s, ai, aj)) continue;
			if (art.ink.at(ai, aj)) d.ink.at(i, j) = 1;
			if (art.head.at(ai, aj)) d.head.at(i, j) = 1;
		}
	std::snprintf(buf, sizeof buf, "Face: %.1f by %.1f mm", fw * s, fh * s);
	log(buf);

	if (!letters.empty()) {
		Mask text;
		text.reset(d.ink.x0, d.ink.y0, px, d.ink.w, d.ink.h, 0);
		fillRings(text, letters);
		for (size_t k = 0; k < text.a.size(); k++)
			if (text.a[k] && area.sd(d.ink.cx(int(k % d.ink.w)), d.ink.cy(int(k / d.ink.w))) < 0) d.ink.a[k] = 1;
	}
	tidy(d.ink, 0.15 * st.minWidth * st.minWidth);
	d.dInk = distanceTo(d.ink);
	d.dHead = distanceTo(d.head);
	return d;
}

/** A solid meshed and tidied, turned from where it is built to where it prints. */
Mesh solidify(const Sdf& f, V3 lo, V3 hi, const Pose& pose, double voxel, size_t maxTriangles, const LogFn& log,
               double zStretch = 1.0) {
	// The box, turned.
	V3 plo(1e300, 1e300, 1e300), phi(-1e300, -1e300, -1e300);
	for (int c = 0; c < 8; c++) {
		V3 p = pose.apply(V3(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z));
		plo = vmin(plo, p);
		phi = vmax(phi, p);
	}
	Mesh m = meshSdf([&](const V3& q) { return f(pose.invert(q)); }, plo, phi, voxel, log, zStretch);
	simplify(m, maxTriangles, 0.2 * voxel, nullptr, true);
	return m;
}

/** Half-cylinder crush ribs standing protrude proud of a round bore. */
double roundRibs(double x, double y, double boreRadius, double protrude, const std::vector<double>& angles) {
	const double rr = 0.5;
	double best = 1e300;
	for (double a : angles) {
		double c = boreRadius + rr - protrude;
		best = std::min(best, std::hypot(x - c * std::cos(a), y - c * std::sin(a)) - rr);
	}
	return best;
}

/** The same for a rectangular bore: two ribs on each side. */
double rectRibs(double x, double y, double hx, double hy, double protrude) {
	const double rr = 0.5, cx = hx + rr - protrude, cy = hy + rr - protrude;
	double best = 1e300;
	for (double s : {-0.5, 0.5}) {
		best = std::min(best, std::hypot(std::fabs(x) - cx, y - s * hy) - rr);
		best = std::min(best, std::hypot(x - s * hx, std::fabs(y) - cy) - rr);
	}
	return best;
}

/** A keychain loop: a ring standing upright on top of a piece, its hole
 *  running along Y, centred at (0, 0, zc). */
double loopRing(const V3& p, double zc, double radius, double tube) {
	return std::hypot(std::hypot(p.x, p.z - zc) - radius, p.y) - tube;
}
const double kLoopRadius = 3.2, kLoopTube = 1.3;  // mm: a 3.8 mm hole

/** A hole along Y through (x, z) = 0, pointed at the top so it prints without support. */
double teardrop(double x, double z, double r) {
	double circle = std::hypot(x, z) - r;
	double diamond = (std::fabs(x) + z) * std::sqrt(0.5) - r;
	return std::min(circle, std::max(diamond, -z));
}

/** A coin cut to the head's own outline instead of a circle or a square:
 *  the head seen from the front, grown by margin all round (holes filled),
 *  with the lettering, if any, on a rounded tab under the chin. The outline
 *  is returned as a mask on the design's grid. */
Design composeFaceShape(const FaceSource& face, const Font* font, const TrinketOptions& o, double D, double margin,
                        const Style& st, Mask& outline, const LogFn& log) {
	char buf[200];
	const double fw = face.x1 - face.x0, fh = face.z1 - face.z0;
	if (!(fw > 0 && fh > 0)) throw std::runtime_error("face: nothing to draw");
	const double s = (D - 2 * margin) / std::max(fw, fh);  // mm per head unit
	const double w = fw * s, h = fh * s;

	// The lettering on its tab, centred under the face.
	Line line;
	if (font && font->loaded() && !o.text.empty()) line = font->layOut(o.text);
	if (line.missing) {
		std::snprintf(buf, sizeof buf, "Lettering: %d characters are not in the font and were left out", line.missing);
		log(buf);
	}
	const bool hasText = line.advance > 0 && line.yMax > line.yMin;
	double tabTop = -h / 2, tabBottom = -h / 2, tabHalfW = 0, em = 0, base = 0;
	std::vector<Ring> letters;
	if (hasText) {
		const double top = line.yMax, bottom = std::min(line.yMin, 0.0), tall = std::max(top - bottom, 0.7);
		em = std::min(0.14 * D / tall, 0.95 * std::max(w, 0.6 * D) / line.advance);
		const double textH = tall * em, textW = line.advance * em, pad = margin + 0.4;
		// The tab tucks under the chin, so its top hides behind the face's outline.
		tabTop = -h / 2 + 0.25 * h;
		const double textTop = -h / 2 - margin - 0.6;
		base = textTop - top * em;
		tabBottom = base + bottom * em - pad;
		tabHalfW = textW / 2 + pad;
		for (const Glyph& g : line.glyphs)
			for (const Ring& r : g.rings) {
				Ring out;
				for (const P2& p : r) out.push_back({-textW / 2 + (g.x + p.x) * em, base + p.y * em});
				letters.push_back(out);
			}
		(void)textH;
		std::snprintf(buf, sizeof buf, "Lettering: %zu glyphs on a tab below the face, %.2f mm high", line.glyphs.size(),
		              em);
		log(buf);
	}

	const double halfW = std::max(w / 2, tabHalfW) + margin + 3, top = h / 2 + margin + 3,
	             bottom = std::min(-h / 2, tabBottom) - margin - 3;
	const double px = std::clamp(D / 1400, 0.02, 0.04);
	Design d;
	d.ink.cover(-halfW, bottom, halfW, top, px, 0);
	d.head.reset(d.ink.x0, d.ink.y0, px, d.ink.w, d.ink.h, 0);
	FaceArt art = face.draw(px / s, st.line / s, st.minWidth / s);
	const double fcx = (face.x0 + face.x1) / 2, fcz = (face.z0 + face.z1) / 2;
	for (int j = 0; j < d.ink.h; j++)
		for (int i = 0; i < d.ink.w; i++) {
			int ai, aj;
			if (!art.ink.cell(fcx + d.ink.cx(i) / s, fcz + d.ink.cy(j) / s, ai, aj)) continue;
			if (art.ink.at(ai, aj)) d.ink.at(i, j) = 1;
			if (art.head.at(ai, aj)) d.head.at(i, j) = 1;
		}
	std::snprintf(buf, sizeof buf, "Face: %.1f by %.1f mm", w, h);
	log(buf);

	// The outline: the head grown by the margin, with the tab, its holes
	// (between strands of hair, say) filled, and its notches rounded over.
	outline = d.head;
	tidy(outline, 0.25 * D * D);
	offset(outline, margin + 1.0);
	if (hasText) {
		for (int j = 0; j < outline.h; j++)
			for (int i = 0; i < outline.w; i++)
				if (roundRect(outline.cx(i), outline.cy(j) - (tabTop + tabBottom) / 2, tabHalfW, (tabTop - tabBottom) / 2,
				              std::min(3.0, (tabTop - tabBottom) / 2)) <= 0)
					outline.at(i, j) = 1;
	}
	offset(outline, -1.0);
	tidy(outline, 0.25 * D * D);

	if (!letters.empty()) {
		Mask text;
		text.reset(d.ink.x0, d.ink.y0, px, d.ink.w, d.ink.h, 0);
		fillRings(text, letters);
		unite(d.ink, text);
	}
	tidy(d.ink, 0.15 * st.minWidth * st.minWidth);
	d.dInk = distanceTo(d.ink);
	d.dHead = distanceTo(d.head);
	return d;
}

Mesh makeCoin(const FaceSource& face, const Font* font, const TrinketOptions& o, const LogFn& log,
              const std::function<void(const char*)>& tick) {
	const double D = std::clamp(o.size > 0 ? o.size : 40.0, 20.0, 80.0), R = D / 2;
	const double rimWidth = std::max(1.4, 0.04 * D);
	const double T = o.engraved ? 3.0 : 2.4;
	const double hLow = 0.5, hHigh = 1.0, rimHeight = o.engraved ? 0.6 : 1.2;
	Style st;
	st.line = 0.5;
	st.minWidth = 0.45;
	TrinketOptions to = o;
	to.border = 0;

	// The coin's outline as a signed distance across it, and its box.
	Design d;
	Area shape;
	Distance2 dOutline;
	double x0 = -R, y0 = -R, x1 = R, y1 = R;
	if (o.faceShape) {
		Mask outline;
		d = composeFaceShape(face, font, to, D, o.rim ? rimWidth + 0.9 : 1.2, st, outline, log);
		dOutline = distanceTo(outline);
		x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
		for (int j = 0; j < outline.h; j++)
			for (int i = 0; i < outline.w; i++)
				if (outline.at(i, j)) {
					x0 = std::min(x0, outline.cx(i) - outline.px);
					x1 = std::max(x1, outline.cx(i) + outline.px);
					y0 = std::min(y0, outline.cy(j) - outline.px);
					y1 = std::max(y1, outline.cy(j) + outline.px);
				}
		if (!(x1 > x0)) throw std::runtime_error("face: no outline to cut the coin to");
		char buf[120];
		std::snprintf(buf, sizeof buf, "Outline: %.1f by %.1f mm", x1 - x0, y1 - y0);
		log(buf);
	} else {
		shape.round = !o.square;
		shape.r = R;
		shape.hx = shape.hy = R;
		shape.corner = 0.18 * D;
		Area art = shape.inset(o.rim ? rimWidth + 0.8 : 1.2);
		d = compose(face, font, to, art, st, log);
	}
	auto outlineSd = [&](double x, double y) { return o.faceShape ? dOutline(x, y) : shape.sd(x, y); };

	// A keychain loop: a ring standing above the top of the coin, on the
	// middle line, its hole clear of the edge.
	const double loopHole = std::max(1.8, 0.045 * D), loopWall = std::max(1.6, 0.04 * D);
	const double loopOuter = loopHole + loopWall;
	double loopY = 0;
	if (o.keychain) {
		double top = R;
		if (o.faceShape) {
			// The outline's highest point near the middle line.
			top = -1e300;
			for (double x = -0.1 * D; x <= 0.1 * D; x += 0.1 * D / 20)
				for (double y = y1; y > y0; y -= 0.05)
					if (outlineSd(x, y) <= 0) {
						top = std::max(top, y);
						break;
					}
		}
		loopY = top + loopHole + 0.4;
		y1 = std::max(y1, loopY + loopOuter + 0.5);
		char buf[120];
		std::snprintf(buf, sizeof buf, "Keychain loop: %.1f mm hole", 2 * loopHole);
		log(buf);
	}
	auto loopSd = [&](double x, double y) {
		const double rho = std::hypot(x, y - loopY);
		return std::max(rho - loopOuter, loopHole - rho);
	};
	tick("design");

	const double cb = 0.4, ct = 0.3, taper = 0.2;
	const Distance2 &dInk = d.dInk, &dHead = d.dHead;
	auto clip = [&](double s) { return s + (o.rim ? rimWidth + 0.3 : 0.5); };
	Sdf f = [&](const V3& p) {
		const double s = outlineSd(p.x, p.y);
		double body = extrude(s + std::max(0.0, cb - p.z) + (o.rim ? 0.0 : std::max(0.0, p.z - (T - ct))), p.z, 0, T);
		if (o.keychain) {
			// The ring, as thick as the coin, its hole cut through the rim too.
			double ring = extrude(loopSd(p.x, p.y) + std::max(0.0, cb - p.z) + std::max(0.0, p.z - (T - ct)), p.z, 0, T);
			body = std::min(body, ring);
		}
		if (o.rim) {
			double ring = std::max(s, -(s + rimWidth));
			body = std::min(body, extrude(ring + std::max(0.0, p.z - (T + rimHeight - 0.3)), p.z, T - 0.5, T + rimHeight));
		}
		if (o.keychain) body = std::max(body, -(std::hypot(p.x, p.y - loopY) - loopHole));
		const double c = clip(s);
		if (o.engraved) {
			double cutLow = extrude(std::max(dHead(p.x, p.y), c), p.z, T - hLow, T + 5);
			double cutHigh = extrude(std::max(dInk(p.x, p.y), c), p.z, T - hHigh, T + 5);
			return std::max(body, -std::min(cutLow, cutHigh));
		}
		double plateau = extrude(std::max(dHead(p.x, p.y), c), p.z, T - 0.5, T + hLow);
		double ink = extrude(std::max(dInk(p.x, p.y) - taper * std::clamp(T + hHigh - p.z, 0.0, hHigh), c), p.z, T - 0.5,
		                     T + hHigh);
		return std::min(body, std::min(plateau, ink));
	};
	const double voxel = std::clamp(std::max(x1 - x0, y1 - y0) / 500, 0.06, 0.11);
	const double pad = o.faceShape ? 1.0 : 0.0;
	Mesh coin = solidify(f, V3(x0 - pad, y0 - pad, 0), V3(x1 + pad, y1 + pad, T + std::max(rimHeight, hHigh)), Pose(),
	                     voxel, o.maxTriangles,
	                     log, 1.5);
	tick("carve");
	std::vector<Mesh> pieces{std::move(coin)};
	return layOut(pieces, 6.0, 180.0);
}

/** The stamp that looks like a bullet: a grip with a strap hole and a
 *  thumb dip, the face on a plug that pushes into it (keyed by a flat, so
 *  the face only goes in the right way round), and a cap. The plug prints
 *  face up, so the face comes out crisp. */
Mesh makeRoundStamp(const FaceSource& face, const Font* font, const TrinketOptions& o, const LogFn& log,
                    const std::function<void(const char*)>& tick) {
	const double D = std::clamp(o.size > 0 ? o.size : 20.0, 12.0, 40.0);
	Area area;
	area.round = true;
	area.r = D / 2;
	Style st;
	Design d = compose(face, font, o, area, st, log);
	tick("design");

	const double loosen = std::clamp(o.fit, -0.2, 0.5);
	const double clr = std::max(0.02, 0.1 + loosen), rib = 0.22, capRib = 0.18;
	const double hr = 1.2, taper = 0.35;
	const double Rd = D / 2 + 1.0, Rg = Rd + 2.4;
	const double Lc = std::max(4.0, 0.22 * D), ringH = 1.0, Li = 4.5;
	const double zg = hr + Lc + ringH, zDieTop = zg + Li;
	const double yFlat = Rd - std::max(1.2, 0.12 * Rd);
	const double Hc = 1.1 * Rg, Hd = 2.0 * Rg, zDome = zg + Hc, zGripTop = zDome + Hd;
	const Distance2& dInk = d.dInk;

	// The plug: the face's relief, the part the cap covers, a ring that
	// shows, and the part that goes into the grip, with its flat.
	Sdf die = [&](const V3& p) {
		const double rho = std::hypot(p.x, p.y);
		double chamfer = std::max(0.0, (hr + 0.4) - p.z) + std::max(0.0, p.z - (zDieTop - 0.6));
		double body = extrude(rho - Rd + chamfer, p.z, hr, zDieTop);
		body = std::max(body, std::min(p.y - yFlat, p.z - zg));  // the flat, on the part in the grip
		double relief = extrude(std::max(dInk(p.x, p.y) - taper * std::clamp(p.z, 0.0, hr), rho - (Rd - 0.6)), p.z, 0,
		                        hr + 0.3);
		return std::min(body, relief);
	};

	// The grip: a bullet with a socket for the plug (crush ribs inside, a
	// cone over it so it prints without support), a dip for the thumb on
	// the side facing whoever stamps, and a strap hole near the top.
	const double Rp = Rd + clr, zPocketTop = zg + Li + 0.3;
	const std::vector<double> ribAngles{-150 * kPi / 180, -90 * kPi / 180, -30 * kPi / 180, 20 * kPi / 180,
	                                    160 * kPi / 180};
	const double zDip = zDome + 0.15 * Hd, dipDepth = 1.2;
	const double rDip = Rg * std::sqrt(std::max(0.0, 1 - std::pow((zDip - zDome) / Hd, 2)));
	const V3 dipCentre(0, -(rDip + 6.0 - dipDepth), zDip), dipAxes(0.42 * Rg, 6.0, 0.55 * Hd);
	const double zHole = zDome + 0.78 * Hd, rHole = 1.6;
	Sdf grip = [&](const V3& p) {
		const double rho = std::hypot(p.x, p.y);
		double body = extrude(rho - Rg + std::max(0.0, (zg + 0.4) - p.z), p.z, zg, zDome + 0.01);
		body = std::min(body, std::max(ellipsoid(p - V3(0, 0, zDome), V3(Rg, Rg, Hd)), zDome - 0.5 - p.z));
		body = smax(body, -ellipsoid(p - dipCentre, dipAxes), 1.5);
		body = std::max(body, -teardrop(p.x, p.z - zHole, rHole));
		// The socket, a little wider at the mouth to lead the plug in.
		double bore = rho - (Rp + std::max(0.0, (zg + 0.5) - p.z));
		double cone = (rho + (p.z - zPocketTop) - Rp) * std::sqrt(0.5);
		double flat = p.y - (yFlat + clr);
		double pocket = std::max(std::max(bore, cone), std::max(flat, zg - 1 - p.z));
		double ribs = std::max(roundRibs(p.x, p.y, Rp, rib, ribAngles), std::max((zg + 0.8) - p.z, p.z - zPocketTop));
		pocket = std::max(pocket, -ribs);
		return std::max(body, -pocket);
	};

	// The cap, flush with the grip, held on the plug by crush ribs.
	const double zCap0 = -1.0 - 1.6, zCap1 = hr + Lc, Rc = Rd + clr;
	Sdf cap = [&](const V3& p) {
		const double rho = std::hypot(p.x, p.y);
		double outer = extrude(rho - Rg + std::max(0.0, (zCap0 + 0.4) - p.z) + std::max(0.0, p.z - (zCap1 - 0.3)), p.z,
		                       zCap0, zCap1);
		double cavity = std::max(rho - (Rc + std::max(0.0, p.z - (zCap1 - 0.5))), -1.0 - p.z);
		double ribs = std::max(roundRibs(p.x, p.y, Rc, capRib, {0, kPi / 2, kPi, 3 * kPi / 2, kPi / 4, 5 * kPi / 4}),
		                       std::max((hr + 0.8) - p.z, p.z - (zCap1 - 0.6)));
		cavity = std::max(cavity, -ribs);
		return std::max(outer, -cavity);
	};

	const double fine = std::clamp(D / 300, 0.06, 0.1), coarse = 0.22;
	const size_t budget = o.maxTriangles / 3;
	Pose up = Pose::flipped(), still;
	if (o.assembled) up = still;
	Mesh plug = solidify(die, V3(-Rd, -Rd, 0), V3(Rd, Rd, zDieTop), up, fine, budget, log, 1.8);
	tick("carve the face");
	Mesh body = solidify(grip, V3(-Rg, -Rg, zg), V3(Rg, Rg, zGripTop), still, coarse, budget, log);
	tick("shape the grip");
	Mesh lid = solidify(cap, V3(-Rg, -Rg, zCap0), V3(Rg, Rg, zCap1), still, coarse, budget, log);
	tick("shape the cap");
	if (o.assembled) {
		Mesh all = plug;
		all.append(body);
		all.append(lid);
		return all;
	}
	std::vector<Mesh> pieces{std::move(body), std::move(plug), std::move(lid)};
	return layOut(pieces, 6.0, 180.0);
}

/** The stamp that looks like a block: a base, the face on a plate that
 *  presses into the base from below (keyed by a cut corner), a domed top
 *  with the design on it pinned on top, and a cap over the face. Every
 *  piece prints with its best side up: the face and the label face up,
 *  the base on its top. */
Mesh makeSquareStamp(const FaceSource& face, const Font* font, const TrinketOptions& o, const LogFn& log,
                     const std::function<void(const char*)>& tick) {
	const double W = std::clamp(o.size > 0 ? o.size : 25.0, 15.0, 40.0), k = W / 25.0;
	Area area;
	area.round = false;
	area.hx = W / 2;
	area.hy = 1.2 * W / 2;
	area.corner = 0.6;
	Style st;
	Design d = compose(face, font, o, area, st, log);
	tick("design");

	const double loosen = std::clamp(o.fit, -0.2, 0.5);
	PinFit pinFit;
	pinFit.clearance = std::max(0.0, pinFit.clearance + loosen);
	const double clr = std::max(0.02, 0.1 + loosen), rib = 0.22, capRib = 0.18;
	const double hr = 1.4, taper = 0.35;
	const double hx = area.hx, hy = area.hy;
	const double dx = hx + 1.2, dy = hy + 1.2, dCorner = 2.5;   // the plate
	const double Lc = 6.0 * std::max(k, 0.8), Li = 4.0, zb0 = hr + Lc, Hb = 15.0 * k, zb1 = zb0 + Hb;
	const double bx = hx + 8.75 * k, by = hy + 8.5 * k, bCorner = 5.0 * k;  // 42.5 x 47 for a 25 x 30 face
	const double tx = bx - 1.4, ty = by - 1.0, tCorner = 4.0 * k, Ht = 13.0 * k, sag = 3.0 * k;
	const double keyAt = dx + dy - 4.0;  // the plate's corner at +x +y is cut off where x + y passes this
	const Distance2& dInk = d.dInk;

	// The plate: the face's relief, the part the cap covers, and the part
	// that goes into the base, with its corner cut.
	const double zPlateTop = zb0 + Li;
	Sdf plate = [&](const V3& p) {
		double chamfer = std::max(0.0, (hr + 0.4) - p.z) + std::max(0.0, p.z - (zPlateTop - 0.5));
		double body = extrude(roundRect(p.x, p.y, dx, dy, dCorner) + chamfer, p.z, hr, zPlateTop);
		body = std::max(body, std::min((p.x + p.y - keyAt) * std::sqrt(0.5), p.z - zb0));
		double relief = extrude(std::max(dInk(p.x, p.y) - taper * std::clamp(p.z, 0.0, hr),
		                                 roundRect(p.x, p.y, dx - 0.6, dy - 0.6, dCorner)),
		                        p.z, 0, hr + 0.3);
		return std::min(body, relief);
	};

	// The base: a block narrowing a little going up, with a pocket for the
	// plate underneath (crush ribs, and the corner that keys it).
	const double px = dx + clr, py = dy + clr, zPocketTop = zb0 + Li + 0.3;
	std::vector<P2> pins{{-0.5 * hx, -0.1 * hy}, {0.5 * hx, 0.35 * hy}};  // so the top fits only one way round
	const double pinDiameter = 3.0, socketDepth = std::min(5.0, 0.4 * Ht);
	std::vector<Socket> inBase, inTop;
	for (const P2& q : pins) {
		Socket s;
		s.mouth = V3(q.x, q.y, zb1);
		s.radius = pinDiameter / 2 + pinFit.clearance;
		s.depth = socketDepth;
		s.axis = V3(0, 0, -1);
		inBase.push_back(s);
		s.axis = V3(0, 0, 1);
		inTop.push_back(s);
	}
	Sdf base = [&](const V3& p) {
		const double t = std::clamp((p.z - zb0) / Hb, 0.0, 1.0);
		double block = roundRect(p.x, p.y, bx - 0.8 * t, by - 0.8 * t, bCorner - 0.5 * t);
		block += std::max(0.0, (zb0 + 0.4) - p.z) + std::max(0.0, p.z - (zb1 - 0.6));
		double body = extrude(block, p.z, zb0, zb1);
		double pocket = roundRect(p.x, p.y, px + std::max(0.0, (zb0 + 0.5) - p.z), py + std::max(0.0, (zb0 + 0.5) - p.z),
		                          dCorner + clr);
		pocket = std::max(pocket, std::max(zb0 - 1 - p.z, p.z - zPocketTop));
		pocket = std::max(pocket, (p.x + p.y - keyAt - clr * std::sqrt(2.0)) * std::sqrt(0.5));
		double ribs = std::max(rectRibs(p.x, p.y, px, py, rib), std::max((zb0 + 0.8) - p.z, p.z - zPocketTop));
		pocket = std::max(pocket, -ribs);
		body = std::max(body, -pocket);
		for (const Socket& s : inBase) body = std::max(body, -s.distance(p));
		return body;
	};

	// The top: domed front to back, with a sunken panel and the design
	// standing in it, as the stamp's label.
	auto surface = [&](double y) { return zb1 + Ht - sag * (y / ty) * (y / ty); };
	const double zLoop = zb1 + Ht + kLoopRadius - 0.5 * kLoopTube;  // sunk into the top's crown
	Sdf top = [&](const V3& p) {
		double u = p.z - surface(p.y);
		double body = smax(roundRect(p.x, p.y, tx, ty, tCorner), u, 1.5 * k);
		body = std::max(body, zb1 - p.z);
		double panel = roundRect(p.x, p.y, hx + 1.6, hy + 1.6, 2.0);
		body = std::max(body, -std::max(panel, -0.5 - u));
		double label = std::max(std::max(dInk(p.x, p.y) - 0.2 * std::clamp(-u, 0.0, 0.5), panel + 0.5),
		                        std::max(u, -0.7 - u));
		body = std::min(body, label);
		for (const Socket& s : inTop) body = std::max(body, -s.distance(p));
		if (o.keychain) body = std::min(body, loopRing(p, zLoop, kLoopRadius, kLoopTube));
		return body;
	};

	// The cap, flush with the base, held on the plate by crush ribs.
	const double zCap0 = -1.0 - 1.6, zCap1 = zb0, cx = dx + clr, cy = dy + clr;
	Sdf cap = [&](const V3& p) {
		double outer = extrude(roundRect(p.x, p.y, bx, by, bCorner) + std::max(0.0, (zCap0 + 0.4) - p.z) +
		                           std::max(0.0, p.z - (zCap1 - 0.3)),
		                       p.z, zCap0, zCap1);
		double cavity = std::max(roundRect(p.x, p.y, cx, cy, dCorner + clr) - std::max(0.0, p.z - (zCap1 - 0.5)), -1.0 - p.z);
		double ribs = std::max(rectRibs(p.x, p.y, cx, cy, capRib), std::max((hr + 0.8) - p.z, p.z - (zCap1 - 0.6)));
		cavity = std::max(cavity, -ribs);
		return std::max(outer, -cavity);
	};

	const double fine = std::clamp(W / 300, 0.07, 0.11), coarse = 0.22;
	const size_t budget = o.maxTriangles / 4;
	Pose flip = Pose::flipped(), still;
	if (o.assembled) flip = still;
	Mesh plateMesh = solidify(plate, V3(-dx, -dy, 0), V3(dx, dy, zPlateTop), flip, fine, budget, log, 1.8);
	tick("carve the face");
	Mesh baseMesh = solidify(base, V3(-bx, -by, zb0), V3(bx, by, zb1), flip, coarse, budget, log);
	tick("shape the base");
	const double topTop = o.keychain ? zLoop + kLoopRadius + kLoopTube + 0.3 : zb1 + Ht;
	Mesh topMesh = solidify(top, V3(-tx, -ty, zb1), V3(tx, ty, topTop), still, 0.13, budget, log, 1.3);
	tick("shape the top");
	Mesh capMesh = solidify(cap, V3(-bx, -by, zCap0), V3(bx, by, zCap1), still, coarse, budget, log);
	tick("shape the cap");
	std::vector<Mesh> pieces{std::move(baseMesh), std::move(topMesh), std::move(plateMesh), std::move(capMesh)};
	char buf[160];
	for (size_t i = 0; i < pins.size(); i++) {
		double length = inBase[i].depth + inTop[i].depth - 2 * pinFit.endGap;
		pieces.push_back(makePin(pinDiameter, length, pinFit));
		std::snprintf(buf, sizeof buf, "Pin: %.2f mm thick, %.2f long", pinDiameter, length);
		log(buf);
	}
	if (o.assembled) {
		Mesh all;
		for (size_t i = 0; i < 4; i++) all.append(pieces[i]);
		return all;
	}
	return layOut(pieces, 6.0, 180.0);
}

}  // namespace

Mesh makeTrinket(const FaceSource& face, const Font* font, const TrinketOptions& o, const LogFn& log,
                 const std::function<void(const char*)>& tick) {
	switch (o.make) {
	case Make::Coin:
		return makeCoin(face, font, o, log, tick);
	case Make::Stamp:
		return makeRoundStamp(face, font, o, log, tick);
	case Make::SquareStamp:
		return makeSquareStamp(face, font, o, log, tick);
	default:
		throw std::runtime_error("not a coin or a stamp");
	}
}

}  // namespace pmo
