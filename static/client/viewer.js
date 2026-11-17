// A 3D view of a binary STL in WebGL, with no libraries: drag to turn it,
// scroll or pinch to zoom, double-click (or 0) to put it back, and the
// arrow keys and + and - work too. Z is up, as the core writes it, and a
// 10 mm grid lies under the model like a build plate.

const MODEL_VERTEX = `
attribute vec3 position;
attribute vec3 normal;
uniform mat4 viewProjection;
varying vec3 vNormal;
void main() {
	vNormal = normal;
	gl_Position = viewProjection * vec4(position, 1.0);
}`;

const MODEL_FRAGMENT = `
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform vec3 color;
uniform vec3 light;   // towards the light
uniform vec3 eye;     // towards the camera
varying vec3 vNormal;
void main() {
	vec3 n = normalize(vNormal);
	if (!gl_FrontFacing) n = -n;
	float diffuse = max(dot(n, light), 0.0);
	float sky = 0.5 + 0.5 * n.z;
	float shine = pow(max(dot(n, normalize(light + eye)), 0.0), 40.0) * 0.22;
	gl_FragColor = vec4(color * (0.3 + 0.2 * sky + 0.6 * diffuse) + shine, 1.0);
}`;

const LINE_VERTEX = `
attribute vec3 position;
uniform mat4 viewProjection;
void main() { gl_Position = viewProjection * vec4(position, 1.0); }`;

const LINE_FRAGMENT = `
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform vec4 color;
void main() { gl_FragColor = color; }`;

/** The triangles of a binary STL, with a flat normal for each, and its
 *  bounding box. */
export function parseStl(buffer) {
	const view = new DataView(buffer);
	const count = buffer.byteLength >= 84 ? view.getUint32(80, true) : 0;
	if (!count || 84 + count * 50 > buffer.byteLength) throw new Error('Not a binary STL.');
	const position = new Float32Array(count * 9);
	const normal = new Float32Array(count * 9);
	const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
	for (let i = 0, b = 0; i < count; i++, b += 9) {
		const o = 84 + i * 50 + 12;  // past the stored normal, often zero
		for (let k = 0; k < 9; k++) {
			const v = view.getFloat32(o + k * 4, true);
			position[b + k] = v;
			const axis = k % 3;
			if (v < min[axis]) min[axis] = v;
			if (v > max[axis]) max[axis] = v;
		}
		const ux = position[b + 3] - position[b], uy = position[b + 4] - position[b + 1], uz = position[b + 5] - position[b + 2];
		const vx = position[b + 6] - position[b], vy = position[b + 7] - position[b + 1], vz = position[b + 8] - position[b + 2];
		let nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
		const len = Math.hypot(nx, ny, nz) || 1;
		nx /= len; ny /= len; nz /= len;
		for (let k = 0; k < 9; k += 3) {
			normal[b + k] = nx; normal[b + k + 1] = ny; normal[b + k + 2] = nz;
		}
	}
	return { count, position, normal, min, max };
}

// Column-major 4x4 matrices, as WebGL takes them.
function perspective(fovy, aspect, near, far) {
	const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
	return [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0];
}

function lookAt(eye, target, up) {
	let zx = eye[0] - target[0], zy = eye[1] - target[1], zz = eye[2] - target[2];
	let l = Math.hypot(zx, zy, zz);
	zx /= l; zy /= l; zz /= l;
	let xx = up[1] * zz - up[2] * zy, xy = up[2] * zx - up[0] * zz, xz = up[0] * zy - up[1] * zx;
	l = Math.hypot(xx, xy, xz);
	xx /= l; xy /= l; xz /= l;
	const yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
	return [
		xx, yx, zx, 0, xy, yy, zy, 0, xz, yz, zz, 0,
		-(xx * eye[0] + xy * eye[1] + xz * eye[2]),
		-(yx * eye[0] + yy * eye[1] + yz * eye[2]),
		-(zx * eye[0] + zy * eye[1] + zz * eye[2]), 1,
	];
}

