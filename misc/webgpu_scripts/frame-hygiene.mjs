// Frame hygiene check for the WebGPU patch stack.
//
// Captures one warm frame of an export with WebGPU Inspector injected before any
// page script, and grades it with the inspector's own frame-issue rules. It fails
// on the rules a warm WebGPU frame should never break: pipelines, shader modules,
// buffers, textures or bind groups created mid-frame, fragmented queue.writeBuffer
// calls, and many submits. The other rules are printed but do not fail: some
// describe the engine's own pass structure, and empty-pass reads a resolve-only
// pass and a timestamp-carrying pass as empty.
//
// Usage: node frame-hygiene.mjs <export_dir> [query]
// WGI_DIR names a copy of WebGPU Inspector holding webgpu_inspector.js (from
// extensions/chrome/) and src/devtools/frame_issues.js with its imports. Run from
// a directory whose node_modules provides `playwright`; it needs a real GPU.
// `?nowritebatch` as the query brings back fragmented writes, so the check can be
// seen to fail.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

const require = createRequire(path.join(process.cwd(), 'package.json'));
const { chromium } = require('playwright');

const exportDir = path.resolve(process.argv[2] ?? 'gate_out');
const query = process.argv[3] ?? '';
const wgiDir = process.env.WGI_DIR;
const settleMs = Number(process.env.SETTLE_MS ?? 30000);
if (!wgiDir) {
	console.log('WGI_DIR is not set');
	process.exit(2);
}
const { analyzeFrameIssues } = await import(pathToFileURL(path.join(wgiDir, 'src/devtools/frame_issues.js')).href);

const FAILING = new Set([
	'pipeline-created-in-frame', 'shader-created-in-frame', 'resource-created-in-frame',
	'bind-group-created-in-frame', 'fragmented-buffer-writes', 'many-submits',
]);
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };

const server = http.createServer((req, res) => {
	const name = new URL(req.url, 'http://x').pathname;
	res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
	res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
	const file = path.join(exportDir, name === '/' ? 'index.html' : path.basename(name));
	if (!fs.existsSync(file)) {
		res.statusCode = 404;
		res.end();
		return;
	}
	res.setHeader('Content-Type', TYPES[path.extname(file)] ?? 'application/octet-stream');
	fs.createReadStream(file).pipe(res);
});
await new Promise((resolve) => { server.listen(0, '127.0.0.1', resolve); });

const browser = await chromium.launch({ channel: 'chrome', headless: false });
const page = await browser.newPage();
await page.addInitScript({ path: path.join(wgiDir, 'webgpu_inspector.js') });
// Object descriptors are only recorded from this call on, so it runs before any page script.
await page.addInitScript(() => { self.webgpuInspector?.initialize(); });
await page.goto(`http://127.0.0.1:${server.address().port}/${query}`, { waitUntil: 'load' });
await page.waitForTimeout(settleMs);
const metadata = await page.evaluate(async () => {
	const inspector = self.webgpuInspector;
	if (!inspector) {
		return null;
	}
	// Two animation frames apart, so the window holds one frame of engine work
	// whichever order the callbacks run in.
	await new Promise((resolve) => requestAnimationFrame(() => {
		inspector.beginFrameCapture();
		requestAnimationFrame(() => {
			inspector.endFrameCapture();
			resolve();
		});
	}));
	const { metadata: captured } = await inspector.saveCaptureData('frame.wgpuc');
	return captured;
});
await browser.close();
server.close();
if (!metadata) {
	console.log('FAIL: the inspector did not load');
	process.exit(1);
}

const objects = metadata.objects ?? {};
const commands = metadata.commands ?? [];
const count = (method) => commands.filter((c) => c.method === method).length;
const graded = analyzeFrameIssues(commands, { getObject: (id) => objects[id] ?? null, getBundleCommands: (id) => objects[id]?.commands ?? [] });
const findings = Array.isArray(graded) ? graded : graded.findings ?? [];
const failures = findings.filter((f) => FAILING.has(f.rule));
for (const f of findings) {
	console.log(`${FAILING.has(f.rule) ? 'FAIL' : 'info'} ${f.rule} x${f.count}: ${String(f.message).replace(/\s+/g, ' ').slice(0, 200)}`);
}
const totals = `${commands.length} commands, ${count('writeBuffer')} writeBuffer, ${count('submit')} submits`;
if (failures.length > 0) {
	console.log(`FAIL: ${failures.map((f) => `${f.rule} x${f.count}`).join(', ')} (${totals})`);
	process.exit(1);
}
console.log(`clean: ${totals}`);
