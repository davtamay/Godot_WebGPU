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
#include "core/templates/hash_set.h"

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

// The compatibility passes below rewrite constructs Tint's SPIR-V reader
// rejects. They run after the main transform, on the rebuilt module. All of
// them follow the same conventions: instructions are (word_count << 16 |
// opcode) headers followed by operands, the id bound lives at word 3, and
// use-counting treats any operand match as a use (literal words that
// coincide with an id keep the target alive, which only errs toward keeping
// things).

enum {
	SPIRV_OP_SOURCE = 3,
	SPIRV_OP_SOURCE_EXTENSION = 4,
	SPIRV_OP_NAME = 5,
	SPIRV_OP_MEMBER_NAME = 6,
	SPIRV_OP_STRING = 7,
	SPIRV_OP_EXECUTION_MODE = 16,
	SPIRV_OP_TYPE_BOOL = 20,
	SPIRV_OP_TYPE_INT = 21,
	SPIRV_OP_TYPE_FLOAT = 22,
	SPIRV_OP_TYPE_VECTOR = 23,
	SPIRV_OP_TYPE_IMAGE = 25,
	SPIRV_OP_TYPE_SAMPLER = 26,
	SPIRV_OP_TYPE_SAMPLED_IMAGE = 27,
	SPIRV_OP_TYPE_ARRAY = 28,
	SPIRV_OP_TYPE_RUNTIME_ARRAY = 29,
	SPIRV_OP_CONSTANT = 43,
	SPIRV_OP_FUNCTION = 54,
	SPIRV_OP_ACCESS_CHAIN = 65,
	SPIRV_OP_COMPOSITE_CONSTRUCT = 80,
	SPIRV_OP_STORE = 62,
	SPIRV_OP_BITCAST = 124,
	SPIRV_OP_IS_NAN = 156,
	SPIRV_OP_IS_INF = 157,
	SPIRV_OP_SELECT = 169,
	SPIRV_OP_U_GREATER_THAN = 172,
	SPIRV_OP_F_UNORD_NOT_EQUAL = 183,
	SPIRV_OP_BITWISE_AND = 199,
	SPIRV_OP_CONTROL_BARRIER = 224,
	SPIRV_OP_MEMORY_BARRIER = 225,
	SPIRV_OP_MODULE_PROCESSED = 330,
	SPIRV_STORAGE_CLASS_UNIFORM_CONSTANT = 0,
	SPIRV_DECORATION_BUILT_IN = 11,
	SPIRV_BUILT_IN_POINT_SIZE = 1,
	SPIRV_SCOPE_DEVICE = 1,
	SPIRV_SCOPE_WORKGROUP = 2,
	SPIRV_IMAGE_OPERANDS_SIGN_ZERO_EXTEND = 0x3000,
	SPIRV_FLOAT_NEG_INF = 0xFF800000,
	SPIRV_FLOAT_POS_INF = 0x7F800000,
	SPIRV_FLOAT_NEG_MAX = 0xFF7FFFFF,
	SPIRV_FLOAT_POS_MAX = 0x7F7FFFFF,
};

// SignExtend/ZeroExtend image operands are SPIR-V 1.4+; the 1.3 downgrade
// must drop them. The bits carry no operand words.
static void _spirv_strip_extended_image_operands(LocalVector<uint32_t> &p_module) {
	static const struct {
		uint32_t opcode;
		uint32_t mask_index;
	} slots[] = {
		{ 87, 5 }, { 88, 5 }, { 89, 6 }, { 90, 6 }, { 91, 5 }, { 92, 5 }, { 93, 6 }, { 94, 6 }, // OpImageSample*.
		{ 95, 5 }, { 96, 6 }, { 97, 6 }, { 98, 5 }, { 99, 4 }, // Fetch, gathers, read, write.
	};
	LocalVector<uint32_t> out;
	out.reserve(p_module.size());
	for (uint32_t w = 0; w < 5; w++) {
		out.push_back(p_module[w]);
	}
	uint32_t i = 5;
	while (i < p_module.size()) {
		uint32_t count = p_module[i] >> 16;
		const uint32_t opcode = p_module[i] & 0xFFFF;
		uint32_t mask_index = 0;
		for (const auto &slot : slots) {
			if (slot.opcode == opcode) {
				mask_index = slot.mask_index;
				break;
			}
		}
		if (mask_index != 0 && count > mask_index) {
			uint32_t mask = p_module[i + mask_index] & ~(uint32_t)SPIRV_IMAGE_OPERANDS_SIGN_ZERO_EXTEND;
			const uint32_t emit_count = (mask == 0 && count == mask_index + 1) ? mask_index : count;
			out.push_back((emit_count << 16) | opcode);
			for (uint32_t w = 1; w < emit_count; w++) {
				out.push_back(w == mask_index ? mask : p_module[i + w]);
			}
		} else {
			for (uint32_t w = 0; w < count; w++) {
				out.push_back(p_module[i + w]);
			}
		}
		i += count;
	}
	p_module = out;
}

