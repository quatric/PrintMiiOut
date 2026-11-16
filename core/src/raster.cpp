#include "raster.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pmo {

namespace {

const float kBig = 1e20f;

/** Felzenszwalb and Huttenlocher's squared distance transform of one row
 *  (f: 0 on the pixels measured from, kBig elsewhere), in place. */
void transform1d(float* f, int n, std::vector<double>& d, std::vector<int>& v, std::vector<double>& z) {
	d.resize(n);
	v.resize(n);
	z.resize(n + 1);
	int k = 0;
	v[0] = 0;
	z[0] = -1e30;
	z[1] = 1e30;
	for (int q = 1; q < n; q++) {
		double s;
		for (;;) {
			int p = v[k];
			s = ((double(f[q]) + double(q) * q) - (double(f[p]) + double(p) * p)) / (2.0 * (q - p));
			if (s > z[k] || k == 0) break;
			k--;
		}
		k++;
		v[k] = q;
		z[k] = s;
		z[k + 1] = 1e30;
	}
	k = 0;
	for (int q = 0; q < n; q++) {
		while (z[k + 1] < q) k++;
		double dq = double(q - v[k]);
		d[q] = dq * dq + f[v[k]];
	}
	for (int q = 0; q < n; q++) f[q] = float(std::min(d[q], double(kBig)));
}

/** Squared distance, in pixels, from every pixel to the nearest one marked in seed. */
std::vector<float> squaredDistance(const std::vector<char>& seed, int w, int h) {
	std::vector<float> f(size_t(w) * h);
	for (size_t i = 0; i < f.size(); i++) f[i] = seed[i] ? 0.0f : kBig;
	std::vector<float> col(h);
	std::vector<double> d, z;
	std::vector<int> v;
	for (int i = 0; i < w; i++) {
		for (int j = 0; j < h; j++) col[j] = f[size_t(j) * w + i];
		transform1d(col.data(), h, d, v, z);
		for (int j = 0; j < h; j++) f[size_t(j) * w + i] = col[j];
	}
	for (int j = 0; j < h; j++) transform1d(&f[size_t(j) * w], w, d, v, z);
	return f;
}

}  // namespace

double Distance2::operator()(double x, double y) const {
	if (d.empty()) return 1e9;
	double fx = (x - d.x0) / d.px - 0.5, fy = (y - d.y0) / d.px - 0.5;
	double cxl = std::clamp(fx, 0.0, double(d.w - 1)), cyl = std::clamp(fy, 0.0, double(d.h - 1));
	double out = std::hypot(fx - cxl, fy - cyl) * d.px;
	int i = std::min(int(cxl), d.w - 2), j = std::min(int(cyl), d.h - 2);
	if (d.w < 2 || d.h < 2) return d.a[0] + out;
	double tx = cxl - i, ty = cyl - j;
	double a = d.at(i, j) * (1 - tx) + d.at(i + 1, j) * tx;
	double b = d.at(i, j + 1) * (1 - tx) + d.at(i + 1, j + 1) * tx;
	return a * (1 - ty) + b * ty + out;
}

void fillRings(Mask& m, const std::vector<Ring>& rings, uint8_t value) {
	if (m.empty()) return;
	struct Cross {
		double x;
		int dir;
	};
	std::vector<std::vector<Cross>> rows(m.h);
	for (const Ring& r : rings) {
		size_t n = r.size();
		if (n < 3) continue;
		for (size_t e = 0; e < n; e++) {
			P2 a = r[e], b = r[(e + 1) % n];
			if (a.y == b.y) continue;
			int dir = b.y > a.y ? 1 : -1;
			double ylo = std::min(a.y, b.y), yhi = std::max(a.y, b.y);
			// Rows whose centre is in [ylo, yhi).
			int j0 = std::max(0, int(std::ceil((ylo - m.y0) / m.px - 0.5)));
			int j1 = std::min(m.h - 1, int(std::ceil((yhi - m.y0) / m.px - 0.5)) - 1);
			for (int j = j0; j <= j1; j++) {
				double y = m.cy(j);
				double x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
				rows[j].push_back({x, dir});
			}
		}
	}
	for (int j = 0; j < m.h; j++) {
		std::vector<Cross>& c = rows[j];
		if (c.empty()) continue;
		std::sort(c.begin(), c.end(), [](const Cross& a, const Cross& b) { return a.x < b.x; });
		int wind = 0;
		for (size_t k = 0; k + 1 < c.size(); k++) {
			wind += c[k].dir;
			if (wind == 0) continue;
			int i0 = std::max(0, int(std::ceil((c[k].x - m.x0) / m.px - 0.5)));
			int i1 = std::min(m.w - 1, int(std::floor((c[k + 1].x - m.x0) / m.px - 0.5)));
			for (int i = i0; i <= i1; i++) m.at(i, j) = value;
		}
	}
}

