// Runs the PrintMiiOut core (WebAssembly) off the page's thread. One build
// per worker: the page starts a fresh worker for each model and ends it
// afterwards, which hands the module's memory back to the browser.
//
// A model:
//   In:  { head, mask, body, font, options }  (ArrayBuffers; body and font
//        may be null)
//   Out: { type: 'log', line } while it works, then
//        { type: 'done', stl } (an ArrayBuffer) or { type: 'error', message }
// Something printed on paper (stickers, a towel, a tote bag):
//   In:  { print: { make, photo, head, mask, font, options } }  (photo, or
//        head and mask for line art; options as stickers.js takes them)
//   Out: { type: 'done', pdf } or { type: 'error', message }
import createPrintMiiOut from './printmiiout-core.js';

async function print({ make, photo, head, mask, font, options }) {
	const core = await createPrintMiiOut();
	const { makeStickers, makeTowel, makeTote, INKS } = await import('./stickers.js');
	const face = {};
	if (head) {
		const drawn = core.drawFace(new Uint8Array(head), new Uint8Array(mask || 0), 1400);
		if (drawn.error) throw new Error(drawn.error);
		face.drawn = drawn;
		face.ink = INKS[options.ink] || INKS.blue;
	} else {
		const image = core.decodeImage(new Uint8Array(photo));
		if (!image) throw new Error('Could not read the picture of your Mii.');
		face.photo = image;
	}
	const fontBytes = font ? new Uint8Array(font) : null;
	const fn = make === 'towel' ? makeTowel : make === 'tote' ? makeTote : makeStickers;
	return fn(core, fontBytes, face, options);
}

self.onmessage = async (event) => {
	if (event.data.print) {
		try {
			const pdf = await print(event.data.print);
			self.postMessage({ type: 'done', pdf: pdf.buffer }, [pdf.buffer]);
		} catch (err) {
			self.postMessage({ type: 'error', message: String((err && err.message) || err) });
		}
		return;
	}
	const { head, mask, body, font, options } = event.data;
	try {
		const core = await createPrintMiiOut();
		const result = core.generate(
			new Uint8Array(head),
			new Uint8Array(mask || 0),
			new Uint8Array(body || 0),
			{
				...options,
				font: new Uint8Array(font || 0),
				log: (line) => self.postMessage({ type: 'log', line }),
			}
		);
		if (result.error) throw new Error(result.error);
		self.postMessage({ type: 'done', stl: result.stl.buffer }, [result.stl.buffer]);
	} catch (err) {
		self.postMessage({ type: 'error', message: String((err && err.message) || err) });
	}
};
