#include "head.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>

namespace pmo {

namespace {

// Mask texture noise leaves islands under ~0.35 GLB units^2; real features are 1+.
const double kSpeckArea = 0.5;
// The mask is cut per triangle on a grid this fine, as six levels of
// subdivision would cut it.
const int kEmbossSegments = 64;

// Glasses sizes are fractions of head width.
const double kGlassAlpha = 0.5;
const double kGlassCell = 0.5;
const double kGlassRest = 5;
const double kGlassProud = 0.02;
const double kGlassMaxSlide = 0.15;
const double kGlassMinDepth = 0.02;
const double kGlassPlate = 0.035;
const double kGlassArm = 0.04;
const double kGlassArmReach = 0.15;
const double kGlassSink = 0.01;

// How the face texture is embossed. Ink (the
// brightest channel this dark or darker: black and grey lines, dark
// outlines) stands at full height. Colour (hearts, coins, tongues, a
// coloured iris or eyebrow) stands at half height, under the ink.
const double kInkValue = 0.5;
const double kColourSaturation = 0.2;
// Share of a feature's area that must be colour before it gets a plateau,
// so a few tinted edge pixels never lift a black and white feature.
const double kToneShare = 0.1;
const double kToneThickness = 1.25, kToneOffset = 0.2;  // 0.75 proud, 0.5 sunk

enum class Ink : uint8_t { None, Dark, Colour, Light };

/** What three texture samples (a grid cell's corners) paint. */
Ink inkOf(const RGBA& p, const RGBA& q, const RGBA& r) {
	if (std::max(p.a, std::max(q.a, r.a)) < 0.1f) return Ink::None;
	double R = (p.r + q.r + r.r) / 3.0, G = (p.g + q.g + r.g) / 3.0, B = (p.b + q.b + r.b) / 3.0;
	double value = std::max(R, std::max(G, B)), low = std::min(R, std::min(G, B));
	if (value <= kInkValue) return Ink::Dark;
	if (value - low >= kColourSaturation) return Ink::Colour;
	return Ink::Light;  // eye whites, teeth, light grey edges
}

double percentile(std::vector<double> x, double p) {
	std::sort(x.begin(), x.end());
	double pos = p / 100.0 * double(x.size() - 1);
	size_t lo = size_t(std::floor(pos)), hi = std::min(lo + 1, x.size() - 1);
	return x[lo] + (x[hi] - x[lo]) * (pos - double(lo));
}

/** Solve A x = b for a 3x3 system, by Gaussian elimination. */
bool solve3(double A[3][3], double b[3], double x[3]) {
	double M[3][4];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) M[i][j] = A[i][j];
		M[i][3] = b[i];
	}
	for (int c = 0; c < 3; c++) {
		int piv = c;
		for (int r = c + 1; r < 3; r++)
			if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
		if (std::fabs(M[piv][c]) < 1e-15) return false;
		for (int k = 0; k < 4; k++) std::swap(M[c][k], M[piv][k]);
		for (int r = 0; r < 3; r++) {
			if (r == c) continue;
			double f = M[r][c] / M[c][c];
			for (int k = c; k < 4; k++) M[r][k] -= f * M[c][k];
		}
	}
	for (int i = 0; i < 3; i++) x[i] = M[i][3] / M[i][i];
	return true;
}

}  // namespace

