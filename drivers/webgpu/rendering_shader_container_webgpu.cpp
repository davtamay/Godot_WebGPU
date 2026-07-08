/**************************************************************************/
/*  rendering_shader_container_webgpu.cpp                                 */
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

#include "rendering_shader_container_webgpu.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/templates/safe_refcount.h"

const uint32_t RenderingShaderContainerWebGPU::FORMAT_VERSION = 1;

// SPIR-V constants used by the pre-transform.
enum {
	SPIRV_MAGIC = 0x07230203,
	SPIRV_VERSION_1_3 = 0x00010300,
	SPIRV_OP_ENTRY_POINT = 15,
	SPIRV_OP_TYPE_STRUCT = 30,
	SPIRV_OP_TYPE_POINTER = 32,
	SPIRV_OP_VARIABLE = 59,
	SPIRV_OP_DECORATE = 71,
	SPIRV_OP_MEMBER_DECORATE = 72,
	SPIRV_DECORATION_NON_WRITABLE = 24,
	SPIRV_DECORATION_BINDING = 33,
	SPIRV_DECORATION_DESCRIPTOR_SET = 34,
	SPIRV_STORAGE_CLASS_INPUT = 1,
	SPIRV_STORAGE_CLASS_UNIFORM = 2,
	SPIRV_STORAGE_CLASS_OUTPUT = 3,
	SPIRV_STORAGE_CLASS_PUSH_CONSTANT = 9,
	SPIRV_STORAGE_CLASS_STORAGE_BUFFER = 12,
};

