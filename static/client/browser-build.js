// Builds the STL in the browser instead of on the server. The server only
// reads the form (/resolve: decrypts QR codes, looks up NNIDs, picks the
// body); the Mii itself comes straight from the Mii API, and the model is
// made by the WebAssembly core in a worker.

// The core's stages, as it reports them, and what to tell the user next.
const NEXT_STAGE = {
	'import and smooth shells': 'Embossing the face…',
	'emboss face features': 'Joining the head…',
	'merge head': 'Fitting the glasses…',
	'glasses': 'Posing the body…',
	'body': 'Fusing everything into one solid…',
	'fuse': 'Smoothing…',
	'sand': 'Tidying up…',
	'specks': 'Slimming the mesh down…',
	'tidy': 'Writing the STL…',
};

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

/**
 * form: the same FormData /submit takes. onStatus(text) hears progress.
 * Resolves to a Blob holding a binary STL.
 */
export async function buildInBrowser(form, onStatus) {
	onStatus('Looking up your Mii…');
	const res = await fetch('/resolve', { method: 'POST', body: form });
	let job;
	try {
		job = await res.json();
	} catch (_) {
		throw new Error(`Server error (${res.status}).`);
	}
	if (!res.ok) throw new Error(job.error || 'Server error.');

	onStatus('Downloading your Mii…');
	const here = new URL('./', import.meta.url);
	const [head, mask, body] = await Promise.all([
		fetchBytes(job.glb, 'the Mii model'),
		fetchBytes(job.png, 'the face texture'),
		job.body ? fetchBytes(new URL(`miiBody${job.body.toUpperCase()}.glb`, here), 'the body') : null,
	]);

	onStatus('Building your Mii in this browser…');
	const worker = new Worker(new URL('worker.js', import.meta.url), { type: 'module' });
	try {
		const stl = await new Promise((resolve, reject) => {
			worker.onmessage = ({ data }) => {
				if (data.type === 'log') {
					const stage = /^\[\s*[\d.]+s\] (.+?): /.exec(data.line);
					if (stage && NEXT_STAGE[stage[1]]) onStatus(NEXT_STAGE[stage[1]]);
				} else if (data.type === 'done') {
					resolve(data.stl);
				} else if (data.type === 'error') {
					reject(new Error(data.message));
				}
			};
			// An uncaught failure in the worker, most likely running out of memory.
			worker.onerror = (e) => {
				e.preventDefault();
				reject(new Error('Your browser ran out of room building this Mii. Try the server instead.'));
			};
			const transfer = [head.buffer, mask.buffer];
			if (body) transfer.push(body.buffer);
			worker.postMessage({
				head: head.buffer,
				mask: mask.buffer,
				body: body ? body.buffer : null,
				options: { body: job.body || '', height: job.height, build: job.build, base: true },
			}, transfer);
		});
		return new Blob([stl], { type: 'model/stl' });
	} finally {
		worker.terminate();
	}
}