// Tint cannot represent infinite float constants in WGSL; clamp them to the
// nearest finite value (the engine uses -inf as a sentinel "smallest" value,
// which -FLT_MAX serves equally).
static void _spirv_clamp_infinite_constants(LocalVector<uint32_t> &p_module) {
	HashSet<uint32_t> float32_types;
	uint32_t i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		const uint32_t opcode = p_module[i] & 0xFFFF;
		if (opcode == SPIRV_OP_TYPE_FLOAT && p_module[i + 2] == 32) {
			float32_types.insert(p_module[i + 1]);
		} else if (opcode == SPIRV_OP_CONSTANT && count == 4 && float32_types.has(p_module[i + 1])) {
			if (p_module[i + 3] == SPIRV_FLOAT_NEG_INF) {
				p_module[i + 3] = SPIRV_FLOAT_NEG_MAX;
			} else if (p_module[i + 3] == SPIRV_FLOAT_POS_INF) {
				p_module[i + 3] = SPIRV_FLOAT_POS_MAX;
			}
		}
		i += count;
	}
}

// Maps result id -> result type id for the value-producing opcodes a select
// condition can come from (all use the (type, result, ...) layout).
static void _spirv_collect_value_types(const LocalVector<uint32_t> &p_module, HashMap<uint32_t, uint32_t> &r_types) {
	uint32_t i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		const uint32_t opcode = p_module[i] & 0xFFFF;
		const bool value_op = opcode == 1 || (opcode >= 41 && opcode <= 44) || (opcode >= 48 && opcode <= 52) || opcode == 57 || opcode == 61 || (opcode >= 79 && opcode <= 81) || opcode == 124 || (opcode >= 154 && opcode <= 157) || (opcode >= 166 && opcode <= 191) || opcode == 245;
		if (value_op && count >= 3) {
			r_types[p_module[i + 2]] = p_module[i + 1];
		}
		i += count;
	}
}

// SPIR-V 1.4 allows OpSelect with a scalar bool condition selecting between
// vectors; 1.3 (and Tint) require the condition to match component-wise.
// Splat the condition into a bool vector right before such selects.
static void _spirv_splat_select_conditions(LocalVector<uint32_t> &p_module) {
	HashMap<uint32_t, uint32_t> value_types;
	_spirv_collect_value_types(p_module, value_types);
	uint32_t bool_type = 0;
	uint32_t bool_type_offset = 0;
	HashMap<uint32_t, uint32_t> bool_vectors; // Component count -> bool vector type id.
	HashMap<uint32_t, uint32_t> all_vector_sizes;
	uint32_t i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		const uint32_t opcode = p_module[i] & 0xFFFF;
		if (opcode == SPIRV_OP_TYPE_BOOL) {
			bool_type = p_module[i + 1];
			bool_type_offset = i;
		} else if (opcode == SPIRV_OP_TYPE_VECTOR) {
			all_vector_sizes[p_module[i + 1]] = p_module[i + 3];
			if (p_module[i + 2] == bool_type && bool_type != 0) {
				bool_vectors[p_module[i + 3]] = p_module[i + 1];
			}
		}
		i += count;
	}
	if (bool_type == 0) {
		return;
	}

	// Find selects that need the splat and the vector sizes involved.
	LocalVector<uint32_t> pending_sizes;
	bool any = false;
	i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		if ((p_module[i] & 0xFFFF) == SPIRV_OP_SELECT) {
			const uint32_t *size = all_vector_sizes.getptr(p_module[i + 1]);
			const uint32_t *cond_type = value_types.getptr(p_module[i + 3]);
			if (size != nullptr && cond_type != nullptr && *cond_type == bool_type) {
				any = true;
				if (!bool_vectors.has(*size) && !pending_sizes.has(*size)) {
					pending_sizes.push_back(*size);
				}
			}
		}
		i += count;
	}
	if (!any) {
		return;
	}

	uint32_t next_id = p_module[3];
	for (const uint32_t size : pending_sizes) {
		bool_vectors[size] = next_id++;
	}

	LocalVector<uint32_t> out;
	out.reserve(p_module.size() + 16);
	i = 0;
	while (i < p_module.size()) {
		const uint32_t count = i < 5 ? 1 : p_module[i] >> 16;
		const uint32_t opcode = i < 5 ? 0xFFFFFFFF : p_module[i] & 0xFFFF;
		if (i >= 5 && opcode == SPIRV_OP_SELECT) {
			const uint32_t *size = all_vector_sizes.getptr(p_module[i + 1]);
			const uint32_t *cond_type = value_types.getptr(p_module[i + 3]);
			if (size != nullptr && cond_type != nullptr && *cond_type == bool_type) {
				const uint32_t vec_type = bool_vectors[*size];
				const uint32_t splat_id = next_id++;
				out.push_back(((3 + *size) << 16) | SPIRV_OP_COMPOSITE_CONSTRUCT);
				out.push_back(vec_type);
				out.push_back(splat_id);
				for (uint32_t c = 0; c < *size; c++) {
					out.push_back(p_module[i + 3]);
				}
				for (uint32_t w = 0; w < count; w++) {
					out.push_back(w == 3 ? splat_id : p_module[i + w]);
				}
				i += count;
				continue;
			}
		}
		for (uint32_t w = 0; w < count; w++) {
			out.push_back(p_module[i + w]);
		}
		// New bool vector types go right after OpTypeBool.
		if (i == bool_type_offset) {
			for (const uint32_t size : pending_sizes) {
				out.push_back((4 << 16) | SPIRV_OP_TYPE_VECTOR);
				out.push_back(bool_vectors[size]);
				out.push_back(bool_type);
				out.push_back(size);
			}
		}
		i += count;
	}
	out[3] = next_id;
	p_module = out;
}

