#include "mesh_ops.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <unordered_map>

namespace pmo {

namespace {

uint64_t edgeKey(int a, int b) {
	if (a > b) std::swap(a, b);
	return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

struct HalfEdge {
	uint64_t key;
	int face;
	int from, to, opposite;
};

/** Every face's three edges, sorted so the faces of each edge sit together. */
std::vector<HalfEdge> sortedHalfEdges(const Mesh& m) {
	std::vector<HalfEdge> he;
	he.reserve(m.f.size() * 3);
	for (int i = 0; i < (int)m.f.size(); i++) {
		const Tri& t = m.f[i];
		for (int k = 0; k < 3; k++) {
			int a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
			he.push_back({edgeKey(a, b), i, a, b, c});
		}
	}
	std::sort(he.begin(), he.end(), [](const HalfEdge& x, const HalfEdge& y) { return x.key < y.key; });
	return he;
}

/** Neighbouring vertices of every vertex, as offsets into one list. */
void adjacency(const Mesh& m, std::vector<int>& start, std::vector<int>& list) {
	size_t n = m.v.size();
	std::vector<std::pair<int, int>> pairs;
	pairs.reserve(m.f.size() * 6);
	for (const Tri& t : m.f)
		for (int k = 0; k < 3; k++) {
			int a = t[k], b = t[(k + 1) % 3];
			pairs.push_back({a, b});
			pairs.push_back({b, a});
		}
	std::sort(pairs.begin(), pairs.end());
	pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
	start.assign(n + 1, 0);
	for (auto& p : pairs) start[p.first + 1]++;
	for (size_t i = 0; i < n; i++) start[i + 1] += start[i];
	list.resize(pairs.size());
	for (size_t i = 0; i < pairs.size(); i++) list[i] = pairs[i].second;
}

}  // namespace

void compact(Mesh& m) {
	bool uv = m.hasUV();
	size_t w = 0;
	for (size_t i = 0; i < m.f.size(); i++) {
		const Tri& t = m.f[i];
		if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
		m.f[w] = t;
		if (uv) m.uv[w] = m.uv[i];
		w++;
	}
	m.f.resize(w);
	if (uv) m.uv.resize(w);
	std::vector<int> remap(m.v.size(), -1);
	int n = 0;
	for (Tri& t : m.f)
		for (int& k : t) {
			if (remap[k] < 0) {
				remap[k] = n++;
			}
			k = remap[k];
		}
	std::vector<V3> v(n);
	for (size_t i = 0; i < remap.size(); i++)
		if (remap[i] >= 0) v[remap[i]] = m.v[i];
	m.v.swap(v);
}

void weld(Mesh& m, double dist, const std::vector<char>* only) {
	if (m.v.empty()) return;
	double cell = std::max(dist, 1e-12);
	std::unordered_map<uint64_t, std::vector<int>> grid;
	grid.reserve(m.v.size());
	auto key = [](int64_t x, int64_t y, int64_t z) {
		return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
	};
	std::vector<int> remap(m.v.size());
	double d2 = dist * dist;
	for (int i = 0; i < (int)m.v.size(); i++) {
		remap[i] = i;
		if (only && !(*only)[i]) continue;
		const V3& p = m.v[i];
		int64_t cx = (int64_t)std::floor(p.x / cell), cy = (int64_t)std::floor(p.y / cell),
		        cz = (int64_t)std::floor(p.z / cell);
		int found = -1;
		for (int dx = -1; dx <= 1 && found < 0; dx++)
			for (int dy = -1; dy <= 1 && found < 0; dy++)
				for (int dz = -1; dz <= 1 && found < 0; dz++) {
					auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
					if (it == grid.end()) continue;
					for (int r : it->second) {
						V3 d = m.v[r] - p;
						if (dot(d, d) <= d2) {
							found = r;
							break;
						}
					}
				}
		if (found >= 0)
			remap[i] = found;
		else
			grid[key(cx, cy, cz)].push_back(i);
	}
	for (Tri& t : m.f)
		for (int& k : t) k = remap[k];
	compact(m);
}

int countOpenEdges(const Mesh& m) {
	auto he = sortedHalfEdges(m);
	int open = 0;
	for (size_t i = 0; i < he.size();) {
		size_t j = i;
		while (j < he.size() && he[j].key == he[i].key) j++;
		if (j - i == 1) open++;
		i = j;
	}
	return open;
}

int countNonManifoldEdges(const Mesh& m) {
	auto he = sortedHalfEdges(m);
	int bad = 0;
	for (size_t i = 0; i < he.size();) {
		size_t j = i;
		while (j < he.size() && he[j].key == he[i].key) j++;
		if (j - i != 2) bad++;
		i = j;
	}
	return bad;
}

std::vector<char> openVertices(const Mesh& m) {
	std::vector<char> open(m.v.size(), 0);
	auto he = sortedHalfEdges(m);
	for (size_t i = 0; i < he.size();) {
		size_t j = i;
		while (j < he.size() && he[j].key == he[i].key) j++;
		if (j - i == 1) open[he[i].from] = open[he[i].to] = 1;
		i = j;
	}
	return open;
}

int fillHoles(Mesh& m) {
	auto he = sortedHalfEdges(m);
	// Each hole edge runs opposite to the face edge it borders, so faces
	// added along it face the same way as their neighbours.
	std::unordered_map<int, std::vector<int>> out;  // from -> list of to
	size_t holeEdges = 0;
	for (size_t i = 0; i < he.size();) {
		size_t j = i;
		while (j < he.size() && he[j].key == he[i].key) j++;
		if (j - i == 1) {
			out[he[i].to].push_back(he[i].from);
			holeEdges++;
		}
		i = j;
	}
	if (!holeEdges) return 0;
	bool uv = m.hasUV();
	int filled = 0;
	for (auto& entry : out) {
		while (!entry.second.empty()) {
			int start = entry.first;
			std::vector<int> loop{start};
			int cur = entry.second.back();
			entry.second.pop_back();
			size_t guard = holeEdges + 1;
			while (cur != start && guard--) {
				loop.push_back(cur);
				auto it = out.find(cur);
				if (it == out.end() || it->second.empty()) break;
				int next = it->second.back();
				it->second.pop_back();
				cur = next;
			}
			if (cur != start || loop.size() < 3) continue;
			if (loop.size() == 3) {
				m.f.push_back({loop[0], loop[1], loop[2]});
				if (uv) m.uv.push_back({});
			} else {
				V3 c;
				for (int k : loop) c += m.v[k];
				c = c / double(loop.size());
				int ci = (int)m.v.size();
				m.v.push_back(c);
				for (size_t k = 0; k < loop.size(); k++) {
					m.f.push_back({loop[k], loop[(k + 1) % loop.size()], ci});
					if (uv) m.uv.push_back({});
				}
			}
			filled++;
		}
	}
	return filled;
}

static void subdivideOnce(Mesh& m, bool smooth) {
	const int V = (int)m.v.size();
	auto he = sortedHalfEdges(m);
	struct Edge {
		int a, b, faces, opp0, opp1;
	};
	std::vector<Edge> edges;
	edges.reserve(he.size() / 2 + 1);
	// face -> its three edge ids, in the order of its corners (k -> k+1)
	std::vector<std::array<int, 3>> faceEdges(m.f.size());
	for (size_t i = 0; i < he.size();) {
		size_t j = i;
		while (j < he.size() && he[j].key == he[i].key) j++;
		Edge e{he[i].from, he[i].to, int(j - i), he[i].opposite, j - i > 1 ? he[i + 1].opposite : -1};
		int id = (int)edges.size();
		edges.push_back(e);
		for (size_t k = i; k < j; k++) {
			const Tri& t = m.f[he[k].face];
			for (int c = 0; c < 3; c++)
				if (edgeKey(t[c], t[(c + 1) % 3]) == he[k].key) faceEdges[he[k].face][c] = id;
		}
		i = j;
	}

	std::vector<V3> sum(V), bsum(V);
	std::vector<int> count(V, 0), bcount(V, 0), faceCount(V, 0);
	std::vector<char> odd(V, 0);
	for (const Edge& e : edges) {
		sum[e.a] += m.v[e.b];
		sum[e.b] += m.v[e.a];
		count[e.a]++;
		count[e.b]++;
		if (e.faces == 1) {
			bsum[e.a] += m.v[e.b];
			bsum[e.b] += m.v[e.a];
			bcount[e.a]++;
			bcount[e.b]++;
		} else if (e.faces > 2) {
			odd[e.a] = odd[e.b] = 1;
		}
	}
	for (const Tri& t : m.f)
		for (int k : t) faceCount[k]++;

	std::vector<V3> nv(V + edges.size());
	for (int i = 0; i < V; i++) {
		const V3& p = m.v[i];
		if (!smooth || odd[i] || count[i] == 0) {
			nv[i] = p;
		} else if (bcount[i] == 0) {
			int n = count[i];
			double beta = n == 3 ? 3.0 / 16.0 : 3.0 / (8.0 * n);
			nv[i] = p * (1.0 - n * beta) + sum[i] * beta;
		} else if (bcount[i] == 2 && faceCount[i] > 1) {
			nv[i] = p * 0.75 + bsum[i] * 0.125;
		} else {
			nv[i] = p;  // a corner: kept where it is
		}
	}
	for (size_t e = 0; e < edges.size(); e++) {
		const Edge& ed = edges[e];
		V3 a = m.v[ed.a], b = m.v[ed.b];
		if (smooth && ed.faces == 2)
			nv[V + e] = (a + b) * 0.375 + (m.v[ed.opp0] + m.v[ed.opp1]) * 0.125;
		else
			nv[V + e] = (a + b) * 0.5;
	}

	bool uv = m.hasUV();
	std::vector<Tri> nf;
	std::vector<std::array<V2, 3>> nuv;
	nf.reserve(m.f.size() * 4);
	if (uv) nuv.reserve(m.f.size() * 4);
	auto mid = [](V2 p, V2 q) { return V2{(p.u + q.u) * 0.5f, (p.v + q.v) * 0.5f}; };
	for (size_t i = 0; i < m.f.size(); i++) {
		const Tri& t = m.f[i];
		int ab = V + faceEdges[i][0], bc = V + faceEdges[i][1], ca = V + faceEdges[i][2];
		nf.push_back({t[0], ab, ca});
		nf.push_back({ab, t[1], bc});
		nf.push_back({ca, bc, t[2]});
		nf.push_back({ab, bc, ca});
		if (uv) {
			const auto& u = m.uv[i];
			V2 mab = mid(u[0], u[1]), mbc = mid(u[1], u[2]), mca = mid(u[2], u[0]);
			nuv.push_back({u[0], mab, mca});
			nuv.push_back({mab, u[1], mbc});
			nuv.push_back({mca, mbc, u[2]});
			nuv.push_back({mab, mbc, mca});
		}
	}
	m.v.swap(nv);
	m.f.swap(nf);
	m.uv.swap(nuv);
}

void subdivide(Mesh& m, int levels, int smoothLevels) {
	for (int i = 0; i < levels; i++) subdivideOnce(m, i < smoothLevels);
}

std::vector<V3> vertexNormals(const Mesh& m) {
	std::vector<V3> n(m.v.size());
	for (const Tri& t : m.f) {
		V3 fn = cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]]);
		for (int k : t) n[k] += fn;
	}
	for (V3& x : n) x = normalized(x);
	return n;
}