Mesh embossMask(const Mesh& mask, const Image& tex, const Mesh* faceline, const BVH* fbvh) {
	Mesh out;
	if (tex.empty() || tex.channels() < 4 || !mask.hasUV()) return out;
	const int n = kEmbossSegments;
	const int stride = n + 1;
	std::vector<int> id(stride * stride);
	std::vector<RGBA> texel(stride * stride);
	std::vector<Ink> kind;  // per face of out
	for (size_t fi = 0; fi < mask.f.size(); fi++) {
		const V3 &A = mask.v[mask.f[fi][0]], &B = mask.v[mask.f[fi][1]], &C = mask.v[mask.f[fi][2]];
		const V2 &a = mask.uv[fi][0], &b = mask.uv[fi][1], &c = mask.uv[fi][2];
		std::fill(id.begin(), id.end(), -1);
		for (int i = 0; i <= n; i++)
			for (int j = 0; i + j <= n; j++) {
				double s = double(i) / n, t = double(j) / n;
				texel[i * stride + j] =
				    tex.nearest(a.u + (b.u - a.u) * s + (c.u - a.u) * t, a.v + (b.v - a.v) * s + (c.v - a.v) * t);
			}
		auto vert = [&](int i, int j) {
			int& slot = id[i * stride + j];
			if (slot < 0) {
				slot = (int)out.v.size();
				out.v.push_back(A + (B - A) * (double(i) / n) + (C - A) * (double(j) / n));
			}
			return slot;
		};
		auto add = [&](int i0, int j0, int i1, int j1, int i2, int j2) {
			Ink k = inkOf(texel[i0 * stride + j0], texel[i1 * stride + j1], texel[i2 * stride + j2]);
			if (k == Ink::None) return;
			out.f.push_back({vert(i0, j0), vert(i1, j1), vert(i2, j2)});
			kind.push_back(k);
		};
		for (int i = 0; i < n; i++)
			for (int j = 0; i + j < n; j++) {
				add(i, j, i + 1, j, i, j + 1);
				if (i + j < n - 1) add(i + 1, j, i + 1, j + 1, i, j + 1);
			}
	}
	if (out.f.empty()) return out;
	V3 lo, hi;
	out.bounds(lo, hi);
	weld(out, 1e-6 * std::max(1.0, length(hi - lo)));

	// Onto the face, a hair off it, like the Shrinkwrap modifier.
	if (faceline && fbvh && !faceline->f.empty()) {
		for (V3& p : out.v) {
			int face = -1;
			V3 q = fbvh->closest(p, &face);
			if (face < 0) continue;
			const Tri& t = faceline->f[face];
			V3 fn = normalized(cross(faceline->v[t[1]] - faceline->v[t[0]], faceline->v[t[2]] - faceline->v[t[0]]));
			p = q + fn * 0.01;
		}
	}

	// Which features get a plateau: those that are partly colour, and
	// those painted entirely in white or grey (light eyebrows), which would
	// otherwise vanish. Black and white features (eyes with their whites)
	// get none, so they print exactly as they always have.
	int count = 0;
	std::vector<int> island = faceIslands(out, count);
	std::vector<double> all(count, 0.0), colour(count, 0.0), ink(count, 0.0);
	for (size_t f = 0; f < out.f.size(); f++) {
		double area = triangleArea(out, (int)f);
		all[island[f]] += area;
		if (kind[f] == Ink::Colour) colour[island[f]] += area;
		if (kind[f] == Ink::Dark) ink[island[f]] += area;
	}
	auto plateau = [&](int f) {
		int i = island[f];
		if (kind[f] == Ink::Light) return ink[i] == 0 && colour[i] == 0;
		return colour[i] >= kToneShare * all[i];
	};

	auto shell = [&](const std::function<bool(int)>& keep, double thickness, double offset) {
		Mesh m = out;
		m.uv.clear();
		keepFaces(m, keep);
		int c = 0;
		std::vector<int> isl = faceIslands(m, c);
		std::vector<double> area(c, 0.0);
		for (size_t f = 0; f < m.f.size(); f++) area[isl[f]] += triangleArea(m, (int)f);
		keepFaces(m, [&](int f) { return area[isl[f]] >= kSpeckArea; });
		if (m.f.empty()) return m;
		smooth(m, 0.6, 25);
		solidify(m, thickness, offset);
		return m;
	};
	// Dark ink stands 1.5 proud and sinks 0.5 so it fuses with the face;
	// a plateau stands half as proud, under the ink that outlines it.
	Mesh relief = shell([&](int f) { return kind[f] == Ink::Dark; }, 2.0, 0.5);
	relief.append(shell(plateau, kToneThickness, kToneOffset));
	return relief;
}

