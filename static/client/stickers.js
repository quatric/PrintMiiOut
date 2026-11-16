// Printed things, as PDFs: sticker sheets after Wii no Ma's Mii name
// stickers (Miiのお名前シール), on sticker paper or on any of the label
// sheets The Print Shop 4 supports, and a towel and a tote bag after the
// ones printed with a Mii stamp's line art.
//
// Everything is drawn in millimetres, y down. Names are the glyph outlines
// of Rodin NTLG (from the core), so they print sharp at any size; faces are
// the Mii API's colour render or the line art the stamps carve, coloured
// like a Shachihata ink.
import { Painter, PdfDocument } from './pdf.js';

/** Colour schemes: five colours for stickers (a to e) and the text on them. */
export const THEMES = {
	// The sheet Wii no Ma printed (ベーシック柄): yellow, blue, green and grey.
	classic: { a: '#f3e383', b: '#93bfea', c: '#98b85c', d: '#c9c9c9', e: '#f0b97a', text: '#2b2b2b', accent: '#2d5db3' },
	// Soft yellow, pink, blue and green, like the pastel sheets in the ad.
	pastel: { a: '#fbe9a2', b: '#f8c2d1', c: '#bddcf5', d: '#d3ebc0', e: '#e4d3f2', text: '#4a4048', accent: '#d75a86' },
	// Rows of every colour.
	rainbow: { a: '#fde07a', b: '#f6a6bb', c: '#a6d77f', d: '#8ec6f1', e: '#c7a8e6', text: '#333333', accent: '#e2487e',
		grid: ['a', 'b', 'c', 'd', 'e'] },
	pink: { a: '#f7b5c8', b: '#fcd6e0', c: '#f19ab4', d: '#e6cbd4', e: '#fbe3b8', text: '#5a3443', accent: '#d9467a' },
	sky: { a: '#c3e5f6', b: '#8ec6ec', c: '#a3dbd2', d: '#d5e2ee', e: '#b9c9f2', text: '#24384d', accent: '#1f6db5' },
	// Greys only, for a black-and-white printer.
	mono: { a: '#e3e3e3', b: '#c6c6c6', c: '#a9a9a9', d: '#d4d4d4', e: '#ececec', text: '#000000', accent: '#000000', gray: true },
};

/** Ink colours for line-art faces, the towel and the tote: Shachihata's
 *  stamp inks as its Mii stamps showed them, and pink. */
export const INKS = {
	blue: '#1f47c4',
	black: '#1b1b1b',
	green: '#1e9a3f',
	red: '#e3312c',
	purple: '#7a3eae',
	pink: '#ec5e98',
};

const PAPERS = { letter: [215.9, 279.4], a4: [210, 297] };
const WHITE = '#ffffff';
const CUT = '#9a9a9a';

function mix(c, toward, t) {
	const a = parseInt(c.slice(1), 16), b = parseInt(toward.slice(1), 16);
	const ch = (v, s) => (v >> s) & 255;
	const m = (s) => Math.round(ch(a, s) + (ch(b, s) - ch(a, s)) * t);
	return '#' + ((m(16) << 16) | (m(8) << 8) | m(0)).toString(16).padStart(6, '0');
}
const light = (c, t = 0.55) => mix(c, WHITE, t);

// ── Faces ─────────────────────────────────────────────────────────────────

/** The opaque part of an RGBA image, cropped and scaled to at most max pixels. */
function prepareImage(width, height, rgba, max) {
	let x0 = width, y0 = height, x1 = -1, y1 = -1;
	for (let y = 0; y < height; y++)
		for (let x = 0; x < width; x++)
			if (rgba[(y * width + x) * 4 + 3] > 6) {
				if (x < x0) x0 = x;
				if (x > x1) x1 = x;
				if (y < y0) y0 = y;
				if (y > y1) y1 = y;
			}
	if (x1 < 0) return { width: 1, height: 1, rgba: new Uint8Array(4) };
	const cw = x1 - x0 + 1, ch = y1 - y0 + 1;
	const s = Math.min(1, max / Math.max(cw, ch));
	const w = Math.max(1, Math.round(cw * s)), h = Math.max(1, Math.round(ch * s));
	// Area average, premultiplied so edges stay clean.
	const acc = new Float64Array(w * h * 4), wsum = new Float64Array(w * h);
	for (let y = y0; y <= y1; y++) {
		const ty = Math.min(h - 1, Math.floor((y - y0) * s));
		for (let x = x0; x <= x1; x++) {
			const tx = Math.min(w - 1, Math.floor((x - x0) * s));
			const i = (y * width + x) * 4, o = ty * w + tx, a = rgba[i + 3] / 255;
			acc[o * 4] += rgba[i] * a;
			acc[o * 4 + 1] += rgba[i + 1] * a;
			acc[o * 4 + 2] += rgba[i + 2] * a;
			acc[o * 4 + 3] += a;
			wsum[o]++;
		}
	}
	const out = new Uint8Array(w * h * 4);
	for (let o = 0; o < w * h; o++) {
		const a = acc[o * 4 + 3];
		if (a > 0) for (let k = 0; k < 3; k++) out[o * 4 + k] = Math.round(acc[o * 4 + k] / a);
		out[o * 4 + 3] = Math.round((a / Math.max(1, wsum[o])) * 255);
	}
	return { width: w, height: h, rgba: out };
}