void smooth(Mesh& m, double factor, int iterations, const std::vector<char>* only) {
	std::vector<int> start, list;
	adjacency(m, start, list);
	std::vector<V3> next(m.v.size());
	for (int it = 0; it < iterations; it++) {
		for (size_t i = 0; i < m.v.size(); i++) {
			int n = start[i + 1] - start[i];
			if (!n || (only && !(*only)[i])) {
				next[i] = m.v[i];
				continue;
			}
			V3 avg;
			for (int k = start[i]; k < start[i + 1]; k++) avg += m.v[list[k]];
			avg = avg / double(n);
			next[i] = m.v[i] + (avg - m.v[i]) * factor;
		}
		m.v.swap(next);
	}
}

void solidify(Mesh& m, double thickness, double offset) {
	const int V = (int)m.v.size();
	auto n = vertexNormals(m);
	auto he = sortedHalfEdges(m);
	Mesh s;
	s.v.resize(V * 2);
	double out = thickness * (1.0 + offset) * 0.5, in = thickness * (1.0 - offset) * 0.5;
	for (int i = 0; i < V; i++) {
		s.v[i] = m.v[i] + n[i] * out;
		s.v[V + i] = m.v[i] - n[i] * in;
	}
	s.f.reserve(m.f.size() * 2 + 64);
	for (const Tri& t : m.f) s.f.push_back(t);
	for (const Tri& t : m.f) s.f.push_back({t[0] + V, t[2] + V, t[1] + V});
	for (size_t i = 0; i < he.size();) {
		size_t j = i;
		while (j < he.size() && he[j].key == he[i].key) j++;
		if (j - i == 1) {
			int a = he[i].from, b = he[i].to;
			s.f.push_back({b, a, a + V});
			s.f.push_back({b, a + V, b + V});
		}
		i = j;
	}
	m = std::move(s);
}

