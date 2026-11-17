#include "pipeline.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#include "body.hpp"
#include "face2d.hpp"
#include "flexi.hpp"
#include "fuse.hpp"
#include "geom.hpp"
#include "head.hpp"
#include "image.hpp"
#include "mesh_ops.hpp"
#include "split.hpp"
#include "text2d.hpp"
#include "trinkets.hpp"

namespace pmo {

namespace {

// Sizes are fractions of head width, so they hold at any import scale.
const double kVoxelSize = 1.0 / 240;  // glasses grid, whatever the fuse resolution
const double kBaseRadius = 0.45;
const double kBaseHeight = 0.05;
const double kBaseSink = 0.025;
const double kBodyThicken = 0.006;
const double kMinLimbRadius = 0.03;
const double kBodySeamWeld = 0.003;
const double kMinIsland = 0.01;
// How far above the chin a head printed alone is cut flat, so it stands
// (a fraction of the head's width).
const double kHeadFlat = 0.03;
// How deep the neck's pin goes into the torso and into the head, at least
// (millimetres at 100% scale).
const double kNeckSocketDepth = 6.0;

struct Part {
	std::string name;
	Mesh mesh;
	Image texture;
};

bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

/** glTF is Y up, these meshes (and the STL) Z up: (x, y, z) -> (x, -z, y). */
V3 toZUp(const V3& p) { return {p.x, -p.z, p.y}; }

Mat4 fromAi(const aiMatrix4x4& a) {
	Mat4 m;
	const float* p = &a.a1;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) m.m[i][j] = p[i * 4 + j];
	return m;
}

Image embeddedTexture(const aiScene* scene, const aiMaterial* mat) {
	Image img;
	aiString path;
	if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &path) != AI_SUCCESS &&
	    mat->GetTexture(aiTextureType_DIFFUSE, 0, &path) != AI_SUCCESS)
		return img;
	const aiTexture* tex = scene->GetEmbeddedTexture(path.C_Str());
	if (!tex) return img;
	if (tex->mHeight == 0) {
		img.load(reinterpret_cast<const uint8_t*>(tex->pcData), tex->mWidth);
	} else {
		std::vector<uint8_t> rgba(size_t(tex->mWidth) * tex->mHeight * 4);
		for (size_t i = 0; i < size_t(tex->mWidth) * tex->mHeight; i++) {
			rgba[i * 4] = tex->pcData[i].r;
			rgba[i * 4 + 1] = tex->pcData[i].g;
			rgba[i * 4 + 2] = tex->pcData[i].b;
			rgba[i * 4 + 3] = tex->pcData[i].a;
		}
		img.setPixels(tex->mWidth, tex->mHeight, std::move(rgba));
	}
	return img;
}

std::vector<Part> loadHead(Bytes glb) {
	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFileFromMemory(glb.data, glb.size, 0, "glb");
	if (!scene || !scene->mRootNode) throw std::runtime_error(std::string("head: ") + importer.GetErrorString());

	std::vector<Mat4> world(scene->mNumMeshes);
	std::vector<char> placed(scene->mNumMeshes, 0);
	std::function<void(const aiNode*, const Mat4&)> walk = [&](const aiNode* n, const Mat4& parent) {
		Mat4 w = parent * fromAi(n->mTransformation);
		for (unsigned i = 0; i < n->mNumMeshes; i++)
			if (!placed[n->mMeshes[i]]) {
				world[n->mMeshes[i]] = w;
				placed[n->mMeshes[i]] = 1;
			}
		for (unsigned i = 0; i < n->mNumChildren; i++) walk(n->mChildren[i], w);
	};
	walk(scene->mRootNode, Mat4());

	std::vector<Part> parts;
	for (unsigned mi = 0; mi < scene->mNumMeshes; mi++) {
		const aiMesh* am = scene->mMeshes[mi];
		Part part;
		part.name = am->mName.C_Str();
		Mesh& m = part.mesh;
		m.v.resize(am->mNumVertices);
		for (unsigned i = 0; i < am->mNumVertices; i++) {
			const aiVector3D& p = am->mVertices[i];
			m.v[i] = toZUp(world[mi].point(V3(p.x, p.y, p.z)));
		}
		bool uv = am->HasTextureCoords(0);
		for (unsigned f = 0; f < am->mNumFaces; f++) {
			const aiFace& face = am->mFaces[f];
			if (face.mNumIndices != 3) continue;
			m.f.push_back({(int)face.mIndices[0], (int)face.mIndices[1], (int)face.mIndices[2]});
			if (uv) {
				std::array<V2, 3> c;
				for (int k = 0; k < 3; k++) {
					const aiVector3D& t = am->mTextureCoords[0][face.mIndices[k]];
					c[k] = {t.x, t.y};  // assimp flips V, so it runs up
				}
				m.uv.push_back(c);
			}
		}
		if (am->mMaterialIndex < scene->mNumMaterials)
			part.texture = embeddedTexture(scene, scene->mMaterials[am->mMaterialIndex]);
		parts.push_back(std::move(part));
	}
	return parts;
}

/** A disc under the feet, so a top-heavy Mii prints upright and stays standing. */
Mesh makeBase(const std::vector<Mesh>& pieces, double unit) {
	double zmin = 1e300;
	for (const Mesh& m : pieces)
		for (const V3& p : m.v) zmin = std::min(zmin, p.z);
	double cx = 0, cy = 0;
	size_t n = 0;
	for (const Mesh& m : pieces)
		for (const V3& p : m.v)
			if (p.z < zmin + unit * 0.05) {
				cx += p.x;
				cy += p.y;
				n++;
			}
	cx /= double(std::max<size_t>(n, 1));
	cy /= double(std::max<size_t>(n, 1));
	const double radius = unit * kBaseRadius, height = unit * kBaseHeight;
	const double zc = zmin + unit * kBaseSink - height / 2;
	const int seg = 128;
	Mesh base;
	// Slight taper hides elephant's foot on the first layers.
	for (int ring = 0; ring < 2; ring++) {
		double r = ring ? radius * 0.95 : radius, z = zc + (ring ? height / 2 : -height / 2);
		for (int i = 0; i < seg; i++) {
			double phi = 2 * M_PI * i / seg;
			base.v.push_back({cx + r * std::sin(phi), cy + r * std::cos(phi), z});
		}
	}
	int bottom = (int)base.v.size(), top = bottom + 1;
	base.v.push_back({cx, cy, zc - height / 2});
	base.v.push_back({cx, cy, zc + height / 2});
	for (int i = 0; i < seg; i++) {
		int j = (i + 1) % seg;
		// The ring runs clockwise seen from above; faces point outward.
		base.f.push_back({i, seg + j, j});
		base.f.push_back({i, seg + i, seg + j});
		base.f.push_back({bottom, i, j});
		base.f.push_back({top, seg + j, seg + i});
	}
	return base;
}

/** Sand the voxel steps off a fused piece and drop loose specks, leaving
 *  its sockets as they are. Returns which vertices are on a socket. */
std::vector<char> finishPiece(Mesh& solid, const std::vector<Socket>& sockets, double voxel) {
	auto onSocket = [&](const Mesh& m) {
		std::vector<char> on(m.v.size(), 0);
		for (size_t i = 0; i < m.v.size(); i++)
			for (const Socket& s : sockets)
				if (std::fabs(s.distance(m.v[i])) < 0.75 * voxel) on[i] = 1;
		return on;
	};
	std::vector<char> free = onSocket(solid);
	for (char& c : free) c = !c;
	smooth(solid, 0.5, 2, &free);
	dropSpecks(solid, kMinIsland);
	return onSocket(solid);
}