Grid<float> distanceFrom(const Mask& m, uint8_t value) {
	Grid<float> g;
	g.reset(m.x0, m.y0, m.px, m.w, m.h);
	std::vector<char> seed(m.a.size());
	bool any = false;
	for (size_t i = 0; i < seed.size(); i++) any |= (seed[i] = m.a[i] == value) != 0;
	if (!any) {
		std::fill(g.a.begin(), g.a.end(), 1e9f);
		return g;
	}
	std::vector<float> sq = squaredDistance(seed, m.w, m.h);
	for (size_t i = 0; i < sq.size(); i++) g.a[i] = std::sqrt(sq[i]) * float(m.px);
	return g;
}

Distance2 distanceTo(const Mask& m) {
	Distance2 out;
	out.d.reset(m.x0, m.y0, m.px, m.w, m.h);
	std::vector<char> in(m.a.size()), outside(m.a.size());
	bool anyIn = false, anyOut = false;
	for (size_t i = 0; i < m.a.size(); i++) {
		in[i] = m.a[i] != 0;
		outside[i] = !in[i];
		anyIn |= in[i] != 0;
		anyOut |= outside[i] != 0;
	}
	if (!anyIn) {
		std::fill(out.d.a.begin(), out.d.a.end(), 1e6f);
		return out;
	}
	std::vector<float> toIn = squaredDistance(in, m.w, m.h);
	std::vector<float> toOut = anyOut ? squaredDistance(outside, m.w, m.h) : std::vector<float>(m.a.size(), 1e12f);
	const float half = 0.5f;
	for (size_t i = 0; i < m.a.size(); i++) {
		float v = in[i] ? -(std::sqrt(toOut[i]) - half) : (std::sqrt(toIn[i]) - half);
		out.d.a[i] = v * float(m.px);
	}
	return out;
}

void offset(Mask& m, double r) {
	if (r == 0 || m.empty()) return;
	if (r > 0) {
		Grid<float> d = distanceFrom(m, 1);
		for (size_t i = 0; i < m.a.size(); i++)
			if (d.a[i] <= r) m.a[i] = 1;
	} else {
		Grid<float> d = distanceFrom(m, 0);
		for (size_t i = 0; i < m.a.size(); i++)
			if (d.a[i] <= -r) m.a[i] = 0;
	}
}

void tidy(Mask& m, double minArea) {
	if (m.empty()) return;
	const double cellArea = m.px * m.px;
	std::vector<int> label(m.a.size(), -1);
	std::vector<int> stack;
	for (int pass = 0; pass < 2; pass++) {
		const uint8_t want = pass == 0 ? 1 : 0;
		std::fill(label.begin(), label.end(), -1);
		for (int j0 = 0; j0 < m.h; j0++)
			for (int i0 = 0; i0 < m.w; i0++) {
				size_t s = size_t(j0) * m.w + i0;
				if (label[s] >= 0 || (m.a[s] != 0) != (want != 0)) continue;
				std::vector<size_t> members;
				bool edge = false;
				stack.assign(1, int(s));
				label[s] = 1;
				while (!stack.empty()) {
					size_t c = size_t(stack.back());
					stack.pop_back();
					members.push_back(c);
					int i = int(c % m.w), j = int(c / m.w);
					if (i == 0 || j == 0 || i == m.w - 1 || j == m.h - 1) edge = true;
					const int nb[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
					for (auto& q : nb) {
						if (q[0] < 0 || q[1] < 0 || q[0] >= m.w || q[1] >= m.h) continue;
						size_t n = size_t(q[1]) * m.w + q[0];
						if (label[n] >= 0 || (m.a[n] != 0) != (want != 0)) continue;
						label[n] = 1;
						stack.push_back(int(n));
					}
				}
				// Specks of ink go; so do pinholes in it, but never the paper
				// around the edge of the grid.
				if (members.size() * cellArea < minArea && (pass == 0 || !edge))
					for (size_t c : members) m.a[c] = pass == 0 ? 0 : 1;
			}
	}
}

void unite(Mask& into, const Mask& from) {
	for (size_t i = 0; i < into.a.size() && i < from.a.size(); i++)
		if (from.a[i]) into.a[i] = 1;
}

}  // namespace pmo
