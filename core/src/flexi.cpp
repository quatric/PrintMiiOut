#include "flexi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

#include "face2d.hpp"
#include "fuse.hpp"
#include "raster.hpp"
#include "sdf.hpp"
#include "split.hpp"

namespace pmo {

namespace {

using LogFn = std::function<void(const std::string&)>;

const double kPi = 3.14159265358979323846;

// What each pixel of the flat figure belongs to.
enum Label : uint8_t { kNothing = 0, kTorso = 1, kArmL = 2, kArmR = 3, kLegL = 4, kLegR = 5 };

/** Calls fn(i, j, w0, w1, w2) for every pixel centre inside the triangle a, b, c. */
template <typename T, typename Fn>
void scan(const Grid<T>& g, const P2& a, const P2& b, const P2& c, Fn fn) {
	double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
	if (std::fabs(area) < 1e-14) return;
	double lx = std::min(a.x, std::min(b.x, c.x)), hx = std::max(a.x, std::max(b.x, c.x));
	double ly = std::min(a.y, std::min(b.y, c.y)), hy = std::max(a.y, std::max(b.y, c.y));
	int i0 = std::max(0, int(std::ceil((lx - g.x0) / g.px - 0.5)));
	int i1 = std::min(g.w - 1, int(std::floor((hx - g.x0) / g.px - 0.5)));
	int j0 = std::max(0, int(std::ceil((ly - g.y0) / g.px - 0.5)));
	int j1 = std::min(g.h - 1, int(std::floor((hy - g.y0) / g.px - 0.5)));
	for (int j = j0; j <= j1; j++) {
		double y = g.cy(j);
		for (int i = i0; i <= i1; i++) {
			double x = g.cx(i);
			double w0 = ((b.x - x) * (c.y - y) - (c.x - x) * (b.y - y)) / area;
			double w1 = ((c.x - x) * (a.y - y) - (a.x - x) * (c.y - y)) / area;
			double w2 = 1 - w0 - w1;
			if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
			fn(i, j, w0, w1, w2);
		}
	}
}

bool on(const Mask& m, const P2& p) {
	int i, j;
	return m.cell(p.x, p.y, i, j) && m.at(i, j);
}

double norm(const P2& p) { return std::hypot(p.x, p.y); }

/** Keeps only the 4-connected part of m holding the seed pixel, or the
 *  largest part when the seed is off the grid or not set. */
void keepComponent(Mask& m, int si, int sj) {
	std::vector<int> comp(m.a.size(), -1);
	std::vector<size_t> sizes;
	std::vector<int> stack;
	for (size_t k = 0; k < m.a.size(); k++) {
		if (!m.a[k] || comp[k] >= 0) continue;
		const int id = int(sizes.size());
		size_t n = 0;
		stack.push_back(int(k));
		comp[k] = id;
		while (!stack.empty()) {
			const int q = stack.back();
			stack.pop_back();
			n++;
			const int i = q % m.w, j = q / m.w;
			const int nb[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
			for (const auto& e : nb) {
				if (e[0] < 0 || e[1] < 0 || e[0] >= m.w || e[1] >= m.h) continue;
				const size_t r = size_t(e[1]) * m.w + e[0];
				if (m.a[r] && comp[r] < 0) {
					comp[r] = id;
					stack.push_back(int(r));
				}
			}
		}
		sizes.push_back(n);
	}
	if (sizes.empty()) return;
	int keep = -1;
	if (si >= 0 && sj >= 0 && si < m.w && sj < m.h) keep = comp[size_t(sj) * m.w + si];
	if (keep < 0) keep = int(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
	for (size_t k = 0; k < m.a.size(); k++) m.a[k] = comp[k] == keep;
}

/** A limb's joint: a knob of radius r on the limb, in a ring on the torso
 *  (r + clearance out to rOut), joined to the limb by a neck w wide and L
 *  long that swings theta either way in a slot cut through the ring. */
struct Joint {
	const char* name = "";
	std::vector<uint8_t> labels;  // what the limb is made of
	P2 at, dir;
	double r = 3, w = 2, rOut = 5, theta = 0.3, L = 6;
	Mask piece;
};

double tri(double z, double T) { return std::clamp(1 - std::fabs(z - T / 2) / (T / 2), 0.0, 1.0); }

}  // namespace

double flexiHeight(const FlexiInput& in) {
	double z0 = 1e300, z1 = in.face.z1;
	for (const Mesh& m : in.body)
		for (const V3& p : m.v) {
			z0 = std::min(z0, p.z);
			z1 = std::max(z1, p.z);
		}
	return z1 - z0;
}

Mesh makeFlexi(const FlexiInput& in, const FlexiOptions& o, const LogFn& log,
               const std::function<void(const char*)>& tick) {
	char buf[256];
	const double H = std::clamp(o.size > 0 ? o.size : 100.0, 70.0, 200.0);

	// The figure seen from the front, in millimetres: x across, y up from the feet.
	double xLo = in.face.x0, xHi = in.face.x1, zLo = 1e300, zHi = in.face.z1;
	for (const Mesh& m : in.body)
		for (const V3& p : m.v) {
			xLo = std::min(xLo, p.x);
			xHi = std::max(xHi, p.x);
			zLo = std::min(zLo, p.z);
			zHi = std::max(zHi, p.z);
		}
	if (!(zHi > zLo)) throw std::runtime_error("articulated: no body to draw");
	const double s = H / (zHi - zLo), xc = (xLo + xHi) / 2;
	auto toMm = [&](double x, double z) { return P2((x - xc) * s, (z - zLo) * s); };
	const double px = std::clamp(H / 2000, 0.04, 0.08), margin = 10;
	const double halfW = (xHi - xLo) / 2 * s + margin;

	Mask lab;
	lab.cover(-halfW, -margin, halfW, H + margin, px, kNothing);
	std::vector<float> depth(lab.a.size(), std::numeric_limits<float>::max());
	const P2 hipL = toMm(in.hipL.x, in.hipL.z), hipR = toMm(in.hipR.x, in.hipR.z);
	for (size_t p = 0; p < in.body.size(); p++) {
		const Mesh& m = in.body[p];
		for (size_t f = 0; f < m.f.size(); f++) {
			const V3 &a = m.v[m.f[f][0]], &b = m.v[m.f[f][1]], &c = m.v[m.f[f][2]];
			const int tag = p < in.tags.size() && f < in.tags[p].size() ? in.tags[p][f] : int(kTagTorso);
			uint8_t label = kTorso;
			if (tag == kTagArmL) label = kArmL;
			if (tag == kTagArmR) label = kArmR;
			if (tag == kTagLegs) {
				const double cx = ((a.x + b.x + c.x) / 3 - xc) * s;
				label = std::fabs(cx - hipL.x) < std::fabs(cx - hipR.x) ? kLegL : kLegR;
			}
			scan(lab, toMm(a.x, a.z), toMm(b.x, b.z), toMm(c.x, c.z), [&](int i, int j, double w0, double w1, double w2) {
				const float y = float(a.y * w0 + b.y * w1 + c.y * w2);
				const size_t k = size_t(j) * lab.w + i;
				if (y < depth[k]) {
					depth[k] = y;
					lab.a[k] = label;
				}
			});
		}
	}
	depth = {};

	// The head on top, as the coins draw it: it belongs to the torso.
	const double line = 0.5, minWidth = 0.45;
	FaceArt art = in.face.draw(px / s, line / s, minWidth / s);
	Mask faceInk;
	faceInk.reset(lab.x0, lab.y0, px, lab.w, lab.h, 0);
	for (int j = 0; j < lab.h; j++)
		for (int i = 0; i < lab.w; i++) {
			int ai, aj;
			if (!art.head.cell(lab.cx(i) / s + xc, lab.cy(j) / s + zLo, ai, aj)) continue;
			if (art.head.at(ai, aj)) lab.at(i, j) = kTorso;
			if (art.ink.at(ai, aj)) faceInk.at(i, j) = 1;
		}
	art = FaceArt();
	auto maskOf = [&](std::initializer_list<uint8_t> labels) {
		Mask m;
		m.reset(lab.x0, lab.y0, px, lab.w, lab.h, 0);
		for (size_t k = 0; k < m.a.size(); k++)
			for (uint8_t l : labels)
				if (lab.a[k] == l) m.a[k] = 1;
		return m;
	};
	Mask torsoMask = maskOf({kTorso});
	tidy(torsoMask, 4.0);
	tick("draw figure");

	// Sizes, all in millimetres. The knob's edge bulges by bulge at mid-height
	// into a groove in its ring, so it cannot fall out of the plate. The ring is
	// open at the neck and spreads easily, so the bulge reaches well past the
	// clearance (about 40 degrees off vertical, still printable).
	const double clr = std::clamp(0.35 + o.fit, 0.15, 0.8);
	const double T = std::clamp(0.032 * H, 3.0, 5.0), bulge = 0.4 * T, inkH = 0.6;
	const double rBase = std::clamp(0.032 * H, 2.6, 5.5), rMin = 1.8;
	auto sizeJoint = [&](Joint& j, double r) {
		j.r = r;
		j.w = std::min(std::max(2.0, 0.55 * r), 1.2 * r);
		j.rOut = r + clr + std::max(1.4, 0.3 * r) + bulge;
		// The slot's mouth stays narrower than the knob: (r + clr) sin(theta) + w/2 + clr < 0.9 r.
		double sn = (0.9 * r - (j.w / 2 + clr)) / (r + clr);
		j.theta = std::asin(std::clamp(sn, 0.1, 0.6));
	};

	// Where each joint goes: on the limb's bone, moved along the limb so
	// the ring sits just inside the torso's edge.
	auto centroid = [&](const Mask& m, P2& c) {
		double sx = 0, sy = 0;
		size_t n = 0;
		for (int j = 0; j < m.h; j++)
			for (int i = 0; i < m.w; i++)
				if (m.at(i, j)) {
					sx += m.cx(i);
					sy += m.cy(j);
					n++;
				}
		if (!n) return false;
		c = P2(sx / n, sy / n);
		return true;
	};
	auto place = [&](Joint& j, const P2& bone) {
		Mask limb;
		limb.reset(lab.x0, lab.y0, px, lab.w, lab.h, 0);
		for (size_t k = 0; k < lab.a.size(); k++)
			for (uint8_t l : j.labels)
				if (lab.a[k] == l) limb.a[k] = 1;
		P2 c;
		if (!centroid(limb, c)) return false;
		P2 d = c - bone;
		double n = norm(d);
		j.dir = n > 1e-9 ? d * (1 / n) : P2(0, -1);
		// Back into the torso if the bone is outside it, then out to its edge.
		P2 at = bone;
		for (double t = 0; t < 30 && !on(torsoMask, at); t += px) at = bone - j.dir * t;
		double e = 0;
		while (e < 60 && on(torsoMask, at + j.dir * e)) e += px;
		j.at = at + j.dir * (e - 0.8 * j.rOut);
		return true;
	};

	std::vector<Joint> joints;
	Mask torso;
	// A small torso may have no room for the usual knobs: shrink them until
	// every ring stays on it.
	for (double r0 = rBase;; r0 *= 0.85) {
		joints.clear();
		{
			Joint a;
			a.name = "left arm";
			a.labels = {kArmL};
			sizeJoint(a, r0);
			if (place(a, toMm(in.shoulderL.x, in.shoulderL.z))) joints.push_back(a);
			a.name = "right arm";
			a.labels = {kArmR};
			if (place(a, toMm(in.shoulderR.x, in.shoulderR.z))) joints.push_back(a);
		}
		{
			// The hips sit close together: shrink both knobs to fit side by side,
			// or, if that leaves them too small, move both legs on one joint.
			Joint l, r;
			l.name = "left leg";
			l.labels = {kLegL};
			r.name = "right leg";
			r.labels = {kLegR};
			sizeJoint(l, r0);
			sizeJoint(r, r0);
			bool hasL = place(l, hipL), hasR = place(r, hipR);
			double rFit = 0;
			if (hasL && hasR) {
				const double sep = norm(l.at - r.at);
				rFit = std::min(r0, (sep - 0.8) / 2 - clr - bulge);
			}
			if (hasL && hasR && rFit >= 2.2) {
				sizeJoint(l, rFit);
				sizeJoint(r, rFit);
				place(l, hipL);
				place(r, hipR);
				joints.push_back(l);
				joints.push_back(r);
			} else if (hasL || hasR) {
				Joint both;
				both.name = "legs";
				both.labels = {kLegL, kLegR};
				sizeJoint(both, r0);
				if (place(both, (hipL + hipR) * 0.5)) joints.push_back(both);
			}
		}
		if (joints.empty()) throw std::runtime_error("articulated: no arms or legs to joint");

		// Each limb: the knob, its neck, and the limb from beyond the ring on.
		Mask taken;  // every limb so far, grown by the clearance
		taken.reset(lab.x0, lab.y0, px, lab.w, lab.h, 0);
		for (Joint& j : joints) {
			Mask keep;
			keep.reset(lab.x0, lab.y0, px, lab.w, lab.h, 0);
			const double away = j.rOut + clr;
			for (int y = 0; y < lab.h; y++)
				for (int x = 0; x < lab.w; x++) {
					const uint8_t l = lab.at(x, y);
					if (std::find(j.labels.begin(), j.labels.end(), l) == j.labels.end()) continue;
					if (norm(P2(lab.cx(x), lab.cy(y)) - j.at) > away) keep.at(x, y) = 1;
				}
			j.L = away + 1.0;
			for (double t = away; t < away + 30; t += px)
				if (on(keep, j.at + j.dir * t)) {
					j.L = t + 1.0;
					break;
				}
			j.piece = keep;
			for (int y = 0; y < lab.h; y++)
				for (int x = 0; x < lab.w; x++) {
					const P2 q = P2(lab.cx(x), lab.cy(y)) - j.at;
					const double along = q.x * j.dir.x + q.y * j.dir.y;
					const double across = std::fabs(q.x * j.dir.y - q.y * j.dir.x);
					if (norm(q) <= j.r || (along >= 0 && along <= j.L && across <= j.w / 2)) j.piece.at(x, y) = 1;
					if (taken.at(x, y)) j.piece.at(x, y) = 0;
				}
			int si, sj;
			j.piece.cell(j.at.x, j.at.y, si, sj);
			keepComponent(j.piece, si, sj);
			Mask grown = j.piece;
			offset(grown, clr);
			unite(taken, grown);
			std::snprintf(buf, sizeof buf, "Joint (%s): knob %.1f mm across, neck %.1f wide, swings %.0f deg each way",
			              j.name, 2 * j.r, j.w, j.theta * 180 / kPi);
			log(buf);
		}

		// The torso: the rings added, the knobs' holes and the room each limb
		// needs to swing cut out, clear of every limb.
		torso = torsoMask;
		for (int y = 0; y < lab.h; y++)
			for (int x = 0; x < lab.w; x++) {
				const P2 p(lab.cx(x), lab.cy(y));
				for (const Joint& j : joints) {
					const double rho = norm(p - j.at);
					if (rho <= j.rOut) torso.at(x, y) = 1;
				}
				for (const Joint& j : joints)
					if (norm(p - j.at) <= j.r + clr) torso.at(x, y) = 0;
			}
		for (const Joint& j : joints) {
			// The top of the limb, turned theta either way about the knob.
			const double reach = j.L + 3, zone = reach + clr + 2 * px;
			const int steps = std::max(4, int(std::ceil(j.theta / (2.0 * kPi / 180))));
			Mask sweep;
			sweep.reset(lab.x0, lab.y0, px, lab.w, lab.h, 0);
			int i0, j0, i1, j1;
			lab.cell(j.at.x - zone, j.at.y - zone, i0, j0);
			lab.cell(j.at.x + zone, j.at.y + zone, i1, j1);
			i0 = std::max(i0, 0), j0 = std::max(j0, 0), i1 = std::min(i1, lab.w - 1), j1 = std::min(j1, lab.h - 1);
			for (int y = j0; y <= j1; y++)
				for (int x = i0; x <= i1; x++) {
					const P2 q = P2(lab.cx(x), lab.cy(y)) - j.at;
					if (norm(q) > reach) continue;
					for (int k = -steps; k <= steps && !sweep.at(x, y); k++) {
						const double a = -j.theta * k / steps, c = std::cos(a), sn = std::sin(a);
						const P2 back = j.at + P2(c * q.x - sn * q.y, sn * q.x + c * q.y);
						if (on(j.piece, back)) sweep.at(x, y) = 1;
					}
				}
			offset(sweep, clr);
			for (size_t k = 0; k < torso.a.size(); k++)
				if (sweep.a[k]) torso.a[k] = 0;
		}
		for (size_t k = 0; k < torso.a.size(); k++)
			if (taken.a[k]) torso.a[k] = 0;
		{
			int si, sj;
			P2 headTop = toMm((in.face.x0 + in.face.x1) / 2, (in.face.z0 + in.face.z1) / 2);
			torso.cell(headTop.x, headTop.y, si, sj);
			keepComponent(torso, si, sj);
		}
		// Every ring has to stay on the head's piece, with its wall standing
		// behind the bulge: with a small torso the swing cut-outs can sever
		// it or shave it to a knife edge, and the limb would print loose.
		bool held = true;
		for (const Joint& j : joints) {
			size_t want = 0, have = 0;
			int i0, j0, i1, j1;
			lab.cell(j.at.x - j.rOut, j.at.y - j.rOut, i0, j0);
			lab.cell(j.at.x + j.rOut, j.at.y + j.rOut, i1, j1);
			for (int y = std::max(j0, 0); y <= std::min(j1, lab.h - 1); y++)
				for (int x = std::max(i0, 0); x <= std::min(i1, lab.w - 1); x++) {
					const double rho = norm(P2(lab.cx(x), lab.cy(y)) - j.at);
					if (rho < j.r + clr + bulge + px || rho > j.rOut) continue;
					want++;
					if (torso.at(x, y)) have++;
				}
			if (10 * have < 7 * want) held = false;
		}
		if (held) break;
		if (r0 * 0.85 < rMin) throw std::runtime_error("articulated: the torso is too small to hold the joints; try a bigger size");
		log("Joints too big for the torso: trying smaller knobs");
	}
	tick("cut joints");

	// What stands proud on top: the face's lines, and a line round each
	// piece's edge (but not round the joints, which have to turn).
	const double band = 0.6;
	struct Piece {
		Mask mask;
		bool isTorso = false;
		const Joint* joint = nullptr;
	};
	std::vector<Piece> pieces;
	pieces.push_back({torso, true, nullptr});
	for (const Joint& j : joints) pieces.push_back({j.piece, false, &j});

	const double voxel = std::clamp(H / 1000, 0.08, 0.15);
	const size_t budget = o.maxTriangles / pieces.size();
	Mesh all;
	for (Piece& pc : pieces) {
		Distance2 dP = distanceTo(pc.mask);
		Mask ink;
		ink.reset(lab.x0, lab.y0, px, lab.w, lab.h, 0);
		bool anyInk = false;
		for (int y = 0; y < lab.h; y++)
			for (int x = 0; x < lab.w; x++) {
				if (!pc.mask.at(x, y)) continue;
				const P2 p(lab.cx(x), lab.cy(y));
				bool nearJoint = false;
				for (const Joint& j : joints)
					if (norm(p - j.at) < j.rOut + clr + 1.5) nearJoint = true;
				const double d = dP(p.x, p.y);
				bool edge = !nearJoint && d > -band;
				bool face = pc.isTorso && faceInk.at(x, y) && d < -0.2;
				if (edge || face) {
					ink.at(x, y) = 1;
					anyInk = true;
				}
			}
		tidy(ink, 0.15 * minWidth * minWidth);
		Distance2 dI;
		if (anyInk) dI = distanceTo(ink);

		double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
		for (int y = 0; y < lab.h; y++)
			for (int x = 0; x < lab.w; x++)
				if (pc.mask.at(x, y)) {
					x0 = std::min(x0, lab.cx(x));
					x1 = std::max(x1, lab.cx(x));
					y0 = std::min(y0, lab.cy(y));
					y1 = std::max(y1, lab.cy(y));
				}
		if (!(x1 > x0)) continue;
		const bool torsoPiece = pc.isTorso;
		const Joint* jt = pc.joint;
		Sdf f = [&](const V3& p) {
			// A little in at the bottom, against elephant's foot closing the joints.
			double body = extrude(dP(p.x, p.y) + 0.75 * std::max(0.0, 0.4 - p.z), p.z, 0, T);
			if (torsoPiece) {
				for (const Joint& j : joints) {
					const double rho = norm(P2(p.x, p.y) - j.at);
					body = std::max(body, -(rho - (j.r + clr + bulge * tri(p.z, T))) / 1.1);
				}
			} else if (jt) {
				const double rho = norm(P2(p.x, p.y) - jt->at);
				double knob = std::max((rho - (jt->r + bulge * tri(p.z, T)) + 0.75 * std::max(0.0, 0.4 - p.z)) / 1.1,
				                       std::max(-p.z, p.z - T));
				body = std::min(body, knob);
			}
			if (anyInk) {
				double lines = extrude(std::max(dI(p.x, p.y) - 0.2 * std::clamp(T + inkH - p.z, 0.0, inkH),
				                                dP(p.x, p.y) + 0.15),
				                       p.z, T - 0.3, T + inkH);
				body = std::min(body, lines);
			}
			return body;
		};
		Mesh m = meshSdf(f, V3(x0 - 1.5, y0 - 1.5, 0), V3(x1 + 1.5, y1 + 1.5, T + inkH), voxel, log, 1.4);
		simplify(m, budget, 0.2 * voxel, nullptr, true);
		all.append(m);
		tick(pc.isTorso ? "carve torso" : "carve limb");
	}
	std::snprintf(buf, sizeof buf, "Articulated: %.0f mm tall, %.1f mm thick, %zu pieces printed in place, %.2f mm round each joint",
	              H, T, pieces.size(), clr);
	log(buf);
	return all;
}

}  // namespace pmo
