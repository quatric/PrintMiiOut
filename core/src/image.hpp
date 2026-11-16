// Decoded textures, looked up bottom row first with wrapping UVs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pmo {

struct RGBA {
	float r = 0, g = 0, b = 0, a = 0;
};

class Image {
public:
	bool load(const uint8_t* data, size_t size);
	/** Raw RGBA8 pixels, rows top to bottom. */
	void setPixels(int width, int height, std::vector<uint8_t> rgba);
	bool empty() const { return w_ == 0; }
	int width() const { return w_; }
	int height() const { return h_; }
	int channels() const { return channels_; }
	/** RGBA8, rows top to bottom. */
	const std::vector<uint8_t>& pixels() const { return px_; }

	/** Texel at column x and row y counted from the bottom; out of range
	 *  wraps. */
	RGBA texel(long long x, long long yFromBottom) const;
	/** Nearest texel under a UV (V up), wrapping like Python's %:
	 *  int(u * width) % width, never negative. */
	RGBA nearest(double u, double v) const;
	/** Bilinear sample under a UV (V up), for displacement. */
	RGBA bilinear(double u, double v) const;

private:
	int w_ = 0, h_ = 0, channels_ = 0;
	std::vector<uint8_t> px_;  // RGBA8, rows top to bottom
};

}  // namespace pmo
