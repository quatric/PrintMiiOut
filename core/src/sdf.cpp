#include "sdf.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace pmo {

namespace {

// The six tetrahedra of a cube around its main diagonal (corner bits:
// x = 1, y = 2, z = 4), the same split as fuse.cpp, so neighbouring cubes
// cut their shared faces alike and the surface stays closed.
const int kTets[6][4] = {{0, 1, 3, 7}, {0, 1, 5, 7}, {0, 2, 3, 7}, {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 4, 6, 7}};
const int kBlock = 8;

/** Open addressing map from grid edge to output vertex (as fuse.cpp has). */
class EdgeMap {
public:
	EdgeMap() : keys_(1 << 16, kEmpty), vals_(1 << 16, -1) {}
	int find(uint64_t key, size_t& slot) const {
		size_t mask = keys_.size() - 1, i = hash(key) & mask;
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

}  // namespace

Mesh meshSdf(const Sdf& f, V3 lo, V3 hi, double voxel, const std::function<void(const std::string&)>& log,
             double zStretch) {
	Mesh out;
	const double dz = voxel * std::max(1.0, zStretch);
	lo -= V3(2 * voxel, 2 * voxel, 2 * dz);
	hi += V3(2 * voxel, 2 * voxel, 2 * dz);
	const int nx = int(std::ceil((hi.x - lo.x) / voxel)), ny = int(std::ceil((hi.y - lo.y) / voxel)),
	          nz = int(std::ceil((hi.z - lo.z) / dz));
	const uint64_t px = uint64_t(nx) + 1, py = uint64_t(ny) + 1;
	auto point = [&](int i, int j, int k) { return V3(lo.x + i * voxel, lo.y + j * voxel, lo.z + k * dz); };
	const double tiny = 1e-9 * voxel;
	auto sample = [&](const V3& p) {
		double v = f(p);
		return v == 0 ? tiny : v;  // never exactly on the surface: zero counts as outside
	};

	EdgeMap edges;
	size_t sampled = 0, blocks = 0, busy = 0;
	std::vector<double> val;
	for (int bk = 0; bk < nz; bk += kBlock)
		for (int bj = 0; bj < ny; bj += kBlock)
			for (int bi = 0; bi < nx; bi += kBlock) {
				const int cx = std::min(kBlock, nx - bi), cy = std::min(kBlock, ny - bj), cz = std::min(kBlock, nz - bk);
				blocks++;
				// The surface cannot be in a block farther from it than the
				// block's half diagonal (with room for an inexact bound).
				V3 c = point(bi, bj, bk) + V3(cx * voxel, cy * voxel, cz * dz) * 0.5;
				double reach = 0.5 * std::sqrt(double(cx * cx + cy * cy) * voxel * voxel + double(cz * cz) * dz * dz);
				if (std::fabs(f(c)) > 1.5 * reach + voxel) continue;
				busy++;
				const int sx = cx + 1, sy = cy + 1, sz = cz + 1;
				val.resize(size_t(sx) * sy * sz);
				for (int k = 0; k < sz; k++)
					for (int j = 0; j < sy; j++)
						for (int i = 0; i < sx; i++) val[(size_t(k) * sy + j) * sx + i] = sample(point(bi + i, bj + j, bk + k));
				sampled += val.size();
				for (int k = 0; k < cz; k++)
					for (int j = 0; j < cy; j++)
						for (int i = 0; i < cx; i++) {
							double v[8];
							V3 pos[8];
							uint64_t gid[8];
							int inside = 0;
							for (int q = 0; q < 8; q++) {
								int di = q & 1, dj = (q >> 1) & 1, dk = (q >> 2) & 1;
								v[q] = val[(size_t(k + dk) * sy + (j + dj)) * sx + (i + di)];
								pos[q] = point(bi + i + di, bj + j + dj, bk + k + dk);
								gid[q] = (uint64_t(bk + k + dk) * py + uint64_t(bj + j + dj)) * px + uint64_t(bi + i + di);
								inside += v[q] < 0;
							}
							if (inside == 0 || inside == 8) continue;
							auto vertex = [&](int ca, int cb) {
								int l = (ca & cb) == ca ? ca : cb, h = l == ca ? cb : ca;
								uint64_t key = gid[l] * 8 + uint64_t(h ^ l);
								size_t slot = 0;
								int id = edges.find(key, slot);
								if (id >= 0) return id;
								double t = v[l] / (v[l] - v[h]);
								id = (int)out.v.size();
								out.v.push_back(pos[l] + (pos[h] - pos[l]) * t);
								edges.insert(key, id, slot);
								return id;
							};
							auto orient = [&](int a, int b, int cc, int d) {
								return dot(pos[b] - pos[a], cross(pos[cc] - pos[a], pos[d] - pos[a]));
							};
							for (const auto& tet : kTets) {
								int in[4], ex[4], ni = 0, ne = 0;
								for (int q = 0; q < 4; q++) {
									if (v[tet[q]] >= 0)
										ex[ne++] = tet[q];
									else
										in[ni++] = tet[q];
								}
								if (ni == 0 || ne == 0) continue;
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
			}

	// Onto the surface: Newton steps along the gradient, never more than
	// half a cell, so a vertex cannot jump to another sheet.
	const double h = 0.05 * voxel;
	auto project = [&](V3& p, int steps) {
		for (int it = 0; it < steps; it++) {
			double d = f(p);
			if (std::fabs(d) < 1e-4 * voxel) break;
			V3 g((f(p + V3(h, 0, 0)) - d) / h, (f(p + V3(0, h, 0)) - d) / h, (f(p + V3(0, 0, h)) - d) / h);
			double g2 = dot(g, g);
			if (g2 < 1e-12) break;
			V3 step = g * (d / g2);
			double l = length(step);
			if (l > 0.5 * voxel) step = step * (0.5 * voxel / l);
			p -= step;
		}
	};
	for (V3& p : out.v) project(p, 1);
	// The grid leaves slivers next to its corners, which no simplifier can
	// collapse: relax every vertex towards its neighbours and put it back on
	// the surface, twice, which evens the triangles out without moving the
	// surface.
	{
		// Neighbours of every vertex, packed: start[i] .. start[i + 1] in nb.
		const size_t V = out.v.size();
		std::vector<int> start(V + 1, 0), nb(out.f.size() * 6);
		for (const Tri& t : out.f)
			for (int k = 0; k < 3; k++) start[t[k] + 1] += 2;
		for (size_t i = 0; i < V; i++) start[i + 1] += start[i];
		{
			std::vector<int> fill(start.begin(), start.end() - 1);
			for (const Tri& t : out.f)
				for (int k = 0; k < 3; k++) {
					nb[fill[t[k]]++] = t[(k + 1) % 3];
					nb[fill[t[k]]++] = t[(k + 2) % 3];
				}
		}
		std::vector<V3> next(out.v.size());
		for (int round = 0; round < 1; round++) {
			for (size_t i = 0; i < V; i++) {
				V3 c;  // each neighbour comes twice, which weighs them all alike
				for (int k = start[i]; k < start[i + 1]; k++) c += out.v[nb[k]];
				int count = start[i + 1] - start[i];
				next[i] = count ? out.v[i] * 0.5 + c * (0.5 / double(count)) : out.v[i];
			}
			for (size_t i = 0; i < out.v.size(); i++) {
				out.v[i] = next[i];
				project(out.v[i], 2);
			}
		}
	}
	if (log) {
		char buf[160];
		std::snprintf(buf, sizeof buf, "Solid: %dx%dx%d at %.3f (%.3f in z), %zu of %zu blocks sampled, %zu triangles", nx,
		              ny, nz, voxel, dz, busy, blocks, out.f.size());
		log(buf);
	}
	return out;
}

}  // namespace pmo
