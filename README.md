# PrintMiiOut

PrintMiiOut converts Nintendo Mii characters into 3D-printable STL models. Enter an NNID/PNID, upload a binary file or QR code, and export a ready-to-print 3D model.

## Features
- **Multiple Input Sources**: Fetch Miis by Nintendo Network ID (NNID), Pretendo Network ID (PNID), binary `.mii` / `.ffsd` files, or QR code images.
- **Blender 3D Pipeline**: Processes GLB meshes with smoothing modifiers and fuses everything into one watertight solid for 3D printing.
- **True Body Proportions**: The body follows the Mii's own height and build, scaled bone by bone the way Nintendo's editor scales it (the same rules as [FFL.js](https://github.com/ariankordi/FFL.js)). Arms, legs, wrists and ankles are held at a printable minimum thickness, so a very short, slim Mii keeps its look without snapping. An "Average" option uses the middle height and build instead.
- **Printable Glasses**: Glasses become a flat frame that rests on the face, with temple arms holding it to the head.
- **Build in Your Browser (beta)**: The same pipeline in C++ ([core/](core/README.md)), compiled to WebAssembly with [assimp](https://github.com/assimp/assimp) reading the Mii and writing the STL. The browser fetches the Mii straight from the Mii API and builds the model itself, so it never waits in the server's queue.
- **Web Interface**: Clean, responsive web UI built with Flask and Vanilla CSS.

## Checking the body against FFL.js

`miibody.py` poses and scales the body GLBs with plain numpy, following FFL.js bone for bone. `tools/reference` renders the same Mii with FFL.js itself, headless on WebGPU, and compares it with any STL from the pipeline:

```bash
cd tools/reference && npm install
node reference.mjs --resource AFLResHigh_2_3.dat --data <hex or base64> --stl ../../mii_head.stl --out out/mii
```

It writes the FFL.js figure as an STL and a PNG, and with `--stl` a front and side comparison (`out/mii-compare.png`) with silhouette overlap scores. `--height`, `--build` and `--body m|f` override the Mii's own values. The FFL resource file (`AFLResHigh_2_3.dat` or `FFLResHigh.dat`) comes from your own console and is not included.

To check the body alone: `python miibody.py miiBodyF_wiiu_plain.glb body.stl --height 28 --build 55` matches the body in the reference STL to within float precision.

## Building in the browser

`core/` is the pipeline without Blender, in C++: assimp imports the head and body GLBs and exports the STL, and the rest (smoothing, the face emboss, glasses, the body, fusing into one solid, simplifying) is our own code. It builds a command line tool for checking it against Blender and a WebAssembly module that the site runs in a Web Worker. The built module is checked in under `static/client/`, so the site needs no build step; see [core/README.md](core/README.md) to rebuild it.

The bodies the browser uses (`static/client/miiBody{M,F}.glb`) are the game's bodies re-saved without mesh quantization, which assimp cannot read, and with the standing pose baked in:

```bash
python tools/bake_body.py miiBodyM_wiiu_plain.glb static/client/miiBodyM.glb
python tools/bake_body.py miiBodyF_wiiu_plain.glb static/client/miiBodyF.glb
```

To compare two STLs side by side (say Blender's and the core's), `tools/reference` takes `--a blender.stl --b core.stl`, with `--face` or `--box x0,y0,z0,x1,y1,z1` to zoom in.

## Credits
- **3D processing**: Powered by Blender, and in the browser by [assimp](https://github.com/assimp/assimp) and [stb_image](https://github.com/nothings/stb).
- **Body scaling**: Rules from [FFL.js](https://github.com/ariankordi/FFL.js) by Arian Kordi, after Nintendo's own editor body.
- **Display font**: [Nintendo U Version 3](https://www.deviantart.com/dledeviant/art/Nintendo-U-Version-3-595000916) by dledeviant, free for non-commercial use.
- **Background tiles**: from [mii-unsecure](https://mii-unsecure.ariankordi.net/) by Arian Kordi, used with permission.
- **Mii rendering**: Powered by [mii-unsecure.ariankordi.net](https://mii-unsecure.ariankordi.net/) by Arian Kordi.

## License
PrintMiiOut's code is licensed under the [GNU Affero General Public License v3.0](LICENSE). If you run a modified copy as a website, you need to offer its source code to the people who use it.

Fonts, images and Mii artwork keep their own terms and are not covered by the AGPL. Rodin NTLG is a commercial font, the Mii characters, renders and favicon are based on Nintendo's Mii designs, and everything else is credited above.
