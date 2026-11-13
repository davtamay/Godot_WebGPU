import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const engine = process.argv[2] || fileURLToPath(new URL('../../', import.meta.url));
const source = fs.readFileSync(path.join(engine, 'platform/web/js/libs/library_godot_audio.js'), 'utf8');
const begin = source.indexOf('const GodotAudioWorklet = {');
const end = source.indexOf('\nautoAddDeps(GodotAudioWorklet', begin);
assert(begin > 0 && end > begin);
const glue = source.slice(begin, end).replace('const GodotAudioWorklet =', 'const library =') + ';globalThis.driver=library.$GodotAudioWorklet;';
let chunks = 0;
for (const size of [1024, 4096, 8192]) {
    const heap = new Float32Array(size), messages = [];
    const port = { postMessage: message => messages.push(structuredClone(message)) };
    const scope = {
        HEAPF32: heap, Float32Array, Promise,
        GodotAudio: { ctx: { destination: {} } },
        GodotRuntime: { heapSub: (h, offset, n) => h.subarray(offset / 4, offset / 4 + n), heapSlice: (h, offset, n) => h.slice(offset / 4, offset / 4 + n) },
    };
    vm.runInNewContext(glue, scope);
    const driver = scope.driver;
    scope.GodotAudioWorklet = driver;
    driver.promise = Promise.resolve();
    driver.worklet = { port, connect() {} };
    let produced = 0, consumed = 0;
    driver.start_no_threads(0, size, (offset, count) => {
        for (let i = 0; i < count; i++) heap[(offset + i) % size] = ++produced;
    }, 0, size, () => {});
    await driver.promise;
    assert.equal(messages.shift().cmd, 'start_nothreads');
    driver.ring_buffer.consumed(0, port);
    assert.equal(messages.length, 0, 'No empty chunk messages');
    let previous;
    for (let i = 0; i < 160; i++) {
        // Fixed and changing quanta, full-ring writes and repeated wrapping.
        const count = [128, 128, 256, 512, 64, size, 192][i % 7];
        driver.ring_buffer.consumed(count, port);
        assert.equal(messages.length, 1);
        const message = messages.shift();
        assert.equal(message.cmd, 'chunk');
        assert.equal(message.data.length, count);
        assert.equal(message.data.byteOffset, 0);
        assert.equal(message.data.buffer.byteLength, count * 4, 'Clone only the payload, not the ring backing storage');
        for (const sample of message.data) assert.equal(sample, ++consumed);
        if (previous) assert.equal(previous.data[0], previous.first, 'Later producer writes cannot alter an already sent chunk');
        previous = { data: message.data, first: message.data[0] };
        chunks++;
    }
}

// Verify the real worklet wakes only the process counter. The C++ mixer reads
// sample counts atomically but waits exclusively on STATE_PROCESS (index 1).
let Processor, notifications = 0;
const monitoredAtomics = Object.create(Atomics);
monitoredAtomics.notify = (array, index, ...args) => {
    assert.equal(array.byteOffset + index * 4, 4, 'No notifications on sample-count counters');
    notifications++;
    return Atomics.notify(array, index, ...args);
};
vm.runInNewContext(fs.readFileSync(path.join(engine, 'platform/web/js/libs/audio.worklet.js'), 'utf8'), {
    AudioWorkletProcessor: class { constructor() { this.port = { postMessage() { throw Error('Unexpected threaded message'); } }; } },
    registerProcessor: (name, type) => { Processor = type; },
    Float32Array, Int32Array, Atomics: monitoredAtomics,
});
const state = new Int32Array(new SharedArrayBuffer(16));
const pcm = new Float32Array(new SharedArrayBuffer(4096 * 4));
pcm.fill(0.25); state[3] = pcm.length;
const processor = new Processor();
processor.parse_message('start', [state, new Float32Array(new SharedArrayBuffer(4096 * 4)), pcm]);
for (let i = 0; i < 4; i++) processor.process([], [[new Float32Array(128), new Float32Array(128)]], {});
assert(notifications > 0);
assert.equal(notifications, state[1], 'Output notifications match the process counter');
const outputNotifications = notifications;
assert.equal(state[3], 4096 - 1024);
processor.process([[new Float32Array(128).fill(0.5)]], [[new Float32Array(128), new Float32Array(128)]], {});
assert.equal(notifications, outputNotifications + 1, 'Capture still wakes immediately');
assert.equal(state[2], 128);
console.log(`PASS: ${chunks} structured-clone PCM chunks, variable payloads/wrap, exact backing sizes and process-only notifications`);
