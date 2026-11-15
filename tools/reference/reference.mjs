/**
 * Builds a Mii with its body the way FFL.js does (Nintendo's own height and
 * build scaling, per bone), headless on WebGPU, and checks a PrintMiiOut STL
 * against it.
 *
 *   node reference.mjs --resource AFLResHigh_2_3.dat --data <hex or base64>
 *        [--height 0-127] [--build 0-127] [--body m|f] [--pose Finish]
 *        [--stl ours.stl] [--out out/name]
 *
 *   node reference.mjs --a blender.stl --b core.stl [--face | --box x0,y0,z0,x1,y1,z1]
 *        [--out out/name]
 *        compares two STLs instead, with no FFL.js figure; --face frames
 *        the face only, --box any region (same units as the STLs).
 *
 * Writes:
 *   <out>-ref.stl      the reference figure: opaque head shapes and the
 *                      scaled, posed body, in the head's own units with the
 *                      head at the origin, Z up, as PrintMiiOut's STLs are
 *   <out>-ref.png      the reference as the game shades it
 *   <out>-compare.png  with --stl: front and side, reference | ours | overlap
 *                      (grey both, red only the reference, blue only ours)
 * and prints the overlap (IoU) and bounds of each view as JSON.
 */
// @ts-check
import * as fs from 'node:fs/promises';
import * as path from 'node:path';
import { parseArgs } from 'node:util';
import * as zlib from 'node:zlib';
import * as THREE from 'three/webgpu';
import { GLTFLoader } from 'three/examples/jsm/loaders/GLTFLoader.js';
import { STLLoader } from 'three/examples/jsm/loaders/STLLoader.js';
import * as SkeletonUtils from 'three/examples/jsm/utils/SkeletonUtils.js';
import { addSkeletonScalingExtensions } from 'ffl.js/helpers/SkeletonScalingExtensions.js';
import { detectModelDesc } from 'ffl.js/helpers/ModelScaleDesc.js';
import { addWebGPUExtensions, createThreeRenderer } from 'ffl.js/helpers/HeadlessWebGPU.js';
import { prepareBodyForCharModel, attachHeadToBody } from 'ffl.js/helpers/BodyUtilities.js';
import { FFL, CharModel, PantsColor, pantsColors } from 'ffl.js';
import FFLShaderNodeMaterial from 'ffl.js/materials/FFLShaderNodeMaterial.js';
import ModuleFFL from 'ffl.js/examples/ffl-emscripten-single-file.cjs';
/* globals process Buffer GPUDevice */

const here = path.dirname(new URL(import.meta.url).pathname);
const repo = path.resolve(here, '..', '..');

/** Opaque head shapes: faceline, beard, nose, forehead, hair, cap. */
const OPAQUE_MAX = 5;

const parseData = text => (/^[\da-f]+$/i.test(text = text.replace(/\s+/g, ''))
	? Buffer.from(text, 'hex')
	: Buffer.from(text, 'base64'));

async function loadGLTF(file) {
	const bytes = await fs.readFile(file);
	/** @type {any} */
	const gltf = await new Promise((resolve, reject) => new GLTFLoader()
		.parse(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength), '', resolve, reject));
	// The body GLBs pack their vertices tightly: skin indices, weights and
	// colours interleaved in one byte buffer, positions as three int16s.
	// three's WebGPU backend widens a whole buffer to 32 bits for the integer
	// skin indices, and WebGPU wants every vertex stride a multiple of four,
	// so unpack every attribute but the skin indices to plain floats.
	gltf.scene.traverse((o) => {
		if (!o.geometry) return;
		for (const [name, attr] of Object.entries(o.geometry.attributes)) {
			if (attr.array instanceof Float32Array && !attr.isInterleavedBufferAttribute) continue;
			const n = attr.count, size = attr.itemSize;
			const out = name === 'skinIndex' ? new Uint16Array(n * size) : new Float32Array(n * size);
			for (let i = 0; i < n; i++)
				for (let k = 0; k < size; k++) out[i * size + k] = attr.getComponent(i, k);
			o.geometry.setAttribute(name, new THREE.BufferAttribute(out, size));
		}
	});
	return gltf;
}