// Makes a Godot-compiled SPIR-V module acceptable to Tint's reader (SPIR-V
// 1.3 under Vulkan 1.1 semantics):
// - The version is downgraded to 1.3 and, per 1.3 rules, OpEntryPoint
//   interfaces are reduced to Input/Output variables (1.4+ lists every
//   global; the shader baker compiles with the running driver's version).
// - The push-constant block becomes a read-only storage buffer at
//   PUSH_CONSTANT_GROUP/PUSH_CONSTANT_BINDING: Tint rejects the PushConstant
//   storage class, and the engine's push constants use std430 packing, which
//   uniform buffers do not allow.
bool RenderingShaderContainerWebGPU::_transform_spirv(Vector<uint8_t> &r_spirv) const {
	ERR_FAIL_COND_V(r_spirv.size() % 4 != 0 || r_spirv.size() < 20, false);
	const uint32_t *words = (const uint32_t *)r_spirv.ptr();
	const uint32_t word_count = r_spirv.size() / 4;
	ERR_FAIL_COND_V(words[0] != SPIRV_MAGIC, false);

	// First pass: variable storage classes (OpEntryPoint appears before the
	// variables it references) and struct member counts (for the push-constant
	// NonWritable decorations).
	HashMap<uint32_t, uint32_t> variable_storage;
	HashMap<uint32_t, uint32_t> struct_member_counts;
	uint32_t i = 5;
	while (i < word_count) {
		const uint32_t instruction_words = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFF;
		ERR_FAIL_COND_V(instruction_words == 0 || i + instruction_words > word_count, false);
		if (opcode == SPIRV_OP_VARIABLE) {
			variable_storage[words[i + 2]] = words[i + 3];
		} else if (opcode == SPIRV_OP_TYPE_STRUCT) {
			struct_member_counts[words[i + 1]] = instruction_words - 2;
		}
		i += instruction_words;
	}

	// Second pass: rebuild the module with the transforms applied.
	LocalVector<uint32_t> out;
	out.reserve(word_count + 8);
	for (uint32_t w = 0; w < 5; w++) {
		out.push_back(words[w]);
	}
	if (out[1] > SPIRV_VERSION_1_3) {
		out[1] = SPIRV_VERSION_1_3;
	}

	uint32_t push_constant_var_id = 0;
	uint32_t push_constant_var_type_id = 0;
	HashMap<uint32_t, uint32_t> push_constant_pointees;
	bool push_constants_found = false;
	uint32_t last_decoration_end = 0;
	i = 5;
	while (i < word_count) {
		const uint32_t instruction_words = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFF;
		const uint32_t start = out.size();
		for (uint32_t w = 0; w < instruction_words; w++) {
			out.push_back(words[i + w]);
		}
		if (opcode == SPIRV_OP_TYPE_POINTER && out[start + 2] == SPIRV_STORAGE_CLASS_PUSH_CONSTANT) {
			out[start + 2] = SPIRV_STORAGE_CLASS_STORAGE_BUFFER;
			// Member-access pointers use this class too; the block struct is
			// resolved through the variable's own pointer type below.
			push_constant_pointees[out[start + 1]] = out[start + 3];
		} else if (opcode == SPIRV_OP_VARIABLE && out[start + 3] == SPIRV_STORAGE_CLASS_PUSH_CONSTANT) {
			out[start + 3] = SPIRV_STORAGE_CLASS_STORAGE_BUFFER;
			push_constant_var_id = out[start + 2];
			push_constant_var_type_id = out[start + 1];
			push_constants_found = true;
		} else if (opcode == SPIRV_OP_DECORATE) {
			last_decoration_end = out.size();
		} else if (opcode == SPIRV_OP_ENTRY_POINT) {
			// Skip the null-terminated name literal to find the interface ids.
			uint32_t n = start + 3;
			while (n < start + instruction_words) {
				const uint32_t w = out[n++];
				if (((w >> 0) & 0xFF) == 0 || ((w >> 8) & 0xFF) == 0 || ((w >> 16) & 0xFF) == 0 || ((w >> 24) & 0xFF) == 0) {
					break;
				}
			}
			uint32_t write = n;
			for (uint32_t r = n; r < start + instruction_words; r++) {
				const uint32_t *storage_class = variable_storage.getptr(out[r]);
				if (storage_class == nullptr || *storage_class == SPIRV_STORAGE_CLASS_INPUT || *storage_class == SPIRV_STORAGE_CLASS_OUTPUT) {
					out[write++] = out[r];
				}
			}
			out.resize(write);
			out[start] = ((write - start) << 16) | SPIRV_OP_ENTRY_POINT;
		}
		i += instruction_words;
	}

	if (push_constants_found) {
		ERR_FAIL_COND_V_MSG(last_decoration_end == 0, false, "SPIR-V module with push constants has no decoration section.");
		// Set/binding for the variable, plus NonWritable on every member so the
		// storage buffer reads as read-only (writable storage is not allowed
		// in vertex stages).
		LocalVector<uint32_t> inject;
		inject.push_back((4 << 16) | SPIRV_OP_DECORATE);
		inject.push_back(push_constant_var_id);
		inject.push_back(SPIRV_DECORATION_DESCRIPTOR_SET);
		inject.push_back(PUSH_CONSTANT_GROUP);
		inject.push_back((4 << 16) | SPIRV_OP_DECORATE);
		inject.push_back(push_constant_var_id);
		inject.push_back(SPIRV_DECORATION_BINDING);
		inject.push_back(PUSH_CONSTANT_BINDING);
		const uint32_t *pointee = push_constant_pointees.getptr(push_constant_var_type_id);
		const uint32_t push_constant_struct_id = pointee != nullptr ? *pointee : 0;
		const uint32_t *member_count = struct_member_counts.getptr(push_constant_struct_id);
		for (uint32_t m = 0; member_count != nullptr && m < *member_count; m++) {
			inject.push_back((4 << 16) | SPIRV_OP_MEMBER_DECORATE);
			inject.push_back(push_constant_struct_id);
			inject.push_back(m);
			inject.push_back(SPIRV_DECORATION_NON_WRITABLE);
		}
		LocalVector<uint32_t> injected;
		injected.reserve(out.size() + inject.size());
		for (uint32_t w = 0; w < last_decoration_end; w++) {
			injected.push_back(out[w]);
		}
		for (uint32_t w = 0; w < inject.size(); w++) {
			injected.push_back(inject[w]);
		}
		for (uint32_t w = last_decoration_end; w < out.size(); w++) {
			injected.push_back(out[w]);
		}
		out = injected;
	}

	r_spirv.resize(out.size() * 4);
	memcpy(r_spirv.ptrw(), out.ptr(), r_spirv.size());
	return true;
}

uint32_t RenderingShaderContainerWebGPU::_format() const {
	return 0x4C534757; // "WGSL"
}

