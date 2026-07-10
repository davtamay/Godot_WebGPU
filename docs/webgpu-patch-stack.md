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
| 07 | [shared] webgpu: Bake shaders to WGSL through Tint at export time | Real shader container (SPIR-V pre-transform: 1.3 downgrade + entry-point interface strip + push-constants -> read-only SSBO at group 0 binding 510 w/ NonWritable members; external tint via GODOT_TINT_PATH, zstd WGSL); driver shader_create_from_container (modules + bind group layouts w/ combined-sampler remap + pipeline layout); baker platform plugin registered unconditionally (first cross-platform baker; metal/d3d12 are host-gated); web export shader_baker/enabled option; coverage 90/205 mobile-renderer variants, gaps documented in webgpu-testing.md | ~8 editor_node.cpp, ~5 export_plugin.cpp, ~4 baker SCsub, ~4 drivers/SCsub | not yet |
| 08 | webgpu: Implement the pipeline layer with a probe triangle draw | Render/compute pipelines (full state mapping), vertex formats, uniform sets -> bind groups (combined-sampler pair remap), push constants via a dynamic-offset ring buffer (bind groups defer to draw time: the offset arrives after uniform sets bind), general render passes/framebuffers, lazy compute passes, barrier no-ops (api_trait_get: WebGPU tracks hazards); probe draws a push-constant-colored triangle through shader_create_from_container + render_pipeline_create | 0 | not yet |
| 09 | webgpu: Add SPIR-V compatibility passes to the shader transform | Dead-binding elimination (glslang keeps declared-but-unused resources in every stage; Tint rejects texture/sampler array declarations even when unreferenced -- 80 of the 115 bake failures were the mobile VERTEX stage tripping on the unused lightmap array), completing the 1.3 downgrade (SignExtend/ZeroExtend image-operand strip, scalar-condition OpSelect splat), infinite-constant clamp, isInf/isNan rewrite as integer bit tests, PointSize store strip (points render 1px on web), OpMemoryBarrier -> control-barrier conversion; bake coverage ~96 -> 187 containers with zero Tint failures | 0 | not yet |
| 10 | webgpu: Fan arrayed uniforms out to one binding per element | The mobile renderer builds uniform sets with all 16 lightmap ids whether or not lightmaps exist, and WGSL has no binding arrays: bind group layouts + bind groups expand arrayed texture/sampler/image uniforms to consecutive bindings in a reserved range (ARRAY_BINDING_BASE 512 + binding*16 + element; Godot's dense binding numbering makes the sized-binding-arrays proposal's binding+i convention collide with neighbors). Modules that dead-eliminated the declaration still work: WebGPU allows layouts to declare bindings the shader does not use. Probe set 0 gains an unreferenced 4-texture array the harness hard-gates via a marker; the SPIR-V switch-codegen for genuinely dynamic indexing stays deferred until lightmap variants are exercised | 0 | not yet |
| 11 | webgpu: Support dynamic buffers with per-frame slices | The mobile renderer's render-pass uniform set uses UNIFORM_BUFFER_DYNAMIC + STORAGE_BUFFER_DYNAMIC unconditionally, so boot needs them: BUFFER_USAGE_DYNAMIC_PERSISTENT buffers allocate one alignment-padded slice per frame in flight (persistently mapped via the CPU shadow, flushed with wgpuQueueWriteBuffer), layouts mark the entries hasDynamicOffset, and binds decode the Vulkan-convention packed frame indices into offsets ordered before the push-constant ring's (binding order). Probe: the triangle's color is now multiplied by a tint read from the ADVANCED slice of a dynamic uniform buffer - selecting the wrong slice blacks the triangle out and fails the strict pixel check | 0 | not yet |

| 12-30 | (rows pending backfill) | Browser bring-up: engine boot through the driver, full-variant baking, SPIR-V preprocessing, WGSL sanitization, 3D feature set, subpass-free mobile path, baked-cache hole tolerance, VRS/particles/octmap fixes | see git log | not yet |
| 31 | [shared] webgpu: Skip baked shader groups the target cannot use | Bake-side veto hook (ShaderBakerExportPluginPlatform::skips_variant): the WebGPU plugin skips multiview variants (WGSL cannot express ViewIndex; stereo will be one pass per view) and FP16 variants (driver reports SUPPORTS_HALF_FLOAT=false); they shipped as dead bytes -- the runtime never requests either. Skipped variants become tolerated cache holes (patch 26); bakes also get faster (no glslang/Tint for skipped variants) | ~10 shader_baker_export_plugin.cpp/.h | not yet |