// ---------------------------------------------------------------------------
// Geometry out of the scene
// ---------------------------------------------------------------------------

/**
 * World-space triangles of a skinned mesh, skinned on the CPU with the bone
 * matrices FFL.js's scaled skeleton made (bone.matrixWorld alone has no scale).
 */
function skinnedTriangles(mesh) {
	const geo = mesh.geometry;
	const pos = geo.attributes.position;
	const si = geo.attributes.skinIndex;
	const sw = geo.attributes.skinWeight;
	const bm = mesh.skeleton.boneMatrices;
	const v = new THREE.Vector3(), t = new THREE.Vector3(), acc = new THREE.Vector3();
	const m = new THREE.Matrix4();
	const out = new Float32Array(pos.count * 3);
	for (let i = 0; i < pos.count; i++) {
		v.fromBufferAttribute(pos, i).applyMatrix4(mesh.bindMatrix);
		acc.set(0, 0, 0);
		for (let k = 0; k < 4; k++) {
			const w = sw.getComponent(i, k);
			if (!w) continue;
			m.fromArray(bm, si.getComponent(i, k) * 16);
			acc.addScaledVector(t.copy(v).applyMatrix4(m), w);
		}
		acc.toArray(out, i * 3);
	}
	return expand(out, geo.index);
}

/** World-space triangles of a plain mesh. */
function meshTriangles(mesh) {
	const pos = mesh.geometry.attributes.position;
	const v = new THREE.Vector3();
	const out = new Float32Array(pos.count * 3);
	for (let i = 0; i < pos.count; i++)
		v.fromBufferAttribute(pos, i).applyMatrix4(mesh.matrixWorld).toArray(out, i * 3);
	return expand(out, mesh.geometry.index);
}

/** Flat triangle list (9 floats a triangle) from positions and an index. */
function expand(pos, index) {
	const n = index ? index.count : pos.length / 3;
	const out = new Float32Array(n * 3);
	for (let i = 0; i < n; i++) {
		const j = index ? index.getX(i) : i;
		out[i * 3] = pos[j * 3];
		out[i * 3 + 1] = pos[j * 3 + 1];
		out[i * 3 + 2] = pos[j * 3 + 2];
	}
	return out;
}

function concat(arrays) {
	const out = new Float32Array(arrays.reduce((n, a) => n + a.length, 0));
	let at = 0;
	for (const a of arrays) {
		out.set(a, at);
		at += a.length;
	}
	return out;
}

/** In place: (p - origin) / scale, so the head sits at the origin in its own units. */
function toHeadFrame(tris, origin, scale) {
	for (let i = 0; i < tris.length; i += 3) {
		tris[i] = (tris[i] - origin.x) / scale;
		tris[i + 1] = (tris[i + 1] - origin.y) / scale;
		tris[i + 2] = (tris[i + 2] - origin.z) / scale;
	}
	return tris;
}

/** Binary STL, converting three's Y up to Blender's Z up: (x, y, z) -> (x, -z, y). */
function encodeSTL(tris) {
	const n = tris.length / 9;
	const buf = Buffer.alloc(84 + n * 50);
	buf.write('PrintMiiOut FFL.js reference', 0);
	buf.writeUInt32LE(n, 80);
	const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3();
	for (let i = 0; i < n; i++) {
		const o = 84 + i * 50;
		const p = k => [tris[i * 9 + k * 3], -tris[i * 9 + k * 3 + 2], tris[i * 9 + k * 3 + 1]];
		const [p0, p1, p2] = [p(0), p(1), p(2)];
		a.fromArray(p0); b.fromArray(p1); c.fromArray(p2);
		const nrm = b.sub(a).cross(c.sub(a)).normalize();
		buf.writeFloatLE(nrm.x, o); buf.writeFloatLE(nrm.y, o + 4); buf.writeFloatLE(nrm.z, o + 8);
		[...p0, ...p1, ...p2].forEach((x, k) => buf.writeFloatLE(x, o + 12 + k * 4));
	}
	return buf;
}