function multiply(a, b) {
	const out = new Array(16);
	for (let c = 0; c < 4; c++)
		for (let r = 0; r < 4; r++)
			out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
	return out;
}

/** "#rrggbb" or "rgb(r, g, b)" as three numbers from 0 to 1. */
function colour(css) {
	const hex = /^#([0-9a-f]{6})$/i.exec(css.trim());
	if (hex) {
		const n = parseInt(hex[1], 16);
		return [(n >> 16 & 255) / 255, (n >> 8 & 255) / 255, (n & 255) / 255];
	}
	const m = css.match(/[\d.]+/g) || [128, 128, 128];
	return [0, 1, 2].map(i => Number(m[i]) / 255);
}

function program(gl, vertex, fragment) {
	const p = gl.createProgram();
	for (const [type, source] of [[gl.VERTEX_SHADER, vertex], [gl.FRAGMENT_SHADER, fragment]]) {
		const s = gl.createShader(type);
		gl.shaderSource(s, source);
		gl.compileShader(s);
		if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
		gl.attachShader(p, s);
	}
	gl.linkProgram(p);
	if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
	return p;
}

/**
 * Shows the STL in buffer on a canvas added to container. The model is
 * drawn in color; the grid in the canvas's CSS colour, so it follows the
 * page's theme. Returns { size: [x, y, z] in mm, dispose() }, or throws
 * when the browser has no WebGL.
 */
