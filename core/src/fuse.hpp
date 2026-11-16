// Fuses overlapping shells into one watertight solid, the way a voxel remesh
// does: voxelize, find the outside by flood fill, and contour the boundary with marching tetrahedra, which never
// leaves a crack or a non-manifold edge.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "geom.hpp"

namespace pmo {

using LogFn = std::function<void(const std::string&)>;

/** A blind hole for a pin: a cylinder from mouth along axis (into the
 *  solid), chamfered at the mouth and ending in a cone that prints without
 *  support. */
struct Socket {
	V3 mouth, axis;
	double radius = 1;
	double depth = 4;  // to where the cone at its end starts
	/** Signed distance to the hole, negative inside it. */
	double distance(const V3& p) const;
	/** The material to leave around a bore of radius r. */
	static double wallFor(double r) { return std::min(std::max(0.5 * r, 0.6), 1.5); }
	/** How deep a bore of radius r should go to hold its pin firmly. */
	static double goodDepth(double r) { return std::min(std::max(3.2 * r, 3.0), 8.0); }
};

/** A solid's signed distance (negative inside) on a grid, kept coarsely:
 *  only within a few voxels of the surface is it more than its sign. */
struct Field {
	V3 lo;
	double voxel = 1, step = 1;
	int nx = 0, ny = 0, nz = 0;
	std::vector<int8_t> q;
	double sample(const V3& p) const;
	bool empty() const { return q.empty(); }
};

/** Keep clear of another piece as it slides into place: whatever lies in
 *  its path, from travel back along dir to where it ends, or within
 *  clearance of that, is cut away. Its own joint face is left alone: a slab
 *  of the given thickness before the joint plane (point, normal pointing to
 *  the other piece), within radius of the point. */
struct Carve {
	const Field* other = nullptr;
	V3 dir;  // the way the other piece moves as it goes in
	double travel = 4, clearance = 0.15;
	V3 jointPoint, jointNormal;
	double jointRadius = 0, jointSlab = 0;
};

/** Any other hollow to leave: its signed distance (negative inside the
 *  hollow), looked at only within the box. Cut exactly, like a socket. */
struct Cut {
	std::function<double(const V3&)> distance;
	V3 lo, hi;
};

struct FuseExtras {
	std::vector<Socket>* sockets = nullptr;
	std::vector<Cut> cuts;
	std::vector<Carve> carves;
	Field* field = nullptr;  // filled with the result's signed distance
};

/** Parts on a grid, before contouring: the distance to them, and which
 *  voxels are outside once narrow gaps are filled. */
struct Voxels {
	V3 lo;
	double voxel = 1;
	int nx = 0, ny = 0, nz = 0;
	std::vector<uint16_t> dist, gq;
	std::vector<uint8_t> state;
	double toQ = 1, tunnel = 0, wide = 0;
	uint16_t wallQ = 0;
	Mesh parts;  // all of them in one, to place the surface exactly
	bool solidAt(const V3& p) const;
	/** How deep a bore of radius r can go from mouth along axis, up to
	 *  maxDepth, with its wall (Socket::wallFor) and the cone past its end
	 *  still inside the solid. A bore leaning to the joint face (normal
	 *  pointing out of the solid) may break out through that face. */
	double room(const V3& mouth, const V3& axis, double r, double maxDepth, const V3& face) const;
};

/** voxel: grid spacing. Every part's triangles count; a part needs no
 *  particular orientation or closure, but gaps wider than about half a
 *  voxel let the outside in. */
Voxels voxelize(const std::vector<const Mesh*>& parts, double voxel, const LogFn& log);

/** The voxels' surface, with sockets and carves cut into it. */
Mesh contour(Voxels& voxels, const LogFn& log, const FuseExtras& extras = {});

/** voxelize, then contour. */
Mesh fuse(const std::vector<const Mesh*>& parts, double voxel, const LogFn& log, const FuseExtras& extras = {});

/** Drop islands smaller than minFraction of the largest one's volume.
 *  Returns how many were dropped. */
int dropSpecks(Mesh& m, double minFraction);

/** Simplify to at most maxTriangles by quadric error edge collapse,
 *  stopping early once a collapse would move the surface more than about
 *  maxError (model units). Never changes the topology, and never moves a
 *  vertex marked in locked (simplify.cpp). flatFirst first collapses, in
 *  cheap passes, the vertices inside flat regions, for meshes that have
 *  many of them. */
void simplify(Mesh& m, size_t maxTriangles, double maxError, const std::vector<char>* locked = nullptr,
              bool flatFirst = false);

}  // namespace pmo