// Tint's reader lacks OpIsNan/OpIsInf. isnan(x) becomes the unordered
// self-inequality; isinf(x) masks the sign bit off the float's bit pattern
// and compares against the largest finite encoding (pure integer ops, so no
// extended instruction set is needed).
static void _spirv_rewrite_non_finite_tests(LocalVector<uint32_t> &p_module) {
	bool has_isinf = false;
	bool any = false;
	uint32_t i = 5;
	while (i < p_module.size()) {
		const uint32_t opcode = p_module[i] & 0xFFFF;
		if (opcode == SPIRV_OP_IS_NAN || opcode == SPIRV_OP_IS_INF) {
			any = true;
			has_isinf = has_isinf || opcode == SPIRV_OP_IS_INF;
		}
		i += p_module[i] >> 16;
	}
	if (!any) {
		return;
	}

	HashMap<uint32_t, uint32_t> value_types;
	_spirv_collect_value_types(p_module, value_types);

	// Existing types/constants reusable for the isinf expansion.
	uint32_t uint_type = 0;
	HashMap<uint32_t, uint32_t> vector_info_type; // Vector type id -> component type.
	HashMap<uint32_t, uint32_t> vector_info_size;
	uint32_t first_function_offset = 0;
	i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		const uint32_t opcode = p_module[i] & 0xFFFF;
		if (opcode == SPIRV_OP_TYPE_INT && p_module[i + 2] == 32 && p_module[i + 3] == 0) {
			uint_type = p_module[i + 1];
		} else if (opcode == SPIRV_OP_TYPE_VECTOR) {
			vector_info_type[p_module[i + 1]] = p_module[i + 2];
			vector_info_size[p_module[i + 1]] = p_module[i + 3];
		} else if (opcode == SPIRV_OP_FUNCTION && first_function_offset == 0) {
			first_function_offset = i;
		}
		i += count;
	}
	ERR_FAIL_COND(first_function_offset == 0);

	uint32_t next_id = p_module[3];
	LocalVector<uint32_t> new_globals;
	uint32_t mask_const = 0;
	uint32_t max_const = 0;
	HashMap<uint32_t, uint32_t> uint_vectors; // Size -> uvecN type id.
	HashMap<uint32_t, uint32_t> mask_composites; // Size -> splatted constant id.
	HashMap<uint32_t, uint32_t> max_composites;
	if (has_isinf) {
		if (uint_type == 0) {
			uint_type = next_id++;
			new_globals.push_back((4 << 16) | SPIRV_OP_TYPE_INT);
			new_globals.push_back(uint_type);
			new_globals.push_back(32);
			new_globals.push_back(0);
		}
		mask_const = next_id++;
		new_globals.push_back((4 << 16) | SPIRV_OP_CONSTANT);
		new_globals.push_back(uint_type);
		new_globals.push_back(mask_const);
		new_globals.push_back(0x7FFFFFFF);
		max_const = next_id++;
		new_globals.push_back((4 << 16) | SPIRV_OP_CONSTANT);
		new_globals.push_back(uint_type);
		new_globals.push_back(max_const);
		new_globals.push_back(SPIRV_FLOAT_POS_MAX);
	}

	// Pre-create uvec types and splatted constants for every vector-typed
	// isinf operand.
	i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		if ((p_module[i] & 0xFFFF) == SPIRV_OP_IS_INF) {
			const uint32_t *operand_type = value_types.getptr(p_module[i + 3]);
			const uint32_t *size = operand_type != nullptr ? vector_info_size.getptr(*operand_type) : nullptr;
			if (size != nullptr && !uint_vectors.has(*size)) {
				// Reuse an existing uvecN if the module has one.
				uint32_t existing = 0;
				for (const KeyValue<uint32_t, uint32_t> &vec : vector_info_type) {
					if (vec.value == uint_type && vector_info_size[vec.key] == *size) {
						existing = vec.key;
						break;
					}
				}
				const uint32_t vec_type = existing != 0 ? existing : next_id++;
				if (existing == 0) {
					new_globals.push_back((4 << 16) | SPIRV_OP_TYPE_VECTOR);
					new_globals.push_back(vec_type);
					new_globals.push_back(uint_type);
					new_globals.push_back(*size);
				}
				uint_vectors[*size] = vec_type;
				const uint32_t mask_vec = next_id++;
				new_globals.push_back(((3 + *size) << 16) | 44); // OpConstantComposite.
				new_globals.push_back(vec_type);
				new_globals.push_back(mask_vec);
				for (uint32_t c = 0; c < *size; c++) {
					new_globals.push_back(mask_const);
				}
				mask_composites[*size] = mask_vec;
				const uint32_t max_vec = next_id++;
				new_globals.push_back(((3 + *size) << 16) | 44);
				new_globals.push_back(vec_type);
				new_globals.push_back(max_vec);
				for (uint32_t c = 0; c < *size; c++) {
					new_globals.push_back(max_const);
				}
				max_composites[*size] = max_vec;
			}
		}
		i += count;
	}

	LocalVector<uint32_t> out;
	out.reserve(p_module.size() + new_globals.size() + 32);
	i = 0;
	while (i < p_module.size()) {
		if (i == first_function_offset) {
			for (const uint32_t w : new_globals) {
				out.push_back(w);
			}
		}
		const uint32_t count = i < 5 ? 1 : p_module[i] >> 16;
		const uint32_t opcode = i < 5 ? 0xFFFFFFFF : p_module[i] & 0xFFFF;
		if (opcode == SPIRV_OP_IS_NAN) {
			out.push_back((5 << 16) | SPIRV_OP_F_UNORD_NOT_EQUAL);
			out.push_back(p_module[i + 1]);
			out.push_back(p_module[i + 2]);
			out.push_back(p_module[i + 3]);
			out.push_back(p_module[i + 3]);
		} else if (opcode == SPIRV_OP_IS_INF) {
			const uint32_t *operand_type = value_types.getptr(p_module[i + 3]);
			const uint32_t *size = operand_type != nullptr ? vector_info_size.getptr(*operand_type) : nullptr;
			const uint32_t int_type = size != nullptr ? uint_vectors[*size] : uint_type;
			const uint32_t mask_id = size != nullptr ? mask_composites[*size] : mask_const;
			const uint32_t max_id = size != nullptr ? max_composites[*size] : max_const;
			const uint32_t bits_id = next_id++;
			const uint32_t masked_id = next_id++;
			out.push_back((4 << 16) | SPIRV_OP_BITCAST);
			out.push_back(int_type);
			out.push_back(bits_id);
			out.push_back(p_module[i + 3]);
			out.push_back((5 << 16) | SPIRV_OP_BITWISE_AND);
			out.push_back(int_type);
			out.push_back(masked_id);
			out.push_back(bits_id);
			out.push_back(mask_id);
			out.push_back((5 << 16) | SPIRV_OP_U_GREATER_THAN);
			out.push_back(p_module[i + 1]);
			out.push_back(p_module[i + 2]);
			out.push_back(masked_id);
			out.push_back(max_id);
		} else {
			for (uint32_t w = 0; w < count; w++) {
				out.push_back(p_module[i + w]);
			}
		}
		i += count;
	}
	out[3] = next_id;
	p_module = out;
}

