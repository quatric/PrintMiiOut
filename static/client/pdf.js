// A small PDF writer for sticker sheets: vector paths in millimetres with y
// running down the page, RGB images with an alpha mask, and shapes defined
// once and drawn many times (the glyphs of a name, say). Streams are
// deflated with the browser's CompressionStream.

const MM = 72 / 25.4;  // points per millimetre

/** A number as PDF writes it: short, no exponent. */
function num(v) {
	if (!Number.isFinite(v)) return '0';
	const s = (Math.round(v * 1000) / 1000).toFixed(3);
	return s.replace(/\.?0+$/, '') || '0';
}

function rgb(c) {
	const n = parseInt(c.slice(1), 16);
	return [(n >> 16 & 255) / 255, (n >> 8 & 255) / 255, (n & 255) / 255].map(v => num(v)).join(' ');
}

async function deflate(bytes) {
	const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate'));
	return new Uint8Array(await new Response(stream).arrayBuffer());
}

const encoder = new TextEncoder();

/**
 * Drawing operations on one page or shape. Paths are built with moveTo,
 * lineTo, curveTo, rect, roundRect, ellipse and closePath, then painted
 * with fill, stroke or clip.
 */
export class Painter {
	constructor() { this.ops = []; }
	op(s) { this.ops.push(s); return this; }

	save() { return this.op('q'); }
	restore() { return this.op('Q'); }
	/** Maps (x, y) to (a x + c y + e, b x + d y + f) from now on. */
	transform(a, b, c, d, e, f) { return this.op(`${num(a)} ${num(b)} ${num(c)} ${num(d)} ${num(e)} ${num(f)} cm`); }
	translate(x, y) { return this.transform(1, 0, 0, 1, x, y); }
	scale(sx, sy = sx) { return this.transform(sx, 0, 0, sy, 0, 0); }
	rotate(rad) {
		const c = Math.cos(rad), s = Math.sin(rad);
		return this.transform(c, s, -s, c, 0, 0);
	}

	fillColor(c) { return this.op(`${rgb(c)} rg`); }
	strokeColor(c) { return this.op(`${rgb(c)} RG`); }
	lineWidth(w) { return this.op(`${num(w)} w`); }
	/** Dashes of on and off (mm), or none for a solid line. */
	dash(on, off) { return this.op(on ? `[${num(on)} ${num(off)}] 0 d` : '[] 0 d'); }
	lineCap(n) { return this.op(`${n} J`); }

	moveTo(x, y) { return this.op(`${num(x)} ${num(y)} m`); }
	lineTo(x, y) { return this.op(`${num(x)} ${num(y)} l`); }
	curveTo(x1, y1, x2, y2, x, y) { return this.op(`${num(x1)} ${num(y1)} ${num(x2)} ${num(y2)} ${num(x)} ${num(y)} c`); }
	closePath() { return this.op('h'); }
	rect(x, y, w, h) { return this.op(`${num(x)} ${num(y)} ${num(w)} ${num(h)} re`); }
	roundRect(x, y, w, h, r) {
		r = Math.max(0, Math.min(r, w / 2, h / 2));
		if (r <= 0) return this.rect(x, y, w, h);
		const k = r * 0.5523;  // a quarter circle as one cubic
		return this.moveTo(x + r, y).lineTo(x + w - r, y).curveTo(x + w - r + k, y, x + w, y + r - k, x + w, y + r)
			.lineTo(x + w, y + h - r).curveTo(x + w, y + h - r + k, x + w - r + k, y + h, x + w - r, y + h)
			.lineTo(x + r, y + h).curveTo(x + r - k, y + h, x, y + h - r + k, x, y + h - r)
			.lineTo(x, y + r).curveTo(x, y + r - k, x + r - k, y, x + r, y).closePath();
	}
	ellipse(cx, cy, rx, ry) {
		const kx = rx * 0.5523, ky = ry * 0.5523;
		return this.moveTo(cx + rx, cy).curveTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry)
			.curveTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy)
			.curveTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry)
			.curveTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy).closePath();
	}
	fill() { return this.op('f'); }
	fillEvenOdd() { return this.op('f*'); }
	stroke() { return this.op('S'); }
	/** Clips to the path built so far (nonzero), for what follows until restore. */
	clip() { return this.op('W n'); }
	/** Draws an image or shape added to the document, its unit square (or
	 *  its own space, for a shape) mapped by the current transform. */
	draw(name) { return this.op(`/${name} Do`); }
	/** An image filling the box x, y, w, h (y down, so it is turned upright). */
	image(name, x, y, w, h) {
		return this.save().transform(w, 0, 0, -h, x, y + h).draw(name).restore();
	}

	text() { return this.ops.join('\n'); }
}

const CAPS = ['butt', 'round', 'square'];

/**
 * Plays a painter's operations on a 2D canvas, which transforms, builds
 * paths and keeps its state the way PDF does. bitmaps and shapes map the
 * names drawn with Do to an ImageBitmap or a shape.
 */
