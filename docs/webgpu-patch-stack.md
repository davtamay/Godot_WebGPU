# WebGPU Patch Stack - Manifest and Rules

## Current stack

| # | Subject | Purpose | Shared lines | Upstream PR |
|---|---------|---------|--------------|-------------|
| 00 | misc: Add WebGPU patch-stack tooling, CI baseline, and docs | Workflow bootstrap; zero engine changes | 0 | n/a (fork-only) |
| 01 | [shared] web: Add opt-in use_rendering_device SCons option | Defines RD_ENABLED on web (default off, requires threads=yes); RD stack becomes linkable, no driver | ~11 (platform/web/detect.py) | not yet |
| 02 | [shared] drivers: Add stubbed WebGPU driver scaffold | RenderingContextDriverWebGPU + RenderingDeviceDriverWebGPU + shader-container-format stubs under drivers/webgpu/, compiled on web when use_rendering_device=yes; unreachable at runtime | 2 (drivers/SCsub) | not yet |
| 03 | [shared] web: Add async WebGPU device pre-init to the loader | experimentalWebGPU config (default off): loader requests adapter/device before start, stashes Module.preinitializedWebGPUDevice (consumed by our own glue in later patches via Module.WebGPU.importJsDevice), warns + falls back to WebGL otherwise; engine still always boots GL until 05 | ~28 engine.js, ~11 config.js, ~12 features.js | not yet |
| 04 | [shared] web: Add webgpu option registering the WebGPU driver | webgpu=yes links the emdawnwebgpu port (Dawn-pinned remote port, needs network on cold builds), defines WEBGPU_ENABLED, implies use_rendering_device; "webgpu" advertised by DisplayServerWeb, boot probes the context and falls back to WebGL 2 (context still reports unavailable until 05) | ~12 detect.py, ~20 display_server_web.cpp | not yet |
| 05 | [shared] webgpu: Implement swap chain presentation with a probe clear | Real context (device import, canvas surfaces) + driver command/swap-chain/present paths; godot_webgpu_probe() presents a cleared frame on a hidden canvas (canvases are locked to their first context type); engine still renders WebGL until textures/shaders land. drivers/webgpu now requires webgpu=yes (not just use_rendering_device) | ~4 SCsub, ~12 display_server_web.cpp | not yet |
| 06 | webgpu: Implement the resource layer with staging uploads | Buffers (upload maps emulated via malloc shadow + wgpuQueueWriteBuffer; downloads deferred), textures + format table (uncompressed/depth/BC; ETC2/ASTC deferred), samplers (border colors degrade to clamp), copy commands, real device limits, benign timestamp/pipeline-cache stubs; probe now uploads a pattern through staging and copies it over the presented frame | 0 | not yet |

(Planned next: 07 shader translation; 08 command recording;
09 renderer fallbacks. Old roadmap 04 was split into 04+05 for smaller,
independently green patches. NOTE: upstream already compiles
RenderingDevice + renderer_rd unconditionally on all platforms including
web; the per-platform RD_ENABLED define is the only gate, which is why
patch 01 is a detect.py-only change.)

## Rules

- **One patch, one purpose.** Subject describes the whole diff with no "and".
- **Placement:** new WebGPU code only under `drivers/webgpu/`; new web glue
  as NEW files under `platform/web/`. New files are rebase-cheap; edits to
  existing files are a weekly tax.
- **Shared files allowed** (<= ~20 lines per patch per file, only when the
  patch's purpose requires it,
  commit tagged `[shared]` in the subject): `platform/web/detect.py`,
  `platform/web/SCsub`, `drivers/SCsub`, `main/main.cpp`,
  `servers/rendering_server.cpp`, `platform/web/js/engine/*`,
  `platform/web/display_server_web.cpp/.h` (added in patch 04: driver
  registration has no other home).
- **Never touched:** `servers/rendering/rendering_device.cpp`,
  `rendering_device_graph.*`, `shader_compiler*`, `modules/glslang`,
  existing drivers, renderer_rd scene/effects code outside the designated
  fallback patch.
- **Size:** target <= 500 changed lines; ~1500 ceiling for pure new-file
  scaffolds only.
- **Clean-stack admission:** no placeholder shaders, no debug prints, no
  commented-out code, nothing that changes flag-off behavior, nothing you
  couldn't defend line-by-line in upstream review. Everything else lives
  in `webgpu-dev`.
- Enforced mechanically by `misc/webgpu_scripts/check-stack.sh`.
