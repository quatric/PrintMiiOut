#include "face2d.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pmo {

namespace {

// What each opaque part is, seen flat: paper with an outline, or filled.
enum Group : uint8_t { kNone = 0, kSkin = 1, kHair = 2, kCap = 3, kBeard = 4 };

bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

Group groupOf(const std::string& name) {
	if (has(name, "Hair")) return kHair;
	if (has(name, "Beard")) return kBeard;
	if (has(name, "Cap")) return kCap;
	return kSkin;  // faceline, forehead, nose
}

bool isOverlay(const std::string& name) { return has(name, "Mask") || has(name, "NoseLine") || has(name, "Glass"); }

/** Whether a texel paints ink: anything drawn that is not white or nearly
 *  so (eye whites and teeth stay paper; irises, lips, brows, lines and
 *  frames of any colour print). */
bool inks(const RGBA& c) {
	if (c.a < 0.5f) return false;
	double lum = 0.3 * c.r + 0.59 * c.g + 0.11 * c.b;
	return lum < 0.78;
}

/** Calls fn(i, j, w0, w1, w2) for every pixel centre inside the triangle a, b, c (x, z). */
template <typename Fn>
void scan(const Mask& g, const V3& a, const V3& b, const V3& c, Fn fn) {
	double area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
	if (std::fabs(area) < 1e-14) return;
	double lx = std::min(a.x, std::min(b.x, c.x)), hx = std::max(a.x, std::max(b.x, c.x));
	double lz = std::min(a.z, std::min(b.z, c.z)), hz = std::max(a.z, std::max(b.z, c.z));
	int i0 = std::max(0, int(std::ceil((lx - g.x0) / g.px - 0.5)));
	int i1 = std::min(g.w - 1, int(std::floor((hx - g.x0) / g.px - 0.5)));
	int j0 = std::max(0, int(std::ceil((lz - g.y0) / g.px - 0.5)));
	int j1 = std::min(g.h - 1, int(std::floor((hz - g.y0) / g.px - 0.5)));
	for (int j = j0; j <= j1; j++) {
		double z = g.cy(j);
		for (int i = i0; i <= i1; i++) {
			double x = g.cx(i);
			double w0 = ((b.x - x) * (c.z - z) - (c.x - x) * (b.z - z)) / area;
			double w1 = ((c.x - x) * (a.z - z) - (a.x - x) * (c.z - z)) / area;
			double w2 = 1 - w0 - w1;
			if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
			fn(i, j, w0, w1, w2);
		}
	}
}

}  // namespace

void faceBounds(const std::vector<FacePart>& parts, double& x0, double& z0, double& x1, double& z1) {
	x0 = z0 = 1e300;
	x1 = z1 = -1e300;
	for (const FacePart& p : parts) {
		if (!p.mesh) continue;
		// The glasses and the nose line draw only where their texture does,
		// but the mask and glasses quads never reach far past the face.
		for (const V3& v : p.mesh->v) {
			x0 = std::min(x0, v.x);
			x1 = std::max(x1, v.x);
			z0 = std::min(z0, v.z);
			z1 = std::max(z1, v.z);
		}
	}
}

