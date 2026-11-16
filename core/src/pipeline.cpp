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
#include "fuse.hpp"
#include "geom.hpp"
#include "head.hpp"
#include "image.hpp"
#include "mesh_ops.hpp"
#include "split.hpp"

namespace pmo {

namespace {

// Sizes are fractions of head width, as in printmiiout.py.
const double kVoxelSize = 1.0 / 240;  // glasses grid, whatever the fuse resolution
const double kBaseRadius = 0.45;
const double kBaseHeight = 0.05;
const double kBaseSink = 0.025;
const double kBodyThicken = 0.006;
const double kMinLimbRadius = 0.03;
const double kBodySeamWeld = 0.003;
const double kMinIsland = 0.01;

struct Part {
	std::string name;
	Mesh mesh;
	Image texture;
};

bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

/** glTF is Y up, Blender (and these meshes) Z up: (x, y, z) -> (x, -z, y). */
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
					c[k] = {t.x, t.y};  // assimp flips V up, as Blender does
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
	const PinFit fit;
	const double clearance = 0.15;  // between pieces that slide past each other
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
			PinSpot spot = pinSpots(outlines[0], axis, 1.6, 5.0, false)[0];
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
			// Legs coming out as one (a pelvis) take two pins, so they cannot turn.
			for (const PinSpot& sp : pinSpots(ol, j.normal, 2.0, 5.0, ji == kHips && j.outlines.size() == 1)) {
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
			std::snprintf(buf, sizeof buf, "Pin for the %s: %.2f mm thick, %.2f long (sockets %.2f and %.2f deep)", j.name,
			              diameter, length, j.on[s].depth, j.off[s].depth);
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

	std::vector<const Mesh*> refs;
	for (const Mesh& m : pieces) refs.push_back(&m);
	const double voxel = unit / o.voxelsPerHead;
	Mesh solid = fuse(refs, voxel, log);
	pieces.clear();
	std::snprintf(buf, sizeof buf, "Fused: %zu triangles", solid.f.size());
	log(buf);
	tick("fuse");

	// Voxels leave a stair-step on feature edges; two passes sand it off.
	smooth(solid, 0.5, 2);
	tick("sand");
	int dropped = dropSpecks(solid, kMinIsland);
	std::snprintf(buf, sizeof buf, "Dropped %d specks", dropped);
	log(buf);
	tick("specks");
	simplify(solid, o.maxTriangles, voxel * 0.25);
	std::snprintf(buf, sizeof buf, "Printable: %zu faces, %d non-manifold edges", solid.f.size(),
	              countNonManifoldEdges(solid));
	log(buf);
	tick("tidy");

	std::vector<uint8_t> stl = exportSTL(solid);
	tick("export");
	return stl;
}

}  // namespace pmo