// Tint's reader lacks GLSL.std.450 ModfStruct; lower it to
// Trunc + subtract + composite (member 0 = fraction, member 1 = whole).
static void _spirv_rewrite_modf_struct(LocalVector<uint32_t> &p_module) {
	const uint32_t OP_EXT_INST = 12;
	const uint32_t GLSL450_MODF_STRUCT = 36;
	const uint32_t GLSL450_TRUNC = 3;
	const uint32_t OP_F_SUB = 131;
	bool any = false;
	HashMap<uint32_t, uint32_t> struct_member_type; // Struct id -> first member type.
	uint32_t i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		const uint32_t opcode = p_module[i] & 0xFFFF;
		if (opcode == 30 && count >= 3) { // OpTypeStruct.
			struct_member_type[p_module[i + 1]] = p_module[i + 2];
		} else if (opcode == OP_EXT_INST && count == 6 && p_module[i + 4] == GLSL450_MODF_STRUCT) {
			any = true;
		}
		i += count;
	}
	if (!any) {
		return;
	}
	uint32_t next_id = p_module[3];
	LocalVector<uint32_t> out;
	out.reserve(p_module.size() + 16);
	i = 0;
	while (i < p_module.size()) {
		const uint32_t count = i < 5 ? 1 : p_module[i] >> 16;
		const uint32_t opcode = i < 5 ? 0xFFFFFFFF : p_module[i] & 0xFFFF;
		if (opcode == OP_EXT_INST && count == 6 && p_module[i + 4] == GLSL450_MODF_STRUCT) {
			const uint32_t result_type = p_module[i + 1];
			const uint32_t result_id = p_module[i + 2];
			const uint32_t set_id = p_module[i + 3];
			const uint32_t x = p_module[i + 5];
			const uint32_t *member_type = struct_member_type.getptr(result_type);
			ERR_FAIL_NULL(member_type);
			const uint32_t whole_id = next_id++;
			const uint32_t frac_id = next_id++;
			out.push_back((6 << 16) | OP_EXT_INST);
			out.push_back(*member_type);
			out.push_back(whole_id);
			out.push_back(set_id);
			out.push_back(GLSL450_TRUNC);
			out.push_back(x);
			out.push_back((5 << 16) | OP_F_SUB);
			out.push_back(*member_type);
			out.push_back(frac_id);
			out.push_back(x);
			out.push_back(whole_id);
			out.push_back((5 << 16) | SPIRV_OP_COMPOSITE_CONSTRUCT);
			out.push_back(result_type);
			out.push_back(result_id);
			out.push_back(frac_id);
			out.push_back(whole_id);
		} else {
			for (uint32_t w = 0; w < count; w++) {
				out.push_back(p_module[i + w]);
			}
		}
		i += count;
	}
	out[3] = next_id;
	p_module = out;
}

