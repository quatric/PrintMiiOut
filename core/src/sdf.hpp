// Solids described by their signed distance (negative inside), and the
// mesher that turns one into a closed surface. Coins and stamps are built
// this way: unions, cuts and extrusions of flat artwork are a min or a max.
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "geom.hpp"

namespace pmo {

using Sdf = std::function<double(const V3&)>;

/** Marching tetrahedra over the box at the given grid spacing, each vertex
 *  then moved onto the surface; only the blocks the surface can reach are
 *  sampled, so f should never claim to be farther than it is (a bound, as
 *  min and max of distances are). Closed and manifold by construction.
 *  zStretch spaces the grid that much wider in z than across, for solids
 *  whose walls stand upright. */
Mesh meshSdf(const Sdf& f, V3 lo, V3 hi, double voxel, const std::function<void(const std::string&)>& log,
             double zStretch = 1.0);

/** A flat shape (signed distance d2 across) carried from z0 up to z1. */
inline double extrude(double d2, double z, double z0, double z1) {
	double wz = std::max(z0 - z, z - z1);
	if (d2 > 0 && wz > 0) return std::hypot(d2, wz);
	return std::max(d2, wz);
}

/** A rectangle of half sizes hx, hy with corners rounded to r. */
inline double roundRect(double x, double y, double hx, double hy, double r) {
	r = std::max(0.0, std::min(r, std::min(hx, hy)));
	double qx = std::fabs(x) - hx + r, qy = std::fabs(y) - hy + r;
	return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
}

/** Smooth union and intersection, blending over k. */
inline double smin(double a, double b, double k) {
	double h = std::clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
	return b * (1 - h) + a * h - k * h * (1 - h);
}
inline double smax(double a, double b, double k) { return -smin(-a, -b, k); }

/** An ellipsoid with semi-axes r about the origin (a close bound). */
inline double ellipsoid(const V3& p, const V3& r) {
	double k0 = std::sqrt((p.x / r.x) * (p.x / r.x) + (p.y / r.y) * (p.y / r.y) + (p.z / r.z) * (p.z / r.z));
	double k1 = std::sqrt((p.x / (r.x * r.x)) * (p.x / (r.x * r.x)) + (p.y / (r.y * r.y)) * (p.y / (r.y * r.y)) +
	                      (p.z / (r.z * r.z)) * (p.z / (r.z * r.z)));
	if (k1 < 1e-12) return -std::min(r.x, std::min(r.y, r.z));
	return k0 * (k0 - 1) / k1;
}

/** A rigid move: p' = R p + t, R a rotation given by its rows. */
struct Pose {
	double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	V3 t;
	V3 apply(const V3& p) const {
		return {r[0][0] * p.x + r[0][1] * p.y + r[0][2] * p.z + t.x, r[1][0] * p.x + r[1][1] * p.y + r[1][2] * p.z + t.y,
		        r[2][0] * p.x + r[2][1] * p.y + r[2][2] * p.z + t.z};
	}
	V3 invert(const V3& q) const {
		V3 d = q - t;
		return {r[0][0] * d.x + r[1][0] * d.y + r[2][0] * d.z, r[0][1] * d.x + r[1][1] * d.y + r[2][1] * d.z,
		        r[0][2] * d.x + r[1][2] * d.y + r[2][2] * d.z};
	}
	/** Turned half a turn about X: upside down, front to back. */
	static Pose flipped() {
		Pose p;
		p.r[1][1] = -1;
		p.r[2][2] = -1;
		return p;
	}
};

}  // namespace pmo