/** Line art as an image: ink in its colour over the head filled white. */
function inkImage(drawn, ink) {
	const n = drawn.width * drawn.height;
	const rgba = new Uint8Array(n * 4);
	const c = parseInt(ink.slice(1), 16);
	const r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
	for (let i = 0; i < n; i++) {
		const t = drawn.ink[i] / 255;
		rgba[i * 4] = Math.round(255 + (r - 255) * t);
		rgba[i * 4 + 1] = Math.round(255 + (g - 255) * t);
		rgba[i * 4 + 2] = Math.round(255 + (b - 255) * t);
		rgba[i * 4 + 3] = Math.max(drawn.ink[i], drawn.head[i]);
	}
	return { width: drawn.width, height: drawn.height, rgba };
}

// ── Lettering ─────────────────────────────────────────────────────────────

// Turned on their side in a vertical name, and the small kana that sit to
// the upper right of their square there.
const ROTATED = new Set([...'ー～〜~-‐‑–—―_()[]{}（）［］「」『』【】〈〉《》…‥：=＝']);
const SMALL = new Set([...'ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶㇰㇱㇲㇳㇴㇵㇶㇷㇸㇹㇺㇻㇼㇽㇾㇿ']);

/** Names and other text, set from glyph outlines drawn as shapes. */
class Lettering {
	constructor(core, font, doc) {
		this.core = core;
		this.font = font;
		this.doc = doc;
		this.cache = new Map();
		this.shapes = new Map();
	}

	line(text) {
		if (!this.cache.has(text)) {
			const [line] = (text && this.font && this.core.outlineText(this.font, [text])) || [null];
			this.cache.set(text, line && line.glyphs.length ? line : null);
		}
		return this.cache.get(text);
	}