// Tint only accepts the constant 1.0 stored to PointSize; the engine writes
// computed sizes for point primitives. Dropping the stores loses point-size
// control on WebGPU (points render 1px) but keeps the variants compiling.
static void _spirv_strip_point_size_stores(LocalVector<uint32_t> &p_module) {
	uint32_t member_struct = 0;
	uint32_t member_index = 0;
	bool has_member = false;
	HashSet<uint32_t> point_size_vars;
	HashMap<uint32_t, uint32_t> pointer_pointees;
	HashMap<uint32_t, uint32_t> variable_types;
	HashSet<uint32_t> member_index_constants;
	uint32_t i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		const uint32_t opcode = p_module[i] & 0xFFFF;
		if (opcode == SPIRV_OP_MEMBER_DECORATE && count == 5 && p_module[i + 3] == SPIRV_DECORATION_BUILT_IN && p_module[i + 4] == SPIRV_BUILT_IN_POINT_SIZE) {
			member_struct = p_module[i + 1];
			member_index = p_module[i + 2];
			has_member = true;
		} else if (opcode == SPIRV_OP_DECORATE && count == 4 && p_module[i + 2] == SPIRV_DECORATION_BUILT_IN && p_module[i + 3] == SPIRV_BUILT_IN_POINT_SIZE) {
			point_size_vars.insert(p_module[i + 1]);
		} else if (opcode == SPIRV_OP_TYPE_POINTER) {
			pointer_pointees[p_module[i + 1]] = p_module[i + 3];
		} else if (opcode == SPIRV_OP_VARIABLE) {
			variable_types[p_module[i + 2]] = p_module[i + 1];
		}
		i += count;
	}
	if (!has_member && point_size_vars.is_empty()) {
		return;
	}
	if (has_member) {
		i = 5;
		while (i < p_module.size()) {
			const uint32_t count = p_module[i] >> 16;
			if ((p_module[i] & 0xFFFF) == SPIRV_OP_CONSTANT && count == 4 && p_module[i + 3] == member_index) {
				member_index_constants.insert(p_module[i + 2]);
			}
			i += count;
		}
	}

	// Pointers reaching PointSize: direct variables plus access chains whose
	// base is a PointSize variable or a gl_PerVertex member access.
	HashSet<uint32_t> point_size_pointers;
	for (const uint32_t var : point_size_vars) {
		point_size_pointers.insert(var);
	}
	i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		if ((p_module[i] & 0xFFFF) == SPIRV_OP_ACCESS_CHAIN && count >= 5) {
			const uint32_t base = p_module[i + 3];
			if (point_size_vars.has(base)) {
				point_size_pointers.insert(p_module[i + 2]);
			} else if (has_member && member_index_constants.has(p_module[i + 4])) {
				const uint32_t *var_type = variable_types.getptr(base);
				const uint32_t *pointee = var_type != nullptr ? pointer_pointees.getptr(*var_type) : nullptr;
				if (pointee != nullptr && *pointee == member_struct) {
					point_size_pointers.insert(p_module[i + 2]);
				}
			}
		}
		i += count;
	}

	// Drop stores through those pointers, then access chains left unused.
	LocalVector<uint32_t> out;
	out.reserve(p_module.size());
	HashSet<uint32_t> removed_chains;
	for (uint32_t w = 0; w < 5; w++) {
		out.push_back(p_module[w]);
	}
	i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		const uint32_t opcode = p_module[i] & 0xFFFF;
		const bool drop = opcode == SPIRV_OP_STORE && point_size_pointers.has(p_module[i + 1]);
		if (!drop) {
			for (uint32_t w = 0; w < count; w++) {
				out.push_back(p_module[i + w]);
			}
		}
		i += count;
	}

	// Remove access chains (and their debug names) that no longer have uses.
	HashMap<uint32_t, uint32_t> uses;
	i = 5;
	while (i < out.size()) {
		const uint32_t count = out[i] >> 16;
		const uint32_t opcode = out[i] & 0xFFFF;
		const bool countable = opcode != SPIRV_OP_NAME && opcode != SPIRV_OP_MEMBER_NAME && opcode != SPIRV_OP_DECORATE && opcode != SPIRV_OP_MEMBER_DECORATE;
		if (countable) {
			const uint32_t skip = (opcode == SPIRV_OP_ACCESS_CHAIN) ? 3 : 1; // Don't count an access chain's own result.
			for (uint32_t w = skip; w < count; w++) {
				if (point_size_pointers.has(out[i + w]) && !(opcode == SPIRV_OP_ACCESS_CHAIN && w == 2)) {
					uses[out[i + w]] = uses.has(out[i + w]) ? uses[out[i + w]] + 1 : 1;
				}
			}
		}
		i += count;
	}
	LocalVector<uint32_t> final_out;
	final_out.reserve(out.size());
	for (uint32_t w = 0; w < 5; w++) {
		final_out.push_back(out[w]);
	}
	i = 5;
	while (i < out.size()) {
		const uint32_t count = out[i] >> 16;
		const uint32_t opcode = out[i] & 0xFFFF;
		bool drop = false;
		if (opcode == SPIRV_OP_ACCESS_CHAIN && point_size_pointers.has(out[i + 2]) && !uses.has(out[i + 2])) {
			drop = true;
			removed_chains.insert(out[i + 2]);
		} else if ((opcode == SPIRV_OP_NAME && removed_chains.has(out[i + 1]))) {
			drop = true;
		}
		if (!drop) {
			for (uint32_t w = 0; w < count; w++) {
				final_out.push_back(out[i + w]);
			}
		}
		i += count;
	}
	p_module = final_out;
}

