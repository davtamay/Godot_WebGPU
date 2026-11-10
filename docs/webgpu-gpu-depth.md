# WebXR GPU depth integration (isolated follow-up)

Branch: codex/webxr-gpu-depth, based on async-readback 4019073651d8f00f2411df959573f1c6d3f42bc4.
Keep this series separate from codex/monday-readiness (667765342213009c068e48d6ba6413e657bc3340).
Do not promote it or install its templates as part of Monday sync without a separate review.

## Contract and ownership

The engine requests matchDepthView and prefers GPU-optimized depth only when
XRGPUBinding.getDepthInformation exists. WebGPU requests float32 / unsigned-short;
WebGL retains its prior formats and CPU-first fallback. API availability is not
proof of a granted session or delivered depth.

Inside an active XR frame, the extension library samples each browser-owned depth
texture using getViewDescriptor(), normDepthBufferFromNormView and rawValueToMeters.
The formats are r32float and r16uint (integer, not UNORM). Array layers are explicit;
a legacy texture without a descriptor is accepted only if it has one layer.
The compute work is submitted before leaving the XR frame. Browser-owned textures
are neither retained in metadata nor destroyed by the engine.

Outputs are engine-owned per-view 2D arrays in normalized view UV coordinates:
- RGBA16float, meters in red, filterable for soft occlusion without CPU readback.
- R32float, meters in red, for full-precision asynchronous mesh readback.

Godot Texture2DArrayRD requires at least two layers, so mono gets an additional
zero-filled layer. view_count remains the actual count; texture_layers describes
the allocation. Every layer is overwritten each frame. Missing eyes, invalid
samples and UV coordinates outside sensor coverage become zero (unknown), never
another eye's data. Half precision sampling saturates at 65504 meters; readback
retains f32 values. The source may expire immediately after normalization submits.

WebXRInterface.get_system_info() returns webxr_depth metadata: status, sequence,
generation, width, height, view_count, texture_layers, valid_mask, units, uv_space,
and per-view validity, projection, pose, source format/layer and normalization.
On success it also returns webxr_depth_texture_array_rd and
webxr_depth_readback_array_rd; native JS handles are removed from script metadata.
The legacy webxr_depth_texture_rd remains a 2D first-view slice, already in meters.
Non-success reports expose no texture RIDs. Resize/session changes replace imports;
pause, missing data, teardown and device loss invalidate consumer access.
Diagnostics report status/format/mask changes, never sensor samples.

Call get_system_info during the XR-driven frame on the rendering thread. The demo
uses driver/threads/thread_model.web=1 (rendering on the main thread) for BOTH
threaded and single-threaded WASM. The threaded build still provides worker support.
A separate rendering-thread configuration must schedule access on that thread and
preserve XR callback lifetime; it is not covered by these desktop tests.

## Addon consumer

The suite's webxr_gpu_depth.gd binds the RGBA16 array directly to soft occlusion.
It supplies eye positions, UV orientation and world scale. Hard occlusion / live
scan requests are throttled, asynchronous R32 snapshots. Captured view projection,
pose and reference-space transforms travel with the readback; late callbacks from
a cleared provider are ignored. CPU/WebGL harvesting remains available.

## Reproducible desktop checks

Run from a directory with Playwright installed (Chrome channel, real WebGPU):

    node /engine/misc/webgpu_scripts/gpu-depth-smoke.mjs /reports/depth-unit.json
    node /engine/misc/webgpu_scripts/gpu-depth-integration.mjs /exports/Web /reports/single.json --no-isolation
    node /engine/misc/webgpu_scripts/gpu-depth-integration.mjs /exports/WebThreads /reports/threaded.json

Export depth_fixture with both templates from this exact source revision, using
one fresh shared bake. The integration harness injects a synthetic sensor into
served JS only; production artifacts contain no sensor hooks. It runs real compiled
Godot, imported texture wrappers, GPU normalization, asynchronous readback after
source destruction, and material sampling checked through screenshot pixels.
The standalone test also exercises invalid descriptors, matrix/scale errors,
40 expired-source frames, handle bounds and actual GPUDevice.destroy() loss.
The addon test is addons/godot_webxr_scene_understanding/tests/test_gpu_depth.gd.
Also run the 30-case async readback fixture, existing Mobile/Forward+ rendering,
MSAA and IWER glue gates, thread pack parity, and check-stack.sh.

These tests use synthetic XR sensor frames with real GPU execution. They do NOT
prove headset depth delivery, XRGPUBinding implementation correctness, compositor
occlusion alignment, passthrough quality, physical device loss or performance.
Quest 3 / Galaxy XR tests are deferred: record browser/OS/build identities, actual
depth grant/format, both-eye masks, moving-object occlusion, scale/recenter, pause /
resume, session restart and scan alignment. Exercise both hosting/thread variants.
Full Closure-enabled release CI remains a separate validation gate.

## Upstream boundary

Normalization lives in library_godot_webxr_ext.js. Shared changes are restricted to
WebXR session negotiation, externs and the existing get_system_info bridge. The
WebGPU driver preserves explicit imported view dimensions; other drivers and
shared renderer code are unchanged by this depth series. Keep [shared] commits
separate when replaying on Godot Foundation updates; no future conflict-free merge
is guaranteed. Run the boundary, build and runtime gates again after replay.

Reference: https://immersive-web.github.io/depth-sensing/ (25 August 2026 editor draft,
reviewed 10 November 2026): XRGPUDepthInformation, view descriptors, formats and
frame-limited texture lifetime. This is implemented against an evolving draft.