	shape(g) {
		if (!g.path.length) return null;
		if (!this.shapes.has(g.cp)) {
			const p = new Painter();
			const a = g.path;
			for (let i = 0; i < a.length;) {
				const op = a[i++];
				if (op === 0) { p.moveTo(a[i], a[i + 1]); i += 2; }
				else if (op === 1) { p.lineTo(a[i], a[i + 1]); i += 2; }
				else if (op === 2) { p.curveTo(a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]); i += 6; }
				else p.closePath();
			}
			p.fill();
			const [x0, y0, x1, y1] = g.box;
			this.shapes.set(g.cp, this.doc.addShape(p, [x0 - 0.05, y0 - 0.05, x1 + 0.05, y1 + 0.05]));
		}
		return this.shapes.get(g.cp);
	}

	/** Ink extent of a line, em: [bottom, top]. */
	static extent(line) {
		let lo = Infinity, hi = -Infinity;
		for (const g of line.glyphs) if (g.path.length) { lo = Math.min(lo, g.box[1]); hi = Math.max(hi, g.box[3]); }
		return lo <= hi ? [lo, hi] : [0, 0.7];
	}

	glyph(p, g, a, b, c, d, e, f) {
		const name = this.shape(g);
		if (name) p.save().transform(a, b, c, d, e, f).draw(name).restore();
	}

	/** One line centred on (cx, cy), size mm per em at most and no wider
	 *  than maxW; align 'left' starts it at cx instead. Returns its width. */
	horizontal(p, text, cx, cy, size, maxW, color, align = 'center') {
		const line = this.line(text);
		if (!line) return 0;
		const s = Math.min(size, maxW / line.advance);
		const [lo, hi] = Lettering.extent(line);
		const w = line.advance * s;
		const x0 = align === 'left' ? cx : align === 'right' ? cx - w : cx - w / 2;
		const base = cy + ((lo + hi) / 2) * s;
		p.fillColor(color);
		for (const g of line.glyphs) this.glyph(p, g, s, 0, 0, -s, x0 + g.x * s, base);
		return w;
	}

	/** Stacked down from top to bottom, centred on cx, as names are written
	 *  on Japanese labels; Latin letters stand upright. */
	vertical(p, text, cx, top, bottom, size, color) {
		const line = this.line(text);
		if (!line) return;
		const cells = line.glyphs.map(g => (g.advance >= 0.9 ? 1 : 0.86));
		const total = cells.reduce((a, b) => a + b, 0);
		const s = Math.min(size, (bottom - top) / total);
		let y = top + (bottom - top - total * s) / 2;
		p.fillColor(color);
		line.glyphs.forEach((g, i) => {
			const cell = cells[i] * s;
			const ch = String.fromCodePoint(g.cp);
			const cy = y + cell / 2;
			if (ROTATED.has(ch)) {
				// A quarter turn clockwise about the middle of its ink.
				const ux = (g.box[0] + g.box[2]) / 2, vy = (g.box[1] + g.box[3]) / 2;
				this.glyph(p, g, 0, s, s, 0, cx - s * vy, cy - s * ux);
			} else {
				let x = cx - (g.advance * s) / 2, base = cy + (cells[i] === 1 ? 0.38 : 0.36) * s;
				if (SMALL.has(ch)) { x += 0.1 * s; base -= 0.1 * s; }
				this.glyph(p, g, s, 0, 0, -s, x, base);
			}
			y += cell;
		});
	}

	/** Round an arc about (cx, cy), reading left to right: along the bottom
	 *  with the letters' tops towards the centre, or along the top with
	 *  them facing out. radius is the middle of the letters. */
	arc(p, text, cx, cy, radius, size, maxSpan, color, top = false) {
		const line = this.line(text);
		if (!line) return;
		const [lo, hi] = Lettering.extent(line);
		let s = Math.min(size, (maxSpan * radius) / line.advance);
		const mid = (lo + hi) / 2;
		// The baseline sits half the letters' height inside or outside the middle.
		const base = top ? radius - mid * s : radius + mid * s;
		const span = (line.advance * s) / base;
		p.fillColor(color);
		for (const g of line.glyphs) {
			const along = (g.x + g.advance / 2) * s / base;
			const phi = top ? -Math.PI / 2 - span / 2 + along : Math.PI / 2 + span / 2 - along;
			const c = Math.cos(phi), sn = Math.sin(phi);
			const t = top ? [-sn, c] : [sn, -c];       // reading direction
			const up = top ? [c, sn] : [-c, -sn];      // towards the letters' tops
			const px = cx + base * c, py = cy + base * sn;
			const u = (g.advance / 2) * s;
			this.glyph(p, g, s * t[0], s * t[1], s * up[0], s * up[1], px - u * t[0], py - u * t[1]);
		}
	}
}

// ── Shapes and fills ──────────────────────────────────────────────────────

/** Builds the outline of a label: 'rect' (corner r), 'round', 'oval' or
 *  'sheet', grown by g all round. */
function outline(p, shape, x, y, w, h, r, g = 0) {
	if (shape === 'round' || shape === 'oval') return p.ellipse(x + w / 2, y + h / 2, w / 2 + g, h / 2 + g);
	return p.roundRect(x - g, y - g, w + 2 * g, h + 2 * g, shape === 'sheet' ? 0 : r + g);
}

/**
 * The background of a sticker, inside the path the caller clipped to:
 * 'w' paper white, a role ('a'..'e', or 'al' for its light tint), 's:a'
 * stripes or 'k:a' a diagonal check in that role's colours.
 */
