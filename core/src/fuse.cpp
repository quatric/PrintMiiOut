#include "fuse.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "mesh_ops.hpp"

namespace pmo {

namespace {

/** Open addressing map from grid edge to output vertex. */
class EdgeMap {
public:
	explicit EdgeMap(size_t expected) {
		size_t cap = 1024;
		while (cap < expected * 2) cap <<= 1;
		keys_.assign(cap, kEmpty);
		vals_.assign(cap, -1);
	}
	/** The vertex for key, or -1 with slot set to where it goes. */
	int find(uint64_t key, size_t& slot) const {
		size_t mask = keys_.size() - 1;
		size_t i = hash(key) & mask;
		while (keys_[i] != kEmpty) {
			if (keys_[i] == key) return vals_[i];
			i = (i + 1) & mask;
		}
		slot = i;
		return -1;
	}
	void insert(uint64_t key, int val, size_t slot) {
		keys_[slot] = key;
		vals_[slot] = val;
		if (++count_ * 2 > keys_.size()) grow();
	}

private:
	static constexpr uint64_t kEmpty = ~uint64_t(0);
	std::vector<uint64_t> keys_;
	std::vector<int> vals_;
	size_t count_ = 0;
	static size_t hash(uint64_t k) {
		k ^= k >> 33;
		k *= 0xff51afd7ed558ccdULL;
		k ^= k >> 33;
		return size_t(k);
	}
	void grow() {
		std::vector<uint64_t> k;
		std::vector<int> v;
		k.swap(keys_);
		v.swap(vals_);
		keys_.assign(k.size() * 2, kEmpty);
		vals_.assign(k.size() * 2, -1);
		size_t mask = keys_.size() - 1;
		for (size_t i = 0; i < k.size(); i++) {
			if (k[i] == kEmpty) continue;
			size_t j = hash(k[i]) & mask;
			while (keys_[j] != kEmpty) j = (j + 1) & mask;
			keys_[j] = k[i];
			vals_[j] = v[i];
		}
	}
};

// The six tetrahedra of a cube around its main diagonal (corner bits:
// x = 1, y = 2, z = 4). Neighbouring cubes split their shared face along
// the same diagonal, so the surface stays closed across cubes.
// How much wider than the wall a passage must be to stay open, in voxels.
const double kTunnel = 1.0;

const int kTets[6][4] = {{0, 1, 3, 7}, {0, 1, 5, 7}, {0, 2, 3, 7}, {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 4, 6, 7}};

}  // namespace

Mesh fuse(const std::vector<const Mesh*>& parts, double voxel, const LogFn& log) {
	V3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300);
	size_t triCount = 0;
	for (const Mesh* m : parts) {
		for (const V3& p : m->v) {
			lo = vmin(lo, p);
			hi = vmax(hi, p);
		}
		triCount += m->f.size();
	}
	Mesh out;
	if (!triCount) return out;

	// Closer than wall blocks the flood fill (anything over half a voxel
	// keeps the outside from leaking through). Distances are tracked out
	// to the wall plus a cube's diagonal, so every corner of a cube the
	// contour crosses has its true distance, not a clamped one.
	const double wall = 0.6 * voxel;
	const double band = wall + 1.8 * voxel;
	lo -= V3(3, 3, 3) * voxel;
	hi += V3(3, 3, 3) * voxel;
	const int nx = int(std::ceil((hi.x - lo.x) / voxel)) + 1;
	const int ny = int(std::ceil((hi.y - lo.y) / voxel)) + 1;
	const int nz = int(std::ceil((hi.z - lo.z) / voxel)) + 1;
	const size_t sx = 1, sy = size_t(nx), sz = size_t(nx) * ny, N = sz * nz;
	auto at = [&](int i, int j, int k) { return size_t(k) * sz + size_t(j) * sy + size_t(i); };
	auto point = [&](int i, int j, int k) { return V3(lo.x + i * voxel, lo.y + j * voxel, lo.z + k * voxel); };

