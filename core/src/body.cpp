#include "body.hpp"
#include "split.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

namespace pmo {

namespace {

const char* kRootBone = "skl_root";
const char* kHeadBone = "head";
const std::set<std::string> kYXZ = {"arm_l1", "arm_l2", "arm_r1", "arm_r2", "elbow_l", "elbow_r"};
const std::set<std::string> kScalar = {"wrist_l", "wrist_r", "shoulder_l", "shoulder_r",
                                       "ankle_l", "ankle_r", "knee_l", "knee_r"};
const std::set<std::string> kYMin1 = {"head"};
const std::set<std::string> kUnscaled = {"all_root", "body"};
const std::map<std::string, std::string> kLimbSegments = {
    {"arm_l1", "arm_l2"},   {"arm_l2", "wrist_l"},  {"elbow_l", "wrist_l"}, {"arm_r1", "arm_r2"},
    {"arm_r2", "wrist_r"},  {"elbow_r", "wrist_r"}, {"foot_l1", "foot_l2"}, {"foot_l2", "ankle_l"},
    {"knee_l", "ankle_l"},  {"foot_r1", "foot_r2"}, {"foot_r2", "ankle_r"}, {"knee_r", "ankle_r"}};
const std::set<std::string> kLimbEnds = {"wrist_l", "wrist_r", "ankle_l", "ankle_r"};

Mat4 fromAi(const aiMatrix4x4& a) {
	Mat4 m;
	const float* p = &a.a1;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) m.m[i][j] = p[i * 4 + j];
	return m;
}

}  // namespace

V3 bodyScale(int height, int build) {
	double x = (build * (height * (0.47 / 128) + 0.4)) / 128 + height * (0.23 / 128) + 0.4;
	double y = height * (0.77 / 128) + 0.5;
	return {x, y, x};
}

int Body::find(const std::string& name) const {
	for (size_t i = 0; i < nodes_.size(); i++)
		if (nodes_[i].name == name) return (int)i;
	return -1;
}

Body::Body(const uint8_t* glb, size_t size, int height, int build) {
	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFileFromMemory(glb, size, 0, "glb");
	if (!scene || !scene->mRootNode) throw std::runtime_error(std::string("body: ") + importer.GetErrorString());

	// Flatten the node tree, parents first.
	std::map<const aiNode*, int> index;
	std::function<void(const aiNode*, int)> walk = [&](const aiNode* n, int parent) {
		int id = (int)nodes_.size();
		index[n] = id;
		Node node;
		node.name = n->mName.C_Str();
		node.parent = parent;
		node.local = fromAi(n->mTransformation);
		node.world = parent < 0 ? node.local : nodes_[parent].world * node.local;
		nodes_.push_back(node);
		if (parent >= 0) nodes_[parent].children.push_back(id);
		for (unsigned i = 0; i < n->mNumChildren; i++) walk(n->mChildren[i], id);
	};
	walk(scene->mRootNode, -1);

	// The skeleton is everything under all_root (the skin's joints), which
	// includes bones no vertex hangs on: they still move their children.
	int root = find("all_root");
	if (root < 0) {
		int skl = find(kRootBone);
		if (skl < 0) throw std::runtime_error("body: no skl_root bone");
		root = nodes_[skl].parent;
	}
	isJoint_.assign(nodes_.size(), 0);
	std::function<void(int)> mark = [&](int i) {
		isJoint_[i] = 1;
		for (int c : nodes_[i].children) mark(c);
	};
	mark(root);

	// FFL.js's Skeleton.update override, bone by bone, parents first.
	scale_ = bodyScale(height, build);
	const double sx = scale_.x, sy = scale_.y, sz = scale_.z;
	std::vector<Mat4> world(nodes_.size());
	for (size_t i = 0; i < nodes_.size(); i++) world[i] = nodes_[i].world;
	scaled_.assign(nodes_.size(), Mat4());
	for (size_t i = 0; i < nodes_.size(); i++) {
		if (!isJoint_[i]) continue;
		const std::string& name = nodes_[i].name;
		Mat4 m = world[i];
		if (!kUnscaled.count(name)) {
			V3 s(sx, sy, sz);
			if (kYXZ.count(name))
				s = V3(sy, sx, sz);
			else if (kScalar.count(name))
				s = V3(sx, sx, sx);
			else if (kYMin1.count(name))
				s = V3(sx, std::max(sy, 1.0), sz);
			m = m.scaledAxes(s);
			int p = nodes_[i].parent;
			if (p >= 0 && nodes_[p].name == kRootBone) {
				V3 t = m.translation();
				m.setTranslation({t.x * sy, t.y * sy + (sx - sy), t.z * sx});
			}
			for (int c : nodes_[i].children) world[c].setTranslation((m * nodes_[c].local).translation());
		}
		scaled_[i] = m;
	}

	int head = find(kHeadBone);
	if (head < 0) throw std::runtime_error("body: no head bone");
	const Mat4& hw = scaled_[head];
	head_ = Mat4();
	for (int c = 0; c < 3; c++) {
		double len = std::sqrt(hw.m[0][c] * hw.m[0][c] + hw.m[1][c] * hw.m[1][c] + hw.m[2][c] * hw.m[2][c]);
		for (int r = 0; r < 3; r++) head_.m[r][c] = hw.m[r][c] / len / kHeadToBody;
	}
	head_.setTranslation(hw.translation());

	// Skin every mesh (glTF ignores a skinned mesh's own node transform).
	for (unsigned mi = 0; mi < scene->mNumMeshes; mi++) {
		const aiMesh* am = scene->mMeshes[mi];
		if (!am->mNumBones) continue;
		size_t nv = am->mNumVertices;
		std::vector<V3> out(nv);
		std::vector<double> best(nv, -1.0);
		std::vector<int> dom(nv, -1);
		for (unsigned b = 0; b < am->mNumBones; b++) {
			const aiBone* bone = am->mBones[b];
			int node = find(bone->mName.C_Str());
			if (node < 0) throw std::runtime_error(std::string("body: unknown bone ") + bone->mName.C_Str());
			Mat4 skin = scaled_[node] * fromAi(bone->mOffsetMatrix);
			for (unsigned w = 0; w < bone->mNumWeights; w++) {
				const aiVertexWeight& vw = bone->mWeights[w];
				const aiVector3D& p = am->mVertices[vw.mVertexId];
				out[vw.mVertexId] += skin.point(V3(p.x, p.y, p.z)) * vw.mWeight;
				if (vw.mWeight > best[vw.mVertexId]) {
					best[vw.mVertexId] = vw.mWeight;
					dom[vw.mVertexId] = node;
				}
			}
		}
		Mesh part;
		part.v = std::move(out);
		for (unsigned f = 0; f < am->mNumFaces; f++) {
			const aiFace& face = am->mFaces[f];
			if (face.mNumIndices == 3)
				part.f.push_back({(int)face.mIndices[0], (int)face.mIndices[1], (int)face.mIndices[2]});
		}
		parts_.push_back(std::move(part));
		partNames_.push_back(am->mName.C_Str());
		dominant_.push_back(std::move(dom));
	}
	if (parts_.empty()) throw std::runtime_error("body: no skinned meshes");
}