function background(p, style, theme, x, y, w, h, unit) {
	if (!style || style === 'w') return;
	const role = (k) => theme[k[0]];
	if (style.startsWith('s:')) {
		const c = role(style.slice(2));
		const period = 2.8 * unit;
		p.fillColor(light(c, 0.55)).rect(x, y, w, h).fill();
		p.fillColor(c);
		for (let yy = y + period * 0.25; yy < y + h; yy += period) p.rect(x, yy, w, period / 2);
		p.fill();
	} else if (style.startsWith('k:')) {
		const c = role(style.slice(2));
		const cell = 3.6 * unit;
		p.fillColor(light(c, 0.85)).rect(x, y, w, h).fill();
		p.save().translate(x + w / 2, y + h / 2).rotate(Math.PI / 4).fillColor(light(c, 0.45));
		const n = Math.ceil(Math.hypot(w, h) / 2 / cell) + 1;
		for (let i = -n; i < n; i++)
			for (let j = -n; j < n; j++)
				if (((i + j) & 1) === 0) p.rect(i * cell, j * cell, cell, cell);
		p.fill().restore();
	} else {
		const c = style.endsWith('l') ? light(role(style), 0.6) : role(style);
		p.fillColor(c).rect(x, y, w, h).fill();
	}
}

// ── Drawing context ───────────────────────────────────────────────────────

class Art {
	constructor(core, font, { photo, drawn, ink, gray }) {
		this.doc = new PdfDocument();
		this.letters = new Lettering(core, font, this.doc);
		const im = drawn ? inkImage(drawn, ink) : photo;
		if (im) {
			this.faceW = im.width;
			this.faceH = im.height;
			this.faceName = this.doc.addImage(im.width, im.height, im.rgba, { gray: !!gray && !drawn });
		}
	}

	/** The face as large as fits the box, centred in it. */
	face(p, x, y, w, h) {
		if (!this.faceName) return;
		const s = Math.min(w / this.faceW, h / this.faceH);
		const fw = this.faceW * s, fh = this.faceH * s;
		p.image(this.faceName, x + (w - fw) / 2, y + (h - fh) / 2, fw, fh);
	}
}

// ── The Wii no Ma sheet ───────────────────────────────────────────────────

// Where everything sits on the sheet, in the pixels of the old template it
// was measured from (808 by 1172). Backgrounds name the theme's colours.
const TEMPLATE = (() => {
	const t = [{ k: 'header', x: 316, y: 40, w: 470, h: 56 }];
	t.push({ k: 'band', x: 40, y: 40, w: 250, h: 98, r: 12, bg: 'c' });
	[['w', 318], ['a', 466], ['c', 614]].forEach(([bg, x]) => t.push({ k: 'square', x, y: 104, w: 140, h: 146, r: 6, bg }));
	t.push({ k: 'circle', x: 40, y: 147, w: 116, h: 116, bg: 's:b' });
	t.push({ k: 'circle', x: 176, y: 159, w: 92, h: 92, bg: 'w' });
	t.push({ k: 'circle', x: 52, y: 280, w: 92, h: 92, bg: 'w' });
	t.push({ k: 'circle', x: 163, y: 269, w: 114, h: 114, bg: 'a' });
	t.push({ k: 'oval', x: 296, y: 284, w: 228, h: 100, bg: 'k:b', accent: true });
	t.push({ k: 'oval', x: 541, y: 284, w: 232, h: 100, bg: 'k:a' });
	[45, 300, 555].forEach((x, i) => t.push({ k: 'pill', x, y: 420, w: 230, h: 62, bg: 'w', accent: i === 2 }));
	for (let row = 0; row < 5; row++)
		for (const x of [45, 233, 421, 609]) t.push({ k: 'row', x, y: 512 + row * 50, w: 166, h: 45, r: 6, grid: row });
	const vbg = [['al', 'w', 'w', 'bl'], ['cl', 'w', 'b', 'w']];
	[790, 976].forEach((y, row) => [50, 103, 156, 209].forEach((x, col) =>
		t.push({ k: 'tall', x, y, w: 44, h: 168, r: 6, bg: vbg[row][col], accent: col === 3 })));
	[788, 860, 932, 1004, 1076].forEach((y, i) => {
		t.push({ k: 'row', x: 280, y, w: 222, h: 56, r: 8, bg: 'w', accent: i === 3, big: true });
		t.push({ k: 'row', x: 524, y, w: 252, h: 56, r: 8, bg: ['s:c', 's:a', 's:b', 's:d', 's:c'][i], big: true });
	});
	t.push({ k: 'footer', x: 600, y: 1146, w: 185, h: 14 });
	return t;
})();

function bounds(items) {
	let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
	for (const it of items) {
		x0 = Math.min(x0, it.x); y0 = Math.min(y0, it.y);
		x1 = Math.max(x1, it.x + it.w); y1 = Math.max(y1, it.y + it.h);
	}
	return { x0, y0, x1, y1, w: x1 - x0, h: y1 - y0 };
}

