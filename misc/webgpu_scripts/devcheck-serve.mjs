// HTTPS server for the device check on other devices (phones, headsets):
// serves the export (gzip), receives the page's report at POST /report, keeps
// each one as a JSON file and logs a one-line summary. The gates export to
// $GODOT_WEBGPU_TESTBED/devcheck_gate_out; serve that folder.
//
// Usage: node devcheck-serve.mjs <export_dir> <port> <reports_dir>
// Env: DEVCHECK_CERT_DIR, a folder holding key.pem and cert.pem.
import https from 'node:https';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import zlib from 'node:zlib';

const dir = path.resolve(process.argv[2] ?? '.');
const port = parseInt(process.argv[3] ?? '8570', 10);
const reportsDir = path.resolve(process.argv[4] ?? 'reports');
const certDir = process.env.DEVCHECK_CERT_DIR;
if (!certDir) {
	console.error('Set DEVCHECK_CERT_DIR to a folder holding key.pem and cert.pem.');
	process.exit(2);
}
fs.mkdirSync(reportsDir, { recursive: true });

const types = {
	'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
	'.pck': 'application/octet-stream', '.png': 'image/png', '.json': 'application/json',
};
const gzipCache = new Map();

function summary(r) {
	const a = r.adapter || {};
	const failed = (r.cases || []).filter((c) => !c.pass && !c.skipped).map((c) => c.name)
		.concat((r.numeric || []).filter((n) => !n.pass).map((n) => n.name));
	const perf = (r.cases || []).filter((c) => c.perf).map((c) => `${c.name} ${c.frame_ms_mean}ms ${c.fps}fps`);
	return `${a.vendor || '?'} ${a.architecture || ''} | ${r.backend} | ${(r.userAgent || '').slice(0, 90)} | failed: ${failed.length ? failed.join(', ') : 'none'} | ${perf.join(' ')}`;
}

const server = https.createServer({
	key: fs.readFileSync(path.join(certDir, 'key.pem')),
	cert: fs.readFileSync(path.join(certDir, 'cert.pem')),
}, (req, res) => {
	const rel = decodeURIComponent(new URL(req.url, 'https://x').pathname);
	if (req.method === 'POST' && rel === '/report') {
		let body = '';
		req.on('data', (c) => { body += c; if (body.length > 4 << 20) req.destroy(); });
		req.on('end', () => {
			try {
				const report = JSON.parse(body);
				const id = new Date().toISOString().replace(/[:.]/g, '-');
				fs.writeFileSync(path.join(reportsDir, `${id}.json`), JSON.stringify(report, null, '\t'));
				console.log(`REPORT ${id} ${summary(report)}`);
				res.writeHead(200, { 'Content-Type': 'text/plain' });
				res.end(id);
			} catch (e) {
				res.writeHead(400);
				res.end();
			}
		});
		return;
	}
	const file = path.join(dir, rel === '/' ? 'index.html' : rel);
	if (!file.startsWith(dir) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
		res.writeHead(404);
		res.end();
		return;
	}
	const body = fs.readFileSync(file);
	const headers = { 'Content-Type': types[path.extname(file)] ?? 'application/octet-stream', 'Cache-Control': 'no-store', 'Vary': 'Accept-Encoding' };
	if (/\bgzip\b/.test(req.headers['accept-encoding'] ?? '') && ['.wasm', '.pck', '.js', '.html', '.json'].includes(path.extname(file))) {
		const stat = fs.statSync(file);
		const key = `${stat.mtimeMs}:${stat.size}`;
		const cached = gzipCache.get(file);
		let gz = cached && cached.key === key ? cached.gz : null;
		if (!gz) {
			gz = zlib.gzipSync(body, { level: 6 });
			gzipCache.set(file, { key, gz });
		}
		headers['Content-Encoding'] = 'gzip';
		res.writeHead(200, headers);
		res.end(gz);
		return;
	}
	res.writeHead(200, headers);
	res.end(body);
});

server.listen(port, '0.0.0.0', () => console.log(`device check: https://0.0.0.0:${port}/ (reports in ${reportsDir})`));
