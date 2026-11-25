# PrintMiiOut core

The model pipeline in C++: a Mii head GLB and its face mask in, a printable STL out. It runs both as a command line tool and as WebAssembly in the browser, where the site builds every model.

- **assimp** (only its glTF importer) reads the GLBs; the zlib it builds deflates the multi-material 3MF. The STL is written directly.
- **stb_image** (vendored in `third_party/`) decodes the textures, and **stb_truetype** reads the lettering font.
- Everything else is here: Loop subdivision and smoothing like Blender's modifiers, the face emboss from the mask texture, the glasses frame, the body posed and scaled like FFL.js (`body.cpp`, the same rules as `miibody.py`), and the fuse.

`--split` (and `split: true` in the module) prints the figure as snap-together parts instead (`split.cpp`, and `splitFigure` in `pipeline.cpp`): plane cuts through the body that follow its bones, each piece fused on its own with its pin sockets cut exactly into its distance field, and pins with crush ribs, all laid out for printing. `--assembled` puts every piece and pin where it goes instead, for checking the fit. `--fit MM` (`fit` in the module) gives every pin, and every piece that slides onto another, that much more room per side, or less when negative, to suit a printer: each socket keeps its size and the pin is made thinner (or thicker) to match.

The fuse works like a voxel remesh. It measures the distance to every part on a grid, flood fills the outside, and contours an offset of the surface with marching tetrahedra, which can never leave a crack or a non-manifold edge. Each vertex is then placed exactly on the offset from its nearest surface point and moved back, so the result is as smooth as the parts going in. Gaps too narrow to print (an arm hanging just off the dress) are filled with a rounded web instead of leaving a row of pinholes. A quadric simplifier that refuses any collapse that would change the topology brings it down to `maxTriangles`.

`--make bobblehead` (`make: 'bobblehead'`) makes a print-in-place bobblehead (`bobblehead` in `pipeline.cpp`): the head is solidified on its own to find its centre of mass, raised clear of the shoulders, and hollowed round a ball on a post that rises from the neck, the ball just above that centre so the head hangs upright; the hollow has a pointed roof and a flared channel, so it prints without support inside, and `--fit` widens the gap round the ball.

`--make articulated` (`make: 'articulated'`, with a body) makes a flat articulated Mii whose joints print in place (`flexi.cpp`): the posed body and the head are drawn from the front, each pixel labelled torso, arm or leg by the body's bone tags, and each limb gets a knob in a ring on the torso, clear of it by `--fit` plus 0.35 mm. The knob's edge bulges at mid-height into a groove in the ring, so it is held in the plate, and the torso is cut away wherever the top of the limb sweeps as it swings, so the neck turns in a slot narrower than the knob. Every piece is a signed distance function meshed by `meshSdf`, the face's lines and each piece's edge raised on top.

`--make coin`, `keychain`, `stamp` and `square-stamp` draw the face flat instead (`face2d.cpp`): the head seen straight on, rasterized with a depth buffer, hair and beard filled, skin and cap outlined where parts meet, and the face textures and the cap's pattern traced, with lines too thin to print thickened. `trinkets.cpp` lays the face and the lettering (`text2d.cpp`, glyph outlines from stb_truetype) out on the coin or stamp face and builds every piece as a signed distance function (`sdf.hpp`) of the design's distance fields (`raster.cpp`), meshed by `meshSdf` (`sdf.cpp`: marching tetrahedra only in the blocks the surface can reach, every vertex projected onto the surface and relaxed, so it stays closed and simplifies well). The stamp faces print face up, keyed so they go in one way round. The keychain is a coin in a bezel with its ring, its name in a straight line, built in steps of 2.0, 2.6 and 3.2 mm (background, face, then lines and rim) so two filament swaps colour it.

