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
| 1 (now) | Patch 03 | Loader smoke in headless Chromium (loader-smoke.mjs): Engine.init() resolves, isWebGPUAvailable exposed, WebGL-fallback warning asserted when no WebGPU device is obtainable, no pageerror |
| 2 | Patch 04 | Browser boots exported project; WebGPU clear-color pixel test (software adapter) |
| 3 | Patch 07 | Triangle pixel test |
| 4 | Patch 08+ | Unlit-cube screenshot diff vs goldens (tolerance ~1-2%) |

Software-adapter Chromium flags are version-dependent; pin Playwright and
record working flags here when Stage 2 lands. CI runners have no GPU -
these are smoke tests, never performance tests.

## Size policy

Brotli-compressed size is the user-facing number. Flag-off builds must not
move at all vs upstream baseline. Flag-on budget: TBD with maintainers;
record deltas per patch in the manifest.
