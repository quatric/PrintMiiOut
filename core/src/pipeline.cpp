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

		Mesh b;
		for (Mesh& part : body.inHeadFrame()) {
			for (V3& p : part.v) p = toZUp(p);
			b.append(part);
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
		pieces.push_back(std::move(b));
		hasBody = true;
		tick("body");
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