export function viewModel(container, buffer, { color = '#8fd14f', label = '3D view of your model' } = {}) {
	const mesh = parseStl(buffer);
	const canvas = document.createElement('canvas');
	canvas.className = 'model-canvas';
	canvas.tabIndex = 0;
	canvas.setAttribute('role', 'img');
	canvas.setAttribute('aria-label', `${label}. Drag to turn it, scroll or pinch to zoom, double-click to reset.`);
	// Some phones refuse a multisampled context, so try again without it.
	const gl = canvas.getContext('webgl', { antialias: true, alpha: true })
		|| canvas.getContext('webgl', { antialias: false, alpha: true })
		|| canvas.getContext('experimental-webgl');
	if (!gl) throw new Error('WebGL is not available.');
	container.append(canvas);

	const size = [0, 1, 2].map(i => mesh.max[i] - mesh.min[i]);
	const centre = [0, 1, 2].map(i => (mesh.min[i] + mesh.max[i]) / 2);
	const radius = Math.max(1e-3, Math.hypot(...size) / 2);

	// Everything that lives on the GPU, so it can be made again if the browser
	// takes the context away (phones do, when memory runs short).
	let modelProgram, lineProgram, buffers = [], positions, normals, grid;
	const upload = (data) => {
		const b = gl.createBuffer();
		gl.bindBuffer(gl.ARRAY_BUFFER, b);
		gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
		buffers.push(b);
		return b;
	};

	// The build plate: 10 mm squares under the model, a square past its edge.
	const step = 10;
	const gx0 = Math.floor(mesh.min[0] / step) * step - step, gx1 = Math.ceil(mesh.max[0] / step) * step + step;
	const gy0 = Math.floor(mesh.min[1] / step) * step - step, gy1 = Math.ceil(mesh.max[1] / step) * step + step;
	const gz = mesh.min[2] - radius * 0.002;
	const lines = [];
	for (let x = gx0; x <= gx1 + 1e-6; x += step) lines.push(x, gy0, gz, x, gy1, gz);
	for (let y = gy0; y <= gy1 + 1e-6; y += step) lines.push(gx0, y, gz, gx1, y, gz);
	const gridData = new Float32Array(lines);
	const gridCount = lines.length / 3;

	function setup() {
		modelProgram = program(gl, MODEL_VERTEX, MODEL_FRAGMENT);
		lineProgram = program(gl, LINE_VERTEX, LINE_FRAGMENT);
		buffers = [];
		positions = upload(mesh.position);
		normals = upload(mesh.normal);
		grid = upload(gridData);
	}
	setup();

	// Flat things (coins, stamp plates) are looked at from higher up.
	const flat = size[2] < 0.5 * Math.max(size[0], size[1]);
	const start = { yaw: -0.45, pitch: flat ? 0.95 : 0.2, zoom: 1 };
	const view = { ...start };
	const fovy = 35 * Math.PI / 180;

	let frame = 0;
	function render() {
		frame = 0;
		const dpr = Math.min(window.devicePixelRatio || 1, 2);
		const w = Math.max(1, Math.round(canvas.clientWidth * dpr)), h = Math.max(1, Math.round(canvas.clientHeight * dpr));
		if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
		gl.viewport(0, 0, w, h);
		gl.clearColor(0, 0, 0, 0);
		gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
		gl.enable(gl.DEPTH_TEST);

		const aspect = w / h;
		// Far enough back that the whole model fits, across or up and down.
		const fit = radius / Math.sin(Math.min(fovy, 2 * Math.atan(Math.tan(fovy / 2) * aspect)) / 2);
		const distance = fit * 0.9 / view.zoom;
		const cp = Math.cos(view.pitch);
		const toEye = [cp * Math.sin(view.yaw), -cp * Math.cos(view.yaw), Math.sin(view.pitch)];
		const eye = centre.map((c, i) => c + toEye[i] * distance);
		const vp = multiply(
			perspective(fovy, aspect, Math.max(distance - radius * 2, distance * 0.01), distance + radius * 3),
			lookAt(eye, centre, [0, 0, 1]));
		// A light over the viewer's shoulder, so the side facing them is lit.
		const right = [Math.cos(view.yaw), Math.sin(view.yaw), 0];
		let light = toEye.map((v, i) => v + right[i] * 0.45 + (i === 2 ? 0.7 : 0));
		const ll = Math.hypot(...light);
		light = light.map(v => v / ll);

		gl.useProgram(lineProgram);
		gl.uniformMatrix4fv(gl.getUniformLocation(lineProgram, 'viewProjection'), false, vp);
		const g = colour(getComputedStyle(canvas).color);
		gl.uniform4f(gl.getUniformLocation(lineProgram, 'color'), g[0], g[1], g[2], 0.55);
		gl.enable(gl.BLEND);
		// Premultiplied, as the page puts the canvas over its background.
		gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
		const lp = gl.getAttribLocation(lineProgram, 'position');
		gl.bindBuffer(gl.ARRAY_BUFFER, grid);
		gl.enableVertexAttribArray(lp);
		gl.vertexAttribPointer(lp, 3, gl.FLOAT, false, 0, 0);
		gl.drawArrays(gl.LINES, 0, gridCount);
		gl.disableVertexAttribArray(lp);
		gl.disable(gl.BLEND);

		gl.useProgram(modelProgram);
		gl.uniformMatrix4fv(gl.getUniformLocation(modelProgram, 'viewProjection'), false, vp);
		gl.uniform3fv(gl.getUniformLocation(modelProgram, 'color'), colour(color));
		gl.uniform3fv(gl.getUniformLocation(modelProgram, 'light'), light);
		gl.uniform3fv(gl.getUniformLocation(modelProgram, 'eye'), toEye);
		const pos = gl.getAttribLocation(modelProgram, 'position'), nor = gl.getAttribLocation(modelProgram, 'normal');
		gl.bindBuffer(gl.ARRAY_BUFFER, positions);
		gl.enableVertexAttribArray(pos);
		gl.vertexAttribPointer(pos, 3, gl.FLOAT, false, 0, 0);
		gl.bindBuffer(gl.ARRAY_BUFFER, normals);
		gl.enableVertexAttribArray(nor);
		gl.vertexAttribPointer(nor, 3, gl.FLOAT, false, 0, 0);
		gl.drawArrays(gl.TRIANGLES, 0, mesh.count * 3);
		gl.disableVertexAttribArray(pos);
		gl.disableVertexAttribArray(nor);
		const error = gl.getError();
		if (error) console.warn('The 3D preview hit a WebGL error:', error);
	}
	const draw = () => { if (!frame) frame = requestAnimationFrame(render); };

	// One slow turn to show it off, unless the viewer would rather not.
	let spin = !matchMedia('(prefers-reduced-motion: reduce)').matches;
	let spinStart = 0;
	function turn(t) {
		if (!spin) return;
		if (!spinStart) spinStart = t;
		const done = Math.min(1, (t - spinStart) / 9000);
		const eased = done < 0.5 ? 2 * done * done : 1 - 2 * (1 - done) * (1 - done);
		view.yaw = start.yaw + eased * Math.PI * 2;
		render();
		if (done < 1) requestAnimationFrame(turn); else spin = false;
	}
	const stopSpin = () => { spin = false; };

	const clampPitch = () => { view.pitch = Math.max(-1.5, Math.min(1.5, view.pitch)); };
	const clampZoom = () => { view.zoom = Math.max(0.4, Math.min(8, view.zoom)); };
	const pointers = new Map();
	const events = new AbortController();
	const on = (type, fn, opts = {}) => canvas.addEventListener(type, fn, { ...opts, signal: events.signal });
	on('pointerdown', (e) => {
		stopSpin();
		canvas.setPointerCapture(e.pointerId);
		pointers.set(e.pointerId, [e.clientX, e.clientY]);
	});
	on('pointermove', (e) => {
		const last = pointers.get(e.pointerId);
		if (!last) return;
		if (pointers.size === 1) {
			view.yaw -= (e.clientX - last[0]) * 0.01;
			view.pitch += (e.clientY - last[1]) * 0.01;
			clampPitch();
		} else if (pointers.size === 2) {
			const [other] = [...pointers].filter(([id]) => id !== e.pointerId).map(([, p]) => p);
			const before = Math.hypot(last[0] - other[0], last[1] - other[1]);
			const after = Math.hypot(e.clientX - other[0], e.clientY - other[1]);
			if (before > 0) view.zoom *= after / before;
			clampZoom();
		}
		pointers.set(e.pointerId, [e.clientX, e.clientY]);
		draw();
	});
	const lift = (e) => pointers.delete(e.pointerId);
	on('pointerup', lift);
	on('pointercancel', lift);
	on('wheel', (e) => {
		e.preventDefault();
		stopSpin();
		view.zoom *= Math.exp(-e.deltaY * (e.deltaMode === 1 ? 0.05 : 0.0015));
		clampZoom();
		draw();
	}, { passive: false });
	const reset = () => { stopSpin(); Object.assign(view, start); draw(); };
	on('dblclick', reset);
	on('keydown', (e) => {
		const keys = {
			ArrowLeft: () => { view.yaw += 0.2; }, ArrowRight: () => { view.yaw -= 0.2; },
			ArrowUp: () => { view.pitch -= 0.15; clampPitch(); }, ArrowDown: () => { view.pitch += 0.15; clampPitch(); },
			'+': () => { view.zoom *= 1.2; clampZoom(); }, '=': () => { view.zoom *= 1.2; clampZoom(); },
			'-': () => { view.zoom /= 1.2; clampZoom(); }, '0': reset, Home: reset,
		};
		if (!keys[e.key]) return;
		e.preventDefault();
		stopSpin();
		keys[e.key]();
		draw();
	});

	on('webglcontextlost', (e) => { e.preventDefault(); spin = false; });
	on('webglcontextrestored', () => {
		try { setup(); draw(); } catch (err) { console.warn('No 3D preview after the context came back:', err); }
	});

	const resized = new ResizeObserver(draw);
	resized.observe(canvas);
	if (spin) requestAnimationFrame(turn); else draw();

	return {
		size,
		dispose() {
			spin = false;
			if (frame) cancelAnimationFrame(frame);
			resized.disconnect();
			events.abort();
			for (const b of buffers) gl.deleteBuffer(b);
			gl.deleteProgram(modelProgram);
			gl.deleteProgram(lineProgram);
			const lose = gl.getExtension('WEBGL_lose_context');
			if (lose) lose.loseContext();
			canvas.remove();
		},
	};
}