/** The figure as pieces that snap together with pins: the head, each arm
 *  from the armhole, the torso with a short neck, and the legs from just
 *  below the shirt or skirt (with the base, if any), laid out for printing
 *  with their pins. The head and arms are made first; the torso and legs
 *  then keep clear of the way they slide in. */
Mesh splitFigure(std::vector<Mesh>& headParts, Mesh& body, const Body& skeleton, double unit, const Options& o,
                 const std::function<void(const std::string&)>& log, const std::function<void(const char*)>& tick) {
	char buf[256];
	const double voxel = unit / o.voxelsPerHead;
	// The printer's fit, within reason: from pins that need a mallet to ones
	// that slip out on a printer that prints holes much too small.
	const double loosen = std::clamp(o.fit, -0.2, 0.5);
	PinFit fit;
	fit.clearance = std::max(0.0, fit.clearance + loosen);
	const double clearance = std::max(0.05, 0.15 + loosen);  // between pieces that slide past each other
	if (loosen != 0) {
		std::snprintf(buf, sizeof buf, "Fit: %.2f %s around pins and between pieces", std::fabs(loosen),
		              loosen > 0 ? "more room" : "less room");
		log(buf);
	}
	auto joint = [&](const char* name) { return toZUp(skeleton.jointInHeadFrame(name)); };
	auto tagged = [](int want) { return [want](int t) { return t == want; }; };
	auto none = [](int, const V3&) { return false; };
	auto verticesTagged = [&](const Mesh& m, int want) {
		std::vector<char> on(m.v.size(), 0);
		for (size_t f = 0; f < m.f.size(); f++)
			if (faceTag(m, int(f)) == want)
				for (int k : m.f[f]) on[k] = 1;
		return on;
	};

	struct Joint {
		const char* name;
		V3 point, normal;  // normal points into the piece that slides on
		double gap = 0;    // between the two pieces' faces at the joint
		std::vector<Outline> outlines;
		std::vector<PinSpot> spots;
		std::vector<Socket> on, off;  // per spot: the socket in the fixed piece and in the one that slides on
	};
	enum { kArmL, kArmR, kNeck, kHips };
	std::vector<Joint> joints(4);

	// Arms: square to the upper arm, just where it comes free of the body:
	// the first cut, going out from the shoulder, that takes nothing far
	// from the arm's bones.
	Mesh arms[2];
	for (int side = 0; side < 2; side++) {
		const int tag = side ? kTagArmR : kTagArmL;
		Joint& j = joints[side ? kArmR : kArmL];
		j.name = side ? "right shoulder" : "left shoulder";
		const V3 shoulder = joint(side ? "arm_r1" : "arm_l1"), elbow = joint(side ? "arm_r2" : "arm_l2"),
		         wrist = joint(side ? "wrist_r" : "wrist_l");
		const V3 axis = normalized(elbow - shoulder);
		auto fromBones = [&](const V3& p) {
			double best = 1e300;
			for (auto [a, b] : {std::pair<V3, V3>{shoulder, elbow}, {elbow, wrist}}) {
				V3 ab = b - a;
				double t = std::clamp(dot(p - a, ab) / std::max(dot(ab, ab), 1e-12), 0.0, 1.0);
				best = std::min(best, length(p - (a + ab * t)));
			}
			return best;
		};
		// The arm's own faces are within its thickness of its bones (the hand
		// a little past the wrist); a cut that leaks takes body far beyond.
		std::vector<char> armFaces = verticesTagged(body, tag);
		double thick = 0;
		for (size_t i = 0; i < body.v.size(); i++)
			if (armFaces[i]) thick = std::max(thick, fromBones(body.v[i]));
		const double reach = std::max(0.12 * unit, 1.5 * thick);
		auto leak = [&](int t, const V3& c) {
			if (t == kTagLegs || t == kTagNeck || t == (side ? kTagArmL : kTagArmR)) return true;
			return fromBones(c) > reach;
		};
		j.normal = axis;
		// Where the arm comes free: the first cut going down the arm.
		double free = -1;
		for (double at = 0.02 * unit; at < 0.25 * unit && free < 0; at += 0.005 * unit) {
			Mesh rest = body, arm;
			std::vector<Outline> outlines;
			if (cutOff(rest, shoulder + axis * at, axis, tagged(tag), leak, arm, outlines)) free = at;
		}
		if (free < 0) throw std::runtime_error("split: could not cut off an arm");
		// The pin runs up the arm into the shoulder. A little further down
		// the arm the shoulder may hold a thicker one; take the cut that
		// takes the thickest pin as deep as it should go.
		const V3 e1 = normalized(cross(axis, std::fabs(axis.x) < 0.9 ? V3(1, 0, 0) : V3(0, 1, 0))), e2 = cross(axis, e1);
		auto roomAlong = [&](const BVH& bvh, const V3& mouth, const V3& dir, double r) {
			auto depth = [&](const V3& o) {
				double t = 0;
				return bvh.raycast(o + dir * 0.01, dir, 50.0, t) ? t : 50.0;
			};
			const double wall = Socket::wallFor(r);
			double room = depth(mouth) - r - wall;
			for (int q = 0; q < 8; q++)
				room = std::min(room, depth(mouth + (e1 * std::cos(q * M_PI / 4) + e2 * std::sin(q * M_PI / 4)) * (r + wall)) - wall);
			return room;
		};
		double bestScore = -1e300;
		Mesh rest;
		for (int k = 0; k <= 12; k++) {
			double at = free + 0.5 * k;
			Mesh candidate = body, arm;
			std::vector<Outline> outlines;
			if (!cutOff(candidate, shoulder + axis * at, axis, tagged(tag), leak, arm, outlines) || outlines.empty()) continue;
			PinSpot spot = pinSpots(outlines[0], axis, 1.6, 5.0, false, fit.clearance)[0];
			BVH inTorso(candidate), inArm(arm);
			// The thickest pin both sides take deep enough; failing that, how
			// close the thinnest comes.
			double score = -1;
			for (double r = spot.diameter / 2 + fit.clearance; r >= 0.8 + fit.clearance - 1e-9; r -= 0.1) {
				double room = std::min(roomAlong(inTorso, spot.centre, -axis, r), roomAlong(inArm, spot.centre, axis, r));
				double want = Socket::goodDepth(r);
				if (room >= want) {
					score = r;
					break;
				}
				score = std::max(score, room / want - 1);
			}
			if (score > bestScore + 0.05) {
				bestScore = score;
				rest = std::move(candidate);
				arms[side] = std::move(arm);
				j.outlines = std::move(outlines);
				j.point = shoulder + axis * at;
			}
		}
		if (rest.f.empty()) throw std::runtime_error("split: could not cut off an arm");
		const double at = dot(j.point - shoulder, axis);
		body = std::move(rest);
		std::snprintf(buf, sizeof buf, "%s: cut %.2f down the arm", j.name, at);
		log(buf);
	}

	// Neck: the torso keeps it up into the head, which has a hole for it,
	// as a minifigure's head sits on its neck.
	{
		Joint& j = joints[kNeck];
		j.name = "neck";
		std::vector<char> top = verticesTagged(body, kTagNeck);
		double cx = 0, cy = 0, n = 0, neckTop = -1e300;
		for (size_t i = 0; i < body.v.size(); i++)
			if (top[i]) {
				cx += body.v[i].x;
				cy += body.v[i].y;
				n++;
				neckTop = std::max(neckTop, body.v[i].z);
			}
		if (!n) throw std::runtime_error("split: the body has no neck");
		cx /= n;
		cy /= n;
		double radius = 0;
		for (size_t i = 0; i < body.v.size(); i++)
			if (top[i]) radius = std::max(radius, std::hypot(body.v[i].x - cx, body.v[i].y - cy));
		double headBottom = 1e300;
		for (const V3& p : headParts[0].v)
			if (std::hypot(p.x - cx, p.y - cy) < 1.3 * radius) headBottom = std::min(headBottom, p.z);
		double z = headBottom + std::min(0.5 * std::max(neckTop - headBottom, 0.0), 0.06 * unit);
		j.point = V3(cx, cy, z);
		j.normal = V3(0, 0, 1);
		j.gap = clearance;
		Mesh inside;  // the top of the neck, hidden in the head: not needed
		if (!cutOff(body, j.point, j.normal, tagged(kTagNeck),
		            [](int t, const V3&) { return t != kTagNeck && t != kTagTorso && t != kTagCap; }, inside, j.outlines))
			throw std::runtime_error("split: could not cut the neck");
		std::snprintf(buf, sizeof buf, "Neck: into the head (from %.2f) up to %.2f", headBottom, z);
		log(buf);
	}

	// Hips: just below the shirt or skirt, where the legs come out.
	Mesh legs;
	{
		Joint& j = joints[kHips];
		j.name = "hips";
		std::vector<char> torso = verticesTagged(body, kTagTorso);
		double hem = 1e300;
		for (size_t i = 0; i < body.v.size(); i++)
			if (torso[i]) hem = std::min(hem, body.v[i].z);
		j.point = V3(0, 0, hem - 0.01 * unit);
		j.normal = V3(0, 0, -1);
		if (!cutOff(body, j.point, j.normal, tagged(kTagLegs), none, legs, j.outlines))
			throw std::runtime_error("split: could not cut off the legs");
	}
	tick("split body");

	// Each piece on its grid, before any socket is cut, so each pin can be
	// sized and aimed with both pieces it joins in view.
	Mesh base;
	if (o.base) base = makeBase({legs}, unit);
	std::vector<const Mesh*> headInputs;
	for (const Mesh& m : headParts) headInputs.push_back(&m);
	std::vector<const Mesh*> legInputs{&legs};
	if (o.base) legInputs.push_back(&base);
	enum { kHead, kLeftArm, kRightArm, kTorso, kLegs };
	const char* names[5] = {"Head", "Left arm", "Right arm", "Torso", "Legs"};
	std::vector<Voxels> grids;
	grids.push_back(voxelize(headInputs, voxel, log));
	grids.push_back(voxelize({&arms[0]}, voxel, log));
	grids.push_back(voxelize({&arms[1]}, voxel, log));
	grids.push_back(voxelize({&body}, voxel, log));
	grids.push_back(voxelize(legInputs, voxel, log));
	tick("voxelize");

	// Pins: as thick as the cross-section and both pieces allow, going in
	// as deep as holds them firmly.
	const int moving[4] = {kLeftArm, kRightArm, kHead, kLegs};
	for (int ji = 0; ji < 4; ji++) {
		Joint& j = joints[ji];
		const Voxels &on = grids[kTorso], &off = grids[moving[ji]];
		std::vector<V3> axes{j.normal};
		for (const Outline& ol : j.outlines)
			// Legs coming out as one (a pelvis) take two pins, so they cannot
			// turn, even on a tiny Mii.
			for (const PinSpot& sp : pinSpots(ol, j.normal, 2.0, 5.0, ji == kHips && j.outlines.size() == 1,
			                                  fit.clearance)) {
				double bestScore = -1, bestR = 0, bestOn = 0, bestOff = 0;
				V3 bestAxis = j.normal;
				// Hips carry the figure: no thinner than 2 mm. A thin neck or
				// arm may go down to 1.6 mm to keep a wall around its socket.
				const double thinnest = ji == kHips ? 2.0 : 1.6;
				for (double r = sp.diameter / 2 + fit.clearance; r >= thinnest / 2 + fit.clearance - 1e-9; r -= 0.1) {
					double want = Socket::goodDepth(r);
					for (const V3& a : axes) {
						double dOn = on.room(sp.centre, -a, r, want, j.normal);
						double dOff = off.room(sp.centre + j.normal * j.gap, a, r, want, -j.normal);
						double score = std::min(dOn, dOff) / want;
						if (score > bestScore + 1e-6) {
							bestScore = score;
							bestR = r;
							bestAxis = a;
							bestOn = dOn;
							bestOff = dOff;
						}
					}
					if (bestScore >= 1) break;  // the thickest that goes deep enough
				}
				if (ji == kNeck) {
					// A thin neck pin goes deeper than its thickness asks, as far
					// as there is room (the head is big and the neck runs on down
					// into the chest), so a tiny Mii keeps its head on.
					const double deep = std::max(Socket::goodDepth(bestR), kNeckSocketDepth);
					bestOn = on.room(sp.centre, -bestAxis, bestR, deep, j.normal);
					bestOff = off.room(sp.centre + j.normal * j.gap, bestAxis, bestR, deep, -j.normal);
				}
				j.spots.push_back({sp.centre, 2 * (bestR - fit.clearance)});
				for (int side = 0; side < 2; side++) {
					Socket k;
					k.mouth = sp.centre + j.normal * (side ? j.gap : 0.0);
					k.axis = side ? bestAxis : -bestAxis;
					k.radius = bestR;
					k.depth = std::max(side ? bestOff : bestOn, 1.5);
					(side ? j.off : j.on).push_back(k);
				}
			}
	}

	// Pins that do the same job come out the same, so any of them fits:
	// both shoulders, and both hips.
	auto matchPins = [&](std::initializer_list<int> which) {
		double r = 1e300;
		for (int ji : which)
			for (const Socket& k : joints[ji].on) r = std::min(r, k.radius);
		for (int ji : which) {
			Joint& j = joints[ji];
			for (size_t s = 0; s < j.spots.size(); s++) {
				if (j.on[s].radius == r) continue;
				double want = Socket::goodDepth(r);
				j.on[s].radius = j.off[s].radius = r;
				j.on[s].depth = std::max(grids[kTorso].room(j.on[s].mouth, j.on[s].axis, r, want, j.normal), 1.5);
				j.off[s].depth = std::max(grids[moving[ji]].room(j.off[s].mouth, j.off[s].axis, r, want, -j.normal), 1.5);
				j.spots[s].diameter = 2 * (r - fit.clearance);
			}
		}
	};
	matchPins({kArmL, kArmR});
	matchPins({kHips});

	struct Piece {
		Mesh mesh;
		std::vector<Socket> sockets;
	};
	std::vector<Piece> pieces(5);
	auto contourPiece = [&](int index, std::vector<Socket> sockets, const std::vector<Carve>& carves, Field* field) {
		FuseExtras extras;
		extras.sockets = &sockets;
		extras.carves = carves;
		extras.field = field;
		pieces[index].mesh = contour(grids[index], log, extras);
		pieces[index].sockets = sockets;
		grids[index] = Voxels();
		std::snprintf(buf, sizeof buf, "%s: %zu triangles", names[index], pieces[index].mesh.f.size());
		log(buf);
	};

	// The head, with a hole for the neck the shape the neck is where the
	// head starts, and a little roomier.
	Field headField, armField[2];
	{
		Joint& j = joints[kNeck];
		std::vector<Outline> rings = slice(body, V3(j.point.x, j.point.y, j.point.z - 0.06 * unit), V3(0, 0, 1));
		Field neckField;
		double best = 1e300;
		for (const Outline& ol : rings) {
			double r = 0;  // the ring around the neck: the one nearest its middle
			for (const V3& q : ol) r += std::hypot(q.x - j.point.x, q.y - j.point.y);
			r /= double(ol.size());
			if (r < best) {
				best = r;
				neckField = prismField(ol, j.point.z - 0.12 * unit, j.point.z, voxel);
			}
		}
		std::vector<Carve> carves;
		if (!neckField.empty()) {
			Carve c;
			c.other = &neckField;
			c.dir = V3(0, 0, -1);
			c.travel = 0;
			c.clearance = clearance;
			carves.push_back(c);
		}
		contourPiece(kHead, j.off, carves, &headField);
		tick("fuse head");
	}
	contourPiece(kLeftArm, joints[kArmL].off, {}, &armField[0]);
	contourPiece(kRightArm, joints[kArmR].off, {}, &armField[1]);
	tick("fuse arms");
	// The torso and legs give way to the head and arms on their way in.
	auto carveFor = [&](const Joint& j, const Field& field, bool spareJoint) {
		Carve c;
		c.other = &field;
		c.dir = j.off.empty() ? -j.normal : -j.off[0].axis;
		double deepest = 0, widest = 0;
		for (const Socket& k : j.off) deepest = std::max(deepest, k.depth);
		for (const Outline& ol : j.outlines)
			for (const V3& q : ol) widest = std::max(widest, length(q - j.point));
		c.travel = deepest + 1.0;
		c.clearance = clearance;
		if (spareJoint) {
			c.jointPoint = j.point;
			c.jointNormal = j.normal;
			c.jointRadius = widest + clearance + 2 * voxel;
			c.jointSlab = clearance + 2 * voxel;
		}
		return c;
	};
	{
		std::vector<Socket> sockets;
		for (int k : {kNeck, kArmL, kArmR, kHips}) sockets.insert(sockets.end(), joints[k].on.begin(), joints[k].on.end());
		contourPiece(kTorso, sockets,
		             {carveFor(joints[kNeck], headField, false), carveFor(joints[kArmL], armField[0], true),
		              carveFor(joints[kArmR], armField[1], true)},
		             nullptr);
		tick("fuse torso");
	}
	headField = Field();
	contourPiece(kLegs, joints[kHips].off,
	             {carveFor(joints[kArmL], armField[0], false), carveFor(joints[kArmR], armField[1], false)}, nullptr);
	armField[0] = armField[1] = Field();
	tick("fuse legs");

	// Sand, then share the triangle budget out by size.
	std::vector<std::vector<char>> locked;
	size_t total = 0;
	for (Piece& p : pieces) {
		locked.push_back(finishPiece(p.mesh, p.sockets, voxel));
		total += p.mesh.f.size();
	}
	tick("sand");
	for (size_t i = 0; i < pieces.size(); i++) {
		size_t budget = total > o.maxTriangles ? size_t(double(o.maxTriangles) * pieces[i].mesh.f.size() / total)
		                                       : pieces[i].mesh.f.size();
		simplify(pieces[i].mesh, budget, voxel * 0.25, &locked[i]);
	}
	tick("tidy");

	// Every piece prints standing as it stands in the figure; the pins lie
	// on their flats.
	std::vector<Mesh> out;
	for (Piece& p : pieces) out.push_back(std::move(p.mesh));
	for (const Joint& j : joints)
		for (size_t s = 0; s < j.spots.size(); s++) {
			double diameter = j.spots[s].diameter;
			double length = j.on[s].depth + j.off[s].depth + j.gap - 2 * fit.endGap;
			out.push_back(makePin(diameter, length, fit));
			std::snprintf(buf, sizeof buf, "Pin for the %s: %.2f mm thick, %.2f long (sockets %.2f across, %.2f and %.2f deep)",
			              j.name, diameter, length, 2 * j.on[s].radius, j.on[s].depth, j.off[s].depth);
			log(buf);
		}
	if (o.assembled) {
		// For checking: every piece where it goes, the pins in their sockets.
		Mesh all;
		for (size_t i = 0; i < 5; i++) all.append(out[i]);
		size_t pin = 5;
		for (const Joint& j : joints)
			for (size_t s = 0; s < j.spots.size(); s++, pin++) {
				Mesh& m = out[pin];
				V3 a = j.off[s].axis, start = j.on[s].mouth - a * (j.on[s].depth - fit.endGap);
				V3 x(1, 0, 0), k = cross(x, a);
				double c = dot(x, a), sn = length(k);
				double flat = 0.84 * j.spots[s].diameter / 2;
				for (V3& p : m.v) {
					V3 q(p.x, p.y, p.z - flat);
					if (sn > 1e-9) {
						V3 u = k / sn;  // Rodrigues
						q = q * c + cross(u, q) * sn + u * (dot(u, q) * (1 - c));
					}
					p = start + q;
				}
				all.append(m);
			}
		return all;
	}
	Mesh all = layOut(out, 6.0, 180.0);
	std::snprintf(buf, sizeof buf, "Printable: %zu pieces and pins, %zu faces, %d non-manifold edges", out.size(),
	              all.f.size(), countNonManifoldEdges(all));
	log(buf);
	return all;
}

