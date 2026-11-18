// Names the resource behind a WebGPU validation error.
//
// Dawn's messages identify buffers and textures only as "[Buffer]", and one
// invalid command invalidates the frame's whole command buffer, so a single
// bad copy shows up as every upload in that frame going missing. This probe
// wraps the page's WebGPU calls, records each buffer and texture descriptor
// as it is created, and checks the calls that most often break a frame
// against the WebGPU rules before passing them on:
//
//   copyBufferToBuffer   offsets and size multiples of 4, CopySrc/CopyDst
//                        usage, in bounds
//   writeBuffer          offset and size multiples of 4, CopyDst usage, in
//                        bounds
//   buffer/texture copy  offset multiple of 4, bytesPerRow multiple of 256
//                        when more than one row, CopySrc/CopyDst usage
//   setVertexBuffer /    Vertex/Index usage
//   setIndexBuffer
//
// Each distinct violation is reported once with the descriptors involved.
// Uncaptured device errors are reported with the last calls made before them.
// Exits 1 when anything was reported.
//
// Usage (from a directory whose package.json provides playwright):
//   node webgpu-call-probe.mjs <export_dir | url> [wait_ms]
// Env: BOOT_ARGS (extra Chrome flags), THROTTLE (CPU throttle rate, e.g. 6
// for a phone-class CPU), VIEWPORT (WxH, default 1280x720).
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { createRequire } from 'node:module';

const require = createRequire(path.join(process.cwd(), 'package.json'));
const { chromium } = require('playwright');

const target = process.argv[2];
const waitMs = parseInt(process.argv[3] ?? '20000', 10);
if (!target) {
	console.error('usage: node webgpu-call-probe.mjs <export_dir | url> [wait_ms]');
	process.exit(2);
}

let server = null;
let url = target;
if (!/^https?:/.test(target)) {
	const dir = path.resolve(target);
	const types = {
		'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
		'.pck': 'application/octet-stream', '.png': 'image/png', '.json': 'application/json',
	};
	server = http.createServer((req, res) => {
		const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
		if (req.method === 'POST') {
			req.resume();
			req.on('end', () => res.end());
			return;
		}
		const file = path.join(dir, rel === '/' ? 'index.html' : rel);
		if (!file.startsWith(dir) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
			res.writeHead(404);
			res.end();
			return;
		}
		res.writeHead(200, {
			'Content-Type': types[path.extname(file)] ?? 'application/octet-stream',
			'Cross-Origin-Opener-Policy': 'same-origin',
			'Cross-Origin-Embedder-Policy': 'require-corp',
			'Cache-Control': 'no-store',
		});
		res.end(fs.readFileSync(file));
	});
	url = await new Promise((resolve) => {
		server.listen(0, '127.0.0.1', () => resolve(`http://127.0.0.1:${server.address().port}/index.html`));
	});
}

