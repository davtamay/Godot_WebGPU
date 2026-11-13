import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

// Exercise the real worklet with a deterministic PCM producer and delayed replies.
// No browser scheduler or audio hardware is needed for these ring/protocol checks.
const source = fs.readFileSync(new URL('../../platform/web/js/libs/audio.worklet.js', import.meta.url), 'utf8');
let Processor;
const messages = [];
vm.runInNewContext(source, {
    AudioWorkletProcessor: class { constructor() { this.port = { postMessage: m => messages.push(m) }; } },
    registerProcessor: (name, type) => { if (name === 'godot-processor') Processor = type; },
    Float32Array, Int32Array, Atomics,
});
assert(Processor);
let cases = 0;
for (const channels of [1, 2, 6]) {
    for (const frames of [64, 128, 256]) {
        for (const quanta of [1, 2, 4, 8, 16, 32]) {
            for (const delay of [0, Math.max(0, Math.floor(quanta / 2) - 1)]) {
                messages.length = 0;
                const processor = new Processor();
                const chunk = channels * frames;
                const size = chunk * quanta;
                let produced = 0, consumed = 0, reads = 0;
                const make = n => Float32Array.from({ length: n }, () => ++produced);
                processor.parse_message('start_nothreads', [make(size), size]);
                const replies = [];
                for (let tick = 0; tick < 128; tick++) {
                    while (replies.length && replies[0].tick <= tick) {
                        processor.parse_message('chunk', make(replies.shift().count));
                    }
                    const output = Array.from({ length: channels }, () => new Float32Array(frames));
                    assert.equal(processor.process([], [output], {}), true);
                    for (let frame = 0; frame < frames; frame++) {
                        for (let ch = 0; ch < channels; ch++) assert.equal(output[ch][frame], ++consumed);
                    }
                    for (const m of messages.splice(0)) {
                        assert.equal(m.cmd, 'read');
                        assert.equal(m.data % chunk, 0);
                        assert(m.data <= size);
                        replies.push({ tick: tick + delay + 1, count: m.data });
                        reads++;
                    }
                }
                if (quanta >= 16) assert.equal(reads, 32, 'Four quanta per message at large buffer sizes');
                if (quanta < 8) assert.equal(reads, 128, 'Small buffers request every quantum');
                processor.parse_message('stop', null);
                assert.equal(processor.process([], [], {}), false);
                assert.equal(processor.pending_read, 0);
                cases++;
            }
        }
    }
}
// No producer response: drain, stall, then recover without a lost read notification.
messages.length = 0;
const recovery = new Processor();
recovery.parse_message('start_nothreads', [new Float32Array(2048).fill(0.25), 2048]);
for (let i = 0; i < 10; i++) recovery.process([], [[new Float32Array(128), new Float32Array(128)]], {});
assert.equal(messages.reduce((sum, m) => sum + m.data, 0), 2048);
recovery.parse_message('chunk', new Float32Array(256).fill(0.5));
messages.length = 0;
const output = [new Float32Array(128), new Float32Array(128)];
recovery.process([], [output], {});
assert(output.every(ch => ch.every(v => v === 0.5)));
assert.equal(messages.length, 1);
assert.equal(messages[0].data, 256);
// Changing render quantum sizes must preserve sample order and exact accounting.
messages.length = 0;
const varying = new Processor();
let next = 0, expected = 0;
const make = n => Float32Array.from({ length: n }, () => ++next);
varying.parse_message('start_nothreads', [make(4096), 4096]);
for (let i = 0; i < 80; i++) {
    const frames = [64, 128, 256, 128][i % 4];
    const out = [new Float32Array(frames), new Float32Array(frames)];
    const input = [new Float32Array(frames).fill(0.125)];
    varying.process([input], [out], {});
    for (let f = 0; f < frames; f++) for (let ch = 0; ch < 2; ch++) assert.equal(out[ch][f], ++expected);
    const emitted = messages.splice(0);
    assert.equal(emitted.filter(m => m.cmd === 'input').length, 1, 'Input remains per-quantum');
    for (const m of emitted) if (m.cmd === 'read') varying.parse_message('chunk', make(m.data));
}
// Threaded path uses the shared counters and emits no read messages.
messages.length = 0;
const threaded = new Processor();
const state = new Int32Array(new SharedArrayBuffer(16));
const pcm = new Float32Array(new SharedArrayBuffer(4096 * 4));
pcm.fill(0.75); state[3] = pcm.length;
threaded.parse_message('start', [state, new Float32Array(new SharedArrayBuffer(4096 * 4)), pcm]);
for (let i = 0; i < 4; i++) {
    const out = [new Float32Array(128), new Float32Array(128)];
    threaded.process([], [out], {});
    assert(out.every(ch => ch.every(v => v === 0.75)));
}
assert.equal(state[3], 4096 - 4 * 256);
assert.equal(state[1], 1, "Threaded output refills batch four quanta");
threaded.process([[new Float32Array(128)]], [[new Float32Array(128), new Float32Array(128)]], {});
assert.equal(state[1], 2, "Input capture wakes the mixer immediately");
assert.equal(messages.length, 0);
// Shared-ring producer follows notifications, including delayed scheduling and wrap.
let threadedCases = 0;
for (const channels of [1, 2, 6]) for (const frames of [64, 128, 256]) {
    for (const quanta of [1, 2, 4, 8, 16, 32]) for (const delay of [0, Math.max(0, Math.floor(quanta / 2) - 1)]) {
        const processor = new Processor(), chunk = channels * frames, size = chunk * quanta;
        const sharedState = new Int32Array(new SharedArrayBuffer(16));
        const sharedPCM = new Float32Array(new SharedArrayBuffer(size * 4));
        let produced = 0, consumed = 0, writePos = 0, pending = null, lastStep = 0;
        const refill = () => {
            const count = size - Atomics.load(sharedState, 3);
            for (let i = 0; i < count; i++) { sharedPCM[writePos] = ++produced; writePos = (writePos + 1) % size; }
            Atomics.add(sharedState, 3, count);
        };
        refill();
        processor.parse_message('start', [sharedState, new Float32Array(new SharedArrayBuffer(size * 4)), sharedPCM]);
        for (let tick = 0; tick < 128; tick++) {
            if (pending !== null && pending <= tick) { refill(); pending = null; }
            const out = Array.from({ length: channels }, () => new Float32Array(frames));
            processor.process([], [out], {});
            for (let frame = 0; frame < frames; frame++) for (let ch = 0; ch < channels; ch++) assert.equal(out[ch][frame], ++consumed);
            const step = Atomics.load(sharedState, 1);
            if (step !== lastStep && pending === null) pending = tick + delay + 1;
            lastStep = step;
        }
        threadedCases++;
    }
}
assert.equal(threadedCases, 108);
console.log(`PASS: ${threadedCases} shared-ring PCM/delay configurations; ${cases} PCM/wrap/delay configurations, underrun recovery, variable quanta, input and threaded parity`);
