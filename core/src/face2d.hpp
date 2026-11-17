// The Mii's face as flat line art, the way Nintendo's Mii stamps drew it:
// seen straight from the front, hair and beard filled in, the face and any
// hat outlined (with its own pattern drawn on), and the eyes, brows, mouth, nose line and glasses from their
// textures. Coins and stamps are carved from it.
#pragma once

#include <string>
#include <vector>

#include "geom.hpp"
#include "image.hpp"
#include "raster.hpp"

namespace pmo {

/** A part of the head as the GLB has it, Z up, facing -Y. */
struct FacePart {
	std::string name;
	const Mesh* mesh = nullptr;
	const Image* texture = nullptr;
	const Mesh* pattern = nullptr;  // where to look the texture up, if not mesh
};

struct FaceArt {
	Mask ink;   // what a stamp prints
	Mask head;  // the whole head seen from the front
};

/** The head's extent seen from the front: x across, z up (head units). */
void faceBounds(const std::vector<FacePart>& parts, double& x0, double& z0, double& x1, double& z1);

/** The art on a grid of pixel size px (head units, x across and z up), with
 *  outlines line wide and no feature thinner than minWidth. */
FaceArt drawFace(const std::vector<FacePart>& parts, double px, double line, double minWidth);

}  // namespace pmo
