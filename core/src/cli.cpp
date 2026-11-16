// Command line front end for the core, to build models and check them
// against the Blender pipeline without a browser.
//
//   printmiiout-core head.glb out.stl [--mask image.png] [--body m|f --body-glb miiBodyM.glb]
//                    [--height N] [--build N] [--no-base] [--split] [--assembled] [--voxels 240] [--max-tris 600000]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "pipeline.hpp"

static std::vector<uint8_t> readFile(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		std::fprintf(stderr, "Cannot read %s\n", path.c_str());
		std::exit(1);
	}
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

int main(int argc, char** argv) {
	std::vector<std::string> pos;
	std::string mask, bodyGlb;
	pmo::Options o;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value\n", a.c_str());
				std::exit(1);
			}
			return argv[++i];
		};
		if (a == "--mask")
			mask = next();
		else if (a == "--body")
			o.body = next()[0];
		else if (a == "--body-glb")
			bodyGlb = next();
		else if (a == "--height")
			o.height = std::atoi(next().c_str());
		else if (a == "--build")
			o.build = std::atoi(next().c_str());
		else if (a == "--no-base")
			o.base = false;
		else if (a == "--split")
			o.split = true;
		else if (a == "--assembled")
			o.split = o.assembled = true;
		else if (a == "--voxels")
			o.voxelsPerHead = std::atof(next().c_str());
		else if (a == "--max-tris")
			o.maxTriangles = size_t(std::atol(next().c_str()));
		else
			pos.push_back(a);
	}
	if (pos.size() != 2) {
		std::fprintf(stderr,
		             "usage: printmiiout-core head.glb out.stl [--mask image.png] [--body m|f --body-glb body.glb]\n"
		             "       [--height N] [--build N] [--no-base] [--split] [--assembled] [--voxels 240] [--max-tris 600000]\n");
		return 1;
	}
	auto head = readFile(pos[0]);
	std::vector<uint8_t> maskData, bodyData;
	if (!mask.empty()) maskData = readFile(mask);
	if (o.body) bodyData = readFile(bodyGlb);
	o.log = [](const std::string& s) { std::cout << s << std::endl; };
	try {
		auto stl = pmo::generate({head.data(), head.size()}, {maskData.data(), maskData.size()},
		                         {bodyData.data(), bodyData.size()}, o);
		std::ofstream out(pos[1], std::ios::binary);
		out.write(reinterpret_cast<const char*>(stl.data()), std::streamsize(stl.size()));
		std::cout << "Done: " << pos[1] << " (" << stl.size() << " bytes)" << std::endl;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "Error: %s\n", e.what());
		return 1;
	}
	return 0;
}
