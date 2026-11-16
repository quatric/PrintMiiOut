// Flat artwork on a pixel grid: masks the face, the lettering and the
// borders are drawn into, and the signed distance to what they cover, which
// the coins and stamps are carved from.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pmo {

struct P2 {
	double x = 0, y = 0;
	P2() = default;
	P2(double x_, double y_) : x(x_), y(y_) {}
	P2 operator+(const P2& o) const { return {x + o.x, y + o.y}; }
	P2 operator-(const P2& o) const { return {x - o.x, y - o.y}; }
	P2 operator*(double s) const { return {x * s, y * s}; }
};

/** A closed outline; a shape is any number of them, filled by the nonzero rule. */
using Ring = std::vector<P2>;

/** A pixel grid over a rectangle, pixel (i, j) centred at lo + (i + 0.5, j + 0.5) * px,
 *  rows running up (Y up, like the artwork). */
template <typename T>
struct Grid {
	double x0 = 0, y0 = 0, px = 1;
	int w = 0, h = 0;
	std::vector<T> a;

	void reset(double lowX, double lowY, double pixel, int width, int height, T fill = T()) {
		x0 = lowX;
		y0 = lowY;
		px = pixel;
		w = width;
		h = height;
		a.assign(size_t(w) * h, fill);
	}
	/** Covering [lowX, highX] x [lowY, highY], at least one pixel each way. */
	void cover(double lowX, double lowY, double highX, double highY, double pixel, T fill = T()) {
		int width = std::max(1, int(std::ceil((highX - lowX) / pixel)));
		int height = std::max(1, int(std::ceil((highY - lowY) / pixel)));
		reset(lowX, lowY, pixel, width, height, fill);
	}
	bool empty() const { return a.empty(); }
	T& at(int i, int j) { return a[size_t(j) * w + i]; }
	const T& at(int i, int j) const { return a[size_t(j) * w + i]; }
	double cx(int i) const { return x0 + (i + 0.5) * px; }
	double cy(int j) const { return y0 + (j + 0.5) * px; }
	/** The pixel under a point, or false outside the grid. */
	bool cell(double x, double y, int& i, int& j) const {
		i = int(std::floor((x - x0) / px));
		j = int(std::floor((y - y0) / px));
		return i >= 0 && j >= 0 && i < w && j < h;
	}
};

using Mask = Grid<uint8_t>;

/** Signed distance to a mask's cover, negative inside, sampled smoothly. */
struct Distance2 {
	Grid<float> d;
	bool empty() const { return d.empty(); }
	/** Bilinear inside the grid; beyond it, the edge value plus the way out,
	 *  so it never claims to be nearer than it is. */
	double operator()(double x, double y) const;
};

/** Fill outlines (nonzero rule) into a mask, setting covered pixels to value. */
void fillRings(Mask& m, const std::vector<Ring>& rings, uint8_t value = 1);

/** Signed Euclidean distance (in the grid's units) to the pixels that are set. */
Distance2 distanceTo(const Mask& m);

/** Unsigned distance (grid units) from every pixel to the nearest pixel holding value. */
Grid<float> distanceFrom(const Mask& m, uint8_t value);

/** Grow (r > 0) or shrink (r < 0) what is set by r, in the grid's units. */
void offset(Mask& m, double r);

/** Clear islands smaller than minArea and fill holes smaller than it (grid units squared). */
void tidy(Mask& m, double minArea);

/** Set pixels in to where they are set in from (both on the same grid). */
void unite(Mask& into, const Mask& from);

}  // namespace pmo