void embossCap(Mesh& cap, const Image& tex) {
	subdivide(cap, 1);
	if (!tex.empty() && cap.hasUV()) {
		std::vector<V2> uv(cap.v.size());
		std::vector<char> seen(cap.v.size(), 0);
		for (size_t f = 0; f < cap.f.size(); f++)
			for (int k = 0; k < 3; k++)
				if (!seen[cap.f[f][k]]) {
					seen[cap.f[f][k]] = 1;
					uv[cap.f[f][k]] = cap.uv[f][k];
				}
		displace(cap, [&](int i) {
			RGBA c = tex.bilinear(uv[i].u, uv[i].v);
			return ((c.r + c.g + c.b) / 3.0 - 0.8745) * -0.8;
		});
	}
	smooth(cap, 0.5, 5);
}

void thickenHair(Mesh& hair) {
	// A few mm of extra volume, expanding outward only.
	solidify(hair, 1.5, 1.0);
	smooth(hair, 0.5, 5);
}

Mesh mergeHead(std::vector<Mesh>& opaque, bool closeNeck) {
	Mesh head;
	if (opaque.empty()) return head;
	if (opaque.size() == 1 && !closeNeck) return opaque[0];
	for (Mesh& m : opaque) {
		m.uv.clear();
		head.append(m);
	}
	// Weld vertices touching across part boundaries, then fill true holes.
	if (opaque.size() > 1) weld(head, 0.05);
	fillHoles(head);
	return head;
}

