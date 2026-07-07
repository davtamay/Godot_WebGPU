# WebGPU - Testing

## Local loop

1. Build:   pwsh misc/webgpu_scripts/build-web.ps1        (or build-web.sh)
2. Export a test project to web from a matching editor build.
3. Serve:   python misc/webgpu_scripts/serve.py <export_dir>
   (COOP/COEP headers required or threaded exports fail silently.)
4. Smoke:   node misc/webgpu_scripts/smoke.mjs http://127.0.0.1:8060/index.html out.png
5. Sizes:   bash misc/webgpu_scripts/wasm-size.sh bin

Prereqs: emsdk (version pinned to match upstream CI - see
.github/workflows/web-baseline.yml), scons, Python 3, Node 18+ with
`npm i playwright && npx playwright install chromium`, brotli.

## CI stages (grow with the stack; never test ahead of it)

| Stage | Lands with | Gates |
|-------|------------|-------|
| 0 (now) | Patch 00 | Stock web template builds; Linux editor builds; WASM sizes recorded |
| 1 | Patch 03 | Browser boots exported project; WebGL fallback log asserted; no pageerror |
| 2 | Patch 04 | WebGPU clear-color pixel test (software adapter) |
| 3 | Patch 07 | Triangle pixel test |
| 4 | Patch 08+ | Unlit-cube screenshot diff vs goldens (tolerance ~1-2%) |

Software-adapter Chromium flags are version-dependent; pin Playwright and
record working flags here when Stage 2 lands. CI runners have no GPU -
these are smoke tests, never performance tests.

## Size policy

Brotli-compressed size is the user-facing number. Flag-off builds must not
move at all vs upstream baseline. Flag-on budget: TBD with maintainers;
record deltas per patch in the manifest.
