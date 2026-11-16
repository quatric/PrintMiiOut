// The Mii body, posed and scaled per bone from the Mii's height and build.
// A C++ port of miibody.py, which follows FFL.js; see that file for the rules.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "geom.hpp"

namespace pmo {

constexpr double kHeadToBody = 7.0;

V3 bodyScale(int height, int build);

class Body {
public:
	/** glb: a body baked by tools/bake_body.py (Finish pose in the node
	 *  transforms, float attributes), read with assimp. Throws on failure. */
	Body(const uint8_t* glb, size_t size, int height, int build);

	/** Hold every limb at least minRadius (body units) from its bone.
	 *  Returns how many vertices moved. */
	int thickenLimbs(double minRadius);

	/** The parts around the head model's origin, in head units, Y up,
	 *  standing upright (only moved under the head, never turned). */
	std::vector<Mesh> inHeadFrame() const;

	/** The rotation the pose gives the head bone (Y up), as a matrix
	 *  with no translation. */
	Mat4 headRotation() const;
	double headTiltDegrees() const;
	V3 scale() const { return scale_; }

	/** Per part, per face: which piece of a split figure it belongs to
	 *  (a Tag from split.hpp), from the part and the bones its corners follow. */
	std::vector<std::vector<int>> faceTags() const;

	/** A joint's position in the frame inHeadFrame() uses. Throws if there is none. */
	V3 jointInHeadFrame(const std::string& name) const;

private:
	struct Node {
		std::string name;
		int parent = -1;
		std::vector<int> children;
		Mat4 local, world;
	};
	std::vector<Node> nodes_;
	std::vector<Mat4> scaled_;  // per node: the scaled world matrix (joints only)
	std::vector<char> isJoint_;
	std::vector<Mesh> parts_;               // skinned, world space, Y up
	std::vector<std::string> partNames_;
	std::vector<std::vector<int>> dominant_;  // per part, per vertex: node it follows most
	Mat4 head_;
	V3 scale_;
	int find(const std::string& name) const;
};

}  // namespace pmo