/** One sticker of the template, in its pixels; unit is pixels per mm. */
function templateItem(art, p, it, o, unit) {
	const L = art.letters, th = o.theme;
	const ink = it.accent ? th.accent : th.text;
	const bg = it.grid !== undefined ? (th.grid || ['a', 'w', 'b', 'w', 'c'])[it.grid] : it.bg;
	const shape = it.k === 'circle' ? 'round' : it.k === 'oval' ? 'oval' : 'rect';
	const r = it.k === 'pill' ? it.h / 2 : it.r || 0;
	const { x, y, w, h } = it;
	const text = o.text;
	if (it.k === 'header') {
		L.horizontal(p, 'Miiのお名前シール', x, y + 26, 24, 230, th.text, 'left');
		L.horizontal(p, 'NAME STICKERS', x + 2, y + 47, 10, 120, th.accent, 'left');
		if (text) L.horizontal(p, text, x + w, y + 22, 13, 190, th.accent, 'right');
		if (o.date) L.horizontal(p, o.date, x + w, y + 42, 10, 190, th.accent, 'right');
		return;
	}
	if (it.k === 'footer') {
		L.horizontal(p, 'printmiiout.com', x + w, y + h / 2, 9, w, '#9a9a9a', 'right');
		return;
	}
	p.save();
	outline(p, shape, x, y, w, h, r);
	p.clip();
	if (it.k === 'band') {
		background(p, 's:' + it.bg, th, x, y + h * 0.68, w, h * 0.32, unit);
		if (text) {
			art.face(p, x + 18, y + 6, 70, 58);
			L.horizontal(p, text, x + 165, y + 36, 20, 140, ink);
		} else art.face(p, x + (w - 70) / 2, y + 6, 70, 58);
	} else {
		background(p, bg, th, x, y, w, h, unit);
		if (it.k === 'square') {
			art.face(p, x + w * 0.08, y + h * 0.1, w * 0.84, h * 0.8);
		} else if (it.k === 'circle') {
			const cx = x + w / 2, cy = y + h / 2, rr = w / 2;
			if (text) {
				art.face(p, cx - rr * 0.48, cy - rr * 0.62, rr * 0.96, rr * 0.86);
				L.arc(p, text, cx, cy, rr * 0.68, rr * 0.25, 2.4, ink);
			} else art.face(p, cx - rr * 0.6, cy - rr * 0.6, rr * 1.2, rr * 1.2);
		} else if (it.k === 'oval') {
			const cx = x + w / 2;
			if (text) {
				art.face(p, cx - h * 0.3, y + h * 0.08, h * 0.6, h * 0.5);
				L.horizontal(p, text, cx, y + h * 0.74, h * 0.27, w * 0.6, ink);
			} else art.face(p, cx - h * 0.4, y + h * 0.1, h * 0.8, h * 0.8);
		} else if (it.k === 'pill' || it.k === 'row') {
			const fh = h * (it.k === 'pill' ? 0.8 : 0.84);
			if (text) {
				art.face(p, x + (it.k === 'pill' ? h * 0.32 : h * 0.12), y + (h - fh) / 2, fh, fh);
				L.horizontal(p, text, x + w * 0.6, y + h / 2, h * (it.big ? 0.4 : it.k === 'pill' ? 0.34 : 0.3), w * 0.58, ink);
			} else art.face(p, x + (w - fh) / 2, y + (h - fh) / 2, fh, fh);
		} else if (it.k === 'tall') {
			if (text) {
				art.face(p, x + 3, y + 5, w - 6, w - 6);
				L.vertical(p, text, x + w / 2, y + w + 4, y + h - 8, w * 0.42, ink);
			} else art.face(p, x + 3, y + (h - w) / 2, w - 6, w - 6);
		}
	}
	p.restore();
	if (o.cutLines) {
		p.strokeColor(CUT).lineWidth(0.25 * unit).dash(1.1 * unit, 0.8 * unit);
		outline(p, shape, x, y, w, h, r);
		p.stroke().dash();
	}
}

/** The template (or part of it) as large as fits the box (mm), turned a
 *  quarter if that makes it larger. */
