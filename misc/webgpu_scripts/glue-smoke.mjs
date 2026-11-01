// WebXR glue smoke test for the WebGPU patch stack.
//
// Boots the webxr_fixture pack on a web template inside IWER, Meta's emulated
// WebXR runtime, so a real XRSession runs library_godot_webxr.js and
// webxr_interface_js.cpp with no headset. Asserts the failures that used to
// reach a device first: a frame callback that throws, hands that freeze across
// an input-source swap, and a root viewport left at eye size after a session.
//
// IWER has no XRGPUBinding and no createProjectionLayer, so this covers the GL
// baseLayer path only.
//
// Usage: node glue-smoke.mjs <template_dir> <pck>
// <template_dir> holds an unpacked web template (godot.js, godot.wasm). Run from
// a directory whose node_modules provides `playwright`. IWER_JS names iwer.min.js
// when it is not under node_modules/iwer/build.
//
// GLUE_BREAK puts a shipped bug back, to prove the harness can fail: `iterator`
// is [wgpu 105]'s exhausted joint iterator, `churn` is the source removal that
// froze hands before [wgpu 50]. Both need an unminified template, because Closure
// renames what they search for.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { createRequire } from 'node:module';

const require = createRequire(path.join(process.cwd(), 'package.json'));
const { chromium } = require('playwright');

const templateDir = path.resolve(process.argv[2] ?? 'template');
const pckPath = path.resolve(process.argv[3] ?? 'glue.pck');
const iwerPath = process.env.IWER_JS ?? path.join(process.cwd(), 'node_modules/iwer/build/iwer.min.js');

const PAGE = `<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>glue-smoke</title></head>
<body style="margin:0">
<canvas id="canvas"></canvas>
<script src="/godot.js"></script>
<script>
new Engine({ 'canvasResizePolicy': 1, 'experimentalWebGPU': false }).startGame({ 'executable': 'godot', 'mainPack': 'index.pck' })
	.catch(function (e) { console.error('startGame failed: ' + e); });
</script>
</body>
</html>`;

const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };

const server = http.createServer((req, res) => {
	const name = new URL(req.url, 'http://x').pathname;
	res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
	res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
	if (name === '/') {
		res.setHeader('Content-Type', 'text/html');
		res.end(PAGE);
		return;
	}
	const file = name === '/index.pck' ? pckPath : path.join(templateDir, path.basename(name));
	if (!fs.existsSync(file)) {
		res.statusCode = 404;
		res.end();
		return;
	}
	res.setHeader('Content-Type', TYPES[path.extname(file)] ?? 'application/octet-stream');
	fs.createReadStream(file).pipe(res);
});
await new Promise((resolve) => { server.listen(0, '127.0.0.1', resolve); });
const url = `http://127.0.0.1:${server.address().port}/`;

const browser = await chromium.launch({
	args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
});
const page = await (await browser.newContext({ viewport: { width: 800, height: 450 } })).newPage();

const glue = [];
const errors = [];
page.on('console', (message) => {
	const text = message.text();
	if (text.startsWith('GLUE ')) {
		glue.push(text);
		console.log(`  ${text}`);
	} else if (message.type() === 'error') {
		errors.push(text);
		console.log(`  console error: ${text.slice(0, 240)}`);
	}
});
page.on('pageerror', (error) => {
	errors.push(String(error));
	console.log(`  pageerror: ${String(error).slice(0, 240)}`);
});

await page.addInitScript({ path: iwerPath });
await page.addInitScript(`
	(() => {
		const device = new IWER.XRDevice(IWER.metaQuest3);
		device.installRuntime({ forceInstall: true });
		device.stereoEnabled = true;
		window.__iwer = device;
		// IWER only rejects a buffer that is too small and copies its input
		// with Array.from, so an exhausted iterator passes as zero joints.
		// Hold it to Chrome's rule, or this harness cannot catch that class.
		const proto = window.XRFrame.prototype;
		const fillPoses = proto.fillPoses;
		const fillJointRadii = proto.fillJointRadii;
		proto.fillPoses = function (spaces, base, transforms) {
			const list = Array.from(spaces);
			if (list.length * 16 !== transforms.length) { throw new TypeError('fillPoses: Buffer sizes must be equal'); }
			return fillPoses.call(this, list, base, transforms);
		};
		proto.fillJointRadii = function (spaces, radii) {
			const list = Array.from(spaces);
			if (list.length !== radii.length) { throw new TypeError('fillJointRadii: Buffer sizes must be equal'); }
			return fillJointRadii.call(this, list, radii);
		};
	})();
`);

