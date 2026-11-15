// PrintMiiOut's model pipeline without Blender: a Mii head GLB (and its
// face mask) in, a printable STL out. Runs natively and as WebAssembly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pmo {

struct Options {
	char body = 0;              // 'm', 'f', or 0 for the head alone
	int height = 64, build = 64;  // the Mii's own, 0 to 127
	bool base = true;           // a disc under the feet
	double voxelsPerHead = 240;   // fuse resolution, across the head's width
	size_t maxTriangles = 600000;
	std::function<void(const std::string&)> log;
};

struct Bytes {
	const uint8_t* data = nullptr;
	size_t size = 0;
};

/** head: the Mii's GLB from the Mii API. mask: its mask_only PNG, which
 *  replaces the mask texture in the GLB (may be empty). body: a body baked
 *  by tools/bake_body.py (needed when options.body is set). Returns a
 *  binary STL. Throws std::runtime_error. */
std::vector<uint8_t> generate(Bytes head, Bytes mask, Bytes body, const Options& options);

}  // namespace pmo
