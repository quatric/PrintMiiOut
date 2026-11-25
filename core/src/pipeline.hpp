// PrintMiiOut's model pipeline: a Mii head GLB (and its face mask) in, a
// printable STL out. Runs natively and, for the site, as WebAssembly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "trinkets.hpp"

namespace pmo {

struct Bytes {
	const uint8_t* data = nullptr;
	size_t size = 0;
};

struct Options {
	Make make = Make::Figure;   // a figure, a bobblehead, a coin, a round or square stamp, or an articulated Mii
	char body = 0;              // 'm', 'f', or 0 for the head alone
	int height = 64, build = 64;  // the Mii's own, 0 to 127
	bool base = true;           // a disc under the feet; the head alone: a flat cut under the chin
	bool split = false;         // the head, arms, torso and legs as pieces that snap together with pins
	bool assembled = false;     // split (or a stamp), but every piece left where it goes, for checking
	double fit = 0;             // millimetres more room (or less, below 0) per side, to suit the printer:
	                            // around every pin and between pieces that slide together (split), round the
	                            // bobblehead's pivot, and for a stamp's cap and joints
	// Coins and stamps (see TrinketOptions).
	double size = 0;            // mm, 0 for the usual size (an articulated Mii: how tall)
	bool squareCoin = false;
	bool faceCoin = false;      // a coin cut to the head's outline
	bool keychain = false;      // a loop for a keychain: on top of a head printed alone, a coin or a square stamp
	bool rim = true;
	bool engraved = false;
	std::string text;           // UTF-8
	bool textBelow = false;
	int border = 0;             // 0 none, 1 line, 2 dashed
	Bytes font;                 // an OpenType font for the text (Rodin NTLG on the site)
	double voxelsPerHead = 240;   // fuse resolution, across the head's width
	size_t maxTriangles = 600000;
	// Set: a figure, a head or a bobblehead also comes out here as a 3MF painted
	// for a multi-material printer (see kSlotNames in pipeline.cpp).
	std::vector<uint8_t>* mmu = nullptr;
	std::function<void(const std::string&)> log;
};

/** head: the Mii's GLB from the Mii API. mask: its mask_only PNG, which
 *  replaces the mask texture in the GLB (may be empty). body: a body baked
 *  by tools/bake_body.py (needed when options.body is set, for a figure or
 *  a bobblehead). Returns a binary STL. Throws std::runtime_error. */
std::vector<uint8_t> generate(Bytes head, Bytes mask, Bytes body, const Options& options);

/** The Mii's face drawn flat as line art (as coins and stamps carve it),
 *  across pixels wide: how much ink covers each pixel and how much of the
 *  head does, 0 to 255, rows top down. */
struct FlatFace {
	int width = 0, height = 0;
	std::vector<uint8_t> ink, head;
};
FlatFace drawFlatFace(Bytes head, Bytes mask, int across);

}  // namespace pmo
