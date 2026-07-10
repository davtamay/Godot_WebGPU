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
| 2.5 | Patch 06 | Probe additionally uploads a 64x64 pattern through the staging path (shadow map -> queue write -> buffer-to-texture -> texture-to-texture into the acquired frame); Dawn validates every copy (gpu-error hard gate), strict local pixel check asserts the pattern color |
| 2.7 | Patch 07 | Probe additionally creates a WGSL shader module containing an override (marker hard-gated, Dawn-validated); shader BAKING is verified locally only (tint is not on CI runners) - see the Shader baking section |
| 3 | Patch 08 | Probe drives a full draw through the real path (WGSL container -> shader_create_from_container -> render_pipeline_create -> push constants -> draw): triangle composited over the pattern frame. Hard gates: "triangle OK" marker + rc 0 + no gpu errors; strict local pixels = pattern rgb(230,102,26) at the corner AND triangle rgb(230,51,230) at the center |
| 3.5 | Patch 10 | Probe's triangle set 0 additionally carries a 4-element texture array the WGSL never references: Dawn validates the fanned-out bind group against the fanned-out layout ("uniform set OK" marker hard-gated) |
| 3.7 | Patch 11 | The triangle's color multiplies in a tint from the advanced slice of a dynamic uniform buffer ("dynamic buffer OK" marker hard-gated); a wrong dynamic offset selects the zeroed slice and fails the strict local pixel check |
| 3.8 (now) | Patch 33 | Loader smoke gains the `xr` scenario: with requiresWebXR + navigator.xr present (shimmed when absent), the loader must keep the WebGL driver exactly once (until the engine renders immersive sessions through WebGPU) and expose isWebXRWebGPUAvailable |
| 4 | Patch 11+ | Browser boots exported project; unlit-cube screenshot diff vs goldens (tolerance ~1-2%) |

Software-adapter Chromium flags (stage 2, recorded when it landed; they are
version-dependent, re-verify on Playwright bumps):
`--enable-unsafe-webgpu --use-webgpu-adapter=swiftshader --enable-features=Vulkan`.
The probe smoke SKIPs (exit 0, with a notice in the log) when the runner's
Chromium cannot deliver a WebGPU device - check the job log, a skip is not a
pass. CI runners have no GPU - these are smoke tests, never performance
tests.

## Shader baking (patch 07)

Baking WebGPU shaders needs an external Tint binary (interim until Tint is
vendored). The canonical source is the fork's release assets, built by the
"WebGPU stack - tint builder" workflow from the same Dawn tag the runtime
bindings pin and verified to produce byte-identical baked WGSL:

    gh release download tint-v20250531.224602 -p 'tint-*'  # pick your platform

Set GODOT_TINT_PATH to the downloaded executable; re-dispatch the workflow
with a new dawn_ref when the emdawnwebgpu pin moves. To build by hand
instead (the same recipe the workflow runs):

    git clone --depth 1 --branch v20250531.224602 https://github.com/google/dawn.git
    cmake -S dawn -B dawn/out -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDAWN_FETCH_DEPENDENCIES=ON -DTINT_BUILD_CMD_TOOLS=ON \
      -DTINT_BUILD_SPV_READER=ON -DTINT_BUILD_WGSL_WRITER=ON \
      -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DTINT_BUILD_TESTS=OFF
    cmake --build dawn/out --target tint_cmd_tint_cmd

Then set GODOT_TINT_PATH to the tint executable and export a web project
with `shader_baker/enabled` on. The project must set
`rendering/rendering_device/driver.web = "webgpu"` and the editor must run
an RD renderer (baking is inactive under --headless, which uses the dummy
renderer).

Translation coverage (baketest project, mobile renderer, 2026-07-08,
patch 09): ZERO Tint failures; 187 WGSL containers baked into the pck
(container count varies run to run with the requested variant set; the
pre-patch-09 baseline was ~96 with 115 per-stage failures). The patch 09
compatibility passes resolved the failures mechanically -- see the
manifest. Note the two deliberate semantic degradations, both logged in
the pass comments: PointSize writes are stripped (point primitives render
1px on web) and OpMemoryBarrier becomes a Workgroup control barrier
(stronger sync, but Device-scope coherence narrows to the workgroup).

Classes that translate only because the offending declaration was unused
in the attempted variants (rgb10a2 storage images, subpass inputs in
TonemapMobile, VRS builtins) remain untranslatable where actually used --
that is renderer-fallback territory (roadmap patch 11), surfaced at bake
time as a Tint error if a future variant set hits them.

True binding-array USE (lightmap-enabled mobile fragments, particles SDF
collision) translates only via dead-code paths today; the dedicated
flattening patch (roadmap 10) makes those variants real.

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
  (clear color rgb(51,153,229) verified exact on 2026-07-08; patch 06 moved
  the asserted color to the uploaded pattern rgb(230,102,26); patch 08 adds
  the drawn triangle rgb(230,51,230) at the canvas center, keeping the
  pattern check at the corner). Re-verify after driver changes to the
  presentation, upload, or draw paths.

## Size policy

Brotli-compressed size is the user-facing number. Flag-off builds must not
move at all vs upstream baseline. Flag-on budget: TBD with maintainers;
record deltas per patch in the manifest.