// WGSL has no standalone memory barrier; convert to a control barrier with
// Workgroup execution scope (strictly more synchronization). Device memory
// scope also narrows to Workgroup: WebGPU offers no cross-workgroup
// coherence primitive to preserve it with.
static void _spirv_convert_memory_barriers(LocalVector<uint32_t> &p_module) {
	bool any = false;
	uint32_t i = 5;
	while (i < p_module.size()) {
		if ((p_module[i] & 0xFFFF) == SPIRV_OP_MEMORY_BARRIER) {
			any = true;
			break;
		}
		i += p_module[i] >> 16;
	}
	if (!any) {
		return;
	}

	uint32_t uint_type = 0;
	uint32_t workgroup_const = 0;
	HashMap<uint32_t, uint32_t> constant_values;
	uint32_t first_function_offset = 0;
	i = 5;
	while (i < p_module.size()) {
		const uint32_t count = p_module[i] >> 16;
		const uint32_t opcode = p_module[i] & 0xFFFF;
		if (opcode == SPIRV_OP_TYPE_INT && p_module[i + 2] == 32 && uint_type == 0) {
			uint_type = p_module[i + 1]; // Any 32-bit integer type works for a scope operand.
		} else if (opcode == SPIRV_OP_CONSTANT && count == 4) {
			constant_values[p_module[i + 2]] = p_module[i + 3];
			if (p_module[i + 3] == SPIRV_SCOPE_WORKGROUP && workgroup_const == 0) {
				workgroup_const = p_module[i + 2];
			}
		} else if (opcode == SPIRV_OP_FUNCTION && first_function_offset == 0) {
			first_function_offset = i;
		}
		i += count;
	}
	ERR_FAIL_COND(first_function_offset == 0);

	uint32_t next_id = p_module[3];
	LocalVector<uint32_t> new_globals;
	if (uint_type == 0) {
		uint_type = next_id++;
		new_globals.push_back((4 << 16) | SPIRV_OP_TYPE_INT);
		new_globals.push_back(uint_type);
		new_globals.push_back(32);
		new_globals.push_back(0);
	}
	if (workgroup_const == 0) {
		workgroup_const = next_id++;
		new_globals.push_back((4 << 16) | SPIRV_OP_CONSTANT);
		new_globals.push_back(uint_type);
		new_globals.push_back(workgroup_const);
		new_globals.push_back(SPIRV_SCOPE_WORKGROUP);
	}
	// Tint only accepts the exact semantics of workgroupBarrier() and
	// storageBarrier(); canonicalize instead of passing the originals.
	const uint32_t SEM_WORKGROUP = 0x108; // AcquireRelease | WorkgroupMemory.
	const uint32_t SEM_STORAGE = 0x48; // AcquireRelease | UniformMemory.
	uint32_t sem_workgroup_const = 0;
	uint32_t sem_storage_const = 0;
	for (const KeyValue<uint32_t, uint32_t> &constant : constant_values) {
		if (constant.value == SEM_WORKGROUP && sem_workgroup_const == 0) {
			sem_workgroup_const = constant.key;
		} else if (constant.value == SEM_STORAGE && sem_storage_const == 0) {
			sem_storage_const = constant.key;
		}
	}
	if (sem_workgroup_const == 0) {
		sem_workgroup_const = next_id++;
		new_globals.push_back((4 << 16) | SPIRV_OP_CONSTANT);
		new_globals.push_back(uint_type);
		new_globals.push_back(sem_workgroup_const);
		new_globals.push_back(SEM_WORKGROUP);
	}
	if (sem_storage_const == 0) {
		sem_storage_const = next_id++;
		new_globals.push_back((4 << 16) | SPIRV_OP_CONSTANT);
		new_globals.push_back(uint_type);
		new_globals.push_back(sem_storage_const);
		new_globals.push_back(SEM_STORAGE);
	}

	LocalVector<uint32_t> out;
	out.reserve(p_module.size() + new_globals.size() + 8);
	i = 0;
	while (i < p_module.size()) {
		if (i == first_function_offset) {
			for (const uint32_t w : new_globals) {
				out.push_back(w);
			}
		}
		const uint32_t count = i < 5 ? 1 : p_module[i] >> 16;
		const uint32_t opcode = i < 5 ? 0xFFFFFFFF : p_module[i] & 0xFFFF;
		if (opcode == SPIRV_OP_MEMORY_BARRIER) {
			const uint32_t *semantics_value = constant_values.getptr(p_module[i + 2]);
			const bool workgroup_memory = semantics_value == nullptr || (*semantics_value & 0x100) != 0;
			out.push_back((4 << 16) | SPIRV_OP_CONTROL_BARRIER);
			out.push_back(workgroup_const);
			out.push_back(workgroup_const);
			out.push_back(workgroup_memory ? sem_workgroup_const : sem_storage_const);
		} else {
			for (uint32_t w = 0; w < count; w++) {
				out.push_back(p_module[i + w]);
			}
		}
		i += count;
	}
	out[3] = next_id;
	p_module = out;
}