void displace(Mesh& m, const std::function<double(int)>& amount) {
	auto n = vertexNormals(m);
	for (size_t i = 0; i < m.v.size(); i++) m.v[i] += n[i] * amount((int)i);
}

std::vector<int> faceIslands(const Mesh& m, int& count) {
	std::vector<int> parent(m.v.size());
	std::iota(parent.begin(), parent.end(), 0);
	std::function<int(int)> find = [&](int x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	};
	for (const Tri& t : m.f) {
		int a = find(t[0]), b = find(t[1]), c = find(t[2]);
		parent[b] = a;
		parent[find(c)] = a;
	}
	std::vector<int> label(m.v.size(), -1), island(m.f.size());
	count = 0;
	for (size_t i = 0; i < m.f.size(); i++) {
		int r = find(m.f[i][0]);
		if (label[r] < 0) label[r] = count++;
		island[i] = label[r];
	}
	return island;
}

double triangleArea(const Mesh& m, int i) {
	const Tri& t = m.f[i];
	return 0.5 * length(cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]]));
}

std::vector<double> islandVolumes(const Mesh& m, const std::vector<int>& island, int count) {
	std::vector<double> vol(count, 0.0);
	for (size_t i = 0; i < m.f.size(); i++) {
		const Tri& t = m.f[i];
		vol[island[i]] += dot(m.v[t[0]], cross(m.v[t[1]], m.v[t[2]])) / 6.0;
	}
	return vol;
}

