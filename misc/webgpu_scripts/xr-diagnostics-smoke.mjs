// Dependency-free check of the optional XR diagnostic hook for weekly rebases.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(new URL('../../modules/webxr/native/library_godot_webxr.js', import.meta.url), 'utf8');
const events = [];
const context = vm.createContext({
    autoAddDeps() {}, mergeInto() {}, LibraryManager: { library: {} },
    window: { dispatchEvent(event) { events.push(event.detail); } },
    CustomEvent: class { constructor(type, options) { this.type = type; this.detail = options.detail; } },
});
vm.runInContext(source.slice(0, source.lastIndexOf('autoAddDeps')).replace('const GodotWebXR =', 'const Library =') + '\nglobalThis.GodotWebXR = Library.$GodotWebXR;', context);
const xr = context.GodotWebXR;
xr.session = { requestAnimationFrame(callback) { callback(10, { getViewerPose: () => ({ views: [{}, {}] }) }); } };
xr.space = {};
xr.gpu_binding = {};
xr.requestAnimationFrame(() => {});
assert.equal(events.length, 0, 'No target acquisition must not confirm a frame');
xr.requestAnimationFrame(() => { xr.frame_subimage = {}; });
assert.equal(events.length, 1);
assert.equal(events[0].binding, 'webgpu');
assert.equal(events[0].views, 2);
xr.requestAnimationFrame(() => { xr.frame_subimage = {}; });
assert.equal(events.length, 1, 'Emit once per session, not each frame');
xr.diagnostic_frame_reported = false;
xr.gpu_binding = null;
xr.disable_webxr_layers = true;
xr.requestAnimationFrame(() => {});
assert.equal(events[1].binding, 'webgl');
console.log('XR diagnostic hook regression checks passed');