// glslang keeps every declared resource in every stage's module; Tint hard
// errors on declarations it cannot express (arrays of textures/samplers)
// even when nothing references them. Remove unused UniformConstant
// variables, then garbage-collect the handle-related types they orphan
// (Tint also rejects the bare array TYPE).
static void _spirv_remove_unused_resource_bindings(LocalVector<uint32_t> &p_module) {
	while (true) {
		HashSet<uint32_t> used;
		uint32_t i = 5;
		while (i < p_module.size()) {
			const uint32_t count = p_module[i] >> 16;
			const uint32_t opcode = p_module[i] & 0xFFFF;
			const bool countable = opcode != SPIRV_OP_NAME && opcode != SPIRV_OP_MEMBER_NAME && opcode != SPIRV_OP_DECORATE && opcode != SPIRV_OP_MEMBER_DECORATE && opcode != SPIRV_OP_ENTRY_POINT && opcode != SPIRV_OP_EXECUTION_MODE && opcode != SPIRV_OP_SOURCE && opcode != SPIRV_OP_SOURCE_EXTENSION && opcode != SPIRV_OP_STRING && opcode != SPIRV_OP_MODULE_PROCESSED;
			if (countable) {
				// Skip each instruction's own result id (and result type,
				// which is a legitimate use of the type).
				uint32_t skip_result = 0;
				if (opcode == SPIRV_OP_VARIABLE || opcode == SPIRV_OP_TYPE_POINTER || opcode == SPIRV_OP_TYPE_ARRAY || opcode == SPIRV_OP_TYPE_RUNTIME_ARRAY || opcode == SPIRV_OP_TYPE_IMAGE || opcode == SPIRV_OP_TYPE_SAMPLER || opcode == SPIRV_OP_TYPE_SAMPLED_IMAGE) {
					skip_result = opcode == SPIRV_OP_VARIABLE ? 2 : 1;
				}
				for (uint32_t w = 1; w < count; w++) {
					if (w != skip_result) {
						used.insert(p_module[i + w]);
					}
				}
			}
			i += count;
		}

		HashSet<uint32_t> removed;
		LocalVector<uint32_t> out;
		out.reserve(p_module.size());
		for (uint32_t w = 0; w < 5; w++) {
			out.push_back(p_module[w]);
		}
		i = 5;
		while (i < p_module.size()) {
			const uint32_t count = p_module[i] >> 16;
			const uint32_t opcode = p_module[i] & 0xFFFF;
			bool drop = false;
			if (opcode == SPIRV_OP_VARIABLE && p_module[i + 3] == SPIRV_STORAGE_CLASS_UNIFORM_CONSTANT && !used.has(p_module[i + 2])) {
				drop = true;
				removed.insert(p_module[i + 2]);
			} else if ((opcode == SPIRV_OP_TYPE_POINTER || opcode == SPIRV_OP_TYPE_ARRAY || opcode == SPIRV_OP_TYPE_RUNTIME_ARRAY || opcode == SPIRV_OP_TYPE_IMAGE || opcode == SPIRV_OP_TYPE_SAMPLER || opcode == SPIRV_OP_TYPE_SAMPLED_IMAGE) && !used.has(p_module[i + 1])) {
				drop = true;
				removed.insert(p_module[i + 1]);
			}
			if (!drop) {
				for (uint32_t w = 0; w < count; w++) {
					out.push_back(p_module[i + w]);
				}
			}
			i += count;
		}
		if (removed.is_empty()) {
			return;
		}

		// Strip debug names and decorations that referenced removed ids.
		LocalVector<uint32_t> cleaned;
		cleaned.reserve(out.size());
		for (uint32_t w = 0; w < 5; w++) {
			cleaned.push_back(out[w]);
		}
		i = 5;
		while (i < out.size()) {
			const uint32_t count = out[i] >> 16;
			const uint32_t opcode = out[i] & 0xFFFF;
			const bool refers = (opcode == SPIRV_OP_NAME || opcode == SPIRV_OP_MEMBER_NAME || opcode == SPIRV_OP_DECORATE || opcode == SPIRV_OP_MEMBER_DECORATE) && removed.has(out[i + 1]);
			if (!refers) {
				for (uint32_t w = 0; w < count; w++) {
					cleaned.push_back(out[i + w]);
				}
			}
			i += count;
		}
		p_module = cleaned;
	}
}

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

	// Compatibility passes for constructs Tint's reader rejects (see each
	// pass for the construct and the chosen lowering).
	_spirv_strip_extended_image_operands(out);
	_spirv_clamp_infinite_constants(out);
	_spirv_splat_select_conditions(out);
	_spirv_rewrite_non_finite_tests(out);
	_spirv_rewrite_modf_struct(out);
	_spirv_strip_point_size_stores(out);
	_spirv_convert_memory_barriers(out);
	_spirv_remove_unused_resource_bindings(out);

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
	// Read-write storage images beyond 32-bit single-channel formats cannot
	// exist in WebGPU; failing the bake here turns those variants into
	// ordinary runtime cache misses (benign) instead of runtime shader
	// failures.
	for (const ReflectionBindingData &binding : reflection_binding_set_uniforms_data) {
		if (binding.type != RDC::UNIFORM_TYPE_IMAGE || binding.writable == 0) {
			continue;
		}
		const bool rw_ok = binding.texture_format == RDC::DATA_FORMAT_R32_SFLOAT || binding.texture_format == RDC::DATA_FORMAT_R32_UINT || binding.texture_format == RDC::DATA_FORMAT_R32_SINT;
		ERR_FAIL_COND_V_MSG(!rw_ok, false, vformat("Variant uses read-write storage on format %d, which WebGPU does not support.", binding.texture_format));
	}

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

		PackedByteArray wgsl = FileAccess::get_file_as_bytes(wgsl_path);
		DirAccess::remove_absolute(spirv_path);
		DirAccess::remove_absolute(wgsl_path);
		ERR_FAIL_COND_V_MSG(wgsl.is_empty(), false, "Tint produced no WGSL output.");

		RenderingShaderContainer::Shader &shader = shaders.ptrw()[i];
		shader.shader_stage = spirv_stages[i].shader_stage;
		// Newer WGSL reserves words Tint's pinned version still emits as
		// identifiers (e.g. struct members named 'target'); rename them.
		{
			String text;
			text.append_utf8((const char *)wgsl.ptr(), wgsl.size());
			bool changed = false;
			for (const char *reserved : { "target", "unorm", "snorm", "filter" }) {
				const String word = reserved;
				int pos = 0;
				while ((pos = text.find(word, pos)) != -1) {
					const char32_t before = pos > 0 ? text[pos - 1] : ' ';
					const char32_t after = pos + word.length() < text.length() ? text[pos + word.length()] : ' ';
					const bool ident_before = is_ascii_identifier_char(before);
					const bool ident_after = is_ascii_identifier_char(after);
					if (!ident_before && !ident_after) {
						text = text.substr(0, pos) + word + "_gd" + text.substr(pos + word.length());
						changed = true;
						pos += word.length() + 3;
					} else {
						pos += word.length();
					}
				}
			}
			if (changed) {
				CharString utf8 = text.utf8();
				wgsl.resize(utf8.length());
				memcpy(wgsl.ptrw(), utf8.get_data(), utf8.length());
			}
		}
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