void keepFaces(Mesh& m, const std::function<bool(int)>& keep) {
	bool uv = m.hasUV();
	size_t w = 0;
	for (size_t i = 0; i < m.f.size(); i++) {
		if (!keep((int)i)) continue;
		m.f[w] = m.f[i];
		if (uv) m.uv[w] = m.uv[i];
		w++;
	}
	m.f.resize(w);
	if (uv) m.uv.resize(w);
	compact(m);
}

// ---------------------------------------------------------------------------
// BVH
// ---------------------------------------------------------------------------

BVH::BVH(const Mesh& m) : mesh_(m) {
	order_.resize(m.f.size());
	std::iota(order_.begin(), order_.end(), 0);
	std::vector<V3> centres(m.f.size());
	for (size_t i = 0; i < m.f.size(); i++) {
		const Tri& t = m.f[i];
		centres[i] = (m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) / 3.0;
	}
	nodes_.reserve(m.f.size() / 2 + 1);
	if (!m.f.empty()) build(0, (int)m.f.size(), centres, 0);
}

int BVH::build(int start, int count, std::vector<V3>& centres, int depth) {
	int id = (int)nodes_.size();
	nodes_.push_back(Node());
	V3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300), clo = lo, chi = hi;
	for (int i = start; i < start + count; i++) {
		const Tri& t = mesh_.f[order_[i]];
		for (int k : t) {
			lo = vmin(lo, mesh_.v[k]);
			hi = vmax(hi, mesh_.v[k]);
		}
		clo = vmin(clo, centres[order_[i]]);
		chi = vmax(chi, centres[order_[i]]);
	}
	nodes_[id].lo = lo;
	nodes_[id].hi = hi;
	if (count <= 4 || depth > 60) {
		nodes_[id].start = start;
		nodes_[id].count = count;
		return id;
	}
	V3 ext = chi - clo;
	int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
	int half = count / 2;
	std::nth_element(order_.begin() + start, order_.begin() + start + half, order_.begin() + start + count,
	                 [&](int a, int b) { return centres[a][axis] < centres[b][axis]; });
	int left = build(start, half, centres, depth + 1);
	int right = build(start + half, count - half, centres, depth + 1);
	nodes_[id].left = left;
	nodes_[id].right = right;
	return id;
}