function paint(ctx, ops, bitmaps, shapes) {
	const css = (t) => `rgb(${Math.round(t[0] * 255)}, ${Math.round(t[1] * 255)}, ${Math.round(t[2] * 255)})`;
	ctx.beginPath();
	for (const op of ops) {
		if (op === 'W n') { ctx.clip(); ctx.beginPath(); continue; }
		const t = op.split(' ');
		const name = t.pop();
		const n = t.map(Number);
		switch (name) {
			case 'q': ctx.save(); break;
			case 'Q': ctx.restore(); break;
			case 'cm': ctx.transform(n[0], n[1], n[2], n[3], n[4], n[5]); break;
			case 'rg': ctx.fillStyle = css(n); break;
			case 'RG': ctx.strokeStyle = css(n); break;
			case 'w': ctx.lineWidth = n[0]; break;
			case 'J': ctx.lineCap = CAPS[n[0]] || 'butt'; break;
			case 'd': {
				const m = /^\[([^\]]*)\]\s+(\S+)/.exec(op);
				ctx.setLineDash(m && m[1].trim() ? m[1].trim().split(/\s+/).map(Number) : []);
				ctx.lineDashOffset = m ? Number(m[2]) : 0;
				break;
			}
			case 'm': ctx.moveTo(n[0], n[1]); break;
			case 'l': ctx.lineTo(n[0], n[1]); break;
			case 'c': ctx.bezierCurveTo(n[0], n[1], n[2], n[3], n[4], n[5]); break;
			case 'h': ctx.closePath(); break;
			case 're': ctx.rect(n[0], n[1], n[2], n[3]); break;
			case 'f': ctx.fill('nonzero'); ctx.beginPath(); break;
			case 'f*': ctx.fill('evenodd'); ctx.beginPath(); break;
			case 'S': ctx.stroke(); ctx.beginPath(); break;
			case 'Do': {
				const key = t[0].slice(1);
				const bitmap = bitmaps.get(key);
				ctx.save();
				if (bitmap) {
					// The image's top row is at the top of its unit square.
					ctx.transform(1, 0, 0, -1, 0, 1);
					ctx.drawImage(bitmap, 0, 0, 1, 1);
				} else if (shapes.has(key)) paint(ctx, shapes.get(key).painter.ops, bitmaps, shapes);
				ctx.restore();
				ctx.beginPath();
				break;
			}
		}
	}
}

/** A PDF document of pages, images and reusable shapes. */
export class PdfDocument {
	constructor() {
		this.pages = [];
		this.images = [];  // { name, width, height, rgb, alpha }
		this.shapes = [];  // { name, bbox, painter }
	}

	/** RGBA8 pixels (rows top to bottom); returns the name to draw it by. */
	addImage(width, height, rgba, { gray = false } = {}) {
		const n = width * height;
		const color = new Uint8Array(n * (gray ? 1 : 3));
		const alpha = new Uint8Array(n);
		let opaque = true;
		for (let i = 0; i < n; i++) {
			const r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
			if (gray) color[i] = Math.round(0.299 * r + 0.587 * g + 0.114 * b);
			else { color[i * 3] = r; color[i * 3 + 1] = g; color[i * 3 + 2] = b; }
			alpha[i] = a;
			if (a !== 255) opaque = false;
		}
		const name = `Im${this.images.length}`;
		this.images.push({ name, width, height, gray, color, alpha: opaque ? null : alpha });
		return name;
	}

	/** A shape drawn by painter in its own units, filled with whatever
	 *  colour is current where it is drawn; bbox [x0, y0, x1, y1]. */
	addShape(painter, bbox) {
		const name = `S${this.shapes.length}`;
		this.shapes.push({ name, bbox, painter });
		return name;
	}

	/** A page w by h mm; painter draws on it in mm, y down from the top left. */
	addPage(w, h, painter) {
		this.pages.push({ w, h, painter });
	}

	/**
	 * Pictures of the first max pages, each a PNG Blob width pixels across,
	 * drawn on an OffscreenCanvas; none where the browser has no
	 * OffscreenCanvas (it is only a preview).
	 */
	async previews(width = 1200, max = 4) {
		if (typeof OffscreenCanvas === 'undefined' || typeof createImageBitmap !== 'function') return [];
		const bitmaps = new Map();
		for (const im of this.images) {
			const n = im.width * im.height;
			const rgba = new Uint8ClampedArray(n * 4);
			for (let i = 0; i < n; i++) {
				const c = im.gray ? [im.color[i], im.color[i], im.color[i]] : [im.color[i * 3], im.color[i * 3 + 1], im.color[i * 3 + 2]];
				rgba[i * 4] = c[0]; rgba[i * 4 + 1] = c[1]; rgba[i * 4 + 2] = c[2];
				rgba[i * 4 + 3] = im.alpha ? im.alpha[i] : 255;
			}
			bitmaps.set(im.name, await createImageBitmap(new ImageData(rgba, im.width, im.height)));
		}
		const shapes = new Map(this.shapes.map(s => [s.name, s]));
		const out = [];
		try {
			for (const page of this.pages.slice(0, max)) {
				const scale = width / page.w;
				const canvas = new OffscreenCanvas(Math.round(page.w * scale), Math.round(page.h * scale));
				const ctx = canvas.getContext('2d');
				if (!ctx) return [];
				ctx.fillStyle = '#ffffff';
				ctx.fillRect(0, 0, canvas.width, canvas.height);
				ctx.imageSmoothingQuality = 'high';
				ctx.setTransform(scale, 0, 0, scale, 0, 0);
				paint(ctx, page.painter.ops, bitmaps, shapes);
				out.push(await canvas.convertToBlob({ type: 'image/png' }));
			}
		} finally {
			for (const b of bitmaps.values()) b.close();
		}
		return out;
	}