/** A solid of revolution about a vertical axis through (x, y): the
 *  profile runs (radius, z) from the axis at the bottom to the axis at
 *  the top. */
Mesh lathe(const std::vector<std::pair<double, double>>& profile, double x, double y, int segments) {
	Mesh m;
	const int n = (int)profile.size();
	for (int k = 1; k < n - 1; k++)
		for (int s = 0; s < segments; s++) {
			double a = 2 * M_PI * s / segments;
			m.v.push_back({x + profile[k].first * std::cos(a), y + profile[k].first * std::sin(a), profile[k].second});
		}
	const int bottom = (int)m.v.size(), top = bottom + 1;
	m.v.push_back({x, y, profile.front().second});
	m.v.push_back({x, y, profile.back().second});
	auto at = [&](int ring, int s) { return ring * segments + (s % segments); };
	for (int s = 0; s < segments; s++) {
		m.f.push_back({bottom, at(0, s + 1), at(0, s)});
		m.f.push_back({top, at(n - 3, s), at(n - 3, s + 1)});
		for (int ring = 0; ring + 1 < n - 2; ring++) {
			m.f.push_back({at(ring, s), at(ring, s + 1), at(ring + 1, s + 1)});
			m.f.push_back({at(ring, s), at(ring + 1, s + 1), at(ring + 1, s)});
		}
	}
	return m;
}

