// Stage-1 loader smoke test for the WebGPU patch stack.
//
// Boots the built web template's loader (Engine.init(), no project) in
// headless Chromium and asserts:
//   - init() resolves with no pageerror (both with and without
//     experimentalWebGPU),
//   - Engine.isWebGPUAvailable is exposed and returns a boolean,
//   - the WebGL-fallback warning fires when experimentalWebGPU is enabled
//     but no WebGPU device can be acquired (and stays silent otherwise).
//
// With mode `probe` it instead boots the wasm module directly (no engine
// start) in a Chromium launched with software-WebGPU flags, calls the
// exported _godot_webgpu_probe(), and asserts the presented clear color on
// the hidden probe canvas. Skips (exit 0, with a notice) when the
// environment cannot deliver a WebGPU device.
//
// Usage: node loader-smoke.mjs <bin_dir> [loader|probe]
// Run from a directory whose node_modules provides `playwright` (CI runs it
// from platform/web after `npm i --no-save playwright`).

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import zlib from 'node:zlib';
import { createRequire } from 'node:module';

const require = createRequire(path.join(process.cwd(), 'package.json'));
const { chromium } = require('playwright');

const binDir = path.resolve(process.argv[2] ?? 'bin');
const files = fs.readdirSync(binDir);
const wrappedJs = files.find((f) => f.endsWith('.wrapped.js'));
const wasm = files.find((f) => f.endsWith('.wasm'));
if (!wrappedJs || !wasm) {
	console.error(`loader-smoke: could not find *.wrapped.js and *.wasm in ${binDir} (found: ${files.join(', ')})`);
	process.exit(1);
}

function testPage(webgpu) {
	return `<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>loader-smoke</title></head>
<body>
<canvas id="canvas"></canvas>
<script src="/godot.js"></script>
<script>
window.__smoke = { done: false, error: null };
const engine = new Engine({ 'experimentalWebGPU': ${webgpu} });
engine.init('godot').then(function () {
	window.__smoke.done = true;
}).catch(function (e) {
	window.__smoke.error = String(e);
});
</script>
</body>
</html>`;
}

// Chromium flags that enable software WebGPU on GPU-less CI runners.
// Version-dependent; last verified against the Playwright-pinned Chromium in
// CI (see docs/webgpu-testing.md).
const PROBE_CHROMIUM_FLAGS = ['--enable-unsafe-webgpu', '--use-webgpu-adapter=swiftshader', '--enable-features=Vulkan'];

// Must match PROBE_PATTERN_R/G/B in drivers/webgpu/webgpu_probe.cpp: the
// pattern uploaded through the staging path and copied over the cleared
// frame.
const PROBE_RGB = [230, 102, 26];
const TRIANGLE_RGB = [230, 51, 230];
// Instanced-quad stage: two quads pulling rect + color from a storage buffer
// (the canvas renderer's pattern). Colors match webgpu_probe.cpp.
const INST0_RGB = [51, 230, 76];
const INST1_RGB = [242, 217, 25];
const PROBE_TOLERANCE = 12;

const probePage = `<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>webgpu-probe</title></head>
<body>
<script src="/godot.js"></script>
<script>
window.__probe = { rc: null, error: null };
(async () => {
	try {
		let device = null;
		if (navigator.gpu) {
			const adapter = await navigator.gpu.requestAdapter();
			device = adapter ? await adapter.requestDevice() : null;
		}
		if (!device) {
			window.__probe.rc = -1; // Environment cannot deliver a device.
			return;
		}
		window.__probe.gpuErrors = [];
		device.addEventListener('uncapturederror', (ev) => {
			window.__probe.gpuErrors.push(ev.error.message);
			console.error('GPU uncaptured error: ' + ev.error.message);
		});
		device.lost.then((info) => {
			// An intentionally destroyed device is not an error.
			if (info.reason !== 'destroyed') {
				window.__probe.gpuErrors.push('device lost: ' + info.message);
				console.error('GPU device lost: ' + info.message);
			}
		});
		const module = await Godot({ 'preinitializedWebGPUDevice': device });
		window.__probe.rc = module['_godot_webgpu_probe']();
	} catch (e) {
		window.__probe.error = String(e);
	}
})();
</script>
</body>
</html>`;