// Runs in the page before any engine code.
function installProbe() {
	const BU = { MAP_READ: 1, MAP_WRITE: 2, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64, STORAGE: 128, INDIRECT: 256, QUERY_RESOLVE: 512 };
	const TU = { COPY_SRC: 1, COPY_DST: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 };
	const flagNames = (table, bits) => Object.keys(table).filter((k) => bits & table[k]).join('|') || 'none';
	const info = new WeakMap();
	const counts = {};
	let nextId = 0;
	const recent = [];
	const seen = new Set();
	const say = (line) => console.log('PROBE ' + line);
	const note = (call) => {
		recent.push(call);
		if (recent.length > 12) {
			recent.shift();
		}
	};
	const describe = (obj) => {
		const i = info.get(obj);
		return i ? JSON.stringify(i) : 'unknown (created before the probe)';
	};
	const report = (rule, detail) => {
		const key = rule + ' ' + detail;
		if (seen.has(key)) {
			return;
		}
		seen.add(key);
		say(rule + ': ' + detail);
	};

	const wrap = (proto, name, before) => {
		const original = proto && proto[name];
		if (!original) {
			return;
		}
		proto[name] = function (...args) {
			counts[name] = (counts[name] ?? 0) + 1;
			try {
				before.apply(this, args);
			} catch (e) {
				say('internal error in the ' + name + ' check: ' + e);
			}
			return original.apply(this, args);
		};
	};

	const createBuffer = GPUDevice.prototype.createBuffer;
	GPUDevice.prototype.createBuffer = function (d) {
		const buffer = createBuffer.call(this, d);
		info.set(buffer, { kind: 'buffer', id: nextId++, size: d.size, usage: flagNames(BU, d.usage), usageBits: d.usage, label: d.label || undefined });
		return buffer;
	};
	const createTexture = GPUDevice.prototype.createTexture;
	GPUDevice.prototype.createTexture = function (d) {
		const texture = createTexture.call(this, d);
		const s = d.size;
		const size = Array.isArray(s) ? s.join('x') : [s.width, s.height ?? 1, s.depthOrArrayLayers ?? 1].join('x');
		info.set(texture, { kind: 'texture', id: nextId++, format: d.format, size, usage: flagNames(TU, d.usage), usageBits: d.usage, label: d.label || undefined });
		return texture;
	};
	const requestDevice = GPUAdapter.prototype.requestDevice;
	GPUAdapter.prototype.requestDevice = async function (...args) {
		const device = await requestDevice.apply(this, args);
		device.addEventListener('uncapturederror', (ev) => {
			const first = ev.error.message.split('\n')[0];
			if (!seen.has('uncaptured ' + first)) {
				seen.add('uncaptured ' + first);
				say('uncaptured device error: ' + ev.error.message.replace(/\n/g, ' | ').slice(0, 400));
				say('  last calls before it: ' + recent.join(' ; '));
			}
		});
		return device;
	};

	const bufferUsage = (buffer, bit) => (info.get(buffer)?.usageBits ?? bit) & bit;
	const textureUsage = (texture, bit) => (info.get(texture)?.usageBits ?? bit) & bit;
	const bufferSize = (buffer) => info.get(buffer)?.size ?? Infinity;

	wrap(GPUCommandEncoder.prototype, 'copyBufferToBuffer', function (src, srcOffset, dst, dstOffset, size) {
		if (typeof srcOffset === 'object') {
			// copyBufferToBuffer(src, dst[, size]) form.
			size = dst ?? bufferSize(src);
			dst = srcOffset;
			srcOffset = 0;
			dstOffset = 0;
		} else if (size === undefined) {
			size = bufferSize(src) - srcOffset;
		}
		note(`copyBufferToBuffer(${srcOffset}->${dstOffset}, ${size})`);
		const bad = [];
		if (size % 4) bad.push(`size ${size} not a multiple of 4`);
		if (srcOffset % 4) bad.push(`source offset ${srcOffset} not a multiple of 4`);
		if (dstOffset % 4) bad.push(`destination offset ${dstOffset} not a multiple of 4`);
		if (!bufferUsage(src, BU.COPY_SRC)) bad.push('source lacks CopySrc');
		if (!bufferUsage(dst, BU.COPY_DST)) bad.push('destination lacks CopyDst');
		if (srcOffset + size > bufferSize(src)) bad.push('reads past the source');
		if (dstOffset + size > bufferSize(dst)) bad.push('writes past the destination');
		if (bad.length) {
			report('copyBufferToBuffer', `${bad.join(', ')}; src=${describe(src)} dst=${describe(dst)} srcOffset=${srcOffset} dstOffset=${dstOffset} size=${size}`);
		}
	});

	wrap(GPUQueue.prototype, 'writeBuffer', function (buffer, offset, data, dataOffset, size) {
		const elementBytes = data.BYTES_PER_ELEMENT ?? 1;
		const bytes = size !== undefined ? size * elementBytes : (data.byteLength - (dataOffset ?? 0) * elementBytes);
		note(`writeBuffer(@${offset}, ${bytes})`);
		const bad = [];
		if (offset % 4) bad.push(`offset ${offset} not a multiple of 4`);
		if (bytes % 4) bad.push(`size ${bytes} not a multiple of 4`);
		if (!bufferUsage(buffer, BU.COPY_DST)) bad.push('buffer lacks CopyDst');
		if (offset + bytes > bufferSize(buffer)) bad.push('writes past the buffer');
		if (bad.length) {
			report('writeBuffer', `${bad.join(', ')}; buffer=${describe(buffer)} offset=${offset} size=${bytes}`);
		}
	});

	const checkLayout = (layout, extent) => {
		const bad = [];
		const rows = (extent.height ?? extent[1] ?? 1) * (extent.depthOrArrayLayers ?? extent[2] ?? 1);
		if ((layout.offset ?? 0) % 4) bad.push(`buffer offset ${layout.offset} not a multiple of 4`);
		if (rows > 1 && layout.bytesPerRow !== undefined && layout.bytesPerRow % 256) bad.push(`bytesPerRow ${layout.bytesPerRow} not a multiple of 256`);
		return bad;
	};
	wrap(GPUCommandEncoder.prototype, 'copyBufferToTexture', function (src, dst, extent) {
		note('copyBufferToTexture');
		const bad = checkLayout(src, extent);
		if (!bufferUsage(src.buffer, BU.COPY_SRC)) bad.push('buffer lacks CopySrc');
		if (!textureUsage(dst.texture, TU.COPY_DST)) bad.push('texture lacks CopyDst');
		if (bad.length) {
			report('copyBufferToTexture', `${bad.join(', ')}; buffer=${describe(src.buffer)} texture=${describe(dst.texture)} layout=${JSON.stringify({ offset: src.offset, bytesPerRow: src.bytesPerRow, rowsPerImage: src.rowsPerImage })}`);
		}
	});
	wrap(GPUCommandEncoder.prototype, 'copyTextureToBuffer', function (src, dst, extent) {
		note('copyTextureToBuffer');
		const bad = checkLayout(dst, extent);
		if (!textureUsage(src.texture, TU.COPY_SRC)) bad.push('texture lacks CopySrc');
		if (!bufferUsage(dst.buffer, BU.COPY_DST)) bad.push('buffer lacks CopyDst');
		if (bad.length) {
			report('copyTextureToBuffer', `${bad.join(', ')}; texture=${describe(src.texture)} buffer=${describe(dst.buffer)} layout=${JSON.stringify({ offset: dst.offset, bytesPerRow: dst.bytesPerRow, rowsPerImage: dst.rowsPerImage })}`);
		}
	});
	wrap(GPUCommandEncoder.prototype, 'copyTextureToTexture', function (src, dst) {
		note('copyTextureToTexture');
		const bad = [];
		if (!textureUsage(src.texture, TU.COPY_SRC)) bad.push('source lacks CopySrc');
		if (!textureUsage(dst.texture, TU.COPY_DST)) bad.push('destination lacks CopyDst');
		if (bad.length) {
			report('copyTextureToTexture', `${bad.join(', ')}; src=${describe(src.texture)} dst=${describe(dst.texture)}`);
		}
	});

	for (const proto of [GPURenderPassEncoder.prototype, GPURenderBundleEncoder.prototype]) {
		wrap(proto, 'setVertexBuffer', function (slot, buffer) {
			if (buffer && !bufferUsage(buffer, BU.VERTEX)) {
				report('setVertexBuffer', `buffer lacks Vertex usage; slot=${slot} buffer=${describe(buffer)}`);
			}
		});
		wrap(proto, 'setIndexBuffer', function (buffer, format) {
			if (!bufferUsage(buffer, BU.INDEX)) {
				report('setIndexBuffer', `buffer lacks Index usage; format=${format} buffer=${describe(buffer)}`);
			}
		});
	}
	window.__webgpuCallProbe = { counts };
}