function drawTemplate(art, p, items, box, o) {
	const b = bounds(items);
	const k = Math.min(box.w / b.w, box.h / b.h), kt = Math.min(box.w / b.h, box.h / b.w);
	p.save();
	let scale;
	if (kt > k * 1.15) {
		scale = kt;
		p.translate(box.x + (box.w - b.h * kt) / 2, box.y + (box.h - b.w * kt) / 2);
		p.transform(0, kt, -kt, 0, kt * b.y1, -kt * b.x0);
	} else {
		scale = k;
		p.translate(box.x + (box.w - b.w * k) / 2 - b.x0 * k, box.y + (box.h - b.h * k) / 2 - b.y0 * k);
		p.scale(k);
	}
	for (const it of items) templateItem(art, p, it, o, 1 / scale);
	p.restore();
}

const TOP_HALF = TEMPLATE.filter(it => it.y < 500);
const BOTTOM_HALF = TEMPLATE.filter(it => it.y >= 500);

// ── Mix and match on label sheets ─────────────────────────────────────────

const STYLES = {
	wide: (th) => [...(th.grid || ['a', 'w', 'b', 'w', 'c']), 's:c', 's:a', 's:b', 's:d'],
	tall: () => ['al', 'w', 'bl', 'cl', 'b', 'w'],
	square: () => ['w', 'a', 'c', 'b', 's:b', 'al', 'd'],
	round: () => ['s:b', 'w', 'a', 'w', 'c', 's:a', 'b', 'w'],
	oval: () => ['k:b', 'k:a', 'k:c', 'k:d'],
	badge: () => ['band:c', 'a', 'band:b', 'w', 'band:a', 'c'],
};

function designOf(label) {
	const { shape, w, h } = label;
	if (shape === 'round') return 'round';
	if (shape === 'oval') return 'oval';
	if (shape === 'sheet') return 'sheet';
	if (Math.min(w, h) >= 120) return 'half';
	const aspect = w / h;
	if (aspect >= 1.6) return 'wide';
	if (aspect <= 0.55) return 'tall';
	return Math.min(w, h) >= 48 ? 'badge' : 'square';
}

/** One label of a sheet, at x, y in mm. */
function drawLabel(art, p, label, x, y, design, style, o, bleed, odd) {
	const L = art.letters, th = o.theme, { w, h, shape } = label;
	const r = label.r || 0;
	const text = o.text;
	const ink = style === 'w' && odd ? th.accent : th.text;
	p.save();
	outline(p, shape, x, y, w, h, r, bleed);
	p.clip();
	const bx = x - bleed, by = y - bleed, bw = w + 2 * bleed, bh = h + 2 * bleed;
	if (style.startsWith('band:')) background(p, 's:' + style.slice(5), th, bx, y + h * 0.78, bw, h * 0.22 + bleed, 1);
	else background(p, style, th, bx, by, bw, bh, Math.min(1, h / 25));
	p.restore();
	// Faces and names stay inside the label itself.
	p.save();
	outline(p, shape, x, y, w, h, r);
	p.clip();
	const pad = Math.min(w, h) * 0.08;
	if (design === 'wide') {
		// Short wide labels keep room for the name.
		const fh = text ? Math.min(h * 0.8, w * 0.3) : h * 0.8;
		if (text) {
			art.face(p, x + Math.max(pad, h * 0.1), y + (h - fh) / 2, fh, fh);
			const left = x + h * 0.1 + fh + h * 0.12;
			L.horizontal(p, text, (left + x + w - pad) / 2, y + h / 2, h * 0.38, x + w - pad - left, ink);
		} else art.face(p, x + (w - fh) / 2, y + (h - fh) / 2, fh, fh);
	} else if (design === 'tall') {
		if (text) {
			art.face(p, x + pad, y + pad, w - 2 * pad, w - 2 * pad);
			L.vertical(p, text, x + w / 2, y + w + pad, y + h - pad, w * 0.42, ink);
		} else art.face(p, x + pad, y + (h - w) / 2, w - 2 * pad, w - 2 * pad);
	} else if (design === 'square' || design === 'badge') {
		const big = design === 'badge';
		if (text && h >= 18) {
			const fh = h * (big ? 0.56 : 0.6);
			art.face(p, x + pad, y + h * (big ? 0.08 : 0.06), w - 2 * pad, fh);
			L.horizontal(p, text, x + w / 2, y + h * (big ? 0.72 : 0.82), h * (big ? 0.13 : 0.2), w - 2 * pad, ink);
		} else art.face(p, x + pad, y + pad, w - 2 * pad, h - 2 * pad);
	} else if (design === 'round') {
		const cx = x + w / 2, cy = y + h / 2, rr = Math.min(w, h) / 2;
		if (text && rr >= 8) {
			art.face(p, cx - rr * 0.48, cy - rr * 0.62, rr * 0.96, rr * 0.86);
			L.arc(p, text, cx, cy, rr * 0.68, rr * 0.25, 2.4, ink);
		} else art.face(p, cx - rr * 0.62, cy - rr * 0.62, rr * 1.24, rr * 1.24);
	} else if (design === 'oval') {
		const cx = x + w / 2;
		const ew = Math.max(w, h), eh = Math.min(w, h);
		if (text) {
			art.face(p, cx - eh * 0.3, y + h / 2 - eh * 0.4, eh * 0.6, eh * 0.48);
			L.horizontal(p, text, cx, y + h / 2 + eh * 0.24, eh * 0.26, ew * 0.56, ink);
		} else art.face(p, cx - eh * 0.4, y + h / 2 - eh * 0.4, eh * 0.8, eh * 0.8);
	}
	p.restore();
	if (o.outlines) {
		p.strokeColor(CUT).lineWidth(0.2).dash();
		outline(p, shape, x, y, w, h, r);
		p.stroke();
	}
}

