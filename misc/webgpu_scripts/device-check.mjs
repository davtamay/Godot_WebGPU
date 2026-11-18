// Runs the device check (devcheck_fixture) in Chrome on this machine and
// fails on any case that misses its reference colours, any failed numeric
// check, or any console error or warning the page reports.
//
// The page samples its own pixels through WebGPU (devcheck.js) and posts a
// report; this script serves the export, waits for that report and grades
// it. The same export can be opened on a phone or headset with
// devcheck-serve.mjs.
//
// Usage (from a directory whose package.json provides playwright):
//   node device-check.mjs <export_dir> [screenshot.png]
// Env: BOOT_ARGS (extra Chrome flags), THROTTLE (CPU throttle rate).
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { createRequire } from 'node:module';

const require = createRequire(path.join(process.cwd(), 'package.json'));
const { chromium } = require('playwright');

const dir = path.resolve(process.argv[2] ?? '.');
const shot = process.argv[3];
const types = {
	'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
	'.pck': 'application/octet-stream', '.png': 'image/png', '.json': 'application/json',
};

const server = http.createServer((req, res) => {
	const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
	if (req.method === 'POST' && rel === '/report') {
		// The page posts its report here; the copy read below is the one graded.
		req.resume();
		req.on('end', () => {
			res.writeHead(200, { 'Content-Type': 'text/plain' });
			res.end('local');
		});
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
const base = await new Promise((resolve) => {
	server.listen(0, '127.0.0.1', () => resolve(`http://127.0.0.1:${server.address().port}`));
});

const extra = (process.env.BOOT_ARGS ?? '').split(' ').filter(Boolean);
const browser = await chromium.launch({ channel: 'chrome', headless: false, args: ['--force-high-performance-gpu', ...extra] });
let failed = 0;
try {
	const context = await browser.newContext({ viewport: { width: 1280, height: 720 } });
	const page = await context.newPage();
	if (process.env.THROTTLE) {
		const cdp = await context.newCDPSession(page);
		await cdp.send('Emulation.setCPUThrottlingRate', { rate: parseFloat(process.env.THROTTLE) });
	}
	await page.goto(`${base}/index.html`, { waitUntil: 'load', timeout: 120000 });
	await page.waitForFunction(() => window.__devcheckReport, null, { timeout: 300000, polling: 500 });
	const r = await page.evaluate(() => window.__devcheckReport);
	if (shot) {
		await page.screenshot({ path: shot });
	}

	const a = r.adapter || {};
	console.log(`adapter: ${a.vendor} ${a.architecture} ${a.description} | backend ${r.backend} ${r.canvasFormat} | ${r.seconds}s`);
	let checked = 0;
	const perf = [];
	for (const c of r.cases) {
		const mark = c.skipped ? 'SKIP' : c.perf ? 'PERF' : c.pass ? 'pass' : 'FAIL';
		let detail = c.detail ?? '';
		if (c.perf) {
			detail = `${c.frame_ms_mean} ms/frame (p90 ${c.frame_ms_p90}), ${c.fps} fps`;
			perf.push(`${c.name} ${c.frame_ms_mean} ms/frame`);
		} else if (c.samples && !c.pass) {
			detail = c.samples.map((s) => `got ${s.got} want ${s.expected} (diff ${s.diff})`).join('; ');
		}
		if (!c.skipped && !c.perf) {
			checked++;
			if (!c.pass) {
				failed++;
			}
		}
		console.log(`${mark.padEnd(4)} ${c.name.padEnd(30)} ${detail}`);
	}
	for (const n of r.numeric) {
		checked++;
		if (!n.pass) {
			failed++;
		}
		console.log(`${(n.pass ? 'pass' : 'FAIL').padEnd(4)} ${n.name.padEnd(30)} ${n.detail}`);
	}
	for (const e of r.errors) {
		console.log(`console: ${e.slice(0, 300)}`);
	}
	const summary = `${checked - failed}/${checked} checks passed, ${r.errors.length} console errors/warnings${perf.length ? ', ' + perf.join(', ') : ''}`;
	if (r.errors.length > 0) {
		failed++;
	}
	console.log(failed ? `device-check: FAILED (${summary})` : `device-check: OK (${summary})`);
} catch (e) {
	failed++;
	console.log(`device-check: FAILED (${String(e).slice(0, 300)})`);
} finally {
	await browser.close();
	server.close();
}
process.exitCode = failed ? 1 : 0;
