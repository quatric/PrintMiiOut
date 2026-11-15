#include "image.hpp"

#include <cmath>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC  // assimp may carry its own copy
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace pmo {

bool Image::load(const uint8_t* data, size_t size) {
	int w = 0, h = 0, n = 0;
	uint8_t* p = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
	if (!p) return false;
	w_ = w;
	h_ = h;
	channels_ = n;
	px_.assign(p, p + size_t(w) * h * 4);
	stbi_image_free(p);
	return true;
}

void Image::setPixels(int width, int height, std::vector<uint8_t> rgba) {
	w_ = width;
	h_ = height;
	channels_ = 4;
	px_ = std::move(rgba);
}

static long long wrap(long long x, long long n) {
	long long r = x % n;
	return r < 0 ? r + n : r;
}

RGBA Image::texel(long long x, long long y) const {
	if (!w_) return {};
	x = wrap(x, w_);
	y = wrap(y, h_);
	const uint8_t* p = &px_[(size_t(h_ - 1 - y) * w_ + x) * 4];
	return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
}

RGBA Image::nearest(double u, double v) const {
	// numpy's astype(int64) truncates toward zero before the modulo.
	return texel((long long)(u * w_), (long long)(v * h_));
}

RGBA Image::bilinear(double u, double v) const {
	if (!w_) return {};
	double x = u * w_ - 0.5, y = v * h_ - 0.5;
	long long x0 = (long long)std::floor(x), y0 = (long long)std::floor(y);
	float fx = float(x - x0), fy = float(y - y0);
	RGBA a = texel(x0, y0), b = texel(x0 + 1, y0), c = texel(x0, y0 + 1), d = texel(x0 + 1, y0 + 1);
	auto mix = [&](float p, float q, float r, float s) {
		return (p * (1 - fx) + q * fx) * (1 - fy) + (r * (1 - fx) + s * fx) * fy;
	};
	return {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b), mix(a.a, b.a, c.a, d.a)};
}

}  // namespace pmo