static bool rayBox(const V3& o, const V3& inv, const V3& lo, const V3& hi, double maxT) {
	double t0 = 0, t1 = maxT;
	for (int a = 0; a < 3; a++) {
		double n = (lo[a] - o[a]) * inv[a], f = (hi[a] - o[a]) * inv[a];
		if (n > f) std::swap(n, f);
		t0 = std::max(t0, n);
		t1 = std::min(t1, f);
		if (t0 > t1) return false;
	}
	return true;
}

bool BVH::raycast(const V3& o, const V3& d, double maxT, double& tHit) const {
	if (nodes_.empty()) return false;
	V3 inv(1.0 / (std::fabs(d.x) > 1e-300 ? d.x : 1e-300), 1.0 / (std::fabs(d.y) > 1e-300 ? d.y : 1e-300),
	       1.0 / (std::fabs(d.z) > 1e-300 ? d.z : 1e-300));
	double best = maxT;
	bool hit = false;
	int stack[128];
	int sp = 0;
	stack[sp++] = 0;
	while (sp) {
		const Node& n = nodes_[stack[--sp]];
		if (!rayBox(o, inv, n.lo, n.hi, best)) continue;
		if (n.left < 0) {
			for (int i = n.start; i < n.start + n.count; i++) {
				const Tri& tri = mesh_.f[order_[i]];
				V3 a = mesh_.v[tri[0]], e1 = mesh_.v[tri[1]] - a, e2 = mesh_.v[tri[2]] - a;
				V3 p = cross(d, e2);
				double det = dot(e1, p);
				if (std::fabs(det) < 1e-18) continue;
				double invDet = 1.0 / det;
				V3 s = o - a;
				double u = dot(s, p) * invDet;
				if (u < 0 || u > 1) continue;
				V3 q = cross(s, e1);
				double v = dot(d, q) * invDet;
				if (v < 0 || u + v > 1) continue;
				double t = dot(e2, q) * invDet;
				if (t >= 0 && t < best) {
					best = t;
					hit = true;
				}
			}
		} else if (sp < 126) {
			stack[sp++] = n.left;
			stack[sp++] = n.right;
		}
	}
	if (hit) tHit = best;
	return hit;
}

V3 closestOnTriangle(const V3& p, const V3& a, const V3& b, const V3& c) {
	// Ericson, Real-Time Collision Detection, 5.1.5.
	V3 ab = b - a, ac = c - a, ap = p - a;
	double d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	V3 bp = p - b;
	double d3 = dot(ab, bp), d4 = dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;
	double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
	V3 cp = p - c;
	double d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;
	double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
	double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	double denom = 1.0 / (va + vb + vc);
	return a + ab * (vb * denom) + ac * (vc * denom);
}

static double boxDist2(const V3& p, const V3& lo, const V3& hi) {
	double d = 0;
	for (int a = 0; a < 3; a++) {
		double x = p[a] < lo[a] ? lo[a] - p[a] : p[a] > hi[a] ? p[a] - hi[a] : 0;
		d += x * x;
	}
	return d;
}

V3 BVH::closest(const V3& p, int* face) const {
	V3 best = p;
	double bestD = 1e300;
	int bestF = -1;
	if (nodes_.empty()) return best;
	int stack[128];
	int sp = 0;
	stack[sp++] = 0;
	while (sp) {
		const Node& n = nodes_[stack[--sp]];
		if (boxDist2(p, n.lo, n.hi) >= bestD) continue;
		if (n.left < 0) {
			for (int i = n.start; i < n.start + n.count; i++) {
				const Tri& t = mesh_.f[order_[i]];
				V3 q = closestOnTriangle(p, mesh_.v[t[0]], mesh_.v[t[1]], mesh_.v[t[2]]);
				V3 d = q - p;
				double dd = dot(d, d);
				if (dd < bestD) {
					bestD = dd;
					best = q;
					bestF = order_[i];
				}
			}
		} else if (sp < 126) {
			double dl = boxDist2(p, nodes_[n.left].lo, nodes_[n.left].hi);
			double dr = boxDist2(p, nodes_[n.right].lo, nodes_[n.right].hi);
			if (dl < dr) {
				stack[sp++] = n.right;
				stack[sp++] = n.left;
			} else {
				stack[sp++] = n.left;
				stack[sp++] = n.right;
			}
		}
	}
	if (face) *face = bestF;
	return best;
}

}  // namespace pmo