/** Sand and tidy a fused piece, leaving what is within half a voxel or so
 *  of a cut (an exact hollow) as it is. */
void finishWithCuts(Mesh& solid, const std::vector<Cut>& cuts, double voxel, size_t maxTriangles) {
	auto onCut = [&](const Mesh& m) {
		std::vector<char> on(m.v.size(), 0);
		for (size_t i = 0; i < m.v.size(); i++)
			for (const Cut& c : cuts)
				if (std::fabs(c.distance(m.v[i])) < 0.75 * voxel) on[i] = 1;
		return on;
	};
	std::vector<char> free = onCut(solid);
	for (char& c : free) c = !c;
	smooth(solid, 0.5, 2, &free);
	dropSpecks(solid, kMinIsland);
	std::vector<char> locked = onCut(solid);
	simplify(solid, maxTriangles, voxel * 0.25, &locked);
}

/** A bobblehead that prints in place: the head hangs on a knob at the top
 *  of a post that comes up out of the neck, inside a hollow in the head
 *  with room all round. The knob sits above the head's centre of mass, so
 *  the head hangs upright and rocks when tapped; the hollow narrows under
 *  the knob, so the head never comes off. Every overhang inside is steep
 *  enough to print without support. */
Mesh bobblehead(std::vector<Mesh>& headParts, Mesh& body, double unit, const Options& o,
                const std::function<void(const std::string&)>& log, const std::function<void(const char*)>& tick) {
	char buf[256];
	const double voxel = unit / o.voxelsPerHead;
	const double loosen = std::clamp(o.fit, -0.2, 0.5);
	const double c = std::max(0.25, 0.45 + loosen);  // room all round the knob and post
	const double rp = std::clamp(0.06 * unit, 1.6, 2.6), rb = rp + 1.3;
	const double alpha = 40 * M_PI / 180;  // overhangs inside: no flatter than this from upright
	const double flare = 6 * M_PI / 180;   // the hollow widens going down, so the head can rock

	// The neck, which the post replaces.
	std::vector<char> neck(body.v.size(), 0);
	for (size_t f = 0; f < body.f.size(); f++)
		if (faceTag(body, int(f)) == kTagNeck)
			for (int k : body.f[f]) neck[k] = 1;
	double cx = 0, cy = 0, n = 0, neckBase = 1e300;
	for (size_t i = 0; i < body.v.size(); i++)
		if (neck[i]) {
			cx += body.v[i].x;
			cy += body.v[i].y;
			n++;
			neckBase = std::min(neckBase, body.v[i].z);
		}
	if (!n) throw std::runtime_error("bobblehead: the body has no neck");
	cx /= n;
	cy /= n;
	double neckRadius = 0;
	for (size_t i = 0; i < body.v.size(); i++)
		if (neck[i]) neckRadius = std::max(neckRadius, std::hypot(body.v[i].x - cx, body.v[i].y - cy));
	double headBottom = 1e300;
	for (const V3& p : headParts[0].v)
		if (std::hypot(p.x - cx, p.y - cy) < 1.3 * neckRadius) headBottom = std::min(headBottom, p.z);

	// Lift the head clear of the shoulders, with a little neck showing.
	Mesh torso = body;
	keepFaces(torso, [&](int f) { return faceTag(torso, f) != kTagNeck && faceTag(torso, f) != kTagCap; });
	compact(torso);
	BVH torsoBVH(torso);
	// Room for the head to rock before it touches the shoulders.
	const double clearOfBody = std::max(2.5, 0.04 * unit);
	double lift = 0;
	for (int it = 0; it < 6; it++) {
		double gap = 1e300;
		for (const Mesh& m : headParts) {
			size_t step = std::max<size_t>(1, m.v.size() / 20000);
			for (size_t i = 0; i < m.v.size(); i += step) {
				V3 p = m.v[i] + V3(0, 0, lift);
				gap = std::min(gap, length(p - torsoBVH.closest(p)));
			}
		}
		if (gap >= clearOfBody) break;
		lift += clearOfBody - gap + 0.05;
	}
	for (Mesh& m : headParts)
		for (V3& p : m.v) p.z += lift;
	headBottom += lift;

	// The head on its grid: its centre of mass, and where there is room.
	std::vector<const Mesh*> headInputs;
	for (const Mesh& m : headParts) headInputs.push_back(&m);
	Voxels headGrid = voxelize(headInputs, voxel, log);
	V3 com;
	{
		double count = 0;
		for (int k = 0; k < headGrid.nz; k += 2)
			for (int j = 0; j < headGrid.ny; j += 2)
				for (int i = 0; i < headGrid.nx; i += 2) {
					V3 q = headGrid.lo + V3(i, j, k) * headGrid.voxel;
					if (!headGrid.solidAt(q)) continue;
					com += q;
					count++;
				}
		if (!count) throw std::runtime_error("bobblehead: the head is empty");
		com = com / count;
	}

	// The pivot: over the neck, leaning towards the centre of mass, and
	// above it, with a wall of head all round the hollow.
	double dx = com.x - cx, dy = com.y - cy, off = std::hypot(dx, dy), most = 0.4 * neckRadius;
	if (off > most) {
		dx *= most / off;
		dy *= most / off;
	}
	const double ax = cx + dx, ay = cy + dy;
	const double Rr = rb + c, apexAbove = Rr / std::sin(alpha), wall = 1.6;
	// The post and its knob: a cone under the knob (no flatter than alpha)
	// and a dome on it, which the head rests on; and round them the hollow,
	// the knob with room round it, a cone over it, and a channel down to the
	// neck that widens, so the head can rock.
	struct Pivot {
		double pz, zk0, zk1;
	};
	auto pivotAt = [&](double pz) { Pivot v{pz, 0, pz - 0.5 * rb}; v.zk0 = v.zk1 - (rb - rp) / std::tan(alpha); return v; };
	const double zPost = neckBase - 3.0;
	auto knobAt = [=](const Pivot& v) {
		return [=](const V3& p) {
			double rho = std::hypot(p.x - ax, p.y - ay), z = p.z;
			double stem = std::max(rho - rp, std::max(zPost - z, z - v.pz));
			double cone = std::max((rho - rp - (z - v.zk0) * std::tan(alpha)) * std::cos(alpha), std::max(v.zk0 - z, z - v.zk1));
			double cyl = std::max(rho - rb, std::max(v.zk1 - z, z - v.pz));
			double dome = std::hypot(rho, z - v.pz) - rb;
			return std::min(std::min(stem, cone), std::min(cyl, dome));
		};
	};
	auto hollowAt = [=](const Pivot& v) {
		auto knob = knobAt(v);
		const double zApex = v.pz + apexAbove;
		return [=](const V3& p) {
			double rho = std::hypot(p.x - ax, p.y - ay);
			double round = knob(p) - c;
			double roof = std::max(rho * std::cos(alpha) - (zApex - p.z) * std::sin(alpha), v.pz + Rr * std::sin(alpha) - p.z);
			double channel =
			    std::max((rho - (rp + c + std::max(0.0, v.zk0 - p.z) * std::tan(flare))) * std::cos(flare), p.z - v.zk0);
			return std::min(round, std::min(roof, channel));
		};
	};
	auto fits = [&](const Pivot& v) {
		auto hollow = hollowAt(v);
		const double reach = Rr + wall + 0.5, step = 0.5;
		for (double z = v.zk0; z <= v.pz + apexAbove + wall; z += step)
			for (double y = ay - reach; y <= ay + reach; y += step)
				for (double x = ax - reach; x <= ax + reach; x += step) {
					V3 q(x, y, z);
					if (hollow(q) <= wall && !headGrid.solidAt(q)) return false;
				}
		return true;
	};
	Pivot pivot = pivotAt(com.z + 0.12 * unit);
	while (!fits(pivot) && pivot.pz > com.z + 0.01 * unit) pivot = pivotAt(pivot.pz - 0.005 * unit);
	if (!fits(pivot)) throw std::runtime_error("bobblehead: no room in the head for the pivot");
	const double pz = pivot.pz, zk0 = pivot.zk0, zk1 = pivot.zk1, zApex = pz + apexAbove;
	std::snprintf(buf, sizeof buf,
	              "Bobblehead: head lifted %.2f, pivot %.2f above its centre of mass, post %.2f thick, knob %.2f, "
	              "%.2f room round them",
	              lift, pz - com.z, 2 * rp, 2 * rb, c);
	log(buf);

	// The neck goes; the post comes up out of the shoulders in its place.
	keepFaces(body, [&](int f) { return faceTag(body, f) != kTagNeck; });
	compact(body);
	fillHoles(body);

	std::vector<std::pair<double, double>> profile{{0, zPost}, {rp, zPost}, {rp, zk0}, {rb, zk1}, {rb, pz}};
	for (int k = 1; k < 16; k++) {
		double a = M_PI / 2 * k / 16;
		profile.push_back({rb * std::cos(a), pz + rb * std::sin(a)});
	}
	profile.push_back({0, pz + rb});
	Mesh post = lathe(profile, ax, ay, 64);
	Cut hollow;
	hollow.distance = hollowAt(pivot);
	const double reach = rb + c + (zk0 - (headBottom - 3)) * std::tan(flare) + 1;
	hollow.lo = V3(ax - reach, ay - reach, headBottom - 3);
	hollow.hi = V3(ax + reach, ay + reach, zApex + 1);
	tick("bobble");

	FuseExtras headExtras;
	headExtras.cuts.push_back(hollow);
	Mesh head = contour(headGrid, log, headExtras);
	headGrid = Voxels();
	finishWithCuts(head, headExtras.cuts, voxel, o.maxTriangles / 2);
	std::snprintf(buf, sizeof buf, "Head: %zu triangles", head.f.size());
	log(buf);
	tick("fuse head");

	std::vector<Mesh> bodyParts{body, post};
	if (o.base) bodyParts.push_back(makeBase({body}, unit));
	std::vector<const Mesh*> bodyInputs;
	for (const Mesh& m : bodyParts) {
		bodyInputs.push_back(&m);
	}
	Mesh rest = fuse(bodyInputs, voxel, log);
	smooth(rest, 0.5, 2);
	dropSpecks(rest, kMinIsland);
	simplify(rest, o.maxTriangles / 2, voxel * 0.25);
	std::snprintf(buf, sizeof buf, "Body: %zu triangles", rest.f.size());
	log(buf);
	tick("fuse body");

	Mesh all = rest;
	all.append(head);
	V3 lo, hi;
	all.bounds(lo, hi);
	for (V3& p : all.v) p.z -= lo.z;
	std::snprintf(buf, sizeof buf, "Printable: head and body in place, %zu faces, %d non-manifold edges", all.f.size(),
	              countNonManifoldEdges(all));
	log(buf);
	return all;
}

