# WebGPU - Testing

## Local loop

1. Build:   pwsh misc/webgpu_scripts/build-web.ps1        (or build-web.sh)
2. Export a test project to web from a matching editor build.
3. Serve:   python misc/webgpu_scripts/serve.py <export_dir>
   (COOP/COEP headers required or threaded exports fail silently.)
4. Smoke:   node misc/webgpu_scripts/smoke.mjs http://127.0.0.1:8060/index.html out.png
5. Sizes:   bash misc/webgpu_scripts/wasm-size.sh bin
6. Loader:  node misc/webgpu_scripts/loader-smoke.mjs bin
   (run from platform/web after `npm i --no-save playwright`)

Prereqs: emsdk (version pinned to match upstream CI - see
.github/workflows/web-baseline.yml), scons, Python 3, Node 18+ with
`npm i playwright && npx playwright install chromium`, brotli.

## CI stages (grow with the stack; never test ahead of it)

| Stage | Lands with | Gates |
|-------|------------|-------|
| 0 | Patch 00 | Stock web template builds; Linux editor builds; WASM sizes recorded |
| 0.5 | Patches 01-02 | rd leg (use_rendering_device=yes) also builds, incl. the drivers/webgpu scaffold from patch 02; sizes recorded per leg; stock leg size must not move vs the Patch 00 baseline |
| 1 | Patch 03 | Loader smoke in headless Chromium (loader-smoke.mjs): Engine.init() resolves, isWebGPUAvailable exposed, WebGL-fallback warning asserted when no WebGPU device is obtainable, no pageerror |
| 1.5 | Patch 04 | rd leg builds with webgpu=yes: emdawnwebgpu port downloads, compiles, and links; port size cost recorded |
| 2 | Patch 05 | WebGPU probe: _godot_webgpu_probe() runs the full driver path (device import, surface, swap chain, clear, submit) under a software adapter; hard gates = probe rc 0, success marker, no pageerror, no GPU validation error. Pixel readback is advisory in CI (see below) and strict locally on a real GPU; exported-project boot moves to stage 3 |
| 2.5 (now) | Patch 06 | Probe additionally uploads a 64x64 pattern through the staging path (shadow map -> queue write -> buffer-to-texture -> texture-to-texture into the acquired frame); Dawn validates every copy (gpu-error hard gate), strict local pixel check asserts the pattern color |
| 3 | Patch 08 | Browser boots exported project; triangle pixel test |
| 4 | Patch 09+ | Unlit-cube screenshot diff vs goldens (tolerance ~1-2%) |

Software-adapter Chromium flags (stage 2, recorded when it landed; they are
version-dependent, re-verify on Playwright bumps):
`--enable-unsafe-webgpu --use-webgpu-adapter=swiftshader --enable-features=Vulkan`.
The probe smoke SKIPs (exit 0, with a notice in the log) when the runner's
Chromium cannot deliver a WebGPU device - check the job log, a skip is not a
pass. CI runners have no GPU - these are smoke tests, never performance
tests.

Pixel readback findings (recorded 2026-07-08, the hard way):
- drawImage()/2D readback of a WebGPU canvas reads the CURRENT texture,
  which is expired (transparent) once the frame is presented; the presented
  frame is only observable via compositor screenshots.
- Headless software adapters (CI) execute all GPU work correctly but do not
  composite WebGPU canvases into screenshots, so the CI pixel check is
  ADVISORY (logged, never fails the job). The hard CI gates are: probe
  returns 0, success marker printed, no pageerror, no GPU validation error.
- Strict pixel verification runs on a real GPU:
  `PROBE_CHANNEL=chrome PROBE_REQUIRE_PIXELS=1 node misc/webgpu_scripts/loader-smoke.mjs bin probe`
  (clear color rgb(51,153,229) verified exact on 2026-07-08; since patch 06
  the asserted color is the uploaded pattern rgb(230,102,26)). Re-verify
  after driver changes to the presentation or upload paths.

## Size policy

Brotli-compressed size is the user-facing number. Flag-off builds must not
move at all vs upstream baseline. Flag-on budget: TBD with maintainers;
record deltas per patch in the manifest.