	async build({ title = '', creator = '' } = {}) {
		const objects = [];  // index + 1 is the object number
		const reserve = () => { objects.push(null); return objects.length; };
		const set = (id, body) => { objects[id - 1] = body; };
		const stream = async (dict, data) => {
			const z = await deflate(data);
			return { dict: `<< ${dict} /Filter /FlateDecode /Length ${z.length} >>`, data: z };
		};

		const catalog = reserve(), pagesId = reserve(), info = reserve();
		const xobjects = [];
		for (const im of this.images) {
			const id = reserve();
			let smask = '';
			if (im.alpha) {
				const sid = reserve();
				set(sid, await stream(`/Type /XObject /Subtype /Image /Width ${im.width} /Height ${im.height} /ColorSpace /DeviceGray /BitsPerComponent 8`, im.alpha));
				smask = ` /SMask ${sid} 0 R`;
			}
			set(id, await stream(`/Type /XObject /Subtype /Image /Width ${im.width} /Height ${im.height} /ColorSpace /${im.gray ? 'DeviceGray' : 'DeviceRGB'} /BitsPerComponent 8 /Interpolate true${smask}`, im.color));
			xobjects.push(`/${im.name} ${id} 0 R`);
		}
		for (const s of this.shapes) {
			const id = reserve();
			set(id, await stream(`/Type /XObject /Subtype /Form /BBox [${s.bbox.map(num).join(' ')}]`, encoder.encode(s.painter.text())));
			xobjects.push(`/${s.name} ${id} 0 R`);
		}
		const resources = `<< /XObject << ${xobjects.join(' ')} >> /ProcSet [/PDF /ImageB /ImageC] >>`;
		const kids = [];
		for (const page of this.pages) {
			const pid = reserve(), cid = reserve();
			// Millimetres, y down: flip the page once for everything drawn on it.
			const content = `${num(MM)} 0 0 ${num(-MM)} 0 ${num(page.h * MM)} cm\n${page.painter.text()}`;
			set(cid, await stream('', encoder.encode(content)));
			set(pid, `<< /Type /Page /Parent ${pagesId} 0 R /MediaBox [0 0 ${num(page.w * MM)} ${num(page.h * MM)}] /Resources ${resources} /Contents ${cid} 0 R >>`);
			kids.push(`${pid} 0 R`);
		}
		set(pagesId, `<< /Type /Pages /Kids [${kids.join(' ')}] /Count ${kids.length} >>`);
		set(catalog, `<< /Type /Catalog /Pages ${pagesId} 0 R /ViewerPreferences << /PrintScaling /None >> >>`);
		const str = (s) => '(' + s.replace(/[\\()]/g, '\\$&').replace(/[^\x20-\x7e]/g, '') + ')';
		set(info, `<< /Title ${str(title)} /Creator ${str(creator)} /Producer ${str(creator)} >>`);

		// Write it out, noting where each object starts.
		const parts = [];
		let length = 0;
		const push = (chunk) => {
			const b = typeof chunk === 'string' ? encoder.encode(chunk) : chunk;
			parts.push(b);
			length += b.length;
		};
		push('%PDF-1.4\n%\xe2\xe3\xcf\xd3\n');
		const offsets = [];
		objects.forEach((body, i) => {
			offsets.push(length);
			push(`${i + 1} 0 obj\n`);
			if (typeof body === 'string') push(body);
			else { push(body.dict); push('\nstream\n'); push(body.data); push('\nendstream'); }
			push('\nendobj\n');
		});
		const xref = length;
		push(`xref\n0 ${objects.length + 1}\n0000000000 65535 f \n`);
		for (const o of offsets) push(`${String(o).padStart(10, '0')} 00000 n \n`);
		push(`trailer\n<< /Size ${objects.length + 1} /Root ${catalog} 0 R /Info ${info} 0 R >>\nstartxref\n${xref}\n%%EOF\n`);
		const out = new Uint8Array(length);
		let at = 0;
		for (const p of parts) { out.set(p, at); at += p.length; }
		return out;
	}
}
