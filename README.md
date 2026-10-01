# PrintMiiOut

PrintMiiOut converts Nintendo Mii characters into 3D-printable STL models. Enter an NNID/PNID, upload a binary file or QR code, and export a ready-to-print 3D model.

## Features
- **Multiple Input Sources**: Fetch Miis by Nintendo Network ID (NNID), Pretendo Network ID (PNID), binary `.mii` / `.ffsd` files, or QR code images.
- **Blender 3D Pipeline**: Processes GLB meshes with armature-bone attachment and smoothing modifiers for 3D printing.
- **Web Interface**: Clean, responsive web UI built with Flask and Vanilla CSS.

## Credits
- **3D processing**: Powered by Blender.
- **Display font**: [Nintendo U Version 3](https://www.deviantart.com/dledeviant/art/Nintendo-U-Version-3-595000916) by dledeviant, free for non-commercial use.
- **Mii rendering**: Powered by [mii-unsecure.ariankordi.net](https://mii-unsecure.ariankordi.net/) by Arian Kordi.