Mesh buildGlasses(const Mesh& quad, const Image& img, const Mesh& head, const BVH& bvh, double unit,
                  double voxelFraction) {
	Mesh out;
	if (img.empty() || img.channels() < 4 || !quad.hasUV() || quad.f.empty() || head.v.empty()) return out;

	// Texture space to model space: position = origin + u * du + v * dv.
	std::vector<V3> P;
	std::vector<V2> UV;
	for (size_t f = 0; f < quad.f.size(); f++)
		for (int k = 0; k < 3; k++) {
			P.push_back(quad.v[quad.f[f][k]]);
			UV.push_back(quad.uv[f][k]);
		}
	double A[3][3] = {}, R[3][3] = {};
	for (size_t i = 0; i < P.size(); i++) {
		double r[3] = {UV[i].u, UV[i].v, 1.0};
		for (int a = 0; a < 3; a++) {
			for (int b = 0; b < 3; b++) A[a][b] += r[a] * r[b];
			for (int c = 0; c < 3; c++) R[a][c] += r[a] * P[i][c];
		}
	}
	double X[3][3];
	for (int c = 0; c < 3; c++) {
		double rhs[3] = {R[0][c], R[1][c], R[2][c]}, x[3];
		double M[3][3];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) M[i][j] = A[i][j];
		if (!solve3(M, rhs, x)) return out;
		for (int r = 0; r < 3; r++) X[r][c] = x[r];
	}
	V3 du(X[0][0], X[0][1], X[0][2]), dv(X[1][0], X[1][1], X[1][2]), origin(X[2][0], X[2][1], X[2][2]);
	double lenU = length(du), lenV = length(dv);
	V3 normal = cross(du, dv);
	if (lenU < 1e-6 || lenV < 1e-6 || length(normal) < 1e-9) return out;
	normal = normalized(normal);
	V3 cmean, hmean;
	for (const V3& p : P) cmean += p / double(P.size());
	for (const V3& p : head.v) hmean += p / double(head.v.size());
	if (dot(normal, cmean - hmean) < 0) normal = -normal;  // always points away from the head

	double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
	for (const V2& t : UV) {
		u0 = std::min(u0, double(t.u));
		v0 = std::min(v0, double(t.v));
		u1 = std::max(u1, double(t.u));
		v1 = std::max(v1, double(t.v));
	}
	const double cell = unit * voxelFraction * kGlassCell;
	const int nu = int(std::min(2048.0, std::max(1.0, std::ceil(lenU * (u1 - u0) / cell))));
	const int nv = int(std::min(2048.0, std::max(1.0, std::ceil(lenV * (v1 - v0) / cell))));

	// The texture is one lens, mirrored across the nose for the other.
	auto texel = [](double t, int size) {
		t = std::fmod(t, 2.0);
		if (t < 0) t += 2.0;
		if (t > 1.0) t = 2.0 - t;
		return std::min((long long)(t * size), (long long)size - 1);
	};
	auto K = [nv](int i, int j) { return size_t(i) * nv + j; };
	auto Pt = [nv](int i, int j) { return size_t(i) * (nv + 1) + j; };
	std::vector<char> keep(size_t(nu) * nv);
	bool any = false;
	for (int i = 0; i < nu; i++) {
		long long x = texel(u0 + (i + 0.5) / nu * (u1 - u0), img.width());
		for (int j = 0; j < nv; j++) {
			long long y = texel(v0 + (j + 0.5) / nv * (v1 - v0), img.height());
			keep[K(i, j)] = img.texel(x, y).a >= kGlassAlpha;
			any |= keep[K(i, j)] != 0;
		}
	}
	if (!any) return out;

	const size_t NP = size_t(nu + 1) * (nv + 1);
	std::vector<V3> front(NP);
	for (int i = 0; i <= nu; i++)
		for (int j = 0; j <= nv; j++)
			front[Pt(i, j)] = origin + du * (u0 + double(i) / nu * (u1 - u0)) + dv * (v0 + double(j) / nv * (v1 - v0));
	auto corners = [&](const std::vector<char>& cells) {
		std::vector<char> pts(NP, 0);
		for (int i = 0; i < nu; i++)
			for (int j = 0; j < nv; j++)
				if (cells[K(i, j)]) pts[Pt(i, j)] = pts[Pt(i + 1, j)] = pts[Pt(i, j + 1)] = pts[Pt(i + 1, j + 1)] = 1;
		return pts;
	};
	std::vector<char> used = corners(keep);

	// How far behind the game's glasses plane the head starts, looking straight back.
	const double reach = unit * 4;
	const double inf = std::numeric_limits<double>::infinity();
	std::vector<double> gap(NP, std::numeric_limits<double>::quiet_NaN());
	std::vector<char> found(NP, 0);
	bool anyFound = false;
	for (size_t p = 0; p < NP; p++) {
		if (!used[p]) continue;
		double t;
		if (bvh.raycast(front[p] + normal * reach, -normal, reach * 2, t)) {
			gap[p] = t - reach;
			found[p] = 1;
			anyFound = true;
		}
	}
	if (!anyFound) return out;

	// Slide the frame straight back until it rests on the face.
	std::vector<double> clear;
	for (size_t p = 0; p < NP; p++)
		if (found[p] && gap[p] > 0) clear.push_back(gap[p]);
	double slide = clear.empty() ? 0.0 : percentile(clear, kGlassRest) - unit * kGlassProud;
	slide = std::clamp(slide, 0.0, unit * kGlassMaxSlide);
	for (V3& p : front) p -= normal * slide;
	for (size_t p = 0; p < NP; p++) gap[p] = found[p] ? gap[p] - slide : inf;

	// A thin plate, fused to the face only where the two touch.
	const double sink = unit * kGlassSink;
	std::vector<double> depth(NP);
	for (size_t p = 0; p < NP; p++) depth[p] = std::clamp(gap[p] + sink, unit * kGlassMinDepth, unit * kGlassPlate);

	// Temple arms run straight back to the head from each side.
	std::vector<char> reachable(size_t(nu) * nv, 0);
	for (int i = 0; i < nu; i++)
		for (int j = 0; j < nv; j++) {
			double g = std::max(std::max(gap[Pt(i, j)], gap[Pt(i + 1, j)]), std::max(gap[Pt(i, j + 1)], gap[Pt(i + 1, j + 1)]));
			reachable[K(i, j)] = keep[K(i, j)] && g <= unit * kGlassArmReach;
		}
	const int armCells = std::max(1, int(std::lround(unit * kGlassArm / cell)));
	const int mid = nu / 2;
	for (int side = 0; side < 2; side++) {
		bool left = side == 0;
		int from = left ? 0 : mid, to = left ? mid : nu;
		int first = -1, last = -1;
		for (int i = from; i < to; i++) {
			bool col = false;
			for (int j = 0; j < nv && !col; j++) col = reachable[K(i, j)] != 0;
			if (col) {
				if (first < 0) first = i;
				last = i;
			}
		}
		if (first < 0) continue;
		int lo = left ? first : std::max(mid, last - armCells + 1);
		int hi = std::min(nu, lo + armCells);
		std::vector<int> rows;
		std::vector<double> weight;
		for (int j = 0; j < nv; j++) {
			int w = 0;
			for (int i = lo; i < hi; i++) w += reachable[K(i, j)];
			if (w) {
				rows.push_back(j);
				weight.push_back(w);
			}
		}
		if (rows.empty()) continue;
		// Aim for the middle of the frame's height, but land on the frame.
		double sw = 0, swr = 0;
		for (size_t r = 0; r < rows.size(); r++) {
			sw += weight[r];
			swr += weight[r] * rows[r];
		}
		double avg = swr / sw;
		int centre = rows[0];
		for (int r : rows)
			if (std::fabs(r - avg) < std::fabs(centre - avg)) centre = r;
		int r0 = std::max(0, centre - armCells / 2), r1 = std::min(nv, r0 + armCells);
		std::vector<char> arm(size_t(nu) * nv, 0);
		bool anyArm = false;
		for (int i = lo; i < hi; i++)
			for (int j = r0; j < r1; j++)
				if (reachable[K(i, j)]) arm[K(i, j)] = anyArm = true;
		if (!anyArm) continue;
		std::vector<char> armPts = corners(arm);
		for (size_t p = 0; p < NP; p++)
			if (armPts[p]) depth[p] = std::max(depth[p], gap[p] + sink);
	}

	out.v.resize(NP * 2);
	for (size_t p = 0; p < NP; p++) {
		out.v[p] = front[p];
		out.v[NP + p] = front[p] - normal * depth[p];
	}
	const int count = int(NP);
	auto quadFace = [&](int a, int b, int c, int d) {
		out.f.push_back({a, b, c});
		out.f.push_back({a, c, d});
	};
	for (int i = 0; i < nu; i++)
		for (int j = 0; j < nv; j++) {
			if (!keep[K(i, j)]) continue;
			int a = int(Pt(i, j)), b = int(Pt(i + 1, j)), c = int(Pt(i + 1, j + 1)), d = int(Pt(i, j + 1));
			quadFace(a, b, c, d);
			quadFace(d + count, c + count, b + count, a + count);
		}
	// Walls wherever a frame cell borders an empty one.
	for (int i = 0; i <= nu; i++)
		for (int j = 0; j < nv; j++) {
			bool low = i > 0 && keep[K(i - 1, j)], high = i < nu && keep[K(i, j)];
			if (low == high) continue;
			int a = int(Pt(i, j)), b = int(Pt(i, j + 1));
			if (high)
				quadFace(a + count, b + count, b, a);
			else
				quadFace(a, b, b + count, a + count);
		}
	for (int i = 0; i < nu; i++)
		for (int j = 0; j <= nv; j++) {
			bool low = j > 0 && keep[K(i, j - 1)], high = j < nv && keep[K(i, j)];
			if (low == high) continue;
			int a = int(Pt(i, j)), b = int(Pt(i + 1, j));
			if (high)
				quadFace(b + count, a + count, a, b);
			else
				quadFace(b, a, a + count, b + count);
		}
	compact(out);
	return out;
}

}  // namespace pmo