const BREAKS = {
	iterator: [['Array.from(input_source.hand.values())', 'input_source.hand.values()']],
	churn: [
		['evt.removed.forEach(GodotWebXR.removeInputSource)', '0'],
		['evt.added.forEach(GodotWebXR.addInputSource)', 'evt.added.forEach(GodotWebXR.addInputSource),evt.removed.forEach(GodotWebXR.removeInputSource)'],
		['name<16&&GodotWebXR.input_sources[name]===input_source', 'name<16'],
	],
};
if (process.env.GLUE_BREAK) {
	const edits = BREAKS[process.env.GLUE_BREAK];
	await page.route('**/godot.js', async (route) => {
		const response = await route.fetch();
		let body = await response.text();
		for (const [needle, replacement] of edits) {
			if (!body.includes(needle)) {
				throw new Error(`negative control did not apply, the template is minified: ${needle}`);
			}
			body = body.replace(needle, replacement);
		}
		await route.fulfill({ response, body });
	});
	console.log(`negative control armed: ${process.env.GLUE_BREAK}`);
}

const saw = (pattern) => glue.some((line) => pattern.test(line));
const until = async (pattern, label) => {
	for (let i = 0; i < 150; i++) {
		if (saw(pattern)) {
			return true;
		}
		await page.waitForTimeout(100);
	}
	console.log(`  timed out waiting for ${label}`);
	return false;
};
const remote = (action, payload) => page.evaluate(([a, p]) => window.__iwer.remote.dispatch(a, p), [action, payload]);
const fingersUp = { x: 0.7071, y: 0, z: 0, w: 0.7071 };
const placeHand = (x) => remote('set_transform', { device: 'hand-right', position: { x, y: 1.35, z: -0.45 }, orientation: fingersUp });

const checks = [];
const check = (label, ok) => {
	checks.push(ok);
	console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}`);
};

await page.goto(url, { waitUntil: 'load', timeout: 120000 });
check('session started', await until(/GLUE session_started immersive-vr/, 'the session'));

if (saw(/session_started/)) {
	await remote('set_input_mode', { mode: 'hand' });
	await remote('set_hand_pose', { device: 'hand-right', poseId: 'open', duration: 0.1 });
	await placeHand(0.18);
	check('hand joints reach the tracker', await until(/GLUE wrist right 0\.[12]/, 'the right wrist'));

	await placeHand(0.42);
	check('hand joints follow the hand', await until(/GLUE wrist right 0\.[34]/, 'the wrist to move'));

	await remote('set_select_value', { device: 'hand-right', value: 1, duration: 0.1 });
	await remote('set_select_value', { device: 'hand-right', value: 0, duration: 0.1 });
	check('a pinch fires select', await until(/GLUE select \d/, 'select'));

	// The swap that froze hands before [wgpu 50]: the input source is replaced,
	// so the hand has to drop out and come back live.
	await remote('set_input_mode', { mode: 'controller' });
	check('a dropped hand stops reporting', await until(/GLUE hand_lost right/, 'the hand to drop'));
	const before = glue.length;
	await remote('set_input_mode', { mode: 'hand' });
	await placeHand(-0.3);
	let back = false;
	for (let i = 0; i < 100 && !back; i++) {
		back = glue.slice(before).some((line) => /GLUE wrist right -0\.[23]/.test(line));
		await page.waitForTimeout(100);
	}
	check('the hand returns live after the swap', back);

	await page.evaluate(() => window.__iwer.activeSession.end());
	check('the session ends', await until(/GLUE session_ended/, 'the session to end'));
	// Vacuous unless the session really resized the root viewport.
	check('the session resized the root viewport', saw(/GLUE session_viewport/));
	check('the root viewport returns to window size', saw(/viewport_restored=true/));
}

check('no page or console errors', errors.length === 0);

await browser.close();
server.close();

const failed = checks.filter((ok) => !ok).length;
console.log(failed ? `glue-smoke: ${failed} of ${checks.length} failed` : `glue-smoke: all ${checks.length} passed`);
process.exit(failed ? 1 : 0);