| 32 | [shared] webgpu: Report unbaked shader versions with an actionable error | The web runtime cannot compile shaders, so a version missing from the baked cache produced a 3-errors-per-variant cascade; _compile_version_start now reports one actionable error naming the shader and the usual cause (materials constructed in scripts are invisible to the export-time baker; save them as .tres) and leaves the version invalid, which version_get_shader already handles | ~11 shader_rd.cpp | not yet |

| 33 | [shared] web: Choose the boot driver by the project's WebXR needs | New web-export option webxr/uses_webxr (default off) emits requiresWebXR into the exported config; the loader then keeps the WebGL driver on browsers that support WebXR but cannot render immersive sessions through WebGPU (a canvas locks to its first context type, so the choice is boot-time only). Detection is capability-based: Engine.isWebXRWebGPUAvailable() (XRGPUBinding) plus an engine-support constant that stays false until the WebXR-WebGPU rendering path lands. One export renders WebGPU on flat browsers and enters XR through the intact WebGL path elsewhere | ~10 export_plugin.cpp, ~13 engine.js, ~12 config.js, ~14 features.js | not yet |

| 34 | [shared] webxr: Bind WebXR sessions to the WebGPU renderer | When the WebGPU driver booted (signal: the pre-initialized device on Module), the WebXR glue creates an XRGPUBinding instead of the XRWebGLBinding, appends the mandatory 'webgpu' session feature (without it the browser mints a WebGL-based session the binding refuses), and creates the projection layer in the binding's DEFAULT shape (explicit textureType produced broken sub-images on Quest Browser's experimental impl). Layer textures cross to C++ as WGPUTexture handles (emdawnwebgpu importJsTexture, cached in a JS map) and the interface wraps them with RD texture_create_from_extension for the renderer-agnostic render-target override -- the OpenXR/Vulkan idiom. The multiview session requirement is skipped on this path (WGSL has none; GLES3::Config does not even exist here). Two presentation fixes ride along: the canvas returns null from getCurrentTexture() while an immersive session owns the compositor (guard makes it throw into the port's error path; the driver skips the frame silently). Adds modules/webxr to the shared allowlist. QUEST-3-VERIFIED: session starts, layer sub-image = 1680x1760 2-layer rgba8unorm array wrapped cleanly; rendering then stops at the renderer's multiview requirement = exactly the next patch's scope. Loader keeps steering XR exports to WebGL until stereo lands (ENGINE_WEBXR_WEBGPU_SUPPORTED stays false) | ~55 library_godot_webxr.js, ~60 webxr_interface_js.cpp, ~1 godot_webxr.h, ~30 webxr.externs.js, ~8 engine.js | not yet |

| 35 | [shared] xr: Draw XR viewports one pass per view without multiview | WGSL has no multiview, so stereo on the WebGPU backend renders the viewport once per view: XRInterface gains get_draw_pass_count()/set_current_draw_pass() (defaults keep every other interface and platform on the existing single-pass path) and the viewport's XR block loops them; the WebXR interface reports ONE view to the renderer, remaps the active pass onto view 0's transforms/projection (the mono camera path needs no changes), and hands each pass its view's layer slice via texture_create_shared_from_slice. Flips ENGINE_WEBXR_WEBGPU_SUPPORTED: XR-flagged exports now genuinely boot WebGPU on XRGPUBinding browsers | ~8 xr_interface.h, ~12 renderer_viewport.cpp, ~4 engine.js, rest in modules/webxr | not yet |

(Planned next: perf/pipeline warm-up passes for XR; Quest Browser ships
experimental WebXR-WebGPU since April 2026. NOTE: upstream
already compiles RenderingDevice + renderer_rd unconditionally on all
platforms including web; the per-platform RD_ENABLED define is the only
gate, which is why patch 01 is a detect.py-only change.)

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
  registration has no other home), `editor/editor_node.cpp`,
  `editor/shader/shader_baker/SCsub`,
  `platform/web/export/export_plugin.cpp/.h` (added in patch 07: shader
  baker registration and the web export option), and
  `editor/export/shader_baker_export_plugin.cpp/.h` (cpp added in patch 15;
  h added in patch 31 for the variant-veto hook),
  `modules/webxr/*` (added in patch 34: the WebXR implementation itself),
  `servers/xr/xr_interface.h` and
  `servers/rendering/renderer_viewport.cpp` (added in patch 35: the
  per-view draw-pass hook and its single call site).
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
