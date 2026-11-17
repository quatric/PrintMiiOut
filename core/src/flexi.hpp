// The articulated Mii: the figure drawn flat from the front, like a paper
// doll, with its arms and legs on joints that print in place. Each limb ends
// in a round knob that prints inside a ring on the torso, already captured:
// the knob's edge bulges at mid-height into a matching groove in the ring,
// so it cannot drop out either way, and it swings in a slot cut for its neck.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "geom.hpp"
#include "trinkets.hpp"

namespace pmo {

struct FlexiInput {
	FaceSource face;                     // the head drawn flat (head units, x across, z up)
	std::vector<Mesh> body;              // the posed body, Z up, facing -Y, in head units
	std::vector<std::vector<int>> tags;  // per body part, per face: a Tag (split.hpp)
	V3 shoulderL, shoulderR, hipL, hipR; // the joints, in the same frame
};

struct FlexiOptions {
	double size = 0;   // mm tall, 0 for the usual size (100)
	double fit = 0;    // mm more room (less below 0) round each joint
	size_t maxTriangles = 600000;
};

/** The figure in one plate, every joint printed in place (millimetres).
 *  Throws std::runtime_error. */
Mesh makeFlexi(const FlexiInput& in, const FlexiOptions& o, const std::function<void(const std::string&)>& log,
               const std::function<void(const char*)>& tick);

/** The figure's height in head units, feet to the top of the head. */
double flexiHeight(const FlexiInput& in);

}  // namespace pmo
