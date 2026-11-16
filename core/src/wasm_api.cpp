// WebAssembly entry point (embind). From JavaScript:
//
//   const core = await createPrintMiiOut();
//   const result = core.generate(headGlb, maskPng, bodyGlb, {
//       make: 'figure',  // or 'bobblehead', 'coin', 'stamp', 'square-stamp'
//       body: 'f', height: 64, build: 64, base: true, split: false, fit: 0,
//       // coins and stamps:
//       size: 0, square: false, rim: true, engraved: false,
//       text: '', textBelow: false, border: 'none', font: fontBytes,
//       log: (line) => console.log(line)
//   });
//   if (result.error) throw new Error(result.error);
//   // result.stl is a Uint8Array holding a binary STL
//
// headGlb, maskPng and bodyGlb are Uint8Arrays; maskPng and bodyGlb may be
// empty (no body when options.body is unset, and none for coins and
// stamps). font is a Uint8Array holding an OpenType font, needed only for
// text.
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <exception>
#include <string>
#include <vector>

#include "pipeline.hpp"

using emscripten::val;

namespace {

std::vector<uint8_t> bytes(const val& v) {
	if (v.isUndefined() || v.isNull()) return {};
	return emscripten::convertJSArrayToNumberVector<uint8_t>(v);
}

val generate(val head, val mask, val body, val options) {
	val result = val::object();
	try {
		pmo::Options o;
		val b = options["body"];
		if (b.isString()) {
			std::string s = b.as<std::string>();
			if (!s.empty()) o.body = s[0];
		}
		if (options["height"].isNumber()) o.height = options["height"].as<int>();
		if (options["build"].isNumber()) o.build = options["build"].as<int>();
		if (!options["base"].isUndefined()) o.base = options["base"].as<bool>();
		if (!options["split"].isUndefined()) o.split = options["split"].as<bool>();
		if (!options["assembled"].isUndefined()) o.assembled = options["assembled"].as<bool>();
		if (options["fit"].isNumber()) o.fit = options["fit"].as<double>();
		if (options["voxelsPerHead"].isNumber()) o.voxelsPerHead = options["voxelsPerHead"].as<double>();
		if (options["maxTriangles"].isNumber()) o.maxTriangles = size_t(options["maxTriangles"].as<double>());
		if (options["make"].isString()) {
			std::string m = options["make"].as<std::string>();
			o.make = m == "bobblehead"     ? pmo::Make::Bobblehead
			         : m == "coin"         ? pmo::Make::Coin
			         : m == "stamp"        ? pmo::Make::Stamp
			         : m == "square-stamp" ? pmo::Make::SquareStamp
			                               : pmo::Make::Figure;
		}
		if (options["size"].isNumber()) o.size = options["size"].as<double>();
		if (!options["square"].isUndefined()) o.squareCoin = options["square"].as<bool>();
		if (!options["rim"].isUndefined()) o.rim = options["rim"].as<bool>();
		if (!options["engraved"].isUndefined()) o.engraved = options["engraved"].as<bool>();
		if (options["text"].isString()) o.text = options["text"].as<std::string>();
		if (!options["textBelow"].isUndefined()) o.textBelow = options["textBelow"].as<bool>();
		if (options["border"].isString()) {
			std::string kind = options["border"].as<std::string>();
			o.border = kind == "line" ? 1 : kind == "dashed" ? 2 : 0;
		}
		std::vector<uint8_t> font = bytes(options["font"]);
		o.font = {font.data(), font.size()};
		val log = options["log"];
		if (log.typeOf().as<std::string>() == "function") o.log = [log](const std::string& line) { log(line); };
		else o.log = [](const std::string&) {};

		std::vector<uint8_t> h = bytes(head), m = bytes(mask), bd = bytes(body);
		std::vector<uint8_t> stl = pmo::generate({h.data(), h.size()}, {m.data(), m.size()},
		                                         {bd.data(), bd.size()}, o);
		// new Uint8Array(view) copies out of the module's memory.
		result.set("stl", val::global("Uint8Array").new_(emscripten::typed_memory_view(stl.size(), stl.data())));
	} catch (const std::exception& e) {
		result.set("error", std::string(e.what()));
	}
	return result;
}

}  // namespace

EMSCRIPTEN_BINDINGS(printmiiout) {
	emscripten::function("generate", &generate);
}
