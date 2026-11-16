// Splitting the figure into pieces that snap together: plane cuts through
// the body with the cross-sections closed again, round pins with crush ribs,
// and the pieces laid out for printing.
#pragma once

#include <functional>
#include <vector>

#include "fuse.hpp"
#include "geom.hpp"

namespace pmo {

/** Which piece of a split figure a body face belongs to. kTagCap marks the
 *  faces fillHoles adds, which carry no tag of their own. */
enum Tag : int { kTagCap = 0, kTagTorso = 1, kTagArmL = 2, kTagArmR = 3, kTagLegs = 4, kTagNeck = 5 };

/** Face tags ride in each face's first UV, which weld, fillHoles and
 *  subdivide all carry along. */
void setFaceTags(Mesh& m, const std::vector<int>& tags);
int faceTag(const Mesh& m, int face);

/** A cross-section outline, in the cut plane. */
using Outline = std::vector<V3>;

/** Cuts m along a plane whose normal points to the side being cut off. The
 *  part beyond the plane that faces with seed(tag) reach without crossing
 *  it moves to off, and both are closed again with the cross-section, whose
 *  outlines are returned. Returns false and leaves m as it was when no seed
 *  is beyond the plane or the part reached has a face (given by its tag
 *  and centre) that is forbidden, which means the plane does not cut it off
 *  cleanly. */
bool cutOff(Mesh& m, const V3& point, const V3& normal, const std::function<bool(int)>& seed,
            const std::function<bool(int tag, const V3& centre)>& forbidden, Mesh& off, std::vector<Outline>& outlines);

/** The closed outlines where a plane meets m, without changing m. */
std::vector<Outline> slice(const Mesh& m, const V3& point, const V3& normal);

/** The plane through pts that fits them best: their centre and the
 *  direction they spread least in. False for fewer than three points. */
bool fitPlane(const std::vector<V3>& pts, V3& centre, V3& normal);

/** The signed distance to an upright prism: the outline (lying flat)
 *  carried from z0 up to z1, on a grid of the given spacing. */
Field prismField(const Outline& outline, double z0, double z1, double voxel);

/** Where a pin goes through a cut, and how thick it is. */
struct PinSpot {
	V3 centre;
	double diameter;
};

/** One pin for a roughly round cross-section, two side by side for a long
 *  one (or whenever there is room, with preferTwo), each as thick as the
 *  walls around it allow. */
std::vector<PinSpot> pinSpots(const Outline& outline, const V3& normal, double minDiameter, double maxDiameter,
                              bool preferTwo);

/** Pin and socket sizes, in model units (millimetres at 100% scale). */
struct PinFit {
	double clearance = 0.1;  // between the pin's body and its socket, each side
	double rib = 0.22;       // how far the crush ribs stand off the body
	double endGap = 0.25;    // left free at the bottom of each socket
};

/** A pin lying on one of its flats, along X, from x = 0, standing on z = 0. */
Mesh makePin(double diameter, double length, const PinFit& fit);

/** Lays pieces out side by side on the bed, each standing on z = 0, in rows
 *  no wider than width. */
Mesh layOut(std::vector<Mesh>& pieces, double gap, double width);

}  // namespace pmo
