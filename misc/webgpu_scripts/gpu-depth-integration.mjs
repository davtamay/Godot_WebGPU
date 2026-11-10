// Compiled Godot + real WebGPU; only the XR sensor is synthetic. No headset claims.
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {execFileSync} from 'node:child_process';
const {chromium}=createRequire(path.join(process.cwd(),'package.json'))('playwright');
const root=path.resolve(process.argv[2]), reportPath=path.resolve(process.argv[3]);
const noIsolation=process.argv.includes('--no-isolation');
const bootstrap=fs.readFileSync(new URL('./depth_fixture/bootstrap.js',import.meta.url),'utf8');
const types={'.html':'text/html','.js':'application/javascript','.wasm':'application/wasm'};
let injections=0;
const server=http.createServer((req,res)=>{
 const pathname=new URL(req.url,'http://localhost').pathname;
 if(pathname==='/favicon.ico'){res.writeHead(204).end();return;}
 const file=path.resolve(root,'.'+(pathname==='/'?'/index.html':pathname));
 if(!file.startsWith(root+path.sep)||!fs.existsSync(file)){res.writeHead(404).end();return;}
 const headers={'Content-Type':types[path.extname(file)]||'application/octet-stream'};
 if(!noIsolation)Object.assign(headers,{'Cross-Origin-Opener-Policy':'same-origin','Cross-Origin-Embedder-Policy':'require-corp'});
 res.writeHead(200,headers);
 if(file.endsWith('index.js')){
  const source=fs.readFileSync(file,'utf8');
  // Closure flattens private XR state into variables. Locate it from the
  // preserved diagnostic guards; fail closed if the generated shape changes.
  const markers=[...source.matchAll(/Module(?:\["GodotWebGPUXR"\]|\.GodotWebGPUXR)\s*=\s*[\w$]+;/g)];
  assert.equal(markers.length,1,'Injection marker must be unique');
  let injected=bootstrap;
  if(!source.includes('GodotWebXR.session')){
   const state=[...source.matchAll(/if\(!([\w$]+)\)return [^;]+?\("no_session"\);if\(!([\w$]+)\)return [^;]+?\("webgl_session"\)/g)];
   const frame=[...source.matchAll(/if\(!([\w$]+)\|\|!([\w$]+)\)return [^;]+?\("no_frame"\)/g)];
   assert.equal(state.length,1,'Closure XR session/binding guards must be unique');
   assert.equal(frame.length,1,'Closure XR frame/pose guard must be unique');
   for(const [field,symbol] of Object.entries({session:state[0][1],gpu_binding:state[0][2],frame:frame[0][1],pose:frame[0][2]})){
    injected=injected.replaceAll('GodotWebXR.'+field,symbol);
   }
   assert.ok(!injected.includes('GodotWebXR.'),'All test-only state references must be resolved');
  }
  injections++;
  res.end(source.replace(markers[0][0],markers[0][0]+'\n'+injected));
 }else fs.createReadStream(file).pipe(res);
});
await new Promise(r=>server.listen(0,'127.0.0.1',r));
const browser=await chromium.launch({channel:'chrome'});
const messages=[],errors=[],samples=[];let result,hosting,timer;
try{
 const page=await browser.newPage({viewport:{width:256,height:256}});
 const done=new Promise((resolve,reject)=>{
  timer=setTimeout(()=>reject(Error('Depth integration timeout')),100000);
  page.on('pageerror',e=>errors.push(String(e)));
  page.on('console',async message=>{
   const text=message.text();messages.push(text);fs.appendFileSync(reportPath+".log",text+"\n");
   try{
    if(text.startsWith('DEPTH_SAMPLE_READY=')){
     const index=Number(text.split('=')[1]);
     const screenshot=reportPath.replace(/\.json$/,`-sample-${index}.png`);
     await page.screenshot({path:screenshot});
     const pixel=JSON.parse(execFileSync('python',['-c','from PIL import Image; import sys,json; print(json.dumps(Image.open(sys.argv[1]).convert("RGB").getpixel((128,128))))',screenshot],{encoding:'utf8'}));
     samples.push({index,pixel,passed:pixel[1]>230&&pixel[0]<20&&pixel[2]<20});
     await page.evaluate(i=>window.__depthSampleAck=i,index);
    }
    if(text.startsWith('DEPTH_INTEGRATION_RESULT=')){result=JSON.parse(text.slice('DEPTH_INTEGRATION_RESULT='.length));resolve();}
   }catch(e){reject(e);}
  });
 });
 await page.goto(`http://127.0.0.1:${server.address().port}/`);
 await done;
 hosting=await page.evaluate(()=>({isolated:crossOriginIsolated,shared:typeof SharedArrayBuffer!=='undefined'}));
 assert.equal(result.passed,true);assert.equal(result.cases.length,14);
 assert.equal(samples.length,8);assert.ok(samples.every(s=>s.passed),'Imported texture shader sampling must be green');
 assert.deepEqual(hosting,{isolated:!noIsolation,shared:!noIsolation});
 assert.equal(errors.length,0);assert.ok(!messages.some(m=>/Invalid CommandBuffer|GPUValidationError|ERROR:/.test(m)));
 console.log('PASS: 14 compiled depth cases, 8 real shader samples, '+(noIsolation?'single/no-SAB':'threaded'));
}finally{
 clearTimeout(timer);fs.writeFileSync(reportPath,JSON.stringify({result,hosting,samples,errors,messages,injections,synthetic_xr:true,real_webgpu:true},null,2));
 await browser.close();server.close();
}