	// Unsigned distance to the nearest triangle, quantized over the band
	// finely enough that the steps never show in the surface.
	const uint16_t kFar = 65535;
	std::vector<uint16_t> dist(N, kFar);
	const double toQ = 65534.0 / band;
	const double band2 = band * band;
	for (const Mesh* m : parts) {
		for (const Tri& t : m->f) {
			const V3 &a = m->v[t[0]], &b = m->v[t[1]], &c = m->v[t[2]];
			V3 tlo = vmin(a, vmin(b, c)), thi = vmax(a, vmax(b, c));
			int i0 = std::max(0, int(std::ceil((tlo.x - band - lo.x) / voxel)));
			int j0 = std::max(0, int(std::ceil((tlo.y - band - lo.y) / voxel)));
			int k0 = std::max(0, int(std::ceil((tlo.z - band - lo.z) / voxel)));
			int i1 = std::min(nx - 1, int(std::floor((thi.x + band - lo.x) / voxel)));
			int j1 = std::min(ny - 1, int(std::floor((thi.y + band - lo.y) / voxel)));
			int k1 = std::min(nz - 1, int(std::floor((thi.z + band - lo.z) / voxel)));
			V3 n = cross(b - a, c - a);
			double nl = length(n);
			bool flat = nl > 1e-20;
			if (flat) n = n / nl;
			for (int k = k0; k <= k1; k++)
				for (int j = j0; j <= j1; j++) {
					size_t row = at(0, j, k);
					for (int i = i0; i <= i1; i++) {
						V3 p = point(i, j, k);
						if (flat && std::fabs(dot(p - a, n)) >= band) continue;
						V3 d = closestOnTriangle(p, a, b, c) - p;
						double d2 = dot(d, d);
						if (d2 >= band2) continue;
						uint16_t q = uint16_t(std::sqrt(d2) * toQ);
						if (q < dist[row + i]) dist[row + i] = q;
					}
				}
		}
	}
	const uint16_t wallQ = uint16_t(wall * toQ);
	const uint16_t wideQ = uint16_t((wall + kTunnel * voxel) * toQ);

	// Flood fill from the grid's corner through every voxel farther than
	// limit from all surfaces, marking what it reaches with bit.
	enum : uint8_t { kOutside = 1, kOpen = 2 };
	std::vector<uint8_t> state(N, 0);
	auto flood = [&](uint16_t limit, uint8_t bit) {
		auto free = [&](size_t idx) { return dist[idx] > limit && !(state[idx] & bit); };
		std::vector<std::array<int, 3>> stack{{0, 0, 0}};
		while (!stack.empty()) {
			auto [i, j, k] = stack.back();
			stack.pop_back();
			size_t row = at(0, j, k);
			if (!free(row + i)) continue;
			int il = i, ir = i;
			while (il > 0 && free(row + il - 1)) il--;
			while (ir < nx - 1 && free(row + ir + 1)) ir++;
			for (int x = il; x <= ir; x++) state[row + x] |= bit;
			const int nb[4][2] = {{j - 1, k}, {j + 1, k}, {j, k - 1}, {j, k + 1}};
			for (auto& q : nb) {
				if (q[0] < 0 || q[0] >= ny || q[1] < 0 || q[1] >= nz) continue;
				size_t r = at(0, q[0], q[1]);
				bool run = false;
				for (int x = il; x <= ir; x++) {
					bool ok = free(r + x);
					if (ok && !run) stack.push_back({x, q[0], q[1]});
					run = ok;
				}
			}
		}
	};
	// Everything reachable from the grid's edge without crossing a surface
	// is outside; the rest, enclosed or near a surface, is solid.
	flood(wallQ, kOutside);
	// A passage only a little wider than the wall would print as a row of
	// pinholes (an arm hanging just off the body, say), so open the outside
	// morphologically: keep only what a ball of radius kTunnel voxels can
	// sweep through. That is the region within kTunnel of the voxels farther
	// than wall + kTunnel from every surface (a second flood fill), and the
	// rest of the outside, the narrow passages, is filled. g is the distance
	// to that region, from each of its voxels' own margin past the limit,
	// so it is smooth and the caps it makes in the passages come out round.
	flood(wideQ, kOpen);
	// The ball samples overestimate g by up to about this much (they see the
	// wide region's edge only at voxel centres), so allow for it: an open
	// surface must never take the cap's value.
	const double slack = 0.3 * voxel;
	const double tunnel = kTunnel * voxel + slack, wide = (wideQ + 1) / toQ;
	const double gScale = 65535.0 / (4.0 * voxel);  // g over [-voxel, 3 voxel)
	std::vector<uint16_t> gq(N, 65535);
	auto uOf = [&](size_t idx) { return (dist[idx] + 0.5) / toQ; };
	{
		const int R = 2;
		struct Off { ptrdiff_t d; double len; };
		std::vector<Off> ball;
		for (int dk = -R; dk <= R; dk++)
			for (int dj = -R; dj <= R; dj++)
				for (int di = -R; di <= R; di++) {
					double len = std::sqrt(double(di * di + dj * dj + dk * dk)) * voxel;
					if (len <= (R + 0.01) * voxel)
						ball.push_back({ptrdiff_t(dk) * ptrdiff_t(sz) + ptrdiff_t(dj) * ptrdiff_t(sy) + di, len});
				}
		for (int k = R; k < nz - R; k++)
			for (int j = R; j < ny - R; j++)
				for (int i = R; i < nx - R; i++) {
					size_t idx = at(i, j, k);
					if (state[idx] != kOutside) continue;
					double g = 3.0 * voxel;
					for (const Off& o : ball) {
						size_t y = size_t(ptrdiff_t(idx) + o.d);
						if (!(state[y] & kOpen)) continue;
						g = std::min(g, o.len - (uOf(y) - wide));
					}
					gq[idx] = uint16_t(std::clamp((g + voxel) * gScale, 0.0, 65534.0));
					if (g <= tunnel) state[idx] |= kOpen;
				}
	}
	auto gOf = [&](size_t idx) {
		if (gq[idx] != 65535) return gq[idx] / gScale - voxel;
		return wide - uOf(idx);  // in the wide region: its margin, negative
	};
	size_t outsideCount = 0, filledCount = 0;
	for (uint8_t st : state) {
		outsideCount += (st & kOpen) != 0;
		filledCount += st == kOutside;
	}
	auto isOut = [&](size_t idx) { return (state[idx] & kOpen) != 0; };
	auto filled = [&](size_t idx) { return state[idx] == kOutside; };
	if (log) {
		char buf[160];
		std::snprintf(buf, sizeof buf, "Voxels: %dx%dx%d at %.3f, %.1f%% outside, %zu filled in narrow gaps", nx, ny,
		              nz, voxel, 100.0 * outsideCount / N, filledCount);
		log(buf);
	}

