import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const source=fs.readFileSync(process.argv[2]||new URL('../../platform/web/js/libs/library_godot_audio.js',import.meta.url),'utf8');
const tick=async()=>{for(let i=0;i<8;i++)await Promise.resolve();};
let cases=0;
function setup(state='running') {
 const library={},timers=new Map(),errors=[];let timerId=0;
 const scope={LibraryManager:{library},autoAddDeps(){},mergeInto:(a,b)=>Object.assign(a,b),Promise,setTimeout:fn=>{timers.set(++timerId,fn);return timerId;},clearTimeout:id=>timers.delete(id),GodotRuntime:{error:(...a)=>errors.push(a)},navigator:{mediaDevices:{}}};
 vm.runInNewContext(source,scope);const idle=scope.GodotAudioIdle=library.$GodotAudioIdle;const audio=scope.GodotAudio=library.$GodotAudio;const calls=[];
 const ctx={state,createMediaStreamSource:stream=>({mediaStream:stream,connect(){},disconnect(){}}),close:()=>Promise.resolve(),suspend(){return operation('suspend');},resume(){return operation('resume');}};
 function operation(name){return new Promise((resolve,reject)=>calls.push({name,resolve:()=>{ctx.state=name==='suspend'?'suspended':'running';resolve();},reject}));}
 audio.ctx=ctx;audio.driver={get_node:()=>({}),close:()=>Promise.resolve()};audio.sample_driver={has_playbacks:()=>false,clear(){}};idle.start(ctx);
 return {idle,audio,ctx,calls,library,scope,timers,errors,fire(){const entries=[...timers.values()];timers.clear();entries.forEach(fn=>fn());}};
}
{
 const {idle,ctx,calls}=setup();idle.request(true);assert.equal(calls[0].name,'suspend');idle.request(false);calls[0].resolve();await tick();assert.equal(calls[1].name,'resume');calls[1].resolve();await tick();assert.equal(ctx.state,'running');assert.equal(idle.owned,false);cases++;
}
{
 const {idle,ctx,calls}=setup();idle.request(true);calls[0].resolve();await tick();idle.request(false);idle.request(true);calls[1].resolve();await tick();assert.equal(calls[2].name,'suspend');calls[2].resolve();await tick();idle.resume();assert.equal(calls.length,3);assert.equal(ctx.state,'suspended');cases++;
}
{
 const {idle,calls}=setup('suspended');idle.request(false);idle.request(true);assert.equal(calls.length,0);assert.equal(idle.owned,false);idle.resume();assert.equal(calls[0].name,'resume');calls[0].resolve();await tick();assert.equal(calls[1].name,'suspend');cases++;
}
{
 const {idle,ctx,calls}=setup();idle.request(true);calls[0].resolve();await tick();idle.request(false);assert.equal(calls[1].name,'resume');idle.resume();assert.equal(calls[2].name,'resume');calls[2].resolve();await tick();calls[1].resolve();await tick();assert.equal(ctx.state,'running');assert.equal(idle.pending,false);cases++;
}
{
 const {idle,ctx,calls}=setup();idle.request(true);calls[0].reject(Error('unsupported'));await tick();idle.request(true);assert.equal(calls.length,1);assert.equal(ctx.state,'running');assert.equal(idle.sleep_failed,true);cases++;
}
{
 const {idle,ctx,calls}=setup();idle.request(true);calls[0].resolve();await tick();idle.request(false);calls[1].reject(Error('activation'));await tick();idle.request(false);assert.equal(calls.length,2);idle.resume();calls[2].resolve();await tick();assert.equal(ctx.state,'running');assert.equal(idle.owned,false);cases++;
}
{
 const {idle,calls}=setup();idle.request(true);idle.request(false);idle.close();const fresh={state:'running'};idle.start(fresh);calls[0].resolve();await tick();assert.equal(calls.length,1);assert.equal(idle.ctx,fresh);assert.equal(idle.pending,false);cases++;
}
for(const kind of ['effects','samples','input']) {
 const s=setup();const {idle,audio,calls,library}=s;library.godot_audio_set_output_sleep(1);calls[0].resolve();await tick();
 if(kind==='effects')library.godot_audio_set_sleep_allowed(0);
 else {if(kind==='samples')audio.sample_driver.has_playbacks=()=>true;else audio.input_requested=true;audio.update_sleep();}
 assert.equal(calls[1].name,'resume');calls[1].resolve();await tick();
 if(kind==='effects')library.godot_audio_set_sleep_allowed(1);
 else {audio.sample_driver.has_playbacks=()=>false;audio.input_requested=false;audio.update_sleep();}
 assert.equal(calls.length,2);assert.equal(s.timers.size,1);s.fire();assert.equal(calls[2].name,'suspend');cases++;
}
{
 const s=setup();let grant,stopped=0;s.scope.navigator.mediaDevices.getUserMedia=()=>new Promise(r=>{grant=r;});s.library.godot_audio_set_output_sleep(1);s.calls[0].resolve();await tick();assert.equal(s.library.godot_audio_input_start(),0);assert.equal(s.audio.input_requested,true);assert.equal(s.calls[1].name,'resume');s.library.godot_audio_input_stop();grant({getTracks:()=>[{stop(){stopped++;}}]});await tick();assert.equal(stopped,1);assert.equal(s.audio.input,null);cases++;
}
{
 const s=setup();let deny;s.scope.navigator.mediaDevices.getUserMedia=()=>new Promise((_,r)=>{deny=r;});s.library.godot_audio_input_start();deny(Error('denied'));await tick();assert.equal(s.audio.input_requested,false);assert.equal(s.errors.length,1);cases++;
}
{
 const s=setup();let grant,stopped=0;s.scope.navigator.mediaDevices.getUserMedia=()=>new Promise(r=>{grant=r;});s.library.godot_audio_input_start();await new Promise((resolve,reject)=>s.audio.close_async(resolve,reject));grant({getTracks:()=>[{stop(){stopped++;}}]});await tick();assert.equal(stopped,1);assert.equal(s.idle.ctx,null);assert.equal(s.audio.ctx,null);cases++;
}
{
 const s=setup();s.ctx.suspend=()=>{throw Error('sync failure');};s.idle.request(true);assert.equal(s.idle.sleep_failed,true);assert.equal(s.idle.pending,false);cases++;
}
console.log(`PASS: ${cases} idle transition, ownership, gesture, inhibitor, permission and shutdown race cases.`);