| Option | Flag | Module | |
|---|---|---|---|
| What to make | `--make figure\|bobblehead\|coin\|keychain\|stamp\|square-stamp\|articulated` | `make` | |
| Size, mm | `--size MM` | `size` | coin across (20–80, 40; a face-shaped coin's height or width), keychain disc across (25–60, 35), round stamp face across (12–40, 20), square stamp face width (15–40, 25; 1.2× as tall), articulated Mii's height (70–200, 100) |
| Rounded square coin | `--square` | `square` | |
| Coin cut to the face's outline | `--face-shape` | `faceShape` | text goes on a tab under the chin |
| Keychain loop | `--keychain` | `keychain` | a ring at the top of a coin, on top of a square stamp, or on top of a head printed alone (the round stamp has its strap hole) |
| No rim | `--no-rim` | `rim: false` | |
| Engraved | `--engraved` | `engraved` | face and text sunk in instead of raised |
| Border | `--border none\|line\|dashed` | `border` | stamps |
| Text | `--text TEXT` | `text` | UTF-8 |
| Font | `--font font.otf` | `font` (Uint8Array) | needed for text |
| Text below | `--text-below` | `textBelow` | a straight line below the face instead of round (or above) it |
| Hair smoothing | `--hair-smoothing 0-3` | `hairSmoothing` | Loop smoothing passes on hair and beard (3); fewer keep the model's own strands and edges |
| Multi-material 3MF | `--mmu OUT.3mf` | `mmu: true` (result in `threemf`) | a figure, head or bobblehead (not split), each face painted the slot of the part it was made from: 1 skin, 2 hair, 3 black (pupils, mouth, glasses, trousers), 4 shirt and hat, 5 white (eye whites, read from the face texture), 6 eyebrows (features above the eyes), 7 base or keychain loop. Read by PrusaSlicer (`slic3rpe:mmu_segmentation`) and Bambu Studio and OrcaSlicer (`paint_color`) |

## Building

Native (command line tool, uses an installed assimp or fetches v6.0.6):

```bash
cmake -S core -B core/build-native -G Ninja -DPMO_SYSTEM_ASSIMP=ON
cmake --build core/build-native
core/build-native/printmiiout-core mii_head.glb out.stl --mask image.png \
    --body f --body-glb static/client/miiBodyF.glb --height 28 --build 55
core/build-native/printmiiout-core mii_head.glb stamp.stl --mask image.png \
    --make stamp --text Jasmine --font static/fonts/RodinNTLGPro-B-print.otf
```

WebAssembly (needs Emscripten; the build copies `printmiiout-core.js` and `.wasm` into `static/client/`):

```bash
emcmake cmake -S core -B core/build-wasm -G Ninja
cmake --build core/build-wasm
```

Pass `-DFETCHCONTENT_SOURCE_DIR_ASSIMP=/path/to/assimp` to build from a local assimp checkout instead of downloading it.

## Using the module

```js
import createPrintMiiOut from './printmiiout-core.js';
const core = await createPrintMiiOut();
const result = core.generate(headGlb, maskPng, bodyGlb, {
    make: 'figure',  // or 'bobblehead', 'coin', 'keychain', 'stamp', 'square-stamp' (see the table above)
    body: 'f', height: 28, build: 55, base: true,
    split: false,  // true: snap-together parts and their pins, laid out for printing
    fit: 0,        // mm more room (less below 0) per side: pins and sliding pieces, the bobblehead's ball, a stamp's joints
    log: (line) => console.log(line),
});
if (result.error) throw new Error(result.error);
// result.stl: Uint8Array, a binary STL
```

Sticker sheets, towels and shirts are drawn as PDFs in JavaScript (`static/client/stickers.js` and `pdf.js`), with three more functions from the module:

```js
core.outlineText(fontBytes, ['Jasmine'])  // glyph outlines in em units, curves kept, kerned
core.decodeImage(pngBytes)               // { width, height, rgba }
core.drawFace(headGlb, maskPng, 1400)    // the face as line art: { width, height, ink, head }
```

`outlineText` gives each glyph's path as a Float32Array of commands (0 move, 1 line, 2 cubic, 3 close; quadratics come out as the cubics they equal) with its advance, kerned pen position and ink box. `drawFace` draws the head flat as coins and stamps carve it (`drawFlatFace` in `pipeline.cpp`), at twice the size and averaged down, and returns how much each pixel is inked and covered by the head, 0 to 255.

`static/client/worker.js` runs it in a Web Worker and `static/client/browser-build.js` drives it from the page. The licenses of the code compiled into the module are in `static/client/THIRD_PARTY_LICENSES.txt`. A large Mii takes around 10 seconds and up to about 1 GB of memory; coins and stamps take a few seconds.

## Checking it

`tools/reference/reference.mjs` compares any STL with FFL.js's own render of the Mii (`--stl`), or two STLs with each other (`--a`, `--b`, with `--face` or `--box` to zoom in).
