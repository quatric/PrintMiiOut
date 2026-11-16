# PrintMiiOut core

The model pipeline in C++, without Blender: a Mii head GLB and its face mask in, a printable STL out. It does what `printmiiout.py` does in Blender, step for step, and runs both as a command line tool and as WebAssembly in the browser.

- **assimp** (only its glTF importer and STL exporter) reads the GLBs and writes the STL.
- **stb_image** (vendored in `third_party/`) decodes the textures.
- Everything else is here: Loop subdivision and smoothing like Blender's modifiers, the face emboss from the mask texture, the glasses frame, the body posed and scaled like FFL.js (`body.cpp`, the same rules as `miibody.py`), and the fuse.

`--split` (and `split: true` in the module) prints the figure as snap-together parts instead (`split.cpp`, and `splitFigure` in `pipeline.cpp`): plane cuts through the body that follow its bones, each piece fused on its own with its pin sockets cut exactly into its distance field, and pins with crush ribs, all laid out for printing. `--assembled` puts every piece and pin where it goes instead, for checking the fit. `miisplit.py` does the same for the Blender pipeline.

The fuse stands in for Blender's voxel remesh. It measures the distance to every part on a grid, flood fills the outside, and contours an offset of the surface with marching tetrahedra, which can never leave a crack or a non-manifold edge. Each vertex is then placed exactly on the offset from its nearest surface point and moved back, so the result is as smooth as the parts going in. Gaps too narrow to print (an arm hanging just off the dress) are filled with a rounded web instead of leaving a row of pinholes. A quadric simplifier that refuses any collapse that would change the topology brings it down to `maxTriangles`.

## Building

Native (command line tool, uses an installed assimp or fetches v6.0.6):

```bash
cmake -S core -B core/build-native -G Ninja -DPMO_SYSTEM_ASSIMP=ON
cmake --build core/build-native
core/build-native/printmiiout-core mii_head.glb out.stl --mask image.png \
    --body f --body-glb static/client/miiBodyF.glb --height 28 --build 55
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
    body: 'f', height: 28, build: 55, base: true,
    split: false,  // true: snap-together parts and their pins, laid out for printing
    log: (line) => console.log(line),
});
if (result.error) throw new Error(result.error);
// result.stl: Uint8Array, a binary STL
```

`static/client/worker.js` runs it in a Web Worker and `static/client/browser-build.js` drives it from the page. The licenses of the code compiled into the module are in `static/client/THIRD_PARTY_LICENSES.txt`. A large Mii takes around 10 seconds and up to about 1 GB of memory.

## Checking it

`tools/reference/reference.mjs` compares any STL with FFL.js's own render of the Mii (`--stl`), or two STLs with each other (`--a`, `--b`, with `--face` or `--box` to zoom in).
