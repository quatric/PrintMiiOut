# PrintMiiOut

PrintMiiOut converts Nintendo Mii characters into 3D-printable STL models. Enter an NNID/PNID, upload a binary file or QR code, and export a ready-to-print 3D model, built right in your browser. It also prints sticker sheets, and towel and shirt designs, as PDFs for your regular printer.

## Features
- **Multiple Input Sources**: Fetch Miis by Nintendo Network ID (NNID), Pretendo Network ID (PNID), binary `.mii` / `.ffsd` files, or QR code images.
- **Built in Your Browser**: The model pipeline is C++ ([core/](core/README.md)) compiled to WebAssembly, with [assimp](https://github.com/assimp/assimp) reading the Mii and writing the STL. The browser fetches the Mii straight from the Mii API and builds the model itself, smoothing it and fusing everything into one watertight solid; the server only reads the form (decrypting QR codes and looking up NNIDs).
- **True Body Proportions**: The body follows the Mii's own height and build, scaled bone by bone the way Nintendo's editor scales it (the same rules as [FFL.js](https://github.com/ariankordi/FFL.js)). Arms, legs, wrists and ankles are held at a printable minimum thickness, so a very short, slim Mii keeps its look without snapping. An "Average" option uses the middle height and build instead.
- **Printable Glasses**: Glasses become a flat frame that rests on the face, with temple arms holding it to the head.
- **Snap-Together Parts**: Optionally, the head, torso, arms and legs (with or without the base) print as separate pieces, each standing as it stands in the figure, and press together on separate pins with crush ribs, like building bricks. The arms come off square to the upper arm where it comes free of the body, the neck stays on the torso and fits into a hole in the head (as a minifigure's does), and the legs come off just below the shirt or skirt, on two pins so they cannot turn. Each pin is as thick as the cut and both pieces allow and goes in as deep as holds it firmly, and the torso and legs keep clear of the way the head and arms slide in. A tiny Mii's legs still get two pins, and its neck pin goes at least 6 mm into the torso and the head. The pins press in with 0.12 mm to spare on their ribs; a **Pin fit** slider gives every pin and the neck up to 0.2 mm more room per side (or 0.1 mm less) for a printer whose holes come out too tight or too loose, and the page remembers it. Print at 100% scale so the pins fit (`--split` on the core's command line, with `--assembled` to check the fit with every piece in place).
- **Head Only**: Just the head in 3D, the face embossed and the glasses fitted as on the figure, standing on a flat cut level with the chin (or left round), with a keychain loop on top if you like (`--keychain`). (`--body` left off on the core's command line; `--no-base` keeps it round.)
- **Print-in-Place Bobblehead**: The head hangs on a ball at the top of a post rising from the body, and prints around it with the ball already inside, so it nods straight off the printer. The ball sits just above the head's centre of mass (found from the solid head, not its shell), so the head hangs upright and rocks back to rest; its chamber has a pointed roof and a flared channel for the post, so it prints without support inside. The head is raised clear of the shoulders to give it room to swing. Print with supports under the head, then wiggle it to free the ball. The **Fit** slider widens or narrows the gap round the ball.
- **Articulated Mii**: A flat Mii, drawn from the front like a paper doll, whose arms and legs swing on joints that print in place. Each limb ends in a round knob that prints already inside a ring on the body: the knob's edge comes to a point at mid-height and sits in a matching groove, so it cannot drop out of the plate, and its neck swings in a slot narrower than the knob, so it cannot pull out sideways. The face's lines and a line round every piece stand proud on top. It prints flat with no supports, 70 to 200 mm tall (100 by default); a tall one gives each leg its own joint, a short one hangs both legs on one. A **Joint fit** slider gives every knob more or less room for your printer.
- **Coins**: The face drawn flat like a sticker, from the same 3D head seen straight on (hair and beard filled, the face and cap outlined, the eyes, brows, mouth and glasses traced from their textures, with lines too fine to print thickened), on a round or rounded-square coin, or one cut to the outline of the head (with any text on a tab under the chin), with or without a rim, raised or engraved, and with a keychain loop at the top if you like. It can be lettered with the Mii's name (read from the Mii data) or any text, arched over the top or straight above or below, in Rodin NTLG Pro.
- **Stamps for an Ink Pad**: A round name stamp in three pieces: a bullet-shaped grip with a thumb dip and a strap hole, a face plug that prints face up (so the face comes out crisp) and presses into the grip on crush ribs with a key so it only goes in one way round, and a cap. A square stamp has a base, a face plate, a domed top that carries a raised preview of the design (and, if you like, a keychain loop), two pins and a cap. Stamped, the face and text read the right way round. Ink them with a stamp pad.
- **Sticker Sheets**: A PDF sheet of name stickers after the one Wii no Ma's Mii sticker shop printed (Miiのお名前シール): the face in big squares, circles with the name arched under it, checked ovals, rows of name labels in yellow, white, blue and green, vertical labels with the name stacked down, and striped labels, each with a dashed line to cut along. Names are vector glyph outlines in Rodin NTLG, so they print sharp at any size; the face is the Mii API's colour render, or the stamps' line art in an ink colour. Six colour schemes (classic, pastel, rainbow, pink, sky, and black and white). It prints on plain sticker paper, or on any of the Avery label sheets [The Print Shop 4](https://www.mackiev.com/printshop4/projects_matching.html?format=us) supports, US Letter or A4: each label gets a sticker that suits its shape (face and name side by side on wide labels, stacked on tall ones, arched round circles, a check on ovals, the whole sheet on full-page labels), in a mix of colours, with a little bleed past each edge, and the label edges can be drawn to test on plain paper first.
- **Towel and Shirt**: Designs after the ones printed with a Mii stamp's line art: a towel or handkerchief with a field of dots, the diagonals in ink, and the face and name small in the corner; and a shirt print with the face in a ring lettered with the name top and bottom. In the colours of Shachihata's stamp inks (blue, black, green, red, purple) or pink, mirrored for iron-on transfer paper if you like.
- **Web Interface**: Clean, responsive web UI built with Flask and Vanilla CSS. When a model is done, you can turn it round in a 3D view (WebGL, no libraries) above the download, and a PDF shows a picture of each page first.

## Checking the body against FFL.js

`miibody.py` poses and scales the body GLBs with plain numpy, following FFL.js bone for bone. `tools/reference` renders the same Mii with FFL.js itself, headless on WebGPU, and compares it with any STL PrintMiiOut made (the site's download, or one from the core's command line tool):

```bash
cd tools/reference && npm install
node reference.mjs --resource AFLResHigh_2_3.dat --data <hex or base64> --stl ../../printmiiout.stl --out out/mii
```

It writes the FFL.js figure as an STL and a PNG, and with `--stl` a front and side comparison (`out/mii-compare.png`) with silhouette overlap scores. `--height`, `--build` and `--body m|f` override the Mii's own values. The FFL resource file (`AFLResHigh_2_3.dat` or `FFLResHigh.dat`) comes from your own console and is not included.

To check the body alone: `python miibody.py miiBodyF_wiiu_plain.glb body.stl --height 28 --build 55` matches the body in the reference STL to within float precision.

## Building in the browser

`core/` is the pipeline, in C++: assimp imports the head and body GLBs and exports the STL, stb_truetype reads the lettering font, and the rest (smoothing, the face emboss, glasses, the body, fusing into one solid, simplifying, the snap-together parts, the bobblehead, and the flat face, coins and stamps) is our own code. It builds a command line tool for checking models without a browser and the WebAssembly module that the site runs in a Web Worker to build every model. The built module is checked in under `static/client/`, so the site needs no build step; see [core/README.md](core/README.md) to rebuild it.

The bodies the browser uses (`static/client/miiBody{M,F}.glb`) are the game's bodies re-saved without mesh quantization, which assimp cannot read, and with the standing pose baked in:

```bash
python tools/bake_body.py miiBodyM_wiiu_plain.glb static/client/miiBodyM.glb
python tools/bake_body.py miiBodyF_wiiu_plain.glb static/client/miiBodyF.glb
```

The font coins and stamps are lettered in (`static/fonts/RodinNTLGPro-B-print.otf`) is Rodin NTLG Pro B cut down to Latin, Greek, Cyrillic, kana, the JIS X 0208 kanji and the symbols Mii names use; the browser fetches it only when there is text (and for every printed design):

```bash
pip install fonttools
python tools/subset_font.py ~/Library/Fonts.old/FOT-RodinNTLGPro-B.otf static/fonts/RodinNTLGPro-B-print.otf
```

The label sheets (`static/client/labels.json`) are the ones The Print Shop 4 lists (`tools/labels/printshop-*.txt`), with each sheet's layout from LibreOffice's label database or gLabels' templates, and the few in neither from the margins on Avery's product pages; every layout is checked against Print Shop's label size and the paper:

```bash
python tools/labels.py labels.xml avery-us-templates.xml avery-iso-templates.xml > static/client/labels.json
```

To compare two STLs side by side (say before and after a change), `tools/reference` takes `--a before.stl --b after.stl`, with `--face` or `--box x0,y0,z0,x1,y1,z1` to zoom in.

## Credits
- **3D processing**: In the browser by [assimp](https://github.com/assimp/assimp), [stb_image](https://github.com/nothings/stb) and stb_truetype.
- **Body scaling**: Rules from [FFL.js](https://github.com/ariankordi/FFL.js) by Arian Kordi, after Nintendo's own editor body.
- **Lettering font**: Rodin NTLG Pro by Fontworks.
- **Label sheets**: Layouts from [LibreOffice](https://github.com/LibreOffice/core)'s label database (extras/source/labels/labels.xml) and [gLabels](https://github.com/jimevins/glabels-qt)' templates; the list of sheets from [The Print Shop 4](https://www.mackiev.com/printshop4/). Avery is a trademark of Avery Products Corporation.
- **Display font**: [Nintendo U Version 3](https://www.deviantart.com/dledeviant/art/Nintendo-U-Version-3-595000916) by dledeviant, free for non-commercial use.
- **Background tiles**: from [mii-unsecure](https://mii-unsecure.ariankordi.net/) by Arian Kordi, used with permission.
- **Mii rendering**: Powered by [mii-unsecure.ariankordi.net](https://mii-unsecure.ariankordi.net/) by Arian Kordi.

## License
PrintMiiOut's code is licensed under the [GNU Affero General Public License v3.0](LICENSE). If you run a modified copy as a website, you need to offer its source code to the people who use it.

Fonts, images and Mii artwork keep their own terms and are not covered by the AGPL. Rodin NTLG is a commercial font, the Mii characters, renders and favicon are based on Nintendo's Mii designs, and everything else is credited above.
