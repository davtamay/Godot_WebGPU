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
		if (source.contains("#define USE_MULTIVIEW")) {
			return true;
		}
		if (!bake_fp16 && source.contains("#define EXPLICIT_FP16")) {
			return true;
		}
	}
	return false;
}