const extra = (process.env.BOOT_ARGS ?? '').split(' ').filter(Boolean);
const [vw, vh] = (process.env.VIEWPORT ?? '1280x720').split('x').map(Number);
const browser = await chromium.launch({ channel: 'chrome', headless: false, args: ['--ignore-certificate-errors', '--force-high-performance-gpu', ...extra] });
let reports = 0;
try {
	const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: vw, height: vh } });
	const page = await context.newPage();
	if (process.env.THROTTLE) {
		const cdp = await context.newCDPSession(page);
		await cdp.send('Emulation.setCPUThrottlingRate', { rate: parseFloat(process.env.THROTTLE) });
		console.log(`CPU throttle x${process.env.THROTTLE}`);
	}
	await page.addInitScript(installProbe);
	page.on('console', (m) => {
		const t = m.text();
		if (t.startsWith('PROBE ')) {
			console.log(t.slice(6));
			if (!t.startsWith('PROBE   last calls')) {
				reports++;
			}
		}
	});
	page.on('pageerror', (e) => console.log(`page error: ${e.message}`));
	await page.goto(url, { waitUntil: 'load', timeout: 120000 });
	await page.waitForTimeout(waitMs);
	const counts = await page.evaluate(() => window.__webgpuCallProbe?.counts ?? null);
	const checked = counts ? Object.values(counts).reduce((a, b) => a + b, 0) : 0;
	if (!checked) {
		console.log('webgpu-call-probe: no WebGPU calls were seen; nothing was checked');
		reports++;
	} else {
		console.log(`checked ${checked} calls: ${Object.entries(counts).map(([k, v]) => `${k} ${v}`).join(', ')}`);
	}
} finally {
	await browser.close();
	if (server) {
		server.close();
	}
}
console.log(reports ? `webgpu-call-probe: ${reports} problem(s) reported` : 'webgpu-call-probe: no problems found');
process.exitCode = reports ? 1 : 0;
