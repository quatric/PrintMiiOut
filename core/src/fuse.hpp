// Fuses overlapping shells into one watertight solid, standing in for
// Blender's voxel remesh (OpenVDB): voxelize, find the outside by flood
// fill, and contour the boundary with marching tetrahedra, which never
// leaves a crack or a non-manifold edge.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "geom.hpp"

namespace pmo {

using LogFn = std::function<void(const std::string&)>;

/** voxel: grid spacing. Every part's triangles count; a part needs no
 *  particular orientation or closure, but gaps wider than about half a
 *  voxel let the outside in. */
Mesh fuse(const std::vector<const Mesh*>& parts, double voxel, const LogFn& log);

/** Drop islands smaller than minFraction of the largest one's volume.
 *  Returns how many were dropped. */
int dropSpecks(Mesh& m, double minFraction);

/** Simplify to at most maxTriangles by quadric error edge collapse,
 *  stopping early once a collapse would move the surface more than about
 *  maxError (model units). Never changes the topology (simplify.cpp). */
void simplify(Mesh& m, size_t maxTriangles, double maxError);

}  // namespace pmo