uint32_t RenderingShaderContainerWebGPU::_format_version() const {
	return FORMAT_VERSION;
}

bool RenderingShaderContainerWebGPU::_set_code_from_spirv(const ReflectShader &p_shader) {
	ERR_FAIL_COND_V_MSG(tint_path.is_empty(), false,
			"WebGPU shaders can only be compiled at export time with the Tint translator configured; runtime shader compilation is not supported on the web platform.");

	const LocalVector<ReflectShaderStage> &spirv_stages = p_shader.shader_stages;
	shaders.resize(spirv_stages.size());
	for (uint32_t i = 0; i < spirv_stages.size(); i++) {
		Vector<uint8_t> spirv = spirv_stages[i].spirv_data();
		ERR_FAIL_COND_V_MSG(!_transform_spirv(spirv), false, "Malformed SPIR-V module.");

		// Translate through the external Tint binary via temporary files.
		// Shaders bake on many worker threads at once, so temporary names use
		// a process-wide counter (FileAccess::create_temp's timestamp-based
		// names collide under this load).
		static SafeNumeric<uint32_t> temp_counter;
		const String spirv_path = OS::get_singleton()->get_temp_path().path_join(vformat("godot_webgpu_%d_%d.spv", OS::get_singleton()->get_process_id(), temp_counter.increment()));
		const String wgsl_path = spirv_path + ".wgsl";
		{
			Ref<FileAccess> spirv_file = FileAccess::open(spirv_path, FileAccess::WRITE);
			ERR_FAIL_COND_V(spirv_file.is_null(), false);
			spirv_file->store_buffer(spirv.ptr(), spirv.size());
		}

		List<String> args;
		args.push_back(spirv_path);
		args.push_back("--format");
		args.push_back("wgsl");
		args.push_back("--allow-non-uniform-derivatives");
		args.push_back("-o");
		args.push_back(wgsl_path);
		String output;
		int exit_code = -1;
		Error err = OS::get_singleton()->execute(tint_path, args, &output, &exit_code, true);
		if (err != OK || exit_code != 0) {
			DirAccess::remove_absolute(spirv_path);
			DirAccess::remove_absolute(wgsl_path);
			ERR_FAIL_V_MSG(false, vformat("Tint translation of shader '%s' stage #%d failed (exit code %d): %s", String::utf8(shader_name.get_data()), i, exit_code, output));
		}

		const PackedByteArray wgsl = FileAccess::get_file_as_bytes(wgsl_path);
		DirAccess::remove_absolute(spirv_path);
		DirAccess::remove_absolute(wgsl_path);
		ERR_FAIL_COND_V_MSG(wgsl.is_empty(), false, "Tint produced no WGSL output.");

		RenderingShaderContainer::Shader &shader = shaders.ptrw()[i];
		shader.shader_stage = spirv_stages[i].shader_stage;
		shader.code_decompressed_size = wgsl.size();
		shader.code_compressed_bytes.resize(wgsl.size());
		uint32_t compressed_size = 0;
		const bool compressed = compress_code(wgsl.ptr(), wgsl.size(), shader.code_compressed_bytes.ptrw(), &compressed_size, &shader.code_compression_flags);
		ERR_FAIL_COND_V_MSG(!compressed, false, vformat("Failed to compress WGSL for stage #%d.", i));
		shader.code_compressed_bytes.resize(compressed_size);
	}
	return true;
}

void RenderingShaderContainerFormatWebGPU::set_tint_path(const String &p_tint_path) {
	tint_path = p_tint_path;
}

Ref<RenderingShaderContainer> RenderingShaderContainerFormatWebGPU::create_container() const {
	Ref<RenderingShaderContainerWebGPU> container;
	container.instantiate();
	container->tint_path = tint_path;
	return container;
}

RenderingShaderContainerFormat::ShaderLanguageVersion RenderingShaderContainerFormatWebGPU::get_shader_language_version() const {
	return SHADER_LANGUAGE_VULKAN_VERSION_1_0;
}

RenderingShaderContainerFormat::ShaderSpirvVersion RenderingShaderContainerFormatWebGPU::get_shader_spirv_version() const {
	return SHADER_SPIRV_VERSION_1_0;
}