function drawLabels(art, p, label, o) {
	const design = designOf(label);
	const [pw, ph] = PAPERS[o.paper];
	if (design === 'sheet') {
		const m = 8;
		drawTemplate(art, p, TEMPLATE, { x: m, y: m, w: pw - 2 * m, h: ph - 2 * m }, { ...o, cutLines: true });
		return;
	}
	const gx = label.cols > 1 ? label.dx - label.w : Infinity;
	const gy = label.rows > 1 ? label.dy - label.h : Infinity;
	const bleed = Math.max(0, Math.min(1, gx / 2, gy / 2));
	const styles = design === 'half' ? null : STYLES[design](o.theme);
	let n = 0;
	for (let row = 0; row < label.rows; row++)
		for (let col = 0; col < label.cols; col++, n++) {
			const x = label.x + col * label.dx, y = label.y + row * label.dy;
			if (design === 'half') {
				const items = label.cols * label.rows === 1 ? TEMPLATE : n % 2 ? BOTTOM_HALF : TOP_HALF;
				const m = Math.min(label.w, label.h) * 0.04;
				drawTemplate(art, p, items, { x: x + m, y: y + m, w: label.w - 2 * m, h: label.h - 2 * m }, { ...o, cutLines: true });
				if (o.outlines) { p.strokeColor(CUT).lineWidth(0.2).dash(); outline(p, 'rect', x, y, label.w, label.h, label.r || 0); p.stroke(); }
				continue;
			}
			const byRow = design === 'wide' || design === 'tall';
			const k = byRow ? row : row * label.cols + col + (design === 'round' ? row : 0);
			drawLabel(art, p, label, x, y, design, styles[k % styles.length], o, bleed, (row + col) % 2 === 1);
		}
}

function today() {
	const d = new Date();
	return `${d.getFullYear()}.${String(d.getMonth() + 1).padStart(2, '0')}.${String(d.getDate()).padStart(2, '0')}`;
}

/**
 * A sheet of stickers as a PDF (Uint8Array).
 * core: the WebAssembly core; font: Rodin NTLG's bytes.
 * face: { photo: {width, height, rgba} } or { drawn: {width, height, ink, head}, ink: '#rrggbb' }.
 * options: text, theme (a THEMES key), paper ('letter' or 'a4'), label (a
 * labels.json entry, or null for plain sticker paper), outlines (draw each
 * label's edge, to test on plain paper).
 */
export async function makeStickers(core, font, face, options) {
	const theme = THEMES[options.theme] || THEMES.classic;
	const photo = face.photo ? prepareImage(face.photo.width, face.photo.height, face.photo.rgba, 720) : null;
	const art = new Art(core, font, { photo, drawn: face.drawn, ink: face.ink, gray: theme.gray });
	const paper = PAPERS[options.paper] ? options.paper : 'letter';
	const [pw, ph] = PAPERS[paper];
	const o = { theme, text: (options.text || '').trim(), paper, outlines: !!options.outlines, cutLines: true, date: today() };
	const p = new Painter();
	if (options.label) drawLabels(art, p, options.label, o);
	else {
		const m = 8;
		drawTemplate(art, p, TEMPLATE, { x: m, y: m, w: pw - 2 * m, h: ph - 2 * m }, o);
	}
	art.doc.addPage(pw, ph, p);
	return art.doc.build({ title: 'Mii name stickers', creator: 'PrintMiiOut (printmiiout.com)' });
}