/** The head's parts ready to draw flat, and what they draw in (head units). */
struct FlatParts {
	Image external;
	std::vector<FacePart> parts;
	FaceSource source;
};

void prepareFlat(std::vector<Part>& parts, Bytes maskBytes, FlatParts& out,
                 const std::function<void(const std::string&)>& log) {
	if (maskBytes.size && !out.external.load(maskBytes.data, maskBytes.size)) log("Warning: could not read the mask PNG");
	for (Part& p : parts) {
		bool overlay = has(p.name, "Mask") || has(p.name, "NoseLine") || has(p.name, "Glass") || has(p.name, "Xlu");
		weld(p.mesh, 0.0001);
		if (!overlay) subdivide(p.mesh, 2);  // smooth outlines
		FacePart f;
		f.name = p.name;
		f.mesh = &p.mesh;
		f.texture = has(p.name, "Mask") && !out.external.empty() ? &out.external : &p.texture;
		out.parts.push_back(f);
	}

	// What is drawn, from a quick drawing: its box.
	double x0, z0, x1, z1;
	faceBounds(out.parts, x0, z0, x1, z1);
	if (!(x1 > x0)) throw std::runtime_error("head: nothing to draw");
	const double unit = x1 - x0;
	FaceArt rough = drawFace(out.parts, unit / 200, unit / 120, 0);
	FaceSource& source = out.source;
	source.x0 = source.z0 = 1e300;
	source.x1 = source.z1 = -1e300;
	for (int j = 0; j < rough.head.h; j++)
		for (int i = 0; i < rough.head.w; i++)
			if (rough.head.at(i, j)) {
				source.x0 = std::min(source.x0, rough.head.cx(i) - rough.head.px);
				source.x1 = std::max(source.x1, rough.head.cx(i) + rough.head.px);
				source.z0 = std::min(source.z0, rough.head.cy(j) - rough.head.px);
				source.z1 = std::max(source.z1, rough.head.cy(j) + rough.head.px);
			}
	const std::vector<FacePart>* drawn = &out.parts;
	source.draw = [drawn](double px, double line, double minWidth) { return drawFace(*drawn, px, line, minWidth); };
}