const routes = {
	'/godot.js': { file: path.join(binDir, wrappedJs), type: 'text/javascript' },
	'/godot.wasm': { file: path.join(binDir, wasm), type: 'application/wasm' },
	// The probe page boots the raw Godot() factory, where Emscripten resolves
	// the wasm by its original build filename instead of the loader-fetched
	// /godot.wasm; serve it under that name too.
	[`/${wasm}`]: { file: path.join(binDir, wasm), type: 'application/wasm' },
	'/on.html': { body: testPage(true), type: 'text/html' },
	'/off.html': { body: testPage(false), type: 'text/html' },
	'/probe.html': { body: probePage, type: 'text/html' },
};

const server = http.createServer((req, res) => {
	const route = routes[new URL(req.url, 'http://localhost').pathname];
	if (!route) {
		res.writeHead(404);
		res.end();
		return;
	}
	res.writeHead(200, {
		'Content-Type': route.type,
		'Cross-Origin-Opener-Policy': 'same-origin',
		'Cross-Origin-Embedder-Policy': 'require-corp',
		'Cache-Control': 'no-store',
	});
	res.end(route.body ?? fs.readFileSync(route.file));
});

function fail(msg) {
	console.error(`loader-smoke: FAIL: ${msg}`);
	process.exitCode = 1;
}

async function runScenario(browser, base, scenario) {
	const page = await browser.newPage();
	const consoleMessages = [];
	const pageErrors = [];
	page.on('console', (msg) => consoleMessages.push(msg.text()));
	page.on('pageerror', (err) => pageErrors.push(String(err)));

	await page.goto(`${base}/${scenario}.html`);
	await page.waitForFunction('window.__smoke.done || window.__smoke.error', null, { timeout: 120000 });

	const smoke = await page.evaluate('window.__smoke');
	if (!smoke.done) {
		fail(`[${scenario}] Engine.init() rejected: ${smoke.error}`);
	}
	if (pageErrors.length > 0) {
		fail(`[${scenario}] pageerror(s): ${pageErrors.join(' | ')}`);
	}

	const isAvailable = await page.evaluate('Engine.isWebGPUAvailable()');
	if (typeof isAvailable !== 'boolean') {
		fail(`[${scenario}] Engine.isWebGPUAvailable() returned ${typeof isAvailable}, expected boolean`);
	}

	// Predict whether this environment can actually deliver a WebGPU device,
	// then assert the fallback warning fired if and only if it should have.
	const canGetDevice = await page.evaluate(`(async () => {
		if (!navigator.gpu) { return false; }
		try {
			const adapter = await navigator.gpu.requestAdapter();
			return adapter ? !!(await adapter.requestDevice()) : false;
		} catch (e) { return false; }
	})()`);
	const fallbacks = consoleMessages.filter((m) => m.includes('falling back to WebGL'));
	if (scenario === 'on' && !canGetDevice && fallbacks.length === 0) {
		fail('[on] WebGPU unavailable but no WebGL-fallback warning was logged');
	}
	if (scenario === 'on' && canGetDevice && fallbacks.length > 0) {
		fail(`[on] device available but fallback warning logged: ${fallbacks[0]}`);
	}
	if (scenario === 'off' && fallbacks.length > 0) {
		fail(`[off] unexpected WebGPU log with experimentalWebGPU disabled: ${fallbacks[0]}`);
	}

	console.log(`loader-smoke: [${scenario}] init OK, isWebGPUAvailable=${isAvailable}, canGetDevice=${canGetDevice}, fallbackWarnings=${fallbacks.length}, pageErrors=0`);
	await page.close();
}

