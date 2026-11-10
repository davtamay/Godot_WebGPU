// Exercises the shipped JS bridge with strict browser-like layer lifetimes.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(process.env.XR_LAYER_SOURCE || new URL('../../modules/webxr/native/library_godot_webxr_ext.js', import.meta.url), 'utf8');
const heap = new Map();
const errors = [];
const pending = [];
const xr = { space: {}, layer: { projection: true }, layer_generation: 3, frame: {} };
const context = vm.createContext({
    GodotWebXR: xr, Module: {},
    GodotRuntime: {
        error: message => errors.push(message),
        getHeapValue: address => heap.get(address),
        setHeapValue: (address, value) => heap.set(address, value),
    },
    XRRigidTransform: class { constructor(position, orientation) { Object.assign(this, { position, orientation }); } },
});
vm.runInContext(source.slice(0, source.lastIndexOf('autoAddDeps')) + '\nglobalThis.ext = GodotWebXRExt; globalThis.GodotWebXRLayers = GodotWebXRExt.$GodotWebXRLayers;', context);
const layers = context.GodotWebXRLayers;
let destroyed = 0;
let rejectState = false;
xr.session = {
    enabledFeatures: ['layers'], renderState: { layers: [] },
    updateRenderState(state) {
        if (rejectState) throw Error('runtime layer limit');
        assert.ok(state.layers.every(layer => !layer.destroyed));
        pending.push(state.layers);
    },
};
const makeLayer = () => ({ destroy() { this.destroyed = true; destroyed++; } });
xr.gpu_binding = { createQuadLayer: makeLayer, createCylinderLayer: makeLayer, createEquirectLayer: makeLayer };
xr.gpu_color_format = 'rgba8unorm';
const params = order => [0, 0, -2, 0, 0, 0, 1, 1, 1, 0, 0, 1, 0.5, order, 256, 128];
const ids = [layers.create(0, params(-1)), layers.create(1, params(0)), layers.create(2, params(2))];
assert.ok(ids.every(Boolean));
assert.deepEqual(Array.from(layers.compose(xr.layer)), [layers.layers[ids[0]].layer, xr.layer, layers.layers[ids[1]].layer, layers.layers[ids[2]].layer]);
const p = params(-1);
p.forEach((value, index) => heap.set(index * 4, value));
let acquired = 0;
xr.getSubImage = () => ({});
xr.getTextureHandle = () => 42;
xr.gpu_binding.getSubImage = () => {
    acquired++;
    return { colorTexture: { width: 256, height: 128 }, viewport: { x: 0, y: 0, width: 256, height: 128 } };
};
assert.equal(context.ext.godot_webxr_layer_frame(ids[0], 0, 100), 0);
assert.equal(acquired, 0, 'Do not acquire before render-state activation');
xr.session.renderState.layers = pending.at(-1);
assert.equal(context.ext.godot_webxr_layer_frame(ids[0], 0, 100), 1);
assert.equal(heap.get(108), 42);
assert.equal(heap.get(112), 256);
layers.clear();
assert.equal(destroyed, 3);
assert.equal(Object.keys(layers.layers).length, 0);

rejectState = true;
assert.equal(layers.create(0, params(0)), 0, 'Render-state rejection must fall back');
assert.equal(Object.keys(layers.layers).length, 0, 'Rejected layer must not leak');
assert.equal(destroyed, 4);
assert.equal(errors.length, 1);
layers.clear();
rejectState = false;
delete xr.gpu_binding.createQuadLayer;
assert.equal(layers.create(0, params(0)), 0);
assert.equal(layers.create(0, params(0)), 0);
assert.equal(errors.length, 2, 'Warn once per kind per session');
layers.clear();
assert.equal(layers.create(0, params(0)), 0);
assert.equal(errors.length, 3, 'A new session must get its own warning');
console.log('PASS: quad/cylinder/equirect ordering, activation, texture metadata, cleanup, setup rejection and session retry');
