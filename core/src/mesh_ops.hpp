// Mesh operations the pipeline needs, standing in for the Blender modifiers
// and bmesh operators printmiiout.py uses.
#pragma once

#include <functional>
#include <vector>

#include "geom.hpp"

namespace pmo {

/** Merge vertices closer than dist (bmesh remove_doubles). If only is given,
 *  just the vertices marked in it merge. Drops faces that collapse. */
void weld(Mesh& m, double dist, const std::vector<char>* only = nullptr);

/** Drop faces with repeated corners and vertices no face uses. */
void compact(Mesh& m);

/** Edges used by exactly one face. */
int countOpenEdges(const Mesh& m);

/** Edges not shared by exactly two faces (open or non-manifold). */
int countNonManifoldEdges(const Mesh& m);

/** Vertices on an open edge. */
std::vector<char> openVertices(const Mesh& m);

/** Close every hole with a fan around its centre (bmesh holes_fill).
 *  Returns how many holes were filled. */
int fillHoles(Mesh& m);

/** Loop subdivision, smoothing like Blender's Subdivision Surface modifier,
 *  with open edges kept as curves and corners pinned. Carries UVs. */
void subdivide(Mesh& m, int levels);

/** Area weighted vertex normals. */
std::vector<V3> vertexNormals(const Mesh& m);

/** Blender's Smooth modifier: each vertex moves factor of the way to the
 *  average of its neighbours, iterations times. */
void smooth(Mesh& m, double factor, int iterations, const std::vector<char>* only = nullptr);

/** Blender's Solidify modifier: thicken a surface into a closed shell.
 *  offset -1 grows it inward, 1 outward, 0 both ways. */
void solidify(Mesh& m, double thickness, double offset);

/** Move each vertex along its normal by amount(vertex). */
void displace(Mesh& m, const std::function<double(int)>& amount);

/** Face islands joined by shared vertices: the island of each face, and how many. */
std::vector<int> faceIslands(const Mesh& m, int& count);

double triangleArea(const Mesh& m, int face);

/** Signed volume of each island (positive for outward facing shells). */
std::vector<double> islandVolumes(const Mesh& m, const std::vector<int>& island, int count);

/** Keep only the faces for which keep(face) is true. */
void keepFaces(Mesh& m, const std::function<bool(int)>& keep);

/** Bounding volume hierarchy over a mesh's triangles, for ray casts and
 *  nearest-point queries. Keeps a reference to the mesh. */
class BVH {
public:
	explicit BVH(const Mesh& m);
	/** Nearest hit along a ray (dir normalized, both faces), within maxT. */
	bool raycast(const V3& origin, const V3& dir, double maxT, double& t) const;
	/** Nearest point on the surface, and the face it is on. */
	V3 closest(const V3& p, int* face = nullptr) const;

private:
	struct Node {
		V3 lo, hi;
		int left = -1, right = -1;  // children, or -1 for a leaf
		int start = 0, count = 0;   // leaf triangles in order_
	};
	const Mesh& mesh_;
	std::vector<Node> nodes_;
	std::vector<int> order_;
	int build(int start, int count, std::vector<V3>& centres, int depth);
};

V3 closestOnTriangle(const V3& p, const V3& a, const V3& b, const V3& c);

}  // namespace pmo
