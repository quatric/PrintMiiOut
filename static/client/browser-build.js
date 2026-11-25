// Builds the STL in the browser, the only place PrintMiiOut builds it. The
// server only reads the form (/resolve: decrypts QR codes, looks up NNIDs,
// picks the body); the Mii itself comes straight from the Mii API, and the
// model is made by the WebAssembly core in a worker.

// The core's stages, as it reports them, and what to tell the user next.
const NEXT_STAGE = {
	'import and smooth shells': 'Embossing the face...',
	'emboss face features': 'Joining the head...',
	'merge head': 'Fitting the glasses...',
	'glasses': 'Posing the body...',
	'body': 'Fusing everything into one solid...',
	'fuse': 'Smoothing...',
	'sand': 'Tidying up...',
	'specks': 'Slimming the mesh down...',
	'tidy': 'Writing the STL...',
};
// Printing in pieces, the body is cut up and each piece made on its own.
const NEXT_SPLIT_STAGE = {
	...NEXT_STAGE,
	'body': 'Cutting the figure into pieces...',
	'split body': 'Fitting the pins...',
	'voxelize': 'Making the head...',
	'fuse head': 'Making the arms...',
	'fuse arms': 'Making the torso...',
	'fuse torso': 'Making the legs...',
	'fuse legs': 'Smoothing...',
};
// A bobblehead: the head and the body are made apart, joined by a pivot.
const NEXT_BOBBLE_STAGE = {
	...NEXT_STAGE,
	'body': 'Fitting the pivot...',
	'bobble': 'Making the head...',
	'fuse head': 'Making the body...',
	'fuse body': 'Writing the STL...',
};
// An articulated Mii: drawn flat, its joints cut, each piece carved.
const NEXT_FLEXI_STAGE = {
	'import and smooth shells': 'Drawing the face...',
	'face art': 'Posing the body...',
	'body': 'Drawing the figure...',
	'draw figure': 'Cutting the joints...',
	'cut joints': 'Carving the body...',
	'carve torso': 'Carving the arms and legs...',
	'export': 'Writing the STL...',
};
// Coins, keychains and stamps: the face drawn flat, then carved.
const NEXT_FLAT_STAGE = {
	'import and smooth shells': 'Drawing the face...',
	'face art': 'Laying out the design...',
	'design': 'Carving...',
	'carve': 'Writing the STL...',
	'carve the face': 'Shaping the handle...',
	'shape the grip': 'Shaping the cap...',
	'shape the base': 'Shaping the top...',
	'shape the top': 'Shaping the cap...',
	'shape the cap': 'Writing the STL...',
};
const FLAT = new Set(['coin', 'keychain', 'stamp', 'square-stamp']);
// Printed on paper: the browser draws a PDF.
export const PRINTS = new Set(['stickers', 'towel', 'shirt']);
// Rodin NTLG Pro B, cut down to the characters a name or a label uses.
const FONT = new URL('../fonts/RodinNTLGPro-B-print.otf', import.meta.url);

async function fetchBytes(url, what) {
	let res;
	try {
		res = await fetch(url);
	} catch (_) {
		throw new Error(`Could not download ${what}. Check your connection and try again.`);
	}
	if (res.status === 404) throw new Error('No Mii was found for that ID.');
	if (res.status >= 400 && res.status < 500) {
		throw new Error('The Mii service rejected that input. Please check it and try again.');
	}
	if (!res.ok) throw new Error('The Mii service is unavailable right now. Please try again later.');
	return new Uint8Array(await res.arrayBuffer());
}

/** Runs one job in a fresh worker; resolves to what it hands back. */
function inWorker(message, transfer, onLog) {
	const worker = new Worker(new URL('worker.js', import.meta.url), { type: 'module' });
	return new Promise((resolve, reject) => {
		worker.onmessage = ({ data }) => {
			if (data.type === 'log') onLog && onLog(data.line);
			else if (data.type === 'done') resolve(data);
			else if (data.type === 'error') reject(new Error(data.message));
		};
		// An uncaught failure in the worker, most likely running out of memory.
		worker.onerror = (e) => {
			e.preventDefault();
			reject(new Error('Your browser ran out of memory building this Mii. Close some other tabs and try again, or try it on a computer.'));
		};
		worker.postMessage(message, transfer);
	}).finally(() => worker.terminate());
}

/**
 * Stickers, a towel or a shirt as a PDF. job: what /resolve said;
 * print: the page's choices (paper, label, theme, faceStyle, ink and its
 * colour, outlines, mirror).
 */
