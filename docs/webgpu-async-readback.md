# Asynchronous GPU readback (isolated follow-up)

Base: Monday candidate 667765342213009c068e48d6ba6413e657bc3340.
Branch: codex/async-readback. Do not merge into or rewrite codex/monday-readiness.

## Shared boundary

The default driver keeps the existing fence wait / synchronous mapping path.
An opt-in driver hook receives byte regions of already-submitted download staging
buffers. RenderingDevice supplies packed buffer ranges or texture rows, retaining
its existing mip/layer/volume layout calculations. WebGPU snapshots these regions
into an owned MapRead | CopyDst buffer before returning, then maps asynchronously.
No source resource, driver, frame or RenderingDevice pointer is retained by the
completion. The callback is deferred through Godot's message queue, exactly once;
mapping failures return an empty PackedByteArray and emit an error. Browser main
thread blocking and SharedArrayBuffer are not required.

The snapshot copy is ordered after submitted downloads and before staging reuse.
Mapping is delayed independently of frame reuse. Copy alignment is four bytes;
mapping size is padded to eight bytes; CPU assembly removes row padding and
alignment prefixes. Snapshot requests exceeding the GPU buffer limit fail
explicitly with an empty callback rather than submitting an invalid buffer.
Synchronous get_data remains limited by WebGPU's lack of a blocking map; this
change does not claim to fix synchronous GPU readback.

The shared commit deliberately adds only rendering_device.cpp to check-stack's
[shared]-only paths; the driver header was already allowed. This is a documented
policy exception for the two async-download dispatch sites, not permission for
unrelated RenderingDevice changes. Keep this adapter separate from WebGPU code
for Monday replay review. Other backends do not opt in.

## Regression fixture

misc/webgpu_scripts/readback_fixture exercises 24 buffer snapshots, including
subranges and repeated writes to the same destination, then six texture layouts:
odd RGBA rows, odd R8 rows, tiled extent, mip chain, 3D volume and an array layer.
All source RIDs are freed immediately after recording. Native Vulkan is the
reference. readback-smoke.mjs verifies byte hashes, exactly-once callbacks, actual
WebGPU boot, page errors, GPU validation and hosting isolation. It delays mapping
150 ms to exercise source destruction and frame/staging reuse. Its separate
--expect-map-failure mode injects rejection only for labelled readback buffers
and requires one empty callback per request; it is not a device-loss hardware test.

Run the harness from a Playwright-equipped directory:

    node /path/to/readback-smoke.mjs /exports/Web /reports/single.json --no-isolation
    node /path/to/readback-smoke.mjs /exports/WebThreads /reports/threaded.json

Repeat with --expect-map-failure for the error lifecycle. The single-threaded
run must have neither isolation headers nor SharedArrayBuffer; the threaded
run requires both. Use a short fixture path on Windows for shader cache paths.

Before release, also run normal rendering/XR desktop gates and hardware tests.
This feature does not complete WebXR GPU depth or broader shader coverage.
