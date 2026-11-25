// WebAssembly entry point (embind). From JavaScript:
//
//   const core = await createPrintMiiOut();
//   const result = core.generate(headGlb, maskPng, bodyGlb, {
//       make: 'figure',  // or 'bobblehead', 'coin', 'keychain', 'stamp', 'square-stamp', 'articulated'
//       body: 'f', height: 64, build: 64, base: true, split: false, fit: 0,
//       // coins and stamps:
//       size: 0, square: false, faceShape: false, rim: true, engraved: false,
//       text: '', textBelow: false, border: 'none', font: fontBytes,
//       log: (line) => console.log(line)
//   });
//   if (result.error) throw new Error(result.error);
//   // result.stl is a Uint8Array holding a binary STL; with mmu: true (a
//   // figure, head or bobblehead, not split), result.threemf holds the same
//   // print as a 3MF painted for a multi-material printer
//
// headGlb, maskPng and bodyGlb are Uint8Arrays; maskPng and bodyGlb may be
// empty (no body when options.body is unset, and none for coins and
// stamps). font is a Uint8Array holding an OpenType font, needed only for
// text.
//
// For sticker sheets, which are drawn as a PDF in JavaScript:
//
//   core.outlineText(fontBytes, ['Jasmine', ...])
//     // [{ advance, ascent, descent, missing,
//     //    glyphs: [{ cp, x, advance, box: [x0, y0, x1, y1], path }] }, ...]
//     // in em units, y up; path is a Float32Array of commands, each a code
//     // then its points: 0 move x y, 1 line x y, 2 cubic x1 y1 x2 y2 x y,
//     // 3 close. Null if the font cannot be read.
//   core.decodeImage(pngBytes)  // { width, height, rgba } or null
//   core.drawFace(headGlb, maskPng, across)
//     // { width, height, ink, head } or { error }: the face as line art,
//     // how much each pixel is inked and covered by the head, 0 to 255
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <exception>
#include <string>
#include <vector>

#include "image.hpp"
#include "pipeline.hpp"
#include "text2d.hpp"

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
		if (options["hairSmoothing"].isNumber()) o.hairSmoothing = options["hairSmoothing"].as<int>();
		std::vector<uint8_t> mmu;
		if (!options["mmu"].isUndefined() && options["mmu"].as<bool>()) o.mmu = &mmu;
		if (options["maxTriangles"].isNumber()) o.maxTriangles = size_t(options["maxTriangles"].as<double>());
		if (options["make"].isString()) {
			std::string m = options["make"].as<std::string>();
			o.make = m == "bobblehead"     ? pmo::Make::Bobblehead
			         : m == "coin"         ? pmo::Make::Coin
			         : m == "keychain"     ? pmo::Make::Keychain
			         : m == "stamp"        ? pmo::Make::Stamp
			         : m == "square-stamp" ? pmo::Make::SquareStamp
			         : m == "articulated"  ? pmo::Make::Articulated
			                               : pmo::Make::Figure;
		}
		if (options["size"].isNumber()) o.size = options["size"].as<double>();
		if (!options["square"].isUndefined()) o.squareCoin = options["square"].as<bool>();
		if (!options["faceShape"].isUndefined()) o.faceCoin = options["faceShape"].as<bool>();
		if (!options["keychain"].isUndefined()) o.keychain = options["keychain"].as<bool>();
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
		if (!mmu.empty())
			result.set("threemf", val::global("Uint8Array").new_(emscripten::typed_memory_view(mmu.size(), mmu.data())));
	} catch (const std::exception& e) {
		result.set("error", std::string(e.what()));
	}
	return result;
}

val outlineText(val fontBytes, val texts) {
	std::vector<uint8_t> data = bytes(fontBytes);
	pmo::Font font;
	if (!font.load(data.data(), data.size())) return val::null();
	val out = val::array();
	const int n = texts["length"].as<int>();
	for (int i = 0; i < n; i++) {
		val t = texts[i];
		pmo::TextPath line = font.outline(t.isString() ? t.as<std::string>() : std::string());
		val glyphs = val::array();
		for (const pmo::OutlineGlyph& g : line.glyphs) {
			val o = val::object();
			o.set("cp", double(g.cp));
			o.set("x", g.x);
			o.set("advance", g.advance);
			val box = val::array();
			box.call<void>("push", g.x0, g.y0, g.x1, g.y1);
			o.set("box", box);
			o.set("path", val::global("Float32Array").new_(emscripten::typed_memory_view(g.path.size(), g.path.data())));
			glyphs.call<void>("push", o);
		}
		val o = val::object();
		o.set("advance", line.advance);
		o.set("ascent", line.ascent);
		o.set("descent", line.descent);
		o.set("missing", line.missing);
		o.set("glyphs", glyphs);
		out.call<void>("push", o);
	}
	return out;
}

val decodeImage(val png) {
	std::vector<uint8_t> data = bytes(png);
	pmo::Image image;
	if (!image.load(data.data(), data.size())) return val::null();
	const std::vector<uint8_t>& px = image.pixels();
	val out = val::object();
	out.set("width", image.width());
	out.set("height", image.height());
	out.set("rgba", val::global("Uint8Array").new_(emscripten::typed_memory_view(px.size(), px.data())));
	return out;
}

val drawFace(val head, val mask, int across) {
	val out = val::object();
	try {
		std::vector<uint8_t> h = bytes(head), m = bytes(mask);
		pmo::FlatFace f = pmo::drawFlatFace({h.data(), h.size()}, {m.data(), m.size()}, across);
		out.set("width", f.width);
		out.set("height", f.height);
		out.set("ink", val::global("Uint8Array").new_(emscripten::typed_memory_view(f.ink.size(), f.ink.data())));
		out.set("head", val::global("Uint8Array").new_(emscripten::typed_memory_view(f.head.size(), f.head.data())));
	} catch (const std::exception& e) {
		out.set("error", std::string(e.what()));
	}
	return out;
}

}  // namespace

EMSCRIPTEN_BINDINGS(printmiiout) {
	emscripten::function("generate", &generate);
	emscripten::function("outlineText", &outlineText);
	emscripten::function("decodeImage", &decodeImage);
	emscripten::function("drawFace", &drawFace);
}
