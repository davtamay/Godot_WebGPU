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
	return format;
}

bool ShaderBakerExportPluginPlatformWebGPU::matches_driver(const String &p_driver) {
	return p_driver == "webgpu";
}

bool ShaderBakerExportPluginPlatformWebGPU::skips_variant(const Vector<String> &p_stage_sources) const {
	// The WebGPU runtime can never select multiview variant groups (WGSL has
	// no ViewIndex; stereo renders one pass per view), so baking them only
	// inflates the exported cache - the load path tolerates the holes.
	// FP16 groups ARE baked: the loader requests the shader-f16 device
	// feature where the adapter offers it and the driver then reports
	// SUPPORTS_HALF_FLOAT, so the runtime selects the FP16 group on capable
	// devices and falls back to the FP32 group elsewhere - which is why both
	// groups must be present in the cache.
	for (const String &source : p_stage_sources) {
		if (source.contains("#define USE_MULTIVIEW")) {
			return true;
		}
	}
	return false;
}
