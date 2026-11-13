import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { Worker } from 'node:worker_threads';

// Execute the actual JS glue in real workers; verify notify-before-wait and
// shutdown of a parked mixer, including when no audio quanta arrive.
const source = fs.readFileSync(new URL('../../platform/web/js/libs/library_godot_audio.js', import.meta.url), 'utf8');
const begin = source.indexOf('const GodotAudioWorklet = {');
const end = source.indexOf('\nautoAddDeps(GodotAudioWorklet', begin);
assert(begin > 0 && end > begin);
const glue = source.slice(begin, end) + ';globalThis.api=GodotAudioWorklet;';
const delay = ms => new Promise(r => setTimeout(r, ms));
async function until(test) {
    const start = Date.now();
    while (!test()) { if (Date.now() - start > 3000) throw Error('Worker wake timed out'); await delay(1); }
}
function apiFor(state) { const scope = { HEAP32: state, Atomics }; vm.runInNewContext(glue, scope); return scope.api; }
const workerSource = `
const {workerData,parentPort}=require('node:worker_threads');const vm=require('node:vm');
const state=new Int32Array(workerData.buffer),scope={HEAP32:state,Atomics};
vm.runInNewContext(workerData.glue,scope);let step=Atomics.load(state,1),wakes=0;
parentPort.postMessage({ready:true});
if(workerData.gated)Atomics.wait(state,5,0);
while(!Atomics.load(state,4)) {
    Atomics.store(state,6,1);
    step=scope.api.godot_audio_worklet_state_wait(0,1,step,-1);
    wakes++;parentPort.postMessage({awake:true,step});
}
parentPort.postMessage({done:true,wakes,step});parentPort.close();
`;
async function check(mode, jitter = 0) {
    const state = new Int32Array(new SharedArrayBuffer(32)), api = apiFor(state), received = [];
    if (mode === 'pre-cancel') Atomics.store(state, 4, 1);
    const worker = new Worker(workerSource, { eval: true, workerData: { buffer: state.buffer, glue, gated: mode === 'before-wait' } });
    let failure;
    worker.on('message', m => received.push(m)); worker.on('error', e => { failure = e; });
    try {
        await until(() => received.some(m => m.ready) || failure); if (failure) throw failure;
        if (mode === 'before-wait') {
            api.godot_audio_worklet_state_notify(0, 1);
            Atomics.store(state, 5, 1); Atomics.notify(state, 5);
            await until(() => received.some(m => m.awake));
            assert.equal(received.find(m => m.awake).step, 1, 'Changed state prevents a lost wake before wait');
        } else if (mode !== 'pre-cancel') {
            await until(() => Atomics.load(state, 6) === 1);
            if (mode === 'parked') {
                await delay(30);
                assert(!received.some(m => m.awake), 'Indefinite wait must not poll');
                api.godot_audio_worklet_state_notify(0, 1);
                await until(() => received.some(m => m.awake));
            }
        }
        if (jitter) await delay(jitter);
        Atomics.store(state, 4, 1);
        api.godot_audio_worklet_state_notify(0, 1);
        await until(() => received.some(m => m.done) || failure); if (failure) throw failure;
        if (mode === 'pre-cancel') assert.equal(received.find(m => m.done).wakes, 0);
    } finally { await worker.terminate(); }
}
// Existing finite-timeout callers retain their behavior.
const finiteState = new Int32Array(new SharedArrayBuffer(16));
assert.equal(apiFor(finiteState).godot_audio_worklet_state_wait(0, 1, 0, 1), 0);
await check('pre-cancel'); await check('before-wait'); await check('parked');
for (let i = 0; i < 40; i++) await check('race', i % 3);
console.log('PASS: finite timeout, pre-cancel, notify-before-wait, parked resume/shutdown, and 40 exit races');
