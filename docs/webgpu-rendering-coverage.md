# WebGPU rendering coverage candidate

This stack is separate from `codex/monday-readiness`, based on completed GPU depth commit `84de1b745c87ebdf0e2a0b7bc2cd2339d995263f`. Do not promote it without the final build/runtime reports. Hardware headset tests are deferred.

## Implementation

- The WebGPU baker has a backend-specific source preparation hook; other backends retain their shader sources. AMD vendor files are unchanged.
- FSR1 and FSR2 use AMD's FP32 algorithms because shader-f16 does not supply the integer-16 operations their half paths require. General FP16 shader support is unchanged.
- FSR1 writes an RGBA16Float intermediate and converts to the final attachment through a raster copy, satisfying WebGPU's exact storage binding formats.
- FSR2 reconstructed depth and SPD counters use real storage-buffer atomics. Exposure uses a two-float storage buffer. Signed 16-bit lookup data is converted to filterable half-float values at upload.
- FSR2 luminance reduction uses two ordered dispatches for reductions beyond six mip levels. An atomic counter alone cannot supply cross-workgroup texture visibility in WebGPU. WGSL uniformity validation remains enabled.
- An optional extended Tint translator handles packed RGB10A2 storage shaders. Set GODOT_TINT_PATH_FALLBACK or place tint-extended.exe next to the editor. Record both translator hashes with every build.
- Native-only image-atomic, subpass, multiview, and noncompact-layout variants are omitted where existing runtime selection uses supported alternatives. This is not new image-atomic or multiview support.
- Octmap and GI debug pipeline initialization respect supported formats and view modes. On core-only WebGPU devices, SDFGI/VoxelGI require texture-formats-tier2 and are unavailable; use baked lightmaps or ambient lighting. This is an explicit capability limit, not equivalent GI rendering. Existing SDFGI voxelization without image atomics retains its pre-existing concurrency limitations.

## Validation contract

The rendering fixture covers baseline, FSR1, FSR2, FSR2 camera motion, FSR2 resolution changes, volumetric fog/FogVolume, SSAO/SSIL, SSR, SDFGI, and reflection probes. Each case renders 65 frames before capture. Run both threaded (COOP/COEP) and single-threaded (no isolation and no SharedArrayBuffer) exports, with normal features and with float16/subgroups/texture tiers withheld. Core runs must explicitly report the GI capability limit, rather than silently count it as supported.

`rendering-coverage.mjs` uses Playwright from the current working directory. Arguments: exported directory, output directory, optional --no-isolation and --core. Any engine error, JavaScript exception, or GPU validation error fails the run. Inspect screenshots and compare FSR outputs to native Vulkan; successful compilation or nonblank output alone does not prove algorithm correctness.

Final release evidence must also include Mobile/Forward+ gates, async readback and GPU-depth regressions, pack parity, engine/compiler/translator hashes, and the unchanged Monday ref. Current progress/results are in the coordinating repositories' CODEX_RENDERING_COVERAGE_HANDOFF.md files.

## Upstream sync boundaries

Keep backend translation/container changes separate from [shared] patches. Shared edits are limited to the baker hook, FSR resource/dispatch adapters, format/capability-aware pipeline initialization, and SDFGI availability handling. The stack checker permits the two explicit FSR adapter files, not a broad new renderer directory exception. No vendor source edits, template installation, pushing, or Monday promotion are required by this candidate.

WebGPU format capability reference: https://www.w3.org/TR/webgpu/#plain-color-formats (consulted 2026-11-10). Feature checks, not browser names or version guesses, select paths.
