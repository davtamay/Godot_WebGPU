/**************************************************************************/
/*  shader_baker_export_plugin_platform_webgpu.cpp                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "shader_baker_export_plugin_platform_webgpu.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "drivers/webgpu/rendering_shader_container_webgpu.h"

RenderingShaderContainerFormat *ShaderBakerExportPluginPlatformWebGPU::create_shader_container_format(const Ref<EditorExportPlatform> &p_platform, const Ref<EditorExportPreset> &p_preset) {
	// Interim external translator until Tint is vendored: GODOT_TINT_PATH
	// wins when set, otherwise a tint executable dropped next to the editor
	// binary is picked up automatically (see docs/webgpu-testing.md for the
	// release assets and the build recipe).
	String tint_path = OS::get_singleton()->get_environment("GODOT_TINT_PATH");
	if (tint_path.is_empty() || !FileAccess::exists(tint_path)) {
		const String editor_dir = OS::get_singleton()->get_executable_path().get_base_dir();
		const char *tint_names[] = { "tint.exe", "tint" };
		for (const char *name : tint_names) {
			const String candidate = editor_dir.path_join(name);
			if (FileAccess::exists(candidate)) {
				tint_path = candidate;
				break;
			}
		}
	}
	ERR_FAIL_COND_V_MSG(tint_path.is_empty() || !FileAccess::exists(tint_path), nullptr,
			"Baking WebGPU shaders requires the Tint translator: set the GODOT_TINT_PATH environment variable to a tint executable, or place one next to the editor binary (prebuilt binaries are on the repository's releases page under the tint-* tag).");

	RenderingShaderContainerFormatWebGPU *format = memnew(RenderingShaderContainerFormatWebGPU);
	format->set_tint_path(tint_path);
	String fallback = OS::get_singleton()->get_environment("GODOT_TINT_PATH_FALLBACK");
	if (fallback.is_empty()) {
		fallback = OS::get_singleton()->get_executable_path().get_base_dir().path_join("tint-extended.exe");
	}
	if (FileAccess::exists(fallback)) {
		format->set_tint_fallback_path(fallback);
	}
	// Optional second translator that reads subgroup operations (a newer
	// Tint built with --allow-non-uniform-subgroup-operations): stages using
	// them then also carry a native WGSL translation for devices with the
	// feature. Without it the bake is identical to a single-translator one.
	const String tint_subgroups_path = OS::get_singleton()->get_environment("GODOT_TINT_PATH_SUBGROUPS");
	if (!tint_subgroups_path.is_empty() && FileAccess::exists(tint_subgroups_path)) {
		format->set_tint_subgroups_path(tint_subgroups_path);
	}
	return format;
}

bool ShaderBakerExportPluginPlatformWebGPU::matches_driver(const String &p_driver) {
	return p_driver == "webgpu";
}

bool ShaderBakerExportPluginPlatformWebGPU::bakes_disabled_groups() const {
	// The web runtime has no shader compiler: a variant the device selects
	// but the cache lacks is a hard failure. Group and variant enablement is
	// decided by the HOST editor's renderer (its FP16 support, its XR state,
	// its subgroup and subpass capabilities), which need not match the
	// browser's, so every group is baked and skips_variant() trims the ones
	// the web can never select.
	return true;
}

bool ShaderBakerExportPluginPlatformWebGPU::skips_variant(const Vector<String> &p_stage_sources) const {
	// The WebGPU runtime can never select multiview variant groups (WGSL has
	// no ViewIndex; stereo renders one pass per view), so baking them only
	// inflates the exported cache - the load path tolerates the holes.
	// FP16 groups are baked by default: the loader requests the shader-f16
	// device feature where the adapter offers it and the driver then reports
	// SUPPORTS_HALF_FLOAT, so the runtime selects the FP16 group on capable
	// devices and falls back to the FP32 group elsewhere - which is why both
	// groups must be present in the cache. Projects that care more about
	// download size than shader speed can opt out through the setting below
	// (surfaced as an export option by the godot_webgpu addon); devices then
	// use the FP32 group everywhere, exactly the pre-FP16 behavior.
	const bool bake_fp16 = ProjectSettings::get_singleton()->get_setting("rendering/webgpu/bake_fp16_shader_variants", true);
	for (const String &source : p_stage_sources) {
		// These runtime paths have dedicated no-image-atomic / separate-pass variants.
		if (source.contains("#define SUBPASS") ||
				(source.contains("#define MODE_RENDER_SDF") && !source.contains("#define NO_IMAGE_ATOMICS")) ||
				(source.contains("emissive_only_map_buffer") && !source.contains("#define NO_IMAGE_ATOMICS"))) {
			return true;
		}
		if (source.contains("#define USE_MULTIVIEW")) {
			return true;
		}
		// Scene shaders under the standard binding layout: the loader requests
		// the core storage-buffer limit, so the web runtime always selects the
		// compact layout's groups (LightStorage::uses_compact_scene_bindings).
		if (source.contains("#define MAX_LIGHTMAP_TEXTURES") && !source.contains("#define SCENE_COMPACT_BINDINGS")) {
			return true;
		}
		// The reduced-lightmap-slot block only differs from the compact one in
		// its lightmap versions; every other variant in it would be a
		// duplicate, and they are the largest shaders in the pack.
		if (source.contains("#define SCENE_FLOOR_LIGHTMAPS") && !source.contains("#define USE_LIGHTMAP")) {
			return true;
		}
		if (!bake_fp16 && source.contains("#define EXPLICIT_FP16")) {
			return true;
		}
	}
	return false;
}

void ShaderBakerExportPluginPlatformWebGPU::prepare_stage_sources(Vector<String> &r_sources) const {
	for (int i = 0; i < r_sources.size(); i++) {
		String source = r_sources[i];
		// FSR's half path also requires Int16, which shader-f16 does not provide.
		// Use AMD's complete FP32 algorithm under either runtime precision variant.
		source = source.replace("#define MODE_FSR_UPSCALE_NORMAL", "#define MODE_FSR_UPSCALE_FALLBACK");
		if (source.contains("#define FFX_FSR2_OPTION_HDR_COLOR_INPUT")) {
			// SPD has an exact shared-memory reduction for devices without quad operations.
			source = source.replace("#define FFX_FSR2_OPTION_HDR_COLOR_INPUT", "#define SPD_NO_WAVE_OPERATIONS\n#define FFX_FSR2_OPTION_HDR_COLOR_INPUT");
			source = source.replace("layout (set = 1, binding = FSR2_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH)        uniform utexture2D r_reconstructed_previous_nearest_depth;", "layout(set = 1, binding = FSR2_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH, std430) readonly buffer WebGPUReadDepth { uint r_reconstructed_previous_nearest_depth[]; };");
			source = source.replace("layout (set = 1, binding = FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH, r32ui) uniform uimage2D   rw_reconstructed_previous_nearest_depth;", "layout(set = 1, binding = FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH, std430) buffer WebGPUWriteDepth { uint rw_reconstructed_previous_nearest_depth[]; };");
			source = source.replace("layout (set = 1, binding = FSR2_BIND_UAV_SPD_GLOBAL_ATOMIC, r32ui)       coherent uniform uimage2D   rw_spd_global_atomic;", "layout(set = 1, binding = FSR2_BIND_UAV_SPD_GLOBAL_ATOMIC, std430) coherent buffer WebGPUCounter { uint rw_spd_global_atomic[]; };");
			source = source.replace("FSR2_BIND_UAV_UPSCALED_OUTPUT /* app controlled format */", "FSR2_BIND_UAV_UPSCALED_OUTPUT, rgba16f");
			source = source.replace("return uintBitsToFloat(texelFetch(r_reconstructed_previous_nearest_depth, iPxPos, 0).r);", "if (any(lessThan(iPxPos, ivec2(0))) || any(greaterThanEqual(iPxPos, MaxRenderSize()))) { return 0.0; } return uintBitsToFloat(r_reconstructed_previous_nearest_depth[iPxPos.y * MaxRenderSize().x + iPxPos.x]);");
			source = source.replace("imageAtomicMax(rw_reconstructed_previous_nearest_depth, iPxSample, uDepth);", "if (all(greaterThanEqual(iPxSample, ivec2(0))) && all(lessThan(iPxSample, MaxRenderSize()))) { atomicMax(rw_reconstructed_previous_nearest_depth[iPxSample.y * MaxRenderSize().x + iPxSample.x], uDepth); }");
			source = source.replace("imageAtomicMin(rw_reconstructed_previous_nearest_depth, iPxSample, uDepth);", "if (all(greaterThanEqual(iPxSample, ivec2(0))) && all(lessThan(iPxSample, MaxRenderSize()))) { atomicMin(rw_reconstructed_previous_nearest_depth[iPxSample.y * MaxRenderSize().x + iPxSample.x], uDepth); }");
			source = source.replace("imageStore(rw_reconstructed_previous_nearest_depth, iPxSample, uvec4(uValue, 0, 0, 0));", "if (all(greaterThanEqual(iPxSample, ivec2(0))) && all(lessThan(iPxSample, MaxRenderSize()))) { rw_reconstructed_previous_nearest_depth[iPxSample.y * MaxRenderSize().x + iPxSample.x] = uValue; }");
			source = source.replace("imageAtomicAdd(rw_spd_global_atomic, ivec2(0,0), 1)", "atomicAdd(rw_spd_global_atomic[0], 1u)");
			source = source.replace("imageStore(rw_spd_global_atomic, ivec2(0,0), uvec4(0));", "atomicExchange(rw_spd_global_atomic[0], 0u);");
			source = source.replace("#define FFX_HALF 1", "#define FFX_HALF 0");
			source = source.replace("uniform image2D    rw_auto_exposure;", "buffer WebGPUExposure { vec2 rw_auto_exposure; };");
			source = source.replace("FSR2_BIND_UAV_AUTO_EXPOSURE, rg32f", "FSR2_BIND_UAV_AUTO_EXPOSURE, std430");
			source = source.replace("uniform texture2D  r_auto_exposure;", "readonly buffer WebGPUReadExposure { vec2 r_auto_exposure; };");
			source = source.replace("binding = FSR2_BIND_SRV_AUTO_EXPOSURE)", "binding = FSR2_BIND_SRV_AUTO_EXPOSURE, std430)");
			source = source.replace("texelFetch(r_auto_exposure, FfxInt32x2(0, 0), 0).x", "r_auto_exposure.x");
			source = source.replace("imageLoad(rw_auto_exposure, ivec2(0,0)).xy", "rw_auto_exposure");
			source = source.replace("imageStore(rw_auto_exposure, ivec2(0,0), vec4(value, 0.0f, 0.0f));", "rw_auto_exposure = value;");

			source = source.replace("    SpdDownsampleMips_0_1(x, y, workGroupID, localInvocationIndex, mips, slice);\n\n    SpdDownsampleNextFour(x, y, workGroupID, localInvocationIndex, 2, mips, slice);\n\n    if (mips <= 6)\n        return;\n\n    if (SpdExitWorkgroup(numWorkGroups, localInvocationIndex, slice))\n        return;\n\n    SpdResetAtomicCounter(slice);", "    // WebGPU dispatch 1 writes the local six mips; dispatch 2 reads them.\n    // A command boundary supplies visibility across workgroups, which an\n    // atomic counter and workgroupBarrier cannot establish in WGSL.\n    if (numWorkGroups != 0u) {\n        SpdDownsampleMips_0_1(x, y, workGroupID, localInvocationIndex, mips, slice);\n        SpdDownsampleNextFour(x, y, workGroupID, localInvocationIndex, 2, mips, slice);\n        return;\n    }\n    if (mips <= 6) return;\n    if (localInvocationIndex == 0u) SpdResetAtomicCounter(slice);");
		}
		r_sources.write[i] = source;
	}
}