/** A coin or a stamp: the head drawn flat from the front, with lettering. */
Mesh flatMii(std::vector<Part>& parts, Bytes maskBytes, const Options& o,
             const std::function<void(const std::string&)>& log, const std::function<void(const char*)>& tick) {
	FlatParts flat;
	prepareFlat(parts, maskBytes, flat, log);
	const FaceSource& source = flat.source;
	tick("face art");

	Font font;
	bool lettered = false;
	if (!o.text.empty()) {
		if (o.font.size) lettered = font.load(o.font.data, o.font.size);
		if (!lettered) log("Warning: no font to letter the text with; leaving it off");
	}
	TrinketOptions t;
	t.make = o.make;
	t.size = o.size;
	t.square = o.squareCoin;
	t.faceShape = o.faceCoin;
	t.rim = o.rim;
	t.engraved = o.engraved;
	t.text = o.text;
	t.textBelow = o.textBelow;
	t.border = o.border;
	t.fit = o.fit;
	t.assembled = o.assembled;
	t.maxTriangles = o.maxTriangles;
	Mesh m = makeTrinket(source, lettered ? &font : nullptr, t, log, tick);
	char buf[160];
	std::snprintf(buf, sizeof buf, "Printable: %zu faces, %d non-manifold edges", m.f.size(), countNonManifoldEdges(m));
	log(buf);
	return m;
}

/** An articulated Mii: the figure drawn flat, its arms and legs on joints
 *  that print in place (flexi.cpp). */
