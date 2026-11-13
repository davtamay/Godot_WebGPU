import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const file = process.argv[2] || new URL('../../platform/web/js/libs/audio.worklet.js', import.meta.url);
let Processor, allocations = 0;
class CountedFloat32Array extends Float32Array { constructor(...args) { super(...args); allocations++; } }
vm.runInNewContext(fs.readFileSync(file, 'utf8'), {
    AudioWorkletProcessor: class { constructor() { this.port = { postMessage() {} }; } },
    registerProcessor: (name, type) => { Processor = type; },
    Float32Array: CountedFloat32Array, Int32Array, Atomics,
});
let cases = 0;
for (const threads of [false, true]) for (const channels of [1, 2, 3, 6]) {
    for (const maxFrames of [3, 64, 128]) for (const extra of [0, 1, channels - 1]) {
        const size = channels * maxFrames * 2 + extra;
        const buffer = threads ? new Float32Array(new SharedArrayBuffer(size * 4)) : new Float32Array(size);
        const state = new Int32Array(new SharedArrayBuffer(16));
        let produced = 0, consumed = 0, views = 0;
        const sample = i => Math.fround(((i % 997) - 498) / 512);
        for (let i = 0; i < size; i++) buffer[i] = sample(++produced);
        const subarray = buffer.subarray;
        buffer.subarray = function (...args) { views++; return subarray.apply(this, args); };
        const processor = new Processor();
        if (threads) { state[3] = size; processor.parse_message('start', [state, new Float32Array(new SharedArrayBuffer(size * 4)), buffer]); }
        else processor.parse_message('start_nothreads', [buffer, size]);
        for (let tick = 0; tick < 90; tick++) {
            const frames = [1, maxFrames, Math.max(1, maxFrames - 1)][tick % 3];
            const output = Array.from({ length: channels }, () => new Float32Array(frames));
            allocations = 0; views = 0;
            assert.equal(processor.process([], [output], {}), true);
            for (let frame = 0; frame < frames; frame++) for (let ch = 0; ch < channels; ch++) assert.equal(output[ch][frame], sample(++consumed));
            assert.equal(views, 0, 'No intermediate output views');
            assert.equal(allocations, 0, 'No output scratch allocation when the quantum changes');
            const refill = Float32Array.from({ length: channels * frames }, () => sample(++produced));
            processor.output.write(refill);
            assert.equal(processor.output.data_left(), size);
        }
        processor.parse_message('stop', null);
        assert.equal(processor.process([], [], {}), false);
        cases++;
    }
}
console.log(`PASS: ${cases} shared/non-shared direct PCM cases, non-aligned wraps, variable quanta and zero output scratch/view allocations`);
