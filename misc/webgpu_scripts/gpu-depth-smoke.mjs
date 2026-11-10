// Actual WebGPU normalization with synthetic XR depth descriptors. Not a headset test.
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
const require = createRequire(path.join(process.cwd(), 'package.json'));
const { chromium } = require('playwright');
const source = fs.readFileSync(new URL('../../modules/webxr/native/library_godot_webxr_ext.js', import.meta.url), 'utf8');
const server = http.createServer((req, res) => { res.setHeader('Content-Type', 'text/html'); res.end('<html><body>GPU depth checks</body></html>'); });
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const browser = await chromium.launch({ channel: 'chrome' });
const errors = [];
try {
 const page = await browser.newPage();
 page.on('pageerror', e => errors.push(String(e)));
 await page.goto(`http://127.0.0.1:${server.address().port}`);
 await page.addScriptTag({ content: `var LibraryManager={library:{}}; function autoAddDeps(){}; function mergeInto(a,b){Object.assign(a,b);}\n${source}\nwindow.GodotWebXRDepth=LibraryManager.library.$GodotWebXRDepth;` });
 const report = await page.evaluate(async () => {
  const adapter = await navigator.gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const validation = []; device.addEventListener('uncapturederror', e => validation.push(String(e.error.message || e.error)));
  const handles = new Map(); let nextHandle = 1;
  window.Module = { preinitializedWebGPUDevice: device, GodotWebGPUXR: { importOwnedTexture: t => { const h=nextHandle++;handles.set(h,t);return h; }, releaseOwnedTexture: h => handles.delete(h) } };
  const identity = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
  const views = [{ eye:'left', projectionMatrix:identity, transform:{matrix:identity} }, { eye:'right', projectionMatrix:identity, transform:{matrix:identity} }];
  window.GodotWebXR = {session:{depthUsage:'gpu-optimized',depthActive:true},gpu_binding:{},frame:{},pose:{views}};
  const results = [];
  const check = (name, condition) => { if (!condition) throw Error(name + " | " + JSON.stringify(validation));results.push(name); };
  let sources=[];
  function make(format='r32float', values=[1,2,3,4,5,6,7,8], layers=1, width=4, height=2) {
   const texture=device.createTexture({size:[width,height,layers],format,usage:GPUTextureUsage.TEXTURE_BINDING|GPUTextureUsage.COPY_DST});sources.push(texture);
   const data=format==='r16uint'?new Uint16Array(values):new Float32Array(values);
   device.queue.writeTexture({texture},data,{bytesPerRow:width*(format==='r16uint'?2:4),rowsPerImage:height},[width,height,layers]);
   return texture;
  }
  function depth(texture, layer=0, matrix=identity, scale=1) { return {texture,width:texture.width,height:texture.height,normDepthBufferFromNormView:{matrix},rawValueToMeters:scale,getViewDescriptor:()=>({dimension:'2d',baseMipLevel:0,mipLevelCount:1,baseArrayLayer:layer,arrayLayerCount:1})}; }
  function frame(items) {
   GodotWebXR.frame={};GodotWebXR.gpu_binding.getDepthInformation=view=>items[views.indexOf(view)]||null;
   return GodotWebXRDepth.prepare();
  }
  async function read(layer=0) {
   const texture=GodotWebXRDepth.state.readback, pitch=Math.ceil(texture.width*4/256)*256;
   const buffer=device.createBuffer({size:pitch*texture.height,usage:GPUBufferUsage.MAP_READ|GPUBufferUsage.COPY_DST});
   const enc=device.createCommandEncoder();enc.copyTextureToBuffer({texture,origin:[0,0,layer]},{buffer,bytesPerRow:pitch,rowsPerImage:texture.height},[texture.width,texture.height,1]);device.queue.submit([enc.finish()]);
   await buffer.mapAsync(GPUMapMode.READ);const bytes=buffer.getMappedRange();const result=[];
   for(let y=0;y<texture.height;y++)result.push(...new Float32Array(bytes,y*pitch,texture.width));buffer.unmap();buffer.destroy();return result;
  }
  const close = (a,b) => a.length===b.length && a.every((v,i)=>Math.abs(v-b[i])<0.00001);
  const floats=make();let info=frame([depth(floats),depth(floats,0,identity,2)]);
  check('both eyes valid',info.valid_mask===3&&info.view_count===2);
  check('float depth and meters',close(await read(0),[1,2,3,4,5,6,7,8]));
  check('per-eye scale',close(await read(1),[2,4,6,8,10,12,14,16]));
  check('one normalization per XR frame',GodotWebXRDepth.prepare().sequence===info.sequence);
  const ints=make('r16uint',[0,1,2,65535,500,1000,1500,2000,100,200,300,400,500,600,700,800],2);
  info=frame([depth(ints,1,identity,0.001),depth(ints,0,identity,0.001)]);
  check('descriptor selects array layer',close(await read(0),[.1,.2,.3,.4,.5,.6,.7,.8]));
  check('uint16 is integer not UNORM',close(await read(1),[0,.001,.002,65.535,.5,1,1.5,2]));
  const flipped=[-1,0,0,0,0,1,0,0,0,0,1,0,1,0,0,1];
  frame([depth(floats,0,flipped),null]);
  check('UV matrix is applied',close(await read(0),[4,3,2,1,8,7,6,5]));
  check('missing eye is zero not stale/left',close(await read(1),Array(8).fill(0)));
  const shifted=identity.slice();shifted[12]=.5;
  frame([depth(floats,0,shifted),null]);check('outside sensor coverage is unknown',close(await read(0),[3,4,0,0,7,8,0,0]));
  const bad=depth(ints);bad.getViewDescriptor=()=>({dimension:'2d',baseArrayLayer:7,arrayLayerCount:1});
  info=frame([bad,null]);check('invalid descriptor rejected',info.valid_mask===0&&info.views[0].reason==='invalid_view_descriptor');check('missing frame clears both layers',close(await read(0),Array(8).fill(0)));
  const legacy=depth(floats);delete legacy.getViewDescriptor;info=frame([legacy,null]);check('unambiguous legacy 2D accepted',info.valid_mask===1);
  const ambiguous=depth(ints);delete ambiguous.getViewDescriptor;info=frame([ambiguous,null]);check('array layer is never guessed',info.valid_mask===0);
  frame([depth(floats),depth(floats)]);const before=GodotWebXRDepth.state.sampleHandle;
  const small=make('r32float',[9,10],1,2,1);info=frame([depth(small),depth(small)]);
  check('resize replaces owned textures',info.width===2&&info.height===1&&info.texture_handle!==before&&handles.size===2);
  for(let i=0;i<40;i++) {
   const texture=make('r32float',[i+1,i+2],1,2,1);frame([depth(texture),depth(texture)]);texture.destroy();
   check('frame expiry '+i,close(await read(0),[i+1,i+2]));
  }
  check('bounded imports over frame churn',handles.size===2);
  const malformed=depth(small);malformed.rawValueToMeters=NaN;info=frame([malformed,null]);check('invalid scale rejected',info.valid_mask===0);
  const nonfinite=make('r32float',[NaN,Infinity,-2,0,1,2,3,4]);frame([depth(nonfinite),null]);check('invalid samples become unknown',close(await read(0),[0,0,0,0,1,2,3,4]));
  GodotWebXR.session.depthActive=false;check('pause exposes no depth',GodotWebXRDepth.prepare().status==='depth_paused');GodotWebXR.session.depthActive=true;
  GodotWebXR.frame=null;check('outside frame rejected',GodotWebXRDepth.prepare().status==='no_frame');
  GodotWebXR.session.depthUsage='cpu-optimized';check('CPU fallback explicit',GodotWebXRDepth.prepare().status==='cpu_depth');GodotWebXR.session.depthUsage='gpu-optimized';
  GodotWebXR.gpu_binding={};check('missing API explicit',GodotWebXRDepth.prepare().status==='unsupported_by_browser');
  GodotWebXR.session={depthUsage:'gpu-optimized',depthActive:true};info=frame([depth(small),depth(small)]);check('restart resets generation and imports',info.generation>1&&handles.size===2);
  await device.queue.onSubmittedWorkDone();await new Promise(r=>setTimeout(r,20));check('no GPU validation errors',validation.length===0);
  device.destroy();await device.lost;await new Promise(r=>setTimeout(r,0));check('device loss releases owned handles',GodotWebXRDepth.prepare().status==='device_lost'&&handles.size===0);
  GodotWebXR.session=null;check('session teardown',GodotWebXRDepth.prepare().status==='no_session'&&GodotWebXRDepth.state===null);
  for(const texture of sources) texture.destroy();
  return {passed:true,checks:results,validation,synthetic_xr:true,real_webgpu:true};
 });
 assert.equal(errors.length,0);
 fs.writeFileSync(process.argv[2],JSON.stringify({...report,page_errors:errors},null,2));
 console.log(`PASS: ${report.checks.length} GPU depth checks (synthetic XR, real WebGPU).`);
} finally {await browser.close();server.close();}