Mesh articulatedMii(std::vector<Part>& parts, Bytes maskBytes, Bytes bodyBytes, const Options& o,
                    const std::function<void(const std::string&)>& log, const std::function<void(const char*)>& tick) {
	if (o.body != 'm' && o.body != 'f') throw std::runtime_error("articulated: it needs a body");
	if (!bodyBytes.size) throw std::runtime_error("body: no body model given");
	FlatParts flat;
	prepareFlat(parts, maskBytes, flat, log);
	tick("face art");

	Body body(bodyBytes.data, bodyBytes.size, o.height, o.build);
	FlexiInput in;
	in.face = flat.source;
	auto pose = [&]() {
		in.body = body.inHeadFrame();
		for (Mesh& m : in.body)
			for (V3& p : m.v) p = toZUp(p);
		in.tags = body.faceTags();
		auto joint = [&](const char* name) { return toZUp(body.jointInHeadFrame(name)); };
		in.shoulderL = joint("arm_l1");
		in.shoulderR = joint("arm_r1");
		in.hipL = joint("foot_l1");
		in.hipR = joint("foot_r1");
	};
	pose();
	// Arms and legs at least 3 mm wide flat, whatever the Mii's build.
	const double tall = std::clamp(o.size > 0 ? o.size : 100.0, 70.0, 200.0);
	const double mmPerUnit = tall / flexiHeight(in);
	int moved = body.thickenLimbs(1.5 / mmPerUnit / kHeadToBody);
	pose();
	char buf[200];
	std::snprintf(buf, sizeof buf, "Body: height=%d build=%d, %d limb vertices held at the printable minimum", o.height,
	              o.build, moved);
	log(buf);
	tick("body");

	FlexiOptions fo;
	fo.size = o.size;
	fo.fit = o.fit;
	fo.maxTriangles = o.maxTriangles;
	Mesh m = makeFlexi(in, fo, log, tick);
	std::snprintf(buf, sizeof buf, "Printable: %zu faces, %d non-manifold edges", m.f.size(), countNonManifoldEdges(m));
	log(buf);
	return m;
}

std::vector<uint8_t> exportSTL(const Mesh& m) {
	aiScene scene;
	scene.mRootNode = new aiNode();
	scene.mRootNode->mNumMeshes = 1;
	scene.mRootNode->mMeshes = new unsigned[1]{0};
	scene.mNumMaterials = 1;
	scene.mMaterials = new aiMaterial*[1]{new aiMaterial()};
	scene.mNumMeshes = 1;
	scene.mMeshes = new aiMesh*[1];
	aiMesh* mesh = new aiMesh();
	scene.mMeshes[0] = mesh;
	mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
	mesh->mMaterialIndex = 0;
	mesh->mNumVertices = (unsigned)m.v.size();
	mesh->mVertices = new aiVector3D[m.v.size()];
	mesh->mNormals = new aiVector3D[m.v.size()];
	auto n = vertexNormals(m);
	for (size_t i = 0; i < m.v.size(); i++) {
		mesh->mVertices[i] = aiVector3D(float(m.v[i].x), float(m.v[i].y), float(m.v[i].z));
		mesh->mNormals[i] = aiVector3D(float(n[i].x), float(n[i].y), float(n[i].z));
	}
	mesh->mNumFaces = (unsigned)m.f.size();
	mesh->mFaces = new aiFace[m.f.size()];
	for (size_t i = 0; i < m.f.size(); i++) {
		aiFace& face = mesh->mFaces[i];
		face.mNumIndices = 3;
		face.mIndices = new unsigned[3]{unsigned(m.f[i][0]), unsigned(m.f[i][1]), unsigned(m.f[i][2])};
	}
	Assimp::Exporter exporter;
	const aiExportDataBlob* blob = exporter.ExportToBlob(&scene, "stlb");
	if (!blob) throw std::runtime_error(std::string("export: ") + exporter.GetErrorString());
	const uint8_t* data = static_cast<const uint8_t*>(blob->data);
	return std::vector<uint8_t>(data, data + blob->size);
}

}  // namespace

FlatFace drawFlatFace(Bytes headBytes, Bytes maskBytes, int across) {
	std::vector<Part> parts = loadHead(headBytes);
	FlatParts flat;
	prepareFlat(parts, maskBytes, flat, [](const std::string&) {});
	const FaceSource& src = flat.source;
	across = std::max(64, std::min(across, 4096));
	// Drawn at twice the size and averaged down, for smooth edges. Lines as
	// bold as the stamps' and nothing finer than a printer keeps.
	const double width = src.x1 - src.x0, px = width / (2 * across);
	FaceArt art = src.draw(px, width / 48, width / 90);
	const double pad = width * 0.01;
	const int i0 = std::max(0, int(std::floor((src.x0 - pad - art.ink.x0) / px)));
	const int i1 = std::min(art.ink.w, int(std::ceil((src.x1 + pad - art.ink.x0) / px)));
	const int j0 = std::max(0, int(std::floor((src.z0 - pad - art.ink.y0) / px)));
	const int j1 = std::min(art.ink.h, int(std::ceil((src.z1 + pad - art.ink.y0) / px)));
	FlatFace out;
	out.width = (i1 - i0) / 2;
	out.height = (j1 - j0) / 2;
	out.ink.assign(size_t(out.width) * out.height, 0);
	out.head.assign(out.ink.size(), 0);
	for (int y = 0; y < out.height; y++)
		for (int x = 0; x < out.width; x++) {
			int ink = 0, head = 0;
			for (int dy = 0; dy < 2; dy++)
				for (int dx = 0; dx < 2; dx++) {
					// Rows go top down; the grid's go up.
					int i = i0 + 2 * x + dx, j = j1 - 1 - (2 * y + dy);
					ink += art.ink.at(i, j) ? 1 : 0;
					head += art.head.at(i, j) ? 1 : 0;
				}
			out.ink[size_t(y) * out.width + x] = uint8_t(ink * 255 / 4);
			out.head[size_t(y) * out.width + x] = uint8_t(head * 255 / 4);
		}
	return out;
}

