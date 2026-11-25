// Command line front end for the core, to build and check models without a
// browser.
//
//   printmiiout-core head.glb out.stl [--mask image.png] [--body m|f --body-glb miiBodyM.glb]
//                    [--height N] [--build N] [--no-base] [--split] [--assembled] [--fit MM] [--mmu OUT.3mf] [--hair-smoothing 0-3]
//                    [--make figure|bobblehead|coin|keychain|stamp|square-stamp|articulated]
//                    [--size MM] [--square] [--face-shape] [--keychain] [--no-rim] [--engraved] [--border none|line|dashed]
//                    [--text TEXT --font font.otf] [--text-below]
//                    [--voxels 240] [--max-tris 600000]
//
// --fit gives pins and sliding pieces that many millimetres more room per
// side (less when negative), for a printer whose holes come out too tight;
// for a bobblehead, more room round its pivot; for a stamp, round its cap and
// joints; for an articulated Mii, round each joint. --size is the coin or keychain across,
// the stamp's face across, or how tall an articulated Mii stands (mm).
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
	std::string mask, bodyGlb, fontPath, mmuPath;
	std::vector<uint8_t> mmu;
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
		else if (a == "--hair-smoothing")
			o.hairSmoothing = std::atoi(next().c_str());
		else if (a == "--mmu")
			mmuPath = next();
		else if (a == "--fit")
			o.fit = std::atof(next().c_str());
		else if (a == "--make") {
			std::string m = next();
			if (m == "figure")
				o.make = pmo::Make::Figure;
			else if (m == "bobblehead")
				o.make = pmo::Make::Bobblehead;
			else if (m == "coin")
				o.make = pmo::Make::Coin;
			else if (m == "keychain")
				o.make = pmo::Make::Keychain;
			else if (m == "stamp")
				o.make = pmo::Make::Stamp;
			else if (m == "square-stamp")
				o.make = pmo::Make::SquareStamp;
			else if (m == "articulated")
				o.make = pmo::Make::Articulated;
			else {
				std::fprintf(stderr, "--make: figure, bobblehead, coin, keychain, stamp, square-stamp or articulated\n");
				return 1;
			}
		} else if (a == "--size")
			o.size = std::atof(next().c_str());
		else if (a == "--square")
			o.squareCoin = true;
		else if (a == "--face-shape")
			o.faceCoin = true;
		else if (a == "--keychain")
			o.keychain = true;
		else if (a == "--no-rim")
			o.rim = false;
		else if (a == "--engraved")
			o.engraved = true;
		else if (a == "--border") {
			std::string b = next();
			o.border = b == "line" ? 1 : b == "dashed" ? 2 : 0;
		} else if (a == "--text")
			o.text = next();
		else if (a == "--font")
			fontPath = next();
		else if (a == "--text-below")
			o.textBelow = true;
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
		             "       [--height N] [--build N] [--no-base] [--split] [--assembled] [--fit MM] [--mmu OUT.3mf] [--hair-smoothing 0-3]\n"
		             "       [--make figure|bobblehead|coin|keychain|stamp|square-stamp|articulated]\n"
		             "       [--size MM] [--square] [--face-shape] [--keychain] [--no-rim] [--engraved] [--border none|line|dashed]\n"
		             "       [--text TEXT --font font.otf] [--text-below]\n"
		             "       [--voxels 240] [--max-tris 600000]\n");
		return 1;
	}
	auto head = readFile(pos[0]);
	std::vector<uint8_t> maskData, bodyData, fontData;
	if (!mask.empty()) maskData = readFile(mask);
	bool flat = o.make == pmo::Make::Coin || o.make == pmo::Make::Keychain || o.make == pmo::Make::Stamp || o.make == pmo::Make::SquareStamp;
	if (o.body && !flat) bodyData = readFile(bodyGlb);
	if (!fontPath.empty()) fontData = readFile(fontPath);
	o.font = {fontData.data(), fontData.size()};
	o.log = [](const std::string& s) { std::cout << s << std::endl; };
	if (!mmuPath.empty()) o.mmu = &mmu;
	try {
		auto stl = pmo::generate({head.data(), head.size()}, {maskData.data(), maskData.size()},
		                         {bodyData.data(), bodyData.size()}, o);
		std::ofstream out(pos[1], std::ios::binary);
		out.write(reinterpret_cast<const char*>(stl.data()), std::streamsize(stl.size()));
		std::cout << "Done: " << pos[1] << " (" << stl.size() << " bytes)" << std::endl;
		if (!mmu.empty()) {
			std::ofstream(mmuPath, std::ios::binary).write(reinterpret_cast<const char*>(mmu.data()), std::streamsize(mmu.size()));
			std::cout << "Multi-material: " << mmuPath << " (" << mmu.size() << " bytes)" << std::endl;
		}
	} catch (const std::exception& e) {
		std::fprintf(stderr, "Error: %s\n", e.what());
		return 1;
	}
	return 0;
}
