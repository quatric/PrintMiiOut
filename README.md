# PrintMiiOut

PrintMiiOut converts Nintendo Mii characters into 3D-printable STL models. Enter an NNID/PNID, upload a binary file or QR code, and export a ready-to-print 3D model.

## Features
- **Multiple Input Sources**: Fetch Miis by Nintendo Network ID (NNID), Pretendo Network ID (PNID), binary `.mii` / `.ffsd` files, or QR code images.
- **Blender 3D Pipeline**: Processes GLB meshes with armature-bone attachment and smoothing modifiers for 3D printing.
- **Web Interface**: Clean, responsive web UI built with Flask and Vanilla CSS.

## Credits
- **3D processing**: Powered by Blender.
- **Display font**: [Nintendo U Version 3](https://www.deviantart.com/dledeviant/art/Nintendo-U-Version-3-595000916) by dledeviant, free for non-commercial use.
- **Background tiles**: from [mii-unsecure](https://mii-unsecure.ariankordi.net/) by Arian Kordi, used with permission.
- **Mii rendering**: Powered by [mii-unsecure.ariankordi.net](https://mii-unsecure.ariankordi.net/) by Arian Kordi.

## License
PrintMiiOut's code is licensed under the [GNU Affero General Public License v3.0](LICENSE). If you run a modified copy as a website, you need to offer its source code to the people who use it.

Fonts, images and Mii artwork keep their own terms and are not covered by the AGPL. Rodin NTLG is a commercial font, the Mii characters, renders and favicon are based on Nintendo's Mii designs, and everything else is credited above.