int Body::thickenLimbs(double minRadius) {
	int moved = 0;
	for (size_t p = 0; p < parts_.size(); p++) {
		Mesh& m = parts_[p];
		for (size_t i = 0; i < m.v.size(); i++) {
			int node = dominant_[p][i];
			if (node < 0) continue;
			const std::string& name = nodes_[node].name;
			V3 a = scaled_[node].translation();
			V3 axis = a;
			auto seg = kLimbSegments.find(name);
			if (seg != kLimbSegments.end()) {
				int end = find(seg->second);
				if (end < 0) continue;
				V3 d = scaled_[end].translation() - a;
				double len = length(d);
				if (len < 1e-9) continue;
				d = d / len;
				double t = std::clamp(dot(m.v[i] - a, d), 0.0, len);
				axis = a + d * t;
			} else if (!kLimbEnds.count(name)) {
				continue;
			}
			V3 off = m.v[i] - axis;
			double r = length(off);
			if (r < minRadius && r > 1e-9) {
				m.v[i] = axis + off * (minRadius / r);
				moved++;
			}
		}
	}
	return moved;
}

std::vector<std::vector<int>> Body::faceTags() const {
	auto tagOfBone = [&](int node) {
		if (node < 0) return int(kTagTorso);
		const std::string& n = nodes_[node].name;
		if (n == kHeadBone) return int(kTagNeck);
		bool arm = n.rfind("arm_", 0) == 0 || n.rfind("elbow_", 0) == 0 || n.rfind("wrist_", 0) == 0;
		if (arm) return int(n.find("_l") != std::string::npos ? kTagArmL : kTagArmR);  // arm_l1, wrist_l
		if (n.rfind("foot_", 0) == 0 || n.rfind("knee_", 0) == 0 || n.rfind("ankle_", 0) == 0) return int(kTagLegs);
		return int(kTagTorso);  // chest, hip and the shoulders, which stay on the torso
	};
	std::vector<std::vector<int>> out(parts_.size());
	for (size_t p = 0; p < parts_.size(); p++) {
		// The legs mesh is one piece, the tops tucked under the shirt or skirt included.
		bool legs = partNames_[p].rfind("legs", 0) == 0;
		for (const Tri& t : parts_[p].f) {
			if (legs) {
				out[p].push_back(kTagLegs);
				continue;
			}
			// The tag most of the corners have; with three different, the first.
			int a = tagOfBone(dominant_[p][t[0]]), b = tagOfBone(dominant_[p][t[1]]),
			    c = tagOfBone(dominant_[p][t[2]]);
			out[p].push_back(b == c ? b : a);
		}
	}
	return out;
}

V3 Body::jointInHeadFrame(const std::string& name) const {
	int i = find(name);
	if (i < 0 || !isJoint_[i]) throw std::runtime_error("body: no joint " + name);
	return (scaled_[i].translation() - head_.translation()) * kHeadToBody;
}

std::vector<Mesh> Body::inHeadFrame() const {
	std::vector<Mesh> out = parts_;
	V3 t = head_.translation();
	for (Mesh& m : out)
		for (V3& p : m.v) p = (p - t) * kHeadToBody;
	return out;
}

Mat4 Body::headRotation() const {
	Mat4 r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) r.m[i][j] = head_.m[i][j] * kHeadToBody;
	return r;
}

double Body::headTiltDegrees() const {
	Mat4 r = headRotation();
	double c = (r.m[0][0] + r.m[1][1] + r.m[2][2] - 1) / 2;
	return std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / M_PI;
}

}  // namespace pmo
