#include "split.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <unordered_map>

#include "mesh_ops.hpp"

namespace pmo {

namespace {

uint64_t edgeKey(int a, int b) {
	if (a > b) std::swap(a, b);
	return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

/** Two directions spanning the plane with this normal. */
void planeBasis(const V3& n, V3& e1, V3& e2) {
	V3 a = std::fabs(n.x) < 0.9 ? V3(1, 0, 0) : V3(0, 1, 0);
	e1 = normalized(cross(n, a));
	e2 = cross(n, e1);
}

struct P2 {
	double x, y;
};

double cross2(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

double signedArea(const std::vector<P2>& p) {
	double s = 0;
	for (size_t i = 0; i < p.size(); i++) {
		const P2 &a = p[i], &b = p[(i + 1) % p.size()];
		s += a.x * b.y - b.x * a.y;
	}
	return s / 2;
}

bool insideTriangle(const P2& p, const P2& a, const P2& b, const P2& c, double sign) {
	return cross2(a, b, p) * sign > 0 && cross2(b, c, p) * sign > 0 && cross2(c, a, p) * sign > 0;
}

/** Ear clipping: triangles (as positions in loop) covering a simple polygon,
 *  each wound the way the polygon runs. */
std::vector<std::array<int, 3>> earClip(const std::vector<P2>& pts) {
	std::vector<std::array<int, 3>> out;
	std::vector<int> idx(pts.size());
	std::iota(idx.begin(), idx.end(), 0);
	double sign = signedArea(pts) >= 0 ? 1.0 : -1.0;
	size_t i = 0, sinceEar = 0;
	while (idx.size() > 3) {
		size_t n = idx.size();
		int a = idx[(i + n - 1) % n], b = idx[i % n], c = idx[(i + 1) % n];
		bool ear = cross2(pts[a], pts[b], pts[c]) * sign > 0;
		for (size_t k = 0; ear && k < n; k++) {
			int q = idx[k];
			if (q == a || q == b || q == c) continue;
			if (insideTriangle(pts[q], pts[a], pts[b], pts[c], sign)) ear = false;
		}
		if (ear) {
			out.push_back({a, b, c});
			idx.erase(idx.begin() + long(i % n));
			sinceEar = 0;
			if (i >= idx.size()) i = 0;
			continue;
		}
		i = (i + 1) % n;
		if (++sinceEar > n) {
			// Not simple (it should be): finish with a fan rather than fail.
			for (size_t k = 1; k + 1 < idx.size(); k++) out.push_back({idx[0], idx[k], idx[k + 1]});
			return out;
		}
	}
	if (idx.size() == 3) out.push_back({idx[0], idx[1], idx[2]});
	return out;
}

/** Closes the open loops of m that lie in the plane with flat caps. Returns
 *  the loops' outlines. */
std::vector<Outline> capPlanarHoles(Mesh& m, const V3& point, const V3& normal, double eps) {
	std::vector<Outline> outlines;
	std::vector<char> onPlane(m.v.size());
	for (size_t i = 0; i < m.v.size(); i++) onPlane[i] = std::fabs(dot(m.v[i] - point, normal)) <= eps;
	// Half-edges seen once border a hole; the hole runs the other way.
	std::unordered_map<uint64_t, int> count;
	count.reserve(m.f.size() * 3);
	for (const Tri& t : m.f)
		for (int k = 0; k < 3; k++) count[edgeKey(t[k], t[(k + 1) % 3])]++;
	std::unordered_map<int, std::vector<int>> next;
	for (const Tri& t : m.f)
		for (int k = 0; k < 3; k++) {
			int a = t[k], b = t[(k + 1) % 3];
			if (count[edgeKey(a, b)] == 1 && onPlane[a] && onPlane[b]) next[b].push_back(a);
		}
	V3 e1, e2;
	planeBasis(normal, e1, e2);
	bool uv = m.hasUV();
	for (auto& entry : next) {
		while (!entry.second.empty()) {
			int start = entry.first;
			std::vector<int> loop{start};
			int cur = entry.second.back();
			entry.second.pop_back();
			size_t guard = m.v.size();
			while (cur != start && guard--) {
				loop.push_back(cur);
				auto it = next.find(cur);
				if (it == next.end() || it->second.empty()) break;
				cur = it->second.back();
				it->second.pop_back();
			}
			if (cur != start || loop.size() < 3) continue;
			std::vector<P2> pts;
			Outline outline;
			for (int k : loop) {
				V3 d = m.v[k] - point;
				pts.push_back({dot(d, e1), dot(d, e2)});
				outline.push_back(m.v[k]);
			}
			for (const auto& tri : earClip(pts)) {
				m.f.push_back({loop[tri[0]], loop[tri[1]], loop[tri[2]]});
				if (uv) m.uv.push_back({});
			}
			outlines.push_back(std::move(outline));
		}
	}
	return outlines;
}

}  // namespace

void setFaceTags(Mesh& m, const std::vector<int>& tags) {
	m.uv.assign(m.f.size(), {});
	for (size_t f = 0; f < m.f.size() && f < tags.size(); f++)
		for (V2& c : m.uv[f]) c = V2{float(tags[f]), 0.0f};
}

int faceTag(const Mesh& m, int face) {
	if (!m.hasUV()) return kTagCap;
	return int(std::lround(m.uv[face][0].u));
}

bool cutOff(Mesh& m0, const V3& point, const V3& n0, const std::function<bool(int)>& seed,
            const std::function<bool(int, const V3&)>& forbidden, Mesh& off, std::vector<Outline>& outlines) {
	const V3 normal = normalized(n0);
	Mesh m = m0;
	const bool uv = m.hasUV();
	V3 lo, hi;
	m.bounds(lo, hi);
	const double scale = std::max(length(hi - lo), 1e-9);
	const double snap = 1e-9 * scale;

	std::vector<double> d(m.v.size());
	for (size_t i = 0; i < m.v.size(); i++) {
		d[i] = dot(m.v[i] - point, normal);
		if (std::fabs(d[i]) < snap) {
			m.v[i] -= normal * d[i];
			d[i] = 0;
		}
	}

	// Split every face the plane crosses, so each lies on one side of it.
	std::unordered_map<uint64_t, int> cutAt;
	auto cutVertex = [&](int a, int b) {
		uint64_t key = edgeKey(a, b);
		auto it = cutAt.find(key);
		if (it != cutAt.end()) return it->second;
		if (a > b) std::swap(a, b);  // the same point from either face
		double t = d[a] / (d[a] - d[b]);
		int id = (int)m.v.size();
		m.v.push_back(m.v[a] + (m.v[b] - m.v[a]) * t);
		d.push_back(0);
		cutAt[key] = id;
		return id;
	};
	const size_t F = m.f.size();
	for (size_t f = 0; f < F; f++) {
		Tri t = m.f[f];
		int sgn[3], pos = 0, neg = 0;
		for (int k = 0; k < 3; k++) {
			sgn[k] = d[t[k]] > 0 ? 1 : d[t[k]] < 0 ? -1 : 0;
			pos += sgn[k] > 0;
			neg += sgn[k] < 0;
		}
		if (!pos || !neg) continue;
		std::vector<Tri> pieces;
		int zero = -1, lone = -1;
		for (int k = 0; k < 3; k++)
			if (sgn[k] == 0) zero = k;
		if (zero >= 0) {
			int a = t[zero], b = t[(zero + 1) % 3], c = t[(zero + 2) % 3];
			int x = cutVertex(b, c);
			pieces = {{a, b, x}, {a, x, c}};
		} else {
			for (int k = 0; k < 3; k++)
				if (sgn[k] != sgn[(k + 1) % 3] && sgn[k] != sgn[(k + 2) % 3]) lone = k;
			int a = t[lone], b = t[(lone + 1) % 3], c = t[(lone + 2) % 3];
			int x = cutVertex(a, b), y = cutVertex(a, c);
			pieces = {{a, x, y}, {x, b, c}, {x, c, y}};
		}
		m.f[f] = pieces[0];
		for (size_t k = 1; k < pieces.size(); k++) {
			m.f.push_back(pieces[k]);
			if (uv) m.uv.push_back(m.uv[f]);
		}
	}

	std::vector<char> beyond(m.f.size());
	for (size_t f = 0; f < m.f.size(); f++) {
		const Tri& t = m.f[f];
		beyond[f] = std::max(d[t[0]], std::max(d[t[1]], d[t[2]])) > 0;
	}
	// Faces sharing an edge, by edge.
	std::vector<std::pair<uint64_t, int>> edges;
	edges.reserve(m.f.size() * 3);
	for (size_t f = 0; f < m.f.size(); f++)
		for (int k = 0; k < 3; k++) edges.push_back({edgeKey(m.f[f][k], m.f[f][(k + 1) % 3]), int(f)});
	std::sort(edges.begin(), edges.end());
	std::vector<std::array<int, 3>> slot(m.f.size());  // where each face's edges sit in edges
	{
		std::vector<int> filled(m.f.size(), 0);
		for (size_t i = 0; i < edges.size(); i++) slot[edges[i].second][filled[edges[i].second]++] = int(i);
	}

	// Flood the far side from the seeds.
	std::vector<char> reached(m.f.size(), 0);
	std::vector<int> stack;
	for (size_t f = 0; f < m.f.size(); f++)
		if (beyond[f] && seed(faceTag(m, int(f)))) {
			reached[f] = 1;
			stack.push_back(int(f));
		}
	if (stack.empty()) return false;
	while (!stack.empty()) {
		int f = stack.back();
		stack.pop_back();
		for (int s : slot[f]) {
			uint64_t key = edges[s].first;
			// Every face on this edge: the run of equal keys around s.
			size_t a = size_t(s), b = size_t(s);
			while (a > 0 && edges[a - 1].first == key) a--;
			while (b + 1 < edges.size() && edges[b + 1].first == key) b++;
			for (size_t i = a; i <= b; i++) {
				int g = edges[i].second;
				if (!reached[g] && beyond[g]) {
					reached[g] = 1;
					stack.push_back(g);
				}
			}
		}
	}
	for (size_t f = 0; f < m.f.size(); f++) {
		if (!reached[f]) continue;
		const Tri& t = m.f[f];
		if (forbidden(faceTag(m, int(f)), (m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) / 3.0)) return false;
	}

	Mesh near;
	off = Mesh();
	near.v = m.v;
	off.v = m.v;
	for (size_t f = 0; f < m.f.size(); f++) {
		Mesh& to = reached[f] ? off : near;
		to.f.push_back(m.f[f]);
		if (uv) to.uv.push_back(m.uv[f]);
	}
	compact(near);
	compact(off);
	const double eps = 1e-7 * scale;
	outlines = capPlanarHoles(off, point, normal, eps);
	capPlanarHoles(near, point, normal, eps);
	m0 = std::move(near);
	return true;
}

std::vector<Outline> slice(const Mesh& m, const V3& point, const V3& n0) {
	const V3 normal = normalized(n0);
	std::vector<double> d(m.v.size());
	for (size_t i = 0; i < m.v.size(); i++) {
		d[i] = dot(m.v[i] - point, normal);
		if (d[i] == 0) d[i] = 1e-12;  // on the plane: count it as above
	}
	// Each face the plane crosses gives a segment between two of its edges.
	std::unordered_map<uint64_t, std::vector<uint64_t>> links;
	std::unordered_map<uint64_t, V3> where;
	for (const Tri& t : m.f) {
		uint64_t ends[2];
		int n = 0;
		for (int k = 0; k < 3 && n < 2; k++) {
			int a = t[k], b = t[(k + 1) % 3];
			if ((d[a] > 0) == (d[b] > 0)) continue;
			uint64_t key = edgeKey(a, b);
			if (!where.count(key)) {
				int lo = std::min(a, b), hi = std::max(a, b);
				where[key] = m.v[lo] + (m.v[hi] - m.v[lo]) * (d[lo] / (d[lo] - d[hi]));
			}
			ends[n++] = key;
		}
		if (n == 2) {
			links[ends[0]].push_back(ends[1]);
			links[ends[1]].push_back(ends[0]);
		}
	}
	std::vector<Outline> out;
	std::unordered_map<uint64_t, char> used;
	for (auto& entry : links) {
		if (used[entry.first]) continue;
		Outline loop;
		uint64_t prev = ~uint64_t(0), cur = entry.first;
		while (!used[cur]) {
			used[cur] = 1;
			loop.push_back(where[cur]);
			const auto& nb = links[cur];
			uint64_t next = ~uint64_t(0);
			for (uint64_t k : nb)
				if (k != prev && !used[k]) {
					next = k;
					break;
				}
			if (next == ~uint64_t(0)) break;
			prev = cur;
			cur = next;
		}
		if (loop.size() >= 3) out.push_back(std::move(loop));
	}
	return out;
}

bool fitPlane(const std::vector<V3>& pts, V3& centre, V3& normal) {
	if (pts.size() < 3) return false;
	centre = V3();
	for (const V3& p : pts) centre += p;
	centre = centre / double(pts.size());
	double a[3][3] = {};
	for (const V3& p : pts) {
		V3 q = p - centre;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) a[i][j] += q[i] * q[j];
	}
	// Jacobi rotations until the matrix is diagonal; v collects them.
	double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	for (int sweep = 0; sweep < 50; sweep++) {
		double off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
		if (off < 1e-15 * (std::fabs(a[0][0]) + std::fabs(a[1][1]) + std::fabs(a[2][2]) + 1e-300)) break;
		for (int p = 0; p < 2; p++)
			for (int q = p + 1; q < 3; q++) {
				if (std::fabs(a[p][q]) < 1e-300) continue;
				double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
				double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
				double c = 1 / std::sqrt(t * t + 1), s = t * c;
				for (int k = 0; k < 3; k++) {
					double akp = a[k][p], akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < 3; k++) {
					double apk = a[p][k], aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (int k = 0; k < 3; k++) {
					double vkp = v[k][p], vkq = v[k][q];
					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
	}
	int least = 0;
	for (int i = 1; i < 3; i++)
		if (a[i][i] < a[least][least]) least = i;
	normal = normalized(V3(v[0][least], v[1][least], v[2][least]));
	return true;
}

namespace {

double distanceToOutline(const P2& p, const std::vector<P2>& poly);
bool insidePolygon(const P2& p, const std::vector<P2>& poly);

}  // namespace

Field prismField(const Outline& outline, double z0, double z1, double voxel) {
	Field f;
	if (outline.size() < 3) return f;
	std::vector<P2> poly;
	double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
	for (const V3& p : outline) {
		poly.push_back({p.x, p.y});
		x0 = std::min(x0, p.x);
		y0 = std::min(y0, p.y);
		x1 = std::max(x1, p.x);
		y1 = std::max(y1, p.y);
	}
	const double margin = 4 * voxel;
	f.voxel = voxel;
	f.step = voxel / 32;
	f.lo = V3(x0 - margin, y0 - margin, z0 - margin);
	f.nx = int(std::ceil((x1 - x0 + 2 * margin) / voxel)) + 1;
	f.ny = int(std::ceil((y1 - y0 + 2 * margin) / voxel)) + 1;
	f.nz = int(std::ceil((z1 - z0 + 2 * margin) / voxel)) + 1;
	f.q.resize(size_t(f.nx) * f.ny * f.nz);
	for (int j = 0; j < f.ny; j++)
		for (int i = 0; i < f.nx; i++) {
			P2 p{f.lo.x + i * voxel, f.lo.y + j * voxel};
			double flat = distanceToOutline(p, poly) * (insidePolygon(p, poly) ? -1 : 1);
			for (int k = 0; k < f.nz; k++) {
				double z = f.lo.z + k * voxel;
				double up = std::max(z0 - z, z - z1);
				double dist = flat > 0 && up > 0 ? std::sqrt(flat * flat + up * up) : std::max(flat, up);
				f.q[(size_t(k) * f.ny + j) * f.nx + i] = int8_t(std::clamp(std::lround(dist / f.step), -127L, 127L));
			}
		}
	return f;
}

namespace {

double distanceToOutline(const P2& p, const std::vector<P2>& poly) {
	double best = 1e300;
	for (size_t i = 0; i < poly.size(); i++) {
		const P2 &a = poly[i], &b = poly[(i + 1) % poly.size()];
		double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
		double t = len2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
		double ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
		best = std::min(best, ex * ex + ey * ey);
	}
	return std::sqrt(best);
}

bool insidePolygon(const P2& p, const std::vector<P2>& poly) {
	bool in = false;
	for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
		const P2 &a = poly[i], &b = poly[j];
		if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
	}
	return in;
}

/** The point of poly farthest from its outline (within the half plane
 *  side * (p - split) . axis >= 0 when side is not 0), and how far that is. */
P2 widestPoint(const std::vector<P2>& poly, const P2& split, const P2& axis, int side, double& radius) {
	double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
	for (const P2& p : poly) {
		x0 = std::min(x0, p.x);
		y0 = std::min(y0, p.y);
		x1 = std::max(x1, p.x);
		y1 = std::max(y1, p.y);
	}
	auto score = [&](const P2& p) {
		if (!insidePolygon(p, poly)) return -1.0;
		double r = distanceToOutline(p, poly);
		if (side) {
			double h = ((p.x - split.x) * axis.x + (p.y - split.y) * axis.y) * side;
			if (h < 0) return -1.0;
			r = std::min(r, h);  // stay clear of the other pin's half
		}
		return r;
	};
	P2 best{(x0 + x1) / 2, (y0 + y1) / 2};
	double bestR = score(best);
	double cx = best.x, cy = best.y, hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
	for (int round = 0; round < 4; round++) {
		const int n = 16;
		for (int i = 0; i <= n; i++)
			for (int j = 0; j <= n; j++) {
				P2 p{cx - hx + 2 * hx * i / n, cy - hy + 2 * hy * j / n};
				double r = score(p);
				if (r > bestR) {
					bestR = r;
					best = p;
				}
			}
		cx = best.x;
		cy = best.y;
		hx /= 4;
		hy /= 4;
	}
	radius = std::max(bestR, 0.0);
	return best;
}

}  // namespace

std::vector<PinSpot> pinSpots(const Outline& outline, const V3& n0, double minDiameter, double maxDiameter,
                              bool preferTwo, double clearance) {
	std::vector<PinSpot> out;
	if (outline.size() < 3) return out;
	const V3 normal = normalized(n0);
	V3 e1, e2;
	planeBasis(normal, e1, e2);
	V3 origin = outline[0];
	std::vector<P2> poly;
	for (const V3& p : outline) poly.push_back({dot(p - origin, e1), dot(p - origin, e2)});
	auto toModel = [&](const P2& p) { return origin + e1 * p.x + e2 * p.y; };
	// A pin as thick as leaves a wall around it about a third of the room.
	auto diameterFor = [&](double room) {
		double wall = std::clamp(0.35 * room, 0.8, 2.0);
		return std::clamp(2 * (room - wall - clearance), minDiameter, maxDiameter);
	};

	// The outline's long axis, from the spread of its points.
	P2 c{0, 0};
	for (const P2& p : poly) {
		c.x += p.x;
		c.y += p.y;
	}
	c.x /= double(poly.size());
	c.y /= double(poly.size());
	double sxx = 0, sxy = 0, syy = 0;
	for (const P2& p : poly) {
		sxx += (p.x - c.x) * (p.x - c.x);
		sxy += (p.x - c.x) * (p.y - c.y);
		syy += (p.y - c.y) * (p.y - c.y);
	}
	double ang = 0.5 * std::atan2(2 * sxy, sxx - syy);
	P2 major{std::cos(ang), std::sin(ang)}, minor{-major.y, major.x};
	double a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300;
	for (const P2& p : poly) {
		double a = (p.x - c.x) * major.x + (p.y - c.y) * major.y, b = (p.x - c.x) * minor.x + (p.y - c.y) * minor.y;
		a0 = std::min(a0, a);
		a1 = std::max(a1, a);
		b0 = std::min(b0, b);
		b1 = std::max(b1, b);
	}
	double radius = 0;
	P2 one = widestPoint(poly, c, major, 0, radius);
	if (preferTwo || (a1 - a0) > 1.7 * (b1 - b0)) {
		// Long: a pin in each half keeps the piece from turning.
		double ra = 0, rb = 0;
		P2 pa = widestPoint(poly, c, major, 1, ra), pb = widestPoint(poly, c, major, -1, rb);
		double room = std::min(ra, rb);
		bool fits = 2 * (room - std::clamp(0.35 * room, 0.8, 2.0) - clearance) >= minDiameter;
		if (fits) {
			out.push_back({toModel(pa), diameterFor(room)});
			out.push_back({toModel(pb), diameterFor(room)});
			return out;
		}
	}
	out.push_back({toModel(one), diameterFor(radius)});
	return out;
}

Mesh makePin(double diameter, double length, const PinFit& fit) {
	Mesh m;
	const double r = diameter / 2;
	const double flat = 0.84 * r;  // flats top and bottom, one to stand on
	const double chamfer = std::min(0.4, 0.2 * diameter);
	const double ramp = std::max(0.0, std::min(1.0, (length - 2 * chamfer) / 4));
	const int N = 96;  // a multiple of 8, so the ribs at 45 degrees get a sample
	const double step = 2 * M_PI / N;
	const double ribWidth = std::max(0.35 / r, 2.2 * step);  // half width, radians
	auto ribAt = [&](double th) {
		double best = 0;
		for (int k = 0; k < 4; k++) {
			double c = M_PI / 4 + k * M_PI / 2;
			double dth = std::fabs(std::remainder(th - c, 2 * M_PI));
			best = std::max(best, 1 - dth / ribWidth);
		}
		return best;
	};
	const double xs[6] = {0, chamfer, chamfer + ramp, length - chamfer - ramp, length - chamfer, length};
	const double shrink[6] = {chamfer, 0, 0, 0, 0, chamfer};
	const double ribs[6] = {0, 0, 1, 1, 0, 0};
	for (int ring = 0; ring < 6; ring++)
		for (int j = 0; j < N; j++) {
			double th = j * step;
			double rad = r - shrink[ring] + fit.rib * ribs[ring] * ribAt(th);
			double y = rad * std::cos(th), z = std::clamp(rad * std::sin(th), -flat, flat);
			m.v.push_back({xs[ring], y, z + flat});
		}
	for (int ring = 0; ring < 5; ring++)
		for (int j = 0; j < N; j++) {
			int a = ring * N + j, b = ring * N + (j + 1) % N, c = b + N, d = a + N;
			m.f.push_back({a, b, c});
			m.f.push_back({a, c, d});
		}
	int c0 = (int)m.v.size(), c1 = c0 + 1;
	m.v.push_back({0, 0, flat});
	m.v.push_back({length, 0, flat});
	for (int j = 0; j < N; j++) {
		int a = j, b = (j + 1) % N;
		m.f.push_back({c0, b, a});
		m.f.push_back({c1, 5 * N + a, 5 * N + b});
	}
	return m;
}

Mesh layOut(std::vector<Mesh>& pieces, double gap, double width) {
	std::vector<size_t> order(pieces.size());
	std::iota(order.begin(), order.end(), 0);
	std::vector<V3> lo(pieces.size()), hi(pieces.size());
	for (size_t i = 0; i < pieces.size(); i++) pieces[i].bounds(lo[i], hi[i]);
	// Deepest first, so each row is about as deep as its pieces.
	std::stable_sort(order.begin(), order.end(),
	                 [&](size_t a, size_t b) { return hi[a].y - lo[a].y > hi[b].y - lo[b].y; });
	Mesh out;
	double x = 0, y = 0, rowDepth = 0;
	for (size_t i : order) {
		if (pieces[i].v.empty()) continue;
		double w = hi[i].x - lo[i].x, dpt = hi[i].y - lo[i].y;
		if (x > 0 && x + w > width) {
			x = 0;
			y += rowDepth + gap;
			rowDepth = 0;
		}
		V3 shift(x - lo[i].x, y - lo[i].y, -lo[i].z);
		for (V3& p : pieces[i].v) p += shift;
		pieces[i].uv.clear();
		out.append(pieces[i]);
		x += w + gap;
		rowDepth = std::max(rowDepth, dpt);
	}
	// Centred on the origin, as slicers expect.
	V3 a, b;
	out.bounds(a, b);
	V3 mid((a.x + b.x) / 2, (a.y + b.y) / 2, 0);
	for (V3& p : out.v) p -= mid;
	return out;
}

}  // namespace pmo