std::vector<uint8_t> generate(Bytes headBytes, Bytes maskBytes, Bytes bodyBytes, const Options& o) {
	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();
	auto last = t0;
	auto log = [&](const std::string& s) {
		if (o.log) o.log(s);
	};
	auto tick = [&](const char* stage) {
		auto now = clock::now();
		char buf[128];
		std::snprintf(buf, sizeof buf, "[%6.2fs] %s: %.2fs", std::chrono::duration<double>(now - t0).count(), stage,
		              std::chrono::duration<double>(now - last).count());
		last = now;
		log(buf);
	};
	char buf[256];

	std::vector<Part> parts = loadHead(headBytes);
	if (o.make == Make::Coin || o.make == Make::Stamp || o.make == Make::SquareStamp) {
		Mesh m = flatMii(parts, maskBytes, o, log, tick);
		std::vector<uint8_t> stl = exportSTL(m);
		tick("export");
		return stl;
	}
	if (o.make == Make::Articulated) {
		Mesh m = articulatedMii(parts, maskBytes, bodyBytes, o, log, tick);
		std::vector<uint8_t> stl = exportSTL(m);
		tick("export");
		return stl;
	}
	if (o.make == Make::Bobblehead && o.body != 'm' && o.body != 'f')
		throw std::runtime_error("bobblehead: it needs a body");
	std::vector<Part*> glass, masks, opaque;
	for (Part& p : parts) {
		if (has(p.name, "NoseLine")) continue;  // a 2D shadow quad
		if (has(p.name, "Glass"))
			glass.push_back(&p);
		else if (has(p.name, "Mask") || has(p.name, "Xlu"))
			masks.push_back(&p);
		else
			opaque.push_back(&p);
	}
	if (opaque.empty()) throw std::runtime_error("head: no opaque shapes in the GLB");

	// Weld UV-seam split vertices, but leave the open seams the shells share.
	for (Part* p : opaque) weld(p->mesh, 0.0001);
	for (Part* p : masks) weld(p->mesh, 0.0001);
	for (Part* p : opaque) subdivide(p->mesh, 3);
	for (Part* p : opaque) {
		if (p->name == "OpaCap") embossCap(p->mesh, p->texture);
		if (p->name == "OpaHair") thickenHair(p->mesh);
	}
	tick("import and smooth shells");

	// The mask_only PNG replaces the mask texture embedded in the GLB.
	Image external;
	if (maskBytes.size && !external.load(maskBytes.data, maskBytes.size)) log("Warning: could not read the mask PNG");
	const Mesh* faceline = nullptr;
	for (Part* p : opaque)
		if (p->name == "OpaFaceline") faceline = &p->mesh;
	// The chin: where a head printed alone is cut flat to stand.
	double chinZ = 1e300;
	if (faceline)
		for (const V3& v : faceline->v) chinZ = std::min(chinZ, v.z);
	std::unique_ptr<BVH> facelineBVH;
	if (faceline) facelineBVH = std::make_unique<BVH>(*faceline);
	std::vector<Mesh> pieces;
	pieces.emplace_back();  // the head goes first
	for (Part* p : masks) {
		const Image& img = has(p->name, "Mask") && !external.empty() ? external : p->texture;
		Mesh feature = embossMask(p->mesh, img, faceline, facelineBVH.get());
		if (!feature.f.empty()) pieces.push_back(std::move(feature));
	}
	facelineBVH.reset();
	tick("emboss face features");

	std::vector<Mesh> shells;
	for (Part* p : opaque) shells.push_back(std::move(p->mesh));
	Mesh head = mergeHead(shells, o.body != 0);
	shells.clear();
	V3 lo, hi;
	head.bounds(lo, hi);
	const double unit = hi.x - lo.x;
	std::snprintf(buf, sizeof buf, "Head: %d open edges, %zu faces, %.2f wide", countOpenEdges(head), head.f.size(), unit);
	log(buf);
	tick("merge head");

	if (!glass.empty()) {
		BVH headBVH(head);
		for (Part* p : glass) {
			Mesh g = buildGlasses(p->mesh, p->texture, head, headBVH, unit, kVoxelSize);
			if (!g.f.empty()) pieces.push_back(std::move(g));
		}
		tick("glasses");
	}
	pieces[0] = std::move(head);

	bool hasBody = false;
	Mesh b;
	if (o.body == 'm' || o.body == 'f') {
		if (!bodyBytes.size) throw std::runtime_error("body: no body model given");
		Body body(bodyBytes.data, bodyBytes.size, o.height, o.build);
		int moved = body.thickenLimbs(unit * kMinLimbRadius / kHeadToBody);
		V3 s = body.scale();
		std::snprintf(buf, sizeof buf,
		              "Body: height=%d build=%d scale=[%.3f, %.3f, %.3f], %d limb vertices held at the printable "
		              "minimum, head tilted %.1f deg",
		              o.height, o.build, s.x, s.y, s.z, moved, body.headTiltDegrees());
		log(buf);

		// Turn the head as the pose turns the head bone, in Z up: C R C^T.
		Mat4 r = body.headRotation();
		auto rotate = [&](const V3& p) {
			V3 y(p.x, p.z, -p.y);  // Z up to Y up
			return toZUp(r.point(y));
		};
		for (Mesh& m : pieces)
			for (V3& p : m.v) p = rotate(p);

		std::vector<Mesh> bodyParts = body.inHeadFrame();
		std::vector<std::vector<int>> tags = body.faceTags();
		for (size_t i = 0; i < bodyParts.size(); i++) {
			for (V3& p : bodyParts[i].v) p = toZUp(p);
			setFaceTags(bodyParts[i], tags[i]);  // which piece each face goes to, if split
			b.append(bodyParts[i]);
		}
		// The torso and legs share a ring of vertices at the waist (or the
		// skirt hem) and the neck is open inside the head. Weld only open
		// edges, so thin limbs never collapse, then fill what stays open.
		weld(b, unit * 1e-5);
		std::vector<char> rim = openVertices(b);
		weld(b, unit * kBodySeamWeld, &rim);
		fillHoles(b);
		subdivide(b, 3);
		fillHoles(b);
		// Thin ankles, wrists and the skirt hem snap once the figure is scaled down.
		displace(b, [&](int) { return unit * kBodyThicken; });
		hasBody = true;
		tick("body");
		if (o.make == Make::Bobblehead) {
			Mesh all = bobblehead(pieces, b, unit, o, log, tick);
			std::vector<uint8_t> stl = exportSTL(all);
			tick("export");
			return stl;
		}
		if (o.split) {
			Mesh all = splitFigure(pieces, b, body, unit, o, log, tick);
			std::vector<uint8_t> stl = exportSTL(all);
			tick("export");
			return stl;
		}
		b.uv.clear();
		pieces.push_back(std::move(b));
	}

	if (hasBody && o.base) pieces.push_back(makeBase(pieces, unit));

	// The head alone stands on a flat cut under the chin instead of a base.
	FuseExtras extras;
	if (!hasBody && o.base) {
		V3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300);
		for (const Mesh& m : pieces) {
			V3 a, b;
			m.bounds(a, b);
			lo = vmin(lo, a);
			hi = vmax(hi, b);
		}
		// Level with the chin, so it stands upright like a bust; hair
		// hanging lower behind is cut off with it.
		const double zCut = (chinZ < 1e299 ? std::max(chinZ, lo.z) : lo.z) + kHeadFlat * unit;
		Cut flat;
		flat.distance = [zCut](const V3& p) { return p.z - zCut; };
		flat.lo = V3(lo.x - unit, lo.y - unit, lo.z - unit);
		flat.hi = V3(hi.x + unit, hi.y + unit, zCut + 0.05 * unit);
		extras.cuts.push_back(flat);
		std::snprintf(buf, sizeof buf, "Flat bottom: cut %.1f%% of the head's height up", 100 * (zCut - lo.z) / (hi.z - lo.z));
		log(buf);
	}

	std::vector<const Mesh*> refs;
	for (const Mesh& m : pieces) refs.push_back(&m);
	const double voxel = unit / o.voxelsPerHead;
	Mesh solid = fuse(refs, voxel, log, extras);
	pieces.clear();
	std::snprintf(buf, sizeof buf, "Fused: %zu triangles", solid.f.size());
	log(buf);
	tick("fuse");

	if (!extras.cuts.empty()) {
		// Sanded and slimmed like the rest, the flat cut kept flat.
		finishWithCuts(solid, extras.cuts, voxel, o.maxTriangles);
		tick("sand");
		tick("specks");
	} else {
		// Voxels leave a stair-step on feature edges; two passes sand it off.
		smooth(solid, 0.5, 2);
		tick("sand");
		int dropped = dropSpecks(solid, kMinIsland);
		std::snprintf(buf, sizeof buf, "Dropped %d specks", dropped);
		log(buf);
		tick("specks");
		simplify(solid, o.maxTriangles, voxel * 0.25);
	}
	std::snprintf(buf, sizeof buf, "Printable: %zu faces, %d non-manifold edges", solid.f.size(),
	              countNonManifoldEdges(solid));
	log(buf);
	tick("tidy");

	std::vector<uint8_t> stl = exportSTL(solid);
	tick("export");
	return stl;
}

}  // namespace pmo
