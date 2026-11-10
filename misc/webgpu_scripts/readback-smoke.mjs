// Real RenderingDevice callbacks after a GPU copy and clear, not CPU mocks.
// Run from a directory providing Playwright. Default expects correct data.
// --expect-current-gap records a known mismatch; it never accepts no callback,
// missing RD support, page errors, or invalid GPU command buffers as evidence.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(path.join(process.cwd(), 'package.json'));
const { chromium } = require('playwright');
const root = path.resolve(process.argv[2]);
const reportPath = path.resolve(process.argv[3]);
const noIsolation = process.argv.includes('--no-isolation');
const failMapping = process.argv.includes('--expect-map-failure');
const types = { '.html': 'text/html', '.js': 'application/javascript', '.wasm': 'application/wasm' };
const server = http.createServer((request, response) => {
    const pathname = new URL(request.url, 'http://localhost').pathname;
    if (pathname === '/favicon.ico') { response.writeHead(204).end(); return; }
    const file = path.resolve(root, '.' + (pathname === '/' ? '/index.html' : pathname));
    if (!file.startsWith(root + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
        response.writeHead(404).end(); return;
    }
    const headers = { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' };
    if (!noIsolation) Object.assign(headers, { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' });
    response.writeHead(200, headers);
    fs.createReadStream(file).pipe(response);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const browser = await chromium.launch({ channel: 'chrome' });
const messages = [], errors = [];
let result, timer;
try {
    const page = await browser.newPage();
    // Delay actual mapping to exercise staging reuse and source destruction.
    // Failure mode rejects only this driver's owned download buffers.
    await page.addInitScript(({ failMapping }) => {
        const original = GPUBuffer.prototype.mapAsync;
        GPUBuffer.prototype.mapAsync = function (...args) {
            if (this.label !== 'Godot async readback') return original.apply(this, args);
            return new Promise(resolve => setTimeout(resolve, 150)).then(() => {
                if (failMapping) throw new Error('Injected readback mapping failure');
                return original.apply(this, args);
            });
        };
    }, { failMapping });
    const completed = new Promise((resolve, reject) => {
        timer = setTimeout(() => reject(Error('No readback report within 60 seconds')), 60000);
        page.on('console', message => {
            const text = message.text(); messages.push(text);
            if (text.startsWith('READBACK_RESULT=')) {
                result = JSON.parse(text.slice('READBACK_RESULT='.length)); resolve(result);
            }
        });
    });
    page.on('pageerror', error => errors.push(String(error)));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await completed;
    const hosting = await page.evaluate(() => ({ isolated: crossOriginIsolated, shared: typeof SharedArrayBuffer !== 'undefined' }));
    fs.writeFileSync(reportPath, JSON.stringify({ result, hosting, failMapping, mappingDelayMs: 150, errors, messages }, null, 2));
    assert.equal(errors.length, 0);
    assert.ok(messages.some(message => message.includes('WebGPU 1.0')));
    if (!noIsolation) assert.deepEqual(hosting, { isolated: true, shared: true });
    assert.ok(!messages.some(message => /Invalid CommandBuffer|GPUValidationError/.test(message)));
    if (noIsolation) assert.deepEqual(hosting, { isolated: false, shared: false });
    if (process.argv.includes('--expect-current-gap')) {
        assert.ok(Object.values(result.results).every(test => test.callback_received === true));
        assert.ok(Object.values(result.results).some(test => test.state === 'mismatch'));
        console.log('CONFIRMED GAP: callbacks arrived but GPU-written data was incorrect.');
    } else if (failMapping) {
        assert.equal(Object.keys(result.results).length, 30);
        assert.ok(Object.values(result.results).every(test => test.callback_received && test.state === 'empty'));
        assert.ok(Object.values(result.callback_counts).every(count => count === 1));
        console.log('PASS: mapping failures deliver exactly one empty callback per request.');
    } else {
        assert.equal(Object.keys(result.results).length, 30);
        assert.ok(result.frames_seen > 3);
        assert.equal(result.passed, true, JSON.stringify(result.results));
        console.log('PASS: asynchronous buffer and texture readback match GPU writes.');
    }
} finally {
    clearTimeout(timer);
    if (!result) fs.writeFileSync(reportPath, JSON.stringify({ result: null, errors, messages }, null, 2));
    await browser.close();
    server.close();
}
