// Small geometry types shared by the PrintMiiOut core.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pmo {

struct V3 {
	double x = 0, y = 0, z = 0;
	V3() = default;
	V3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
	double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
	double& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
	V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
	V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
	V3 operator*(double s) const { return {x * s, y * s, z * s}; }
	V3 operator/(double s) const { return {x / s, y / s, z / s}; }
	V3 operator-() const { return {-x, -y, -z}; }
	V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
	V3& operator-=(const V3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
	V3& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
};

inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalized(const V3& a) {
	double l = length(a);
	return l > 1e-300 ? a / l : V3();
}
inline V3 vmin(const V3& a, const V3& b) { return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)}; }
inline V3 vmax(const V3& a, const V3& b) { return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)}; }

struct V2 {
	float u = 0, v = 0;
};

using Tri = std::array<int, 3>;

/** An indexed triangle mesh, optionally with a UV per face corner. */
struct Mesh {
	std::vector<V3> v;
	std::vector<Tri> f;
	std::vector<std::array<V2, 3>> uv;  // empty, or one per face

	bool hasUV() const { return !uv.empty() && uv.size() == f.size(); }
	void append(const Mesh& o) {
		int at = (int)v.size();
		bool keepUV = (f.empty() || hasUV()) && o.hasUV();
		if (!keepUV) uv.clear();
		v.insert(v.end(), o.v.begin(), o.v.end());
		for (size_t i = 0; i < o.f.size(); i++) {
			f.push_back({o.f[i][0] + at, o.f[i][1] + at, o.f[i][2] + at});
			if (keepUV) uv.push_back(o.uv[i]);
		}
	}
	void bounds(V3& lo, V3& hi) const {
		lo = V3(1e300, 1e300, 1e300);
		hi = V3(-1e300, -1e300, -1e300);
		for (const V3& p : v) {
			lo = vmin(lo, p);
			hi = vmax(hi, p);
		}
	}
};

/** 4x4 matrix, row major, acting on column vectors (p' = M p), like glTF and assimp. */
struct Mat4 {
	double m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

	static Mat4 identity() { return Mat4(); }
	Mat4 operator*(const Mat4& o) const {
		Mat4 r;
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++) {
				double s = 0;
				for (int k = 0; k < 4; k++) s += m[i][k] * o.m[k][j];
				r.m[i][j] = s;
			}
		return r;
	}
	V3 point(const V3& p) const {
		return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
		        m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
		        m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
	}
	V3 translation() const { return {m[0][3], m[1][3], m[2][3]}; }
	void setTranslation(const V3& t) { m[0][3] = t.x; m[1][3] = t.y; m[2][3] = t.z; }
	/** Scales the matrix's own axes (post-multiplies by a diagonal scale). */
	Mat4 scaledAxes(const V3& s) const {
		Mat4 r = *this;
		for (int i = 0; i < 4; i++) {
			r.m[i][0] *= s.x;
			r.m[i][1] *= s.y;
			r.m[i][2] *= s.z;
		}
		return r;
	}
};

}  // namespace pmo
