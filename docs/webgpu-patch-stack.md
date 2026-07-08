# WebGPU Patch Stack - Manifest and Rules

## Current stack

| # | Subject | Purpose | Shared lines | Upstream PR |
|---|---------|---------|--------------|-------------|
| 00 | misc: Add WebGPU patch-stack tooling, CI baseline, and docs | Workflow bootstrap; zero engine changes | 0 | n/a (fork-only) |
| 01 | [shared] web: Add opt-in use_rendering_device SCons option | Defines RD_ENABLED on web (default off, requires threads=yes); RD stack becomes linkable, no driver | ~11 (platform/web/detect.py) | not yet |
| 02 | [shared] drivers: Add stubbed WebGPU driver scaffold | RenderingContextDriverWebGPU + RenderingDeviceDriverWebGPU + shader-container-format stubs under drivers/webgpu/, compiled on web when use_rendering_device=yes; unreachable at runtime | 2 (drivers/SCsub) | not yet |

(Planned next: 03 web pre-init + fallback; 04
presentation/clear; 05 resources; 06 shader translation; 07 command
recording; 08 renderer fallbacks. NOTE: upstream already compiles
RenderingDevice + renderer_rd unconditionally on all platforms including
web; the per-platform RD_ENABLED define is the only gate, which is why
patch 01 is a detect.py-only change.)

## Rules

- **One patch, one purpose.** Subject describes the whole diff with no "and".
- **Placement:** new WebGPU code only under `drivers/webgpu/`; new web glue
  as NEW files under `platform/web/`. New files are rebase-cheap; edits to
  existing files are a weekly tax.
- **Shared files allowed** (each in its designated patch, diff <= ~20 lines,
  commit tagged `[shared]` in the subject): `platform/web/detect.py`,
  `platform/web/SCsub`, `drivers/SCsub`, `main/main.cpp`,
  `servers/rendering_server.cpp`, `platform/web/js/engine/*`.
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