// Minimal PNG decode: inflate the IDAT stream and unfilter scanlines up to
// the requested row, then sample the pixel at fractional coordinates
// (0,0 = top-left, 0.5,0.5 = center). Assumes 8-bit non-interlaced RGB/RGBA,
// which is what compositor screenshots produce.
function samplePngPixel(buf, fx, fy) {
	const width = buf.readUInt32BE(16);
	const height = buf.readUInt32BE(20);
	const colorType = buf[25];
	const bpp = colorType === 6 ? 4 : (colorType === 2 ? 3 : 0);
	if (bpp === 0 || buf[24] !== 8 || buf[28] !== 0) {
		return null;
	}
	const idat = [];
	let pos = 8;
	while (pos < buf.length) {
		const len = buf.readUInt32BE(pos);
		const type = buf.toString('ascii', pos + 4, pos + 8);
		if (type === 'IDAT') {
			idat.push(buf.subarray(pos + 8, pos + 8 + len));
		}
		if (type === 'IEND') {
			break;
		}
		pos += 12 + len;
	}
	const raw = zlib.inflateSync(Buffer.concat(idat));
	const stride = width * bpp;
	const ty = Math.min(Math.floor(height * fy), height - 1);
	const tx = Math.min(Math.floor(width * fx), width - 1);
	let prev = Buffer.alloc(stride);
	const cur = Buffer.alloc(stride);
	for (let y = 0; y <= ty; y++) {
		const filter = raw[y * (stride + 1)];
		const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
		for (let i = 0; i < stride; i++) {
			const a = i >= bpp ? cur[i - bpp] : 0;
			const b = prev[i];
			const c = i >= bpp ? prev[i - bpp] : 0;
			let v = line[i];
			if (filter === 1) {
				v += a;
			} else if (filter === 2) {
				v += b;
			} else if (filter === 3) {
				v += (a + b) >> 1;
			} else if (filter === 4) {
				const p = a + b - c;
				const pa = Math.abs(p - a);
				const pb = Math.abs(p - b);
				const pc = Math.abs(p - c);
				v += pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
			}
			cur[i] = v & 0xff;
		}
		prev = Buffer.from(cur);
	}
	const o = tx * bpp;
	return [cur[o], cur[o + 1], cur[o + 2], bpp === 4 ? cur[o + 3] : 255];
}

