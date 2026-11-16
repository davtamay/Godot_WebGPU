import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
// Usage: node test-worker-groups.mjs EXPORTED_MATRIX RESULTS ENGINE_REVISION
const [build, output, revision] = process.argv.slice(2);
assert.ok(build && output && revision, 'Build, output and expected engine revision required');
const root = path.resolve(build), out = path.resolve(output);
fs.mkdirSync(out, {recursive:true});
const require = createRequire(path.join(process.env.GODOT_WEBGPU_TESTBED || process.cwd(), 'package.json'));
const {chromium} = require('playwright');
const server = http.createServer((req,res) => {
 const name = new URL(req.url, 'http://local').pathname;
 if (name === '/favicon.ico') { res.writeHead(204).end(); return; }
 const file = path.resolve(root, '.'+name);
 if (!file.startsWith(root+path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {res.writeHead(404).end();return;}
 const headers = {'Content-Type':{'.html':'text/html','.js':'application/javascript','.wasm':'application/wasm'}[path.extname(file)] || 'application/octet-stream','Cache-Control':'no-store'};
 if (name.startsWith('/WebThreads/')) Object.assign(headers, {'Cross-Origin-Opener-Policy':'same-origin','Cross-Origin-Embedder-Policy':'require-corp'});
 res.writeHead(200,headers); fs.createReadStream(file).pipe(res);
});
await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
const rows=[];
try {
 for (const mode of ['Web','WebThreads']) {
  let browser;const row={mode,errors:[]};
  try {
   browser=await chromium.launch({channel:process.env.PLAYWRIGHT_CHANNEL || 'chrome'});
   const page=await browser.newPage();
   page.on('pageerror',e=>row.errors.push(String(e)));
   page.on('console',m=>{if(m.type()==='error')row.errors.push(m.text());});
   await page.goto(`http://127.0.0.1:${server.address().port}/${mode}/index.html`);
   await page.waitForFunction(()=>window.workerGroupResult,null,{timeout:60000});
   row.result=await page.evaluate(()=>workerGroupResult);
   assert.equal(row.result.passed,true);assert.equal(row.result.groups,384);assert.equal(row.result.version.hash,revision);
   row.hosting=await page.evaluate(()=>({shared:typeof SharedArrayBuffer!=='undefined',isolated:crossOriginIsolated}));
   assert.deepEqual(row.hosting,{shared:mode==='WebThreads',isolated:mode==='WebThreads'});
   await page.evaluate(()=>engine.requestQuit());
   await page.waitForFunction(()=>window.audioFixtureExit!==undefined,null,{timeout:5000});
   row.exit=await page.evaluate(()=>audioFixtureExit);assert.equal(row.exit,0);
   await page.waitForTimeout(100);assert.deepEqual(row.errors,[]);row.passed=true;
  } catch(e) {row.passed=false;row.failure=String(e);process.exitCode=1;}
  finally {await browser?.close();rows.push(row);fs.writeFileSync(path.join(out,'results.json'),JSON.stringify(rows,null,2));console.log(mode,row.passed?'PASS':'FAIL',row.failure||'384 groups reclaimed; clean exit');}
 }
} finally {await new Promise(resolve=>server.close(resolve));}
