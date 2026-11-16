// Quadric error edge collapse (Garland and Heckbert) that never changes the
// mesh's topology: each collapse must pass the link condition, so a closed
// manifold stays closed and manifold, which slicers need.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <vector>

#include "fuse.hpp"
#include "mesh_ops.hpp"

namespace pmo {

namespace {

struct Quadric {
	double a[10] = {};  // xx xy xz xw yy yz yw zz zw ww
	void addPlane(const V3& n, double d) {
		a[0] += n.x * n.x; a[1] += n.x * n.y; a[2] += n.x * n.z; a[3] += n.x * d;
		a[4] += n.y * n.y; a[5] += n.y * n.z; a[6] += n.y * d;
		a[7] += n.z * n.z; a[8] += n.z * d;
		a[9] += d * d;
	}
	Quadric& operator+=(const Quadric& o) {
		for (int i = 0; i < 10; i++) a[i] += o.a[i];
		return *this;
	}
	double eval(const V3& p) const {
		return a[0] * p.x * p.x + 2 * a[1] * p.x * p.y + 2 * a[2] * p.x * p.z + 2 * a[3] * p.x +
		       a[4] * p.y * p.y + 2 * a[5] * p.y * p.z + 2 * a[6] * p.y + a[7] * p.z * p.z + 2 * a[8] * p.z + a[9];
	}
	/** The point of least error, if the quadric pins one down. */
	bool optimum(V3& p) const {
		double m00 = a[0], m01 = a[1], m02 = a[2], m11 = a[4], m12 = a[5], m22 = a[7];
		double c00 = m11 * m22 - m12 * m12, c01 = m02 * m12 - m01 * m22, c02 = m01 * m12 - m02 * m11;
		double det = m00 * c00 + m01 * c01 + m02 * c02;
		double scale = m00 + m11 + m22;
		if (std::fabs(det) < 1e-9 * scale * scale * scale || scale <= 0) return false;
		double c11 = m00 * m22 - m02 * m02, c12 = m01 * m02 - m00 * m12, c22 = m00 * m11 - m01 * m01;
		double bx = -a[3], by = -a[6], bz = -a[8];
		p = V3((c00 * bx + c01 * by + c02 * bz) / det, (c01 * bx + c11 * by + c12 * bz) / det,
		       (c02 * bx + c12 * by + c22 * bz) / det);
		return true;
	}
};

struct Candidate {
	double cost;
	int u;
	uint32_t stamp;
	bool operator<(const Candidate& o) const { return cost > o.cost; }  // min-heap
};

class Simplifier {
public:
	explicit Simplifier(Mesh& m, const std::vector<char>* locked) : m_(m), locked_(locked), alive_(m.f.size(), 1), faces_(m.v.size()), q_(m.v.size()),
	                               stamp_(m.v.size(), 0), target_(m.v.size(), -1), pos_(m.v.size()),
	                               tries_(m.v.size(), 0), bestCost_(m.v.size(), 1e300) {
		for (int f = 0; f < (int)m.f.size(); f++) {
			const Tri& t = m.f[f];
			V3 n = cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]]);
			double len = length(n);
			if (len < 1e-30) continue;
			n = n / len;
			double d = -dot(n, m.v[t[0]]);
			for (int k : t) {
				q_[k].addPlane(n, d);
				faces_[k].push_back(f);
			}
		}
		live_ = m.f.size();
	}

	/** First, cheaply, collapse every edge shorter than shortLen to its
	 *  middle: contouring leaves many next to grid points. */
	void collapseShort(double shortLen) {
		const double s2 = shortLen * shortLen;
		for (size_t f = 0; f < m_.f.size(); f++) {
			for (int k = 0; k < 3 && alive_[f]; k++) {
				int a = m_.f[f][k], b = m_.f[f][(k + 1) % 3];
				V3 d = m_.v[a] - m_.v[b];
				if (dot(d, d) >= s2) continue;
				V3 mid = (m_.v[a] + m_.v[b]) * 0.5;
				if (isLocked(a) || isLocked(b)) continue;
				if (valid(a, b, mid)) merge(a, b, mid);
			}
		}
	}

	/** Collapse, in a few cheap passes, every vertex whose faces all lie in
	 *  one plane into a neighbour: the surface stays where it was, and the
	 *  heap has far less to do. Meshes contoured from a grid have acres of
	 *  such triangles on their flat faces. */
	void collapseFlat(double cosTolerance, size_t targetTris) {
		for (int round = 0; round < 4 && live_ > targetTris; round++) {
			size_t merged = 0;
			for (int u = 0; u < (int)m_.v.size() && live_ > targetTris; u++) {
				if (faces_[u].empty() || isLocked(u)) continue;
				V3 n0;
				bool any = false, flat = true;
				for (int f : faces_[u]) {
					if (!alive_[f]) continue;
					const Tri& t = m_.f[f];
					V3 n = normalized(cross(m_.v[t[1]] - m_.v[t[0]], m_.v[t[2]] - m_.v[t[0]]));
					if (!any) {
						n0 = n;
						any = true;
					} else if (dot(n, n0) < cosTolerance) {
						flat = false;
						break;
					}
				}
				if (!any || !flat) continue;
				std::vector<int>& nb = scratchC_;
				neighbours(u, nb);
				for (int v : nb) {
					if (isLocked(v)) continue;
					V3 p = m_.v[v];
					if (valid(u, v, p)) {
						merge(u, v, p);
						merged++;
						break;
					}
				}
			}
			if (!merged) break;
		}
	}

	void run(size_t targetTris, double maxCost, double shortLen, bool flatFirst) {
		if (flatFirst) collapseFlat(0.99995, targetTris);
		if (shortLen > 0) collapseShort(shortLen);
		for (int u = 0; u < (int)m_.v.size(); u++)
			if (!faces_[u].empty()) push(u);
		while (live_ > targetTris && !heap_.empty()) {
			Candidate c = heap_.top();
			heap_.pop();
			if (c.stamp != stamp_[c.u] || target_[c.u] < 0) continue;
			if (c.cost > maxCost) break;
			int u = c.u, v = target_[u];
			if (!valid(u, v, pos_[u])) {
				// Blocked: try the next cheapest once, then wait until a
				// neighbour's collapse changes things around u.
				target_[u] = -1;
				stamp_[u]++;
				if (++tries_[u] < 2) push(u, v);
				continue;
			}
			collapse(u, v, pos_[u]);
		}
		std::vector<Tri> f;
		for (size_t i = 0; i < m_.f.size(); i++)
			if (alive_[i]) f.push_back(m_.f[i]);
		m_.f.swap(f);
		m_.uv.clear();
		compact(m_);
	}