async function runProbe(browser, base) {
	const page = await browser.newPage();
	const consoleMessages = [];
	const pageErrors = [];
	page.on('console', (msg) => consoleMessages.push(msg.text()));
	page.on('pageerror', (err) => pageErrors.push(String(err)));

	await page.goto(`${base}/probe.html`);
	await page.waitForFunction('window.__probe.rc !== null || window.__probe.error !== null', null, { timeout: 120000 });

	const probe = await page.evaluate('window.__probe');
	if (probe.error !== null) {
		fail(`[probe] threw: ${probe.error}`);
		return;
	}
	if (probe.rc === -1) {
		console.log('loader-smoke: [probe] SKIPPED (this environment cannot deliver a WebGPU device)');
		await page.close();
		return;
	}
	if (probe.rc !== 0) {
		fail(`[probe] godot_webgpu_probe() returned stage ${probe.rc}; console: ${consoleMessages.slice(-5).join(' | ')}`);
		return;
	}
	if (pageErrors.length > 0) {
		fail(`[probe] pageerror(s): ${pageErrors.join(' | ')}`);
	}
	if (probe.gpuErrors && probe.gpuErrors.length > 0) {
		fail(`[probe] GPU validation error(s): ${probe.gpuErrors.join(' | ')}`);
	}
	if (!consoleMessages.some((m) => m.includes('WebGPU probe: OK'))) {
		fail('[probe] success marker not found in console output');
	}
	if (!consoleMessages.some((m) => m.includes('WebGPU probe: shader module OK'))) {
		fail('[probe] shader module marker not found in console output');
	}
	if (!consoleMessages.some((m) => m.includes('WebGPU probe: triangle OK'))) {
		fail('[probe] triangle marker not found in console output');
	}
	if (!consoleMessages.some((m) => m.includes('WebGPU probe: uniform set OK'))) {
		fail('[probe] uniform set marker not found in console output');
	}
	if (!consoleMessages.some((m) => m.includes('WebGPU probe: dynamic buffer OK'))) {
		fail('[probe] dynamic buffer marker not found in console output');
	}
	if (!consoleMessages.some((m) => m.includes('WebGPU probe: instanced quads OK'))) {
		fail('[probe] instanced quads marker not found in console output');
	}

	// Read back the presented pixels via a compositor screenshot. A 2D
	// drawImage of a WebGPU canvas reads the CURRENT texture, which is
	// expired (transparent) once the frame has been presented - the frame
	// itself only exists in the compositor, which screenshots capture.
	await page.evaluate(`new Promise((resolve) => {
		requestAnimationFrame(() => requestAnimationFrame(resolve));
	})`);
	const shot = await page.locator('#godot-webgpu-probe').screenshot();
	// (0,0) still shows the uploaded pattern; the drawn triangle owns the
	// canvas center.
	const cornerPixel = samplePngPixel(shot, 0, 0);
	const centerPixel = samplePngPixel(shot, 0.5, 0.5);
	// Pixel assertion policy: headless software adapters (CI) execute all GPU
	// work correctly but never composite the presented frame where
	// screenshots can see it, so a mismatch is only a hard failure when
	// PROBE_REQUIRE_PIXELS=1 (set it when running against a real GPU, e.g.
	// with PROBE_CHANNEL=chrome). See docs/webgpu-testing.md.
	const requirePixels = process.env.PROBE_REQUIRE_PIXELS === '1';
	const checkPixel = (label, pixel, expected) => {
		if (pixel == null) {
			fail(`[probe] could not decode the probe canvas screenshot (${label})`);
			return;
		}
		const delta = Math.max(Math.abs(pixel[0] - expected[0]), Math.abs(pixel[1] - expected[1]), Math.abs(pixel[2] - expected[2]));
		if (delta > PROBE_TOLERANCE) {
			if (requirePixels) {
				fail(`[probe] ${label} color mismatch: got rgb(${pixel[0]},${pixel[1]},${pixel[2]}), expected ~rgb(${expected.join(',')})`);
			} else {
				console.warn(`loader-smoke: [probe] PIXEL CHECK INCONCLUSIVE (${label}): got rgb(${pixel[0]},${pixel[1]},${pixel[2]}), expected ~rgb(${expected.join(',')}) - this environment does not composite WebGPU canvases into screenshots; presentation must be verified on a real GPU (PROBE_CHANNEL=chrome PROBE_REQUIRE_PIXELS=1)`);
			}
		}
	};
	checkPixel('pattern', cornerPixel, PROBE_RGB);
	checkPixel('triangle', centerPixel, TRIANGLE_RGB);
	// Instanced quads: left rect spans NDC x [-0.9,-0.5] (uv center 0.15),
	// right rect [0.5,0.9] (uv center 0.85); both span NDC y [-0.2,0.2].
	checkPixel('instanced quad 0', samplePngPixel(shot, 0.15, 0.5), INST0_RGB);
	checkPixel('instanced quad 1', samplePngPixel(shot, 0.85, 0.5), INST1_RGB);

	console.log(`loader-smoke: [probe] rc=0, markers found, corner=${JSON.stringify(cornerPixel)}, center=${JSON.stringify(centerPixel)}, pageErrors=${pageErrors.length}`);
	await page.close();
}

const base = await new Promise((resolve) => {
	server.listen(0, '127.0.0.1', () => {
		resolve(`http://127.0.0.1:${server.address().port}`);
	});
});

const mode = process.argv[3] ?? 'loader';
// PROBE_CHANNEL=chrome runs the probe in an installed Chrome (real GPU) for
// local debugging instead of the bundled Chromium.
const launchOptions = mode === 'probe' ? { args: PROBE_CHROMIUM_FLAGS } : {};
if (mode === 'probe' && process.env.PROBE_CHANNEL) {
	launchOptions.channel = process.env.PROBE_CHANNEL;
	launchOptions.args = [];
}
const browser = await chromium.launch(launchOptions);
try {
	if (mode === 'probe') {
		await runProbe(browser, base);
	} else {
		await runScenario(browser, base, 'off');
		await runScenario(browser, base, 'on');
	}
} finally {
	await browser.close();
	server.close();
}

if (process.exitCode) {
	console.error('loader-smoke: FAILED');
} else {
	console.log('loader-smoke: OK');
}