/** Triangles from an STL (Blender Z up) back into three's Y up: (x, y, z) -> (x, z, -y). */
function decodeSTL(bytes) {
	const geo = new STLLoader().parse(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
	const p = geo.attributes.position.array;
	const out = new Float32Array(p.length);
	for (let i = 0; i < p.length; i += 3) {
		out[i] = p[i];
		out[i + 1] = p[i + 2];
		out[i + 2] = -p[i + 1];
	}
	return out;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

function bounds(tris) {
	const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
	for (let i = 0; i < tris.length; i++) {
		const k = i % 3;
		if (tris[i] < min[k]) min[k] = tris[i];
		if (tris[i] > max[k]) max[k] = tris[i];
	}
	return { min, max };
}

function triMesh(tris, material) {
	const geo = new THREE.BufferGeometry();
	geo.setAttribute('position', new THREE.BufferAttribute(tris, 3));
	geo.computeVertexNormals();
	return new THREE.Mesh(geo, material);
}

/** Orthographic camera looking at the box from `dir`, filling w x h pixels. */
function viewCamera(box, dir, w, h) {
	const centre = new THREE.Vector3(...box.min).add(new THREE.Vector3(...box.max)).multiplyScalar(0.5);
	const size = new THREE.Vector3(...box.max).sub(new THREE.Vector3(...box.min));
	const across = dir.x !== 0 ? size.z : size.x;
	const half = Math.max(size.y / 2, across / 2 * h / w) * 1.08;
	const cam = new THREE.OrthographicCamera(-half * w / h, half * w / h, half, -half, 0.1, 10000);
	cam.position.copy(centre).addScaledVector(dir, size.length() * 2 + 10);
	cam.lookAt(centre);
	return cam;
}

async function renderRGBA(renderer, scene, camera, w, h) {
	const rt = new THREE.RenderTarget(w, h, { samples: 4 });
	renderer.setRenderTarget(rt);
	renderer.render(scene, camera);
	const px = /** @type {Uint8Array} */ (await renderer.readRenderTargetPixelsAsync(rt, 0, 0, w, h));
	renderer.setRenderTarget(null);
	rt.dispose();
	// WebGPU pads each row to 256 bytes; drop the padding.
	const stride = Math.ceil(w * 4 / 256) * 256;
	if (stride === w * 4 || px.length < stride * (h - 1) + w * 4) return new Uint8Array(px);
	const out = new Uint8Array(w * h * 4);
	for (let y = 0; y < h; y++) out.set(px.subarray(y * stride, y * stride + w * 4), y * w * 4);
	return out;
}

function shadedScene(tris, colour) {
	const scene = new THREE.Scene();
	scene.add(new THREE.AmbientLight(0xffffff, 0.45));
	const key = new THREE.DirectionalLight(0xffffff, 2.2);
	key.position.set(0.6, 1, 1.2);
	scene.add(key);
	const fill = new THREE.DirectionalLight(0xffffff, 0.6);
	fill.position.set(-1, 0.3, -0.5);
	scene.add(fill);
	scene.add(triMesh(tris, new THREE.MeshLambertMaterial({ color: colour, side: THREE.DoubleSide })));
	return scene;
}

function silhouetteScene(tris) {
	const scene = new THREE.Scene();
	scene.add(triMesh(tris, new THREE.MeshBasicMaterial({ color: 0xffffff, side: THREE.DoubleSide })));
	return scene;
}

/** Overlap of two silhouettes, and an image of it. */
function overlap(a, b, w, h) {
	const img = new Uint8Array(w * h * 4);
	let both = 0, either = 0;
	for (let i = 0; i < w * h; i++) {
		const inA = a[i * 4 + 3] > 127, inB = b[i * 4 + 3] > 127;
		if (inA && inB) both++;
		if (inA || inB) either++;
		const c = inA && inB ? [150, 150, 150] : inA ? [220, 40, 40] : inB ? [40, 90, 230] : [255, 255, 255];
		img.set([...c, 255], i * 4);
	}
	return { iou: either ? both / either : 1, img };
}

function onWhite(px) {
	const out = new Uint8Array(px.length);
	for (let i = 0; i < px.length; i += 4) {
		const a = px[i + 3] / 255;
		for (let k = 0; k < 3; k++) out[i + k] = Math.round(px[i + k] * a + 255 * (1 - a));
		out[i + 3] = 255;
	}
	return out;
}

/** Panels side by side, each w x h. */
function tile(panels, w, h) {
	const W = w * panels.length;
	const out = new Uint8Array(W * h * 4);
	panels.forEach((p, n) => {
		for (let y = 0; y < h; y++)
			out.set(p.subarray(y * w * 4, (y + 1) * w * 4), (y * W + n * w) * 4);
	});
	return { px: out, w: W, h };
}

function stack(rows) {
	const w = rows[0].w;
	const out = new Uint8Array(rows.reduce((n, r) => n + r.px.length, 0));
	let at = 0;
	for (const r of rows) {
		out.set(r.px, at);
		at += r.px.length;
	}
	return { px: out, w, h: rows.reduce((n, r) => n + r.h, 0) };
}

function encodePNG(px, w, h) {
	const crcTable = Array.from({ length: 256 }, (_, n) => {
		let c = n;
		for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
		return c >>> 0;
	});
	const crc = (buf) => {
		let c = 0xFFFFFFFF;
		for (const b of buf) c = crcTable[(c ^ b) & 0xFF] ^ (c >>> 8);
		return (c ^ 0xFFFFFFFF) >>> 0;
	};
	const chunk = (type, data) => {
		const len = Buffer.alloc(4);
		len.writeUInt32BE(data.length);
		const td = Buffer.concat([Buffer.from(type), data]);
		const c = Buffer.alloc(4);
		c.writeUInt32BE(crc(td));
		return Buffer.concat([len, td, c]);
	};
	const raw = Buffer.alloc((w * 4 + 1) * h);
	for (let y = 0; y < h; y++) {
		raw[y * (w * 4 + 1)] = 0;
		Buffer.from(px.buffer, px.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
	}
	const ihdr = Buffer.alloc(13);
	ihdr.writeUInt32BE(w, 0);
	ihdr.writeUInt32BE(h, 4);
	ihdr.set([8, 6, 0, 0, 0], 8);
	return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
		chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

// ---------------------------------------------------------------------------
// Two STLs side by side
// ---------------------------------------------------------------------------

async function compareSTLs(values) {
	const zoom = values.face || !!values.box;
	const W = Number(values.width), H = zoom ? W : W * 3 / 2;
	await fs.mkdir(path.dirname(values.out), { recursive: true });
	addWebGPUExtensions();
	const renderer = await createThreeRenderer();
	renderer.onDeviceLost = () => {};
	try {
		const a = decodeSTL(await fs.readFile(values.a));
		const b = decodeSTL(await fs.readFile(values.b));
		const ba = bounds(a), bb = bounds(b);
		const box = {
			min: ba.min.map((x, k) => Math.min(x, bb.min[k])),
			max: ba.max.map((x, k) => Math.max(x, bb.max[k]))
		};
		if (values.face) {
			// The head sits at the origin; frame its front half.
			box.min = [-36, 0, -10];
			box.max = [36, 60, 30];
		} else if (values.box) {
			const v = values.box.split(',').map(Number);
			if (v.length !== 6 || v.some(Number.isNaN)) throw new Error('--box wants six numbers');
			box.min = v.slice(0, 3);
			box.max = v.slice(3);
		}
		const metrics = {};
		const rows = [];
		const views = zoom
			? [['front', new THREE.Vector3(0, 0, 1)], ['angle', new THREE.Vector3(0.7, 0.15, 0.7).normalize()]]
			: [['front', new THREE.Vector3(0, 0, 1)], ['side', new THREE.Vector3(1, 0, 0)]];
		for (const [name, dir] of views) {
			const cam = viewCamera(box, dir, W, H);
			if (!zoom) {
				const o = overlap(await renderRGBA(renderer, silhouetteScene(a), cam, W, H),
					await renderRGBA(renderer, silhouetteScene(b), cam, W, H), W, H);
				metrics[`iou_${name}`] = +o.iou.toFixed(4);
			}
			rows.push(tile([
				onWhite(await renderRGBA(renderer, shadedScene(a, 0xd8a070), cam, W, H)),
				onWhite(await renderRGBA(renderer, shadedScene(b, 0x70a0d8), cam, W, H))
			], W, H));
		}
		const img = stack(rows);
		await fs.writeFile(`${values.out}-ab.png`, encodePNG(img.px, img.w, img.h));
		console.log(JSON.stringify({ a: a.length / 9, b: b.length / 9, ...metrics }));
	} finally {
		renderer.dispose();
		const device = /** @type {any} */ (renderer.backend).device;
		if (device instanceof GPUDevice) {
			await device.queue.onSubmittedWorkDone();
			device.destroy();
		}
	}
}

// ---------------------------------------------------------------------------
// The reference figure
// ---------------------------------------------------------------------------

async function main() {
	const { values } = parseArgs({
		options: {
			resource: { type: 'string', short: 'r' },
			data: { type: 'string', short: 'd' },
			height: { type: 'string' },
			build: { type: 'string' },
			body: { type: 'string' },
			pose: { type: 'string', default: 'Finish' },
			stl: { type: 'string' },
			out: { type: 'string', short: 'o', default: 'out/mii' },
			width: { type: 'string', default: '512' },
			a: { type: 'string' },
			b: { type: 'string' },
			face: { type: 'boolean', default: false },
			box: { type: 'string' }
		}
	});
	if (values.a && values.b) {
		await compareSTLs(values);
		return;
	}
	if (!values.resource || !values.data) {
		console.error('usage: node reference.mjs --resource AFLResHigh_2_3.dat --data <hex|base64> [--height N] [--build N] [--body m|f] [--pose Finish] [--stl ours.stl] [--out out/name]');
		process.exit(1);
	}
	const W = Number(values.width), H = W * 3 / 2;
	await fs.mkdir(path.dirname(values.out), { recursive: true });

	addSkeletonScalingExtensions(THREE.Skeleton);
	addWebGPUExtensions();
	const renderer = await createThreeRenderer();
	renderer.onDeviceLost = () => {};
	const ffl = await FFL.initWithResource(await fs.readFile(values.resource), ModuleFFL);
	ffl.setRenderer(renderer);
	const matClass = /** @type {any} */ (FFLShaderNodeMaterial);

	try {
		const charModel = new CharModel(ffl, parseData(values.data), 0, matClass, renderer);
		const info = charModel.charInfo;
		const height = values.height !== undefined ? Number(values.height) : info.height;
		const build = values.build !== undefined ? Number(values.build) : info.build;
		const scale = charModel.getBodyScale(build, height);

		const bodyType = values.body ?? (info.gender === 1 ? 'f' : 'm');
		const gltf = await loadGLTF(path.join(repo, `miiBody${bodyType.toUpperCase()}_wiiu_plain.glb`));
		const model = SkeletonUtils.clone(gltf.scene);
		const mixer = new THREE.AnimationMixer(model);
		const clip = gltf.animations.find(a => a.name === values.pose);
		if (values.pose !== 'bind') {
			if (!clip) throw new Error(`No animation named ${values.pose}`);
			mixer.clipAction(clip).play();
		}
		mixer.update(0);
		const body = { model, animations: gltf.animations, mixer, scaleDesc: detectModelDesc(model) };
		prepareBodyForCharModel(body, matClass, charModel.favoriteColor, scale,
			pantsColors[PantsColor.GrayNormal]);
		attachHeadToBody(body, charModel.meshes);

		const scene = new THREE.Scene();
		scene.add(model);
		scene.updateMatrixWorld(true);
		model.traverse(o => o instanceof THREE.SkinnedMesh && o.skeleton.update());
		charModel.meshes.traverse(m => { m.frustumCulled = false; });

		// The figure in the head's frame: head at the origin, in its own units.
		const headPos = new THREE.Vector3(), headScale = new THREE.Vector3();
		charModel.meshes.matrixWorld.decompose(headPos, new THREE.Quaternion(), headScale);
		const parts = [];
		model.traverse((o) => {
			if (o instanceof THREE.SkinnedMesh) parts.push(skinnedTriangles(o));
		});
		charModel.meshes.traverse((o) => {
			if (o instanceof THREE.Mesh && !(o instanceof THREE.SkinnedMesh) &&
				o.geometry.userData.modulateType <= OPAQUE_MAX)
				parts.push(meshTriangles(o));
		});
		const ref = toHeadFrame(concat(parts), headPos, headScale.x);
		await fs.writeFile(`${values.out}-ref.stl`, encodeSTL(ref));

		const refBox = bounds(ref);
		const metrics = {
			gender: info.gender, body: bodyType, height, build, scale,
			reference: { min: refBox.min.map(x => +x.toFixed(2)), max: refBox.max.map(x => +x.toFixed(2)) }
		};

		// The reference as the game shades it, framed like the comparison.
		const shadedCam = viewCamera(refBox, new THREE.Vector3(0, 0, 1), W, H);
		// Back from head units to the scene's own.
		shadedCam.position.multiplyScalar(headScale.x).add(headPos);
		for (const k of ['left', 'right', 'top', 'bottom']) shadedCam[k] *= headScale.x;
		shadedCam.near *= headScale.x;
		shadedCam.far *= headScale.x;
		shadedCam.updateProjectionMatrix();
		await fs.writeFile(`${values.out}-ref.png`, encodePNG(onWhite(await renderRGBA(renderer, scene, shadedCam, W, H)), W, H));

		if (values.stl) {
			const ours = decodeSTL(await fs.readFile(values.stl));
			const ourBox = bounds(ours);
			metrics.ours = { min: ourBox.min.map(x => +x.toFixed(2)), max: ourBox.max.map(x => +x.toFixed(2)) };
			const box = {
				min: refBox.min.map((x, k) => Math.min(x, ourBox.min[k])),
				max: refBox.max.map((x, k) => Math.max(x, ourBox.max[k]))
			};
			const rows = [];
			for (const [name, dir] of [['front', new THREE.Vector3(0, 0, 1)], ['side', new THREE.Vector3(1, 0, 0)]]) {
				const cam = viewCamera(box, dir, W, H);
				const a = await renderRGBA(renderer, silhouetteScene(ref), cam, W, H);
				const b = await renderRGBA(renderer, silhouetteScene(ours), cam, W, H);
				const o = overlap(a, b, W, H);
				metrics[`iou_${name}`] = +o.iou.toFixed(4);
				rows.push(tile([
					onWhite(await renderRGBA(renderer, shadedScene(ref, 0xd8a070), cam, W, H)),
					onWhite(await renderRGBA(renderer, shadedScene(ours, 0x70a0d8), cam, W, H)),
					o.img
				], W, H));
			}
			const img = stack(rows);
			await fs.writeFile(`${values.out}-compare.png`, encodePNG(img.px, img.w, img.h));
		}
		console.log(JSON.stringify(metrics));
		charModel.dispose();
	} finally {
		ffl.dispose();
		renderer.dispose();
		const device = /** @type {any} */ (renderer.backend).device;
		if (device instanceof GPUDevice) {
			await device.queue.onSubmittedWorkDone();
			device.destroy();
		}
	}
}

main().catch((e) => {
	console.error(e);
	process.exit(1);
});