FaceArt drawFace(const std::vector<FacePart>& parts, double px, double line, double minWidth) {
	FaceArt art;
	double x0, z0, x1, z1;
	faceBounds(parts, x0, z0, x1, z1);
	if (x0 > x1) return art;
	const double pad = line + minWidth + 4 * px;
	Mask group;
	group.cover(x0 - pad, z0 - pad, x1 + pad, z1 + pad, px, kNone);
	const int W = group.w, H = group.h;
	const float kFar = std::numeric_limits<float>::max();
	std::vector<float> depth(size_t(W) * H, kFar);  // y: the viewer is at -y
	// Which part shows at each pixel, for a hat's own pattern.
	std::vector<int16_t> shown(size_t(W) * H, -1);

	// The opaque parts, nearest wins.
	for (size_t pi = 0; pi < parts.size(); pi++) {
		const FacePart& p = parts[pi];
		if (!p.mesh || isOverlay(p.name)) continue;
		const Group gid = groupOf(p.name);
		const Mesh& m = *p.mesh;
		for (const Tri& t : m.f) {
			const V3 &a = m.v[t[0]], &b = m.v[t[1]], &c = m.v[t[2]];
			scan(group, a, b, c, [&](int i, int j, double w0, double w1, double w2) {
				float y = float(a.y * w0 + b.y * w1 + c.y * w2);
				size_t k = size_t(j) * W + i;
				if (y < depth[k]) {
					depth[k] = y;
					group.a[k] = gid;
					shown[k] = int16_t(pi);
				}
			});
		}
	}

	// The textures drawn over the face, where nothing nearer hides them
	// (a fringe over the eyebrows, say).
	const double tolerance = 0.02 * (x1 - x0);
	Mask features;
	features.reset(group.x0, group.y0, px, W, H, 0);
	for (const FacePart& p : parts) {
		if (!p.mesh || !isOverlay(p.name) || !p.texture || p.texture->empty() || !p.mesh->hasUV()) continue;
		const Mesh& m = *p.mesh;
		const Image& tex = *p.texture;
		for (size_t f = 0; f < m.f.size(); f++) {
			const V3 &a = m.v[m.f[f][0]], &b = m.v[m.f[f][1]], &c = m.v[m.f[f][2]];
			const V2 &ta = m.uv[f][0], &tb = m.uv[f][1], &tc = m.uv[f][2];
			scan(group, a, b, c, [&](int i, int j, double w0, double w1, double w2) {
				size_t k = size_t(j) * W + i;
				double y = a.y * w0 + b.y * w1 + c.y * w2;
				if (depth[k] != kFar && y > depth[k] + tolerance) return;
				double u = ta.u * w0 + tb.u * w1 + tc.u * w2, v = ta.v * w0 + tb.v * w1 + tc.v * w2;
				if (inks(tex.nearest(u, v))) features.a[k] = 1;
			});
		}
	}
	// A hat's pattern (a beanie's stripes and stitching), drawn on it like
	// the face's textures where the hat shows. Looked up on the hat as the
	// GLB has it: smoothing moves the surface but not its texture, and
	// would bend the stripes.
	for (size_t pi = 0; pi < parts.size(); pi++) {
		const FacePart& p = parts[pi];
		const Mesh* m = p.pattern ? p.pattern : p.mesh;
		if (!m || groupOf(p.name) != kCap || isOverlay(p.name) || !p.texture || p.texture->empty() || !m->hasUV())
			continue;
		std::vector<float> near(size_t(W) * H, kFar);
		std::vector<V2> uv(size_t(W) * H);
		for (size_t f = 0; f < m->f.size(); f++) {
			const V3 &a = m->v[m->f[f][0]], &b = m->v[m->f[f][1]], &c = m->v[m->f[f][2]];
			const V2 &ta = m->uv[f][0], &tb = m->uv[f][1], &tc = m->uv[f][2];
			scan(group, a, b, c, [&](int i, int j, double w0, double w1, double w2) {
				size_t k = size_t(j) * W + i;
				if (shown[k] != int16_t(pi)) return;
				float y = float(a.y * w0 + b.y * w1 + c.y * w2);
				if (y >= near[k]) return;
				near[k] = y;
				uv[k] = {float(ta.u * w0 + tb.u * w1 + tc.u * w2), float(ta.v * w0 + tb.v * w1 + tc.v * w2)};
			});
		}
		for (size_t k = 0; k < near.size(); k++)
			if (near[k] != kFar && inks(p.texture->bilinear(uv[k].u, uv[k].v))) features.a[k] = 1;
	}
	// Texture lines can be finer than a printer prints: thicken just the
	// thin ones (what an opening of minWidth would lose), not the shapes.
	if (minWidth > 0) {
		Mask open = features;
		offset(open, -minWidth / 2);
		offset(open, minWidth / 2);
		Mask thin = features;
		for (size_t k = 0; k < thin.a.size(); k++) thin.a[k] = features.a[k] && !open.a[k];
		// Real strokes are long; the slivers an opening shaves off the edge
		// of a blob are not, and would come back as nubs.
		tidy(thin, 1.5 * minWidth * minWidth);
		offset(thin, 0.3 * minWidth);
		unite(features, thin);
	}

	// Outlines on every border between parts that look different flat,
	// centred on it; hair and beard are filled.
	Mask edge;
	edge.reset(group.x0, group.y0, px, W, H, 0);
	for (int j = 0; j < H; j++)
		for (int i = 0; i < W; i++) {
			uint8_t g = group.at(i, j);
			bool border = (i + 1 < W && group.at(i + 1, j) != g) || (j + 1 < H && group.at(i, j + 1) != g);
			if (border) edge.at(i, j) = 1;
		}
	Grid<float> fromEdge = distanceFrom(edge, 1);
	art.ink.reset(group.x0, group.y0, px, W, H, 0);
	art.head.reset(group.x0, group.y0, px, W, H, 0);
	for (size_t k = 0; k < art.ink.a.size(); k++) {
		uint8_t g = group.a[k];
		bool filled = g == kHair || g == kBeard;
		art.ink.a[k] = filled || fromEdge.a[k] <= 0.5 * line || features.a[k];
		art.head.a[k] = g != kNone || art.ink.a[k];
	}
	tidy(art.ink, 0.25 * minWidth * minWidth);
	return art;
}

}  // namespace pmo