// ── Towel and tote bag ────────────────────────────────────────────────────

/** Flips the page left to right, for iron-on transfer paper. */
function mirrored(p, pw, mirror) {
	if (mirror) p.transform(-1, 0, 0, 1, pw, 0);
}

/**
 * A towel's print: a field of dots in the ink and grey, the diagonals in
 * ink, and the face with the name small in the bottom corner.
 */
export async function makeTowel(core, font, face, options) {
	const ink = INKS[options.ink] || INKS.blue;
	const art = new Art(core, font, { drawn: face.drawn, photo: face.photo && prepareImage(face.photo.width, face.photo.height, face.photo.rgba, 720), ink });
	const [pw, ph] = PAPERS[options.paper] || PAPERS.letter;
	const side = Math.min(pw, ph) - 16;
	const x0 = (pw - side) / 2, y0 = (ph - side) / 2;
	const p = new Painter();
	p.save();
	mirrored(p, pw, options.mirror);
	// The dots: a staggered grid over the middle, grey, with the ink ones
	// running along the diagonals into a lattice of diamonds.
	const n = 12, pitch = (side * 0.72) / n, field = x0 + side * 0.14, top = y0 + side * 0.1;
	const gray = '#b9b9b9', dot = pitch * 0.17;
	const mod = (a, m) => ((a % m) + m) % m;
	for (let j = 0; j <= n; j++)
		for (let i = 0; i <= n; i++) {
			if ((i + j) % 2) continue;
			const onDiagonal = mod(i + j - 2, 8) === 0 || mod(i - j - 2, 8) === 0;
			p.fillColor(onDiagonal ? ink : gray).ellipse(field + i * pitch, top + j * pitch, dot, dot).fill();
		}
	// The face in the corner, and the name beside it.
	const fs = side * 0.085;
	const fx = x0 + side * 0.86 - fs, fy = y0 + side * 0.9 - fs;
	art.face(p, fx, fy, fs, fs);
	const text = (options.text || '').trim();
	if (text) art.letters.horizontal(p, text.toUpperCase(), fx - side * 0.012, fy + fs * 0.6, fs * 0.2, side * 0.3, gray, 'right');
	p.restore();
	art.doc.addPage(pw, ph, p);
	return art.doc.build({ title: 'Mii towel', creator: 'PrintMiiOut (printmiiout.com)' });
}

/**
 * A tote bag's badge: a ring in the ink lettered in white along the top
 * and the bottom, two dots between, and the face in the middle.
 */
export async function makeTote(core, font, face, options) {
	const ink = INKS[options.ink] || INKS.blue;
	const art = new Art(core, font, { drawn: face.drawn, photo: face.photo && prepareImage(face.photo.width, face.photo.height, face.photo.rgba, 900), ink });
	const [pw, ph] = PAPERS[options.paper] || PAPERS.letter;
	const R = (Math.min(pw, ph) - 30) / 2;
	const cx = pw / 2, cy = ph / 2;
	const ring = R * 0.22, mid = R - ring / 2;
	const p = new Painter();
	p.save();
	mirrored(p, pw, options.mirror);
	p.fillColor(ink).ellipse(cx, cy, R, R).fill();
	p.fillColor(WHITE).ellipse(cx, cy, R - ring, R - ring).fill();
	const text = (options.text || '').trim();
	if (text) {
		art.letters.arc(p, text, cx, cy, mid, ring * 0.58, 2.3, WHITE, true);
		art.letters.arc(p, text, cx, cy, mid, ring * 0.58, 2.3, WHITE, false);
	}
	for (const sx of [-1, 1]) p.fillColor(WHITE).ellipse(cx + sx * mid, cy, ring * 0.11, ring * 0.11).fill();
	const f = (R - ring) * 1.25;
	art.face(p, cx - f / 2, cy - f / 2, f, f);
	p.restore();
	art.doc.addPage(pw, ph, p);
	return art.doc.build({ title: 'Mii tote bag', creator: 'PrintMiiOut (printmiiout.com)' });
}