	// The outside ends where the distance reaches the wall, so contour the
	// distance there: a smooth offset of the true surface, which is moved
	// back in afterwards. (Contouring the sign alone would snap to voxel
	// layers.) Where a passage was filled, the cap is where g reaches the
	// ball's radius. Values are never exactly zero, so no surface vertex
	// sits on a grid point.
	const double level = (wallQ + 1) / toQ;
	auto value = [&](size_t idx) {
		double u = uOf(idx);
		if (isOut(idx)) return std::max(std::min(u - level, tunnel - gOf(idx)), 1e-9 * voxel);
		if (filled(idx)) return std::min(tunnel - gOf(idx), -1e-9 * voxel);
		// Farther than the wall but not outside: past the surface, inside
		// the solid, where the signed distance is negative.
		if (dist[idx] > wallQ) return -u - level;
		return std::min(u - level, -1e-9 * voxel);
	};
	// Whether the distance, not a cap, decides the value at a corner.
	auto onOffset = [&](size_t idx) {
		if (filled(idx)) return false;
		return !isOut(idx) || uOf(idx) - level <= tunnel - gOf(idx);
	};

	EdgeMap edges(triCount);
	std::vector<char> exact;  // per vertex: on the offset, not a gap's cap
	const size_t offs[8] = {0, sx, sy, sx + sy, sz, sx + sz, sy + sz, sx + sy + sz};
	for (int k = 0; k < nz - 1; k++)
		for (int j = 0; j < ny - 1; j++)
			for (int i = 0; i < nx - 1; i++) {
				size_t base = at(i, j, k);
				int sum = 0;
				for (size_t o : offs) sum += isOut(base + o);
				if (sum == 0 || sum == 8) continue;
				size_t idx[8];
				double val[8];
				V3 pos[8];
				for (int c = 0; c < 8; c++) {
					idx[c] = base + offs[c];
					val[c] = value(idx[c]);
					pos[c] = point(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
				}
				auto vertex = [&](int ca, int cb) {
					int l = (ca & cb) == ca ? ca : cb, h = l == ca ? cb : ca;
					uint64_t key = uint64_t(idx[l]) * 8 + uint64_t(h ^ l);
					size_t slot = 0;
					int id = edges.find(key, slot);
					if (id >= 0) return id;
					double t = val[l] / (val[l] - val[h]);
					id = (int)out.v.size();
					out.v.push_back(pos[l] + (pos[h] - pos[l]) * t);
					exact.push_back(onOffset(idx[l]) && onOffset(idx[h]));
					edges.insert(key, id, slot);
					return id;
				};
				for (const auto& tet : kTets) {
					int in[4], ex[4], ni = 0, ne = 0;
					for (int q = 0; q < 4; q++) {
						if (isOut(idx[tet[q]]))
							ex[ne++] = tet[q];
						else
							in[ni++] = tet[q];
					}
					if (ni == 0 || ne == 0) continue;
					// Wind each piece from the grid corners, whose positions are
					// exact, rather than from its own vertices: a sliver's normal
					// can point anywhere. A piece cut from the rays out of one
					// corner keeps that corner's view of the opposite face.
					auto orient = [&](int a, int b, int c, int d) {
						return dot(pos[b] - pos[a], cross(pos[c] - pos[a], pos[d] - pos[a]));
					};
					if (ni == 1 || ne == 1) {
						int a = ni == 1 ? in[0] : ex[0];
						const int* rest = ni == 1 ? ex : in;
						int p0 = vertex(a, rest[0]), p1 = vertex(a, rest[1]), p2 = vertex(a, rest[2]);
						bool outward = orient(a, rest[0], rest[1], rest[2]) > 0;
						if (outward != (ni == 1)) std::swap(p1, p2);
						out.f.push_back({p0, p1, p2});
					} else {
						int q[4] = {vertex(in[0], ex[0]), vertex(in[0], ex[1]), vertex(in[1], ex[1]),
						            vertex(in[1], ex[0])};
						if (orient(in[0], ex[0], ex[1], in[1]) < 0) std::swap(q[1], q[3]);
						out.f.push_back({q[0], q[1], q[2]});
						out.f.push_back({q[0], q[2], q[3]});
					}
				}
			}
	// The distance is unsigned, so it folds where a cube corner lies past
	// the true surface and interpolating it there misplaces the vertex by
	// up to a voxel, which shows as rings. Put every vertex exactly on the
	// offset instead: level away from its nearest surface point.
	Mesh all;
	for (const Mesh* m : parts) all.append(*m);
	all.uv.clear();
	BVH bvh(all);
	{
		// A vertex can still start on the wrong side of its nearest surface
		// (an inside corner near it was ambiguous); its outward normal says
		// which side the offset belongs on.
		std::vector<V3> n = vertexNormals(out);
		for (size_t i = 0; i < out.v.size(); i++) {
			if (!exact[i]) continue;
			V3& p = out.v[i];
			V3 q = bvh.closest(p);
			V3 d = p - q;
			double l = length(d);
			if (l < 1e-12) {
				p = q + n[i] * level;
				continue;
			}
			d = d / l;
			if (dot(d, n[i]) < 0) d = d * -1.0;
			p = q + d * level;
		}
	}
	// Back from the offset to the surface itself.
	displace(out, [&](int) { return -level; });
	// Gap caps come out of the grid stepped; sand them and their surrounds.
	std::vector<char> rough(exact.size());
	for (size_t i = 0; i < exact.size(); i++) rough[i] = !exact[i];
	for (int ring = 0; ring < 3; ring++) {
		std::vector<char> grown = rough;
		for (const Tri& t : out.f)
			if (rough[t[0]] || rough[t[1]] || rough[t[2]]) grown[t[0]] = grown[t[1]] = grown[t[2]] = 1;
		rough.swap(grown);
	}
	smooth(out, 0.5, 20, &rough);
	return out;
}

int dropSpecks(Mesh& m, double minFraction) {
	int count = 0;
	auto island = faceIslands(m, count);
	if (count <= 1) return 0;
	auto vol = islandVolumes(m, island, count);
	double biggest = 0;
	for (double& v : vol) {
		v = std::fabs(v);
		biggest = std::max(biggest, v);
	}
	int dropped = 0;
	std::vector<char> keep(count);
	for (int i = 0; i < count; i++) {
		keep[i] = vol[i] >= biggest * minFraction;
		if (!keep[i]) dropped++;
	}
	if (dropped) keepFaces(m, [&](int f) { return keep[island[f]] != 0; });
	return dropped;
}

}  // namespace pmo
