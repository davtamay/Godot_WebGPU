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
// Usage: node loader-smoke.mjs <bin_dir>
// Run from a directory whose node_modules provides `playwright` (CI runs it
// from platform/web after `npm i --no-save playwright`).

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
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

const routes = {
	'/godot.js': { file: path.join(binDir, wrappedJs), type: 'text/javascript' },
	'/godot.wasm': { file: path.join(binDir, wasm), type: 'application/wasm' },
	'/on.html': { body: testPage(true), type: 'text/html' },
	'/off.html': { body: testPage(false), type: 'text/html' },
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

const base = await new Promise((resolve) => {
	server.listen(0, '127.0.0.1', () => {
		resolve(`http://127.0.0.1:${server.address().port}`);
	});
});

const browser = await chromium.launch();
try {
	await runScenario(browser, base, 'off');
	await runScenario(browser, base, 'on');
} finally {
	await browser.close();
	server.close();
}

if (process.exitCode) {
	console.error('loader-smoke: FAILED');
} else {
	console.log('loader-smoke: OK');
}