private:
	Mesh& m_;
	const std::vector<char>* locked_;
	std::vector<char> alive_;
	std::vector<std::vector<int>> faces_;
	std::vector<Quadric> q_;
	std::vector<uint32_t> stamp_;
	std::vector<int> target_;
	std::vector<V3> pos_;
	std::vector<uint8_t> tries_;
	std::vector<double> bestCost_;
	std::priority_queue<Candidate> heap_;
	size_t live_ = 0;
	std::vector<int> scratchA_, scratchB_, scratchC_, scratchD_;

	void neighbours(int u, std::vector<int>& out) const {
		out.clear();
		for (int f : faces_[u]) {
			if (!alive_[f]) continue;
			for (int k : m_.f[f])
				if (k != u) out.push_back(k);
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
	}

	bool isLocked(int u) const { return locked_ && size_t(u) < locked_->size() && (*locked_)[u]; }

	/** Cheapest collapse of u into a neighbour (other than skip). */
	void push(int u, int skip = -1) {
		target_[u] = -1;
		if (isLocked(u)) return;
		std::vector<int>& nb = scratchC_;
		neighbours(u, nb);
		double best = 1e300;
		int bestV = -1;
		V3 bestP;
		for (int v : nb) {
			if (v == skip || isLocked(v)) continue;
			V3 p;  // kept near the edge it came from
			double cost = edgeCost(u, v, p);
			if (cost < best) {
				best = cost;
				bestV = v;
				bestP = p;
			}
		}
		target_[u] = bestV;
		if (bestV < 0) return;
		pos_[u] = bestP;
		bestCost_[u] = best;
		heap_.push({std::max(best, 0.0), u, stamp_[u]});
	}

	double edgeCost(int u, int v, V3& p) const {
		Quadric q = q_[u];
		q += q_[v];
		V3 mid = (m_.v[u] + m_.v[v]) * 0.5;
		if (!q.optimum(p) || length(p - mid) > length(m_.v[u] - m_.v[v])) p = mid;
		return std::max(q.eval(p), 0.0);
	}

	bool valid(int u, int v, const V3& p) {
		// Link condition: the only neighbours u and v share are the far
		// corners of the two faces on the edge.
		neighbours(u, scratchA_);
		neighbours(v, scratchB_);
		int shared = 0;
		for (size_t i = 0, j = 0; i < scratchA_.size() && j < scratchB_.size();) {
			if (scratchA_[i] < scratchB_[j]) i++;
			else if (scratchA_[i] > scratchB_[j]) j++;
			else { shared++; i++; j++; }
		}
		int edgeFaces = 0;
		for (int f : faces_[u])
			if (alive_[f] && (m_.f[f][0] == v || m_.f[f][1] == v || m_.f[f][2] == v)) edgeFaces++;
		if (edgeFaces != 2 || shared != 2) return false;
		// No face may flip or collapse to a sliver.
		for (int x : {u, v})
			for (int f : faces_[x]) {
				if (!alive_[f]) continue;
				const Tri& t = m_.f[f];
				bool hasU = t[0] == u || t[1] == u || t[2] == u, hasV = t[0] == v || t[1] == v || t[2] == v;
				if (hasU && hasV) continue;
				V3 a = m_.v[t[0]], b = m_.v[t[1]], c = m_.v[t[2]];
				V3 before = cross(b - a, c - a);
				V3 pa = (t[0] == x) ? p : a, pb = (t[1] == x) ? p : b, pc = (t[2] == x) ? p : c;
				V3 after = cross(pb - pa, pc - pa);
				double la = length(after);
				if (dot(before, after) <= 0.3 * length(before) * la) return false;
				double e = std::max(dot(pb - pa, pb - pa), std::max(dot(pc - pb, pc - pb), dot(pa - pc, pa - pc)));
				if (la < 0.05 * e) return false;  // skinnier than about 3 degrees
			}
		return true;
	}

	/** Merge u into v at p: topology and quadrics, nothing queued. */
	void merge(int u, int v, const V3& p) {
		for (int f : faces_[u]) {
			if (!alive_[f]) continue;
			Tri& t = m_.f[f];
			if (t[0] == v || t[1] == v || t[2] == v) {
				alive_[f] = 0;
				live_--;
				continue;
			}
			for (int& k : t)
				if (k == u) k = v;
			faces_[v].push_back(f);
		}
		faces_[u].clear();
		faces_[u].shrink_to_fit();
		auto& fv = faces_[v];
		fv.erase(std::remove_if(fv.begin(), fv.end(), [&](int f) { return !alive_[f]; }), fv.end());
		m_.v[v] = p;
		q_[v] += q_[u];
		target_[u] = -1;
		stamp_[u]++;
	}

	/** Merge, then re-score only what the merge changed: v itself, the
	 *  neighbours whose best collapse involved u or v, and every
	 *  neighbour's edge to v. */
	void collapse(int u, int v, const V3& p) {
		merge(u, v, p);
		std::vector<int> nb;
		nb.swap(scratchD_);
		neighbours(v, nb);
		stamp_[v]++;
		tries_[v] = 0;
		push(v);
		for (int w : nb) {
			tries_[w] = 0;
			if (target_[w] == u || target_[w] == v || target_[w] < 0) {
				stamp_[w]++;
				push(w);
				continue;
			}
			if (isLocked(w) || isLocked(v)) continue;
			V3 q;
			double c = edgeCost(w, v, q);
			if (c < bestCost_[w]) {
				target_[w] = v;
				pos_[w] = q;
				bestCost_[w] = c;
				stamp_[w]++;
				heap_.push({c, w, stamp_[w]});
			}
		}
		nb.swap(scratchD_);
	}
};

}  // namespace

void simplify(Mesh& m, size_t maxTriangles, double maxError, const std::vector<char>* locked, bool flatFirst) {
	if (m.f.size() <= maxTriangles) return;
	Simplifier s(m, locked);
	s.run(maxTriangles, maxError * maxError, maxError * 1.6, flatFirst);
}

}  // namespace pmo