async function buildPrint(job, print, onStatus) {
	const drawn = job.make !== 'stickers' || print.faceStyle === 'drawn';
	const [photo, head, mask, font] = await Promise.all([
		drawn ? null : fetchBytes(job.face, 'the picture of your Mii'),
		drawn ? fetchBytes(job.glb, 'the Mii model') : null,
		drawn ? fetchBytes(job.png, 'the face texture') : null,
		fetchBytes(FONT, 'the font'),
	]);
	onStatus(job.make === 'stickers' ? 'Laying out your stickers...' : 'Drawing the design...');
	const transfer = [font.buffer];
	for (const b of [photo, head, mask]) if (b) transfer.push(b.buffer);
	const { pdf, previews, pages } = await inWorker({
		print: {
			make: job.make,
			photo: photo && photo.buffer,
			head: head && head.buffer,
			mask: mask && mask.buffer,
			font: font.buffer,
			options: { ...print, text: job.text || '' },
		},
	}, transfer);
	return { file: new Blob([pdf], { type: 'application/pdf' }), previews: previews || [], pages: pages || 1 };
}

/**
 * form: the page's FormData, which /resolve reads. onStatus(text) hears
 * progress. print: the choices for something printed on paper.
 * Resolves to { file, previews, pages }: file is a Blob holding a binary
 * STL, or a PDF for stickers, a towel or a shirt; previews are PNG Blobs
 * of the PDF's first pages (none for an STL, or where the browser cannot
 * draw them), and pages how many the PDF has.
 */
export async function buildInBrowser(form, onStatus, print = {}) {
	onStatus('Looking up your Mii...');
	const res = await fetch('/resolve', { method: 'POST', body: form });
	let job;
	try {
		job = await res.json();
	} catch (_) {
		throw new Error(`Server error (${res.status}).`);
	}
	if (!res.ok) throw new Error(job.error || 'Server error.');

	onStatus('Downloading your Mii...');
	const here = new URL('./', import.meta.url);
	const make = job.make || 'figure';
	if (PRINTS.has(make)) return buildPrint(job, print, onStatus);
	const flat = FLAT.has(make);
	const lettered = flat && !!(job.text || '').trim();
	const [head, mask, body, font] = await Promise.all([
		fetchBytes(job.glb, 'the Mii model'),
		fetchBytes(job.png, 'the face texture'),
		job.body && !flat ? fetchBytes(new URL(`miiBody${job.body.toUpperCase()}.glb`, here), 'the body') : null,
		lettered ? fetchBytes(FONT, 'the font') : null,
	]);

	onStatus('Building your Mii in this browser...');
	const stages = flat ? NEXT_FLAT_STAGE
		: make === 'bobblehead' ? NEXT_BOBBLE_STAGE
		: make === 'articulated' ? NEXT_FLEXI_STAGE
		: job.split && job.body ? NEXT_SPLIT_STAGE : NEXT_STAGE;
	const transfer = [head.buffer, mask.buffer];
	if (body) transfer.push(body.buffer);
	if (font) transfer.push(font.buffer);
	const { stl, threemf } = await inWorker({
		head: head.buffer,
		mask: mask.buffer,
		body: body ? body.buffer : null,
		font: font ? font.buffer : null,
		options: {
			make,
			body: flat ? '' : job.body || '',
			height: job.height,
			build: job.build,
			base: job.base !== false,
			split: make === 'figure' && !!job.split,
			mmu: !!job.mmu,
			fit: Number(job.fit) || 0,
			size: Number(job.size) || 0,
			square: job.shape === 'square',
			faceShape: job.shape === 'face',
			keychain: !!job.keychain,
			rim: job.rim !== false,
			engraved: job.relief === 'engraved',
			text: lettered ? job.text : '',
			textBelow: job.text_pos === 'below',
			border: job.border || 'none',
		},
	}, transfer, (line) => {
		const stage = /^\[\s*[\d.]+s\] (.+?): /.exec(line);
		if (stage && stages[stage[1]]) onStatus(stages[stage[1]]);
	});
	const model = new Blob([stl], { type: 'model/stl' });
	// Multi-material: the 3MF is the download, the STL still drives the 3D view.
	if (threemf) return { file: new Blob([threemf], { type: 'model/3mf' }), model, previews: [], pages: 0 };
	return { file: model, previews: [], pages: 0 };
}
