// Runs the PrintMiiOut core (WebAssembly) off the page's thread. One build
// per worker: the page starts a fresh worker for each model and ends it
// afterwards, which hands the module's memory back to the browser.
//
// In:  { head, mask, body, font, options }  (ArrayBuffers; body and font
//      may be null)
// Out: { type: 'log', line } while it works, then
//      { type: 'done', stl } (an ArrayBuffer) or { type: 'error', message }
import createPrintMiiOut from './printmiiout-core.js';

self.onmessage = async (event) => {
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
