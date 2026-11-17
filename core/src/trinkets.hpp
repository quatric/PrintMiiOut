// Coins and stamps: the Mii's face as flat line art, with lettering, carved
// into a coin or the face of a stamp that works on an ink pad.
#pragma once

#include <functional>
#include <string>

#include "face2d.hpp"
#include "geom.hpp"
#include "text2d.hpp"

namespace pmo {

enum class Make { Figure, Bobblehead, Coin, Stamp, SquareStamp, Articulated };

struct TrinketOptions {
	Make make = Make::Coin;
	double size = 0;          // mm: the coin across, a stamp face across (0 picks the usual size)
	bool square = false;      // coin: a rounded square instead of a circle
	bool keychain = false;    // a ring for a keychain: at the top of a coin, on top of a square stamp (the round one has its strap hole)
	bool faceShape = false;   // coin: cut to the outline of the head instead (size is then its height or width)
	bool rim = true;          // coin: a raised rim round the edge
	bool engraved = false;    // coin: the face and lettering cut in instead of standing out
	std::string text;         // UTF-8, lettered above or below the face
	bool textBelow = false;   // a straight line under the face; otherwise round the top (or along it)
	int border = 0;           // stamps: 0 none, 1 a line round the face, 2 a dashed one
	double fit = 0;           // stamps: mm more room (less below 0) per side for the cap and joints
	bool assembled = false;   // stamps: every piece where it goes, for checking
	size_t maxTriangles = 600000;
};

/** The head seen from the front, and a way to draw it at any resolution. */
struct FaceSource {
	double x0 = 0, z0 = 0, x1 = 0, z1 = 0;  // head units
	std::function<FaceArt(double px, double line, double minWidth)> draw;
};

/** The coin or stamp, laid out for printing (millimetres). font may be
 *  null when there is no text. Throws std::runtime_error. */
Mesh makeTrinket(const FaceSource& face, const Font* font, const TrinketOptions& o,
                 const std::function<void(const std::string&)>& log, const std::function<void(const char*)>& tick);

}  // namespace pmo
