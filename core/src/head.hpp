// The head: smoothed shells, raised face features, glasses.
#pragma once

#include <vector>

#include "geom.hpp"
#include "image.hpp"
#include "mesh_ops.hpp"

namespace pmo {

/** Raise the features the mask texture paints (eyes, brows, mouth) into
 *  relief on the face. faceline may be null. Empty if nothing is painted. */
/** What a face feature is painted, for multi-material prints. */
enum FeatureInk : int { kInkDark = 0, kInkWhite = 1, kInkBrow = 2 };
/** paint (optional): the features flat on the face, a hair off it, every
 *  inked face kept, its FeatureInk in inks. */
Mesh embossMask(const Mesh& mask, const Image& texture, const Mesh* faceline, const BVH* facelineBVH,
                Mesh* paint = nullptr, std::vector<int>* inks = nullptr);

/** Hat stripes, pressed in from the cap texture. */
void embossCap(Mesh& cap, const Image& texture);

/** Hair grows outward a little so it stands off the face. */
void thickenHair(Mesh& hair);

/** Join the opaque shells (face, forehead, nose, hair, hat), weld their
 *  seams and close what stays open. */
Mesh mergeHead(std::vector<Mesh>& opaque, bool closeNeck, std::vector<int>* shellOf = nullptr);

/** The glasses as a flat plate cut from their texture, resting on the face
 *  with temple arms. Empty if there is no frame to cut. */
Mesh buildGlasses(const Mesh& quad, const Image& texture, const Mesh& head, const BVH& headBVH, double unit,
                  double voxelFraction);

}  // namespace pmo
