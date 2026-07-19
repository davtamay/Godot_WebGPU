/**************************************************************************/
/*  spirv_preprocess.cpp                                                  */
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

#include "spirv_preprocess.h"

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

#include <cfloat>
#include <cmath>
#include <cstring>

namespace spirv_preprocess {

// ---- SPIR-V opcode constants ----

static constexpr uint16_t OP_TYPE_BOOL = 20;
static constexpr uint16_t OP_TYPE_INT = 21;
static constexpr uint16_t OP_TYPE_FLOAT = 22;
static constexpr uint16_t OP_TYPE_IMAGE = 25;
static constexpr uint16_t OP_TYPE_SAMPLER = 26;
static constexpr uint16_t OP_TYPE_SAMPLED_IMAGE = 27;
static constexpr uint16_t OP_TYPE_POINTER = 32;
static constexpr uint16_t OP_CONSTANT_TRUE = 41;
static constexpr uint16_t OP_CONSTANT_FALSE = 42;
static constexpr uint16_t OP_CONSTANT = 43;
static constexpr uint16_t OP_CONSTANT_COMPOSITE = 44;
static constexpr uint16_t OP_SPEC_CONSTANT_TRUE = 48;
static constexpr uint16_t OP_SPEC_CONSTANT_FALSE = 49;
static constexpr uint16_t OP_SPEC_CONSTANT = 50;
static constexpr uint16_t OP_SPEC_CONSTANT_COMPOSITE = 51;
static constexpr uint16_t OP_SPEC_CONSTANT_OP = 52;
static constexpr uint16_t OP_FUNCTION = 54;
static constexpr uint16_t OP_FUNCTION_END = 56;
static constexpr uint16_t OP_VARIABLE = 59;
static constexpr uint16_t OP_LOAD = 61;
static constexpr uint16_t OP_DECORATE = 71;
static constexpr uint16_t OP_COPY_OBJECT = 83;
static constexpr uint16_t OP_COPY_LOGICAL = 400;
static constexpr uint16_t OP_TYPE_VECTOR = 23;
static constexpr uint16_t OP_STORE = 62;
static constexpr uint16_t OP_ACCESS_CHAIN = 65;
static constexpr uint16_t OP_MEMBER_DECORATE = 72;
static constexpr uint16_t OP_FNEGATE = 127;
static constexpr uint16_t OP_RETURN = 253;
static constexpr uint16_t OP_RETURN_VALUE = 254;
static constexpr uint16_t OP_ENTRY_POINT = 15;
static constexpr uint16_t OP_TYPE_ARRAY = 28;
static constexpr uint16_t OP_TYPE_RUNTIME_ARRAY = 29;
static constexpr uint16_t OP_IN_BOUNDS_ACCESS_CHAIN = 66;

// SPIR-V storage class values.
static constexpr uint32_t SC_INPUT = 1;
static constexpr uint32_t SC_OUTPUT = 3;

// SPIR-V decoration values.
static constexpr uint32_t DECO_BUILTIN = 11;
static constexpr uint32_t DECO_SPEC_ID = 1;

// SPIR-V BuiltIn values.
static constexpr uint32_t BUILTIN_POSITION = 0;
static constexpr uint32_t BUILTIN_VIEW_INDEX = 4440;

// SPIR-V execution model values.
static constexpr uint32_t EXEC_MODEL_VERTEX = 0;

// Binding slot used by the push-constant ring buffer emulation inside group 3.
// Must match the C++ constant PUSH_CONSTANT_RING_BINDING in
// rendering_device_driver_webgpu.cpp.

// ---- Inline helpers ----

// Read a little-endian u32 from byte data at the given word index.
// Returns 0 for out-of-bounds access (malformed SPIR-V safety).
static inline uint32_t read_word(const uint8_t *p_data, int64_t p_size, uint32_t p_word_idx) {
	uint32_t off = p_word_idx * 4;
	if (off + 3 >= (uint32_t)p_size) {
		return 0;
	}
	uint32_t val;
	memcpy(&val, p_data + off, 4);
	return val; // Assumes little-endian host (which Godot targets require).
}

// Append a u32 as 4 little-endian bytes to a Vector<uint8_t>.
static inline void push_word(Vector<uint8_t> &r_out, uint32_t p_word) {
	int64_t old_size = r_out.size();
	r_out.resize(old_size + 4);
	memcpy(r_out.ptrw() + old_size, &p_word, 4);
}

// Copy a range of bytes from source data to Vector<uint8_t>.
static inline void append_bytes(Vector<uint8_t> &r_out, const uint8_t *p_src, int64_t p_offset, int64_t p_count) {
	if (p_count <= 0) {
		return;
	}
	int64_t old_size = r_out.size();
	r_out.resize(old_size + p_count);
	memcpy(r_out.ptrw() + old_size, p_src + p_offset, p_count);
}

// ---- eval_spec_op ----

// Evaluate a SPIR-V specialization constant operation.
// Returns the computed value as a u64.
static uint64_t eval_spec_op(uint32_t p_opcode, const Vector<uint64_t> &p_operands) {
	auto a = [&]() -> uint64_t { return p_operands.size() > 0 ? p_operands[0] : 0; };
	auto b = [&]() -> uint64_t { return p_operands.size() > 1 ? p_operands[1] : 0; };
	auto c = [&]() -> uint64_t { return p_operands.size() > 2 ? p_operands[2] : 0; };

	switch (p_opcode) {
		// Integer arithmetic.
		case 126: return (uint64_t)(-(int32_t)a()); // SNegate
		case 128: return a() + b(); // IAdd (wrapping)
		case 130: return a() - b(); // ISub (wrapping)
		case 132: return a() * b(); // IMul (wrapping)
		case 134: return b() != 0 ? a() / b() : 0; // UDiv
		case 135: { // SDiv
			int32_t d = (int32_t)b();
			return d != 0 ? (uint64_t)((int32_t)a() / d) : 0;
		}
		case 137: return b() != 0 ? a() % b() : 0; // UMod

		// Logical.
		case 164: return (uint64_t)(a() == b()); // LogicalEqual
		case 165: return (uint64_t)(a() != b()); // LogicalNotEqual
		case 166: return (uint64_t)((a() != 0) || (b() != 0)); // LogicalOr
		case 167: return (uint64_t)((a() != 0) && (b() != 0)); // LogicalAnd
		case 168: return (uint64_t)(a() == 0); // LogicalNot

		// Select: condition, true_val, false_val.
		case 169: return a() != 0 ? b() : c(); // Select

		// Integer comparison.
		case 170: return (uint64_t)(a() == b()); // IEqual
		case 171: return (uint64_t)(a() != b()); // INotEqual
		case 172: return (uint64_t)((uint32_t)a() > (uint32_t)b()); // UGreaterThan
		case 173: return (uint64_t)((int32_t)a() > (int32_t)b()); // SGreaterThan
		case 174: return (uint64_t)((uint32_t)a() >= (uint32_t)b()); // UGreaterThanEqual
		case 175: return (uint64_t)((int32_t)a() >= (int32_t)b()); // SGreaterThanEqual
		case 176: return (uint64_t)((uint32_t)a() < (uint32_t)b()); // ULessThan
		case 177: return (uint64_t)((int32_t)a() < (int32_t)b()); // SLessThan
		case 178: return (uint64_t)((uint32_t)a() <= (uint32_t)b()); // ULessThanEqual
		case 179: return (uint64_t)((int32_t)a() <= (int32_t)b()); // SLessThanEqual

		// Bitwise.
		case 194: return (uint64_t)((uint32_t)a() >> ((uint32_t)b() & 31)); // ShiftRightLogical
		case 195: return (uint64_t)((int32_t)a() >> ((uint32_t)b() & 31)); // ShiftRightArithmetic
		case 196: return (uint64_t)((uint32_t)a() << ((uint32_t)b() & 31)); // ShiftLeftLogical
		case 197: return a() | b(); // BitwiseOr
		case 198: return a() ^ b(); // BitwiseXor
		case 199: return a() & b(); // BitwiseAnd
		case 200: return (uint64_t)(uint32_t)(~(uint32_t)a()); // Not

		// Composite.
		case 81: return a(); // CompositeExtract (return first operand)

		// Conversion (values unchanged for const-eval of integers).
		case 109:
		case 110:
		case 111:
		case 112:
		case 113:
		case 114: return a(); // ConvertF/S/U, UConvert, SConvert

		// Default: return 0 for unhandled operations.
		default: return 0;
	}
}

// ---- freeze_spec_constant_ops ----

Vector<uint8_t> freeze_spec_constant_ops(const Vector<uint8_t> &p_bytes) {
	const uint8_t *data = p_bytes.ptr();
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);

	if (total_words < 5) {
		return p_bytes;
	}

	// Collect type info: type_id -> true if bool type.
	HashMap<uint32_t, bool> type_bool;
	// Collect type info for int types: type_id -> (width, signed).
	HashMap<uint32_t, uint32_t> type_int_width;

	// Collect constant scalar values: result_id -> value (as u64).
	HashMap<uint32_t, uint64_t> constants;
	// Track which IDs are bool-typed.
	HashSet<uint32_t> bool_ids;

	// First pass: collect types and constant values.
	uint32_t pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);

		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		switch (op) {
			case OP_TYPE_BOOL: {
				if (wc >= 2) {
					uint32_t id = read_word(data, len, pos + 1);
					type_bool.insert(id, true);
				}
			} break;

			case OP_TYPE_INT: {
				if (wc >= 4) {
					uint32_t id = read_word(data, len, pos + 1);
					uint32_t width = read_word(data, len, pos + 2);
					type_int_width.insert(id, width);
				}
			} break;

			case OP_CONSTANT:
			case OP_SPEC_CONSTANT: {
				if (wc >= 4) {
					uint32_t type_id = read_word(data, len, pos + 1);
					uint32_t result_id = read_word(data, len, pos + 2);
					uint64_t val = (uint64_t)read_word(data, len, pos + 3);
					// For 64-bit constants, also grab the high word.
					if (wc >= 5) {
						val |= ((uint64_t)read_word(data, len, pos + 4) << 32);
					}
					constants.insert(result_id, val);
					if (type_bool.has(type_id)) {
						bool_ids.insert(result_id);
					}
				}
			} break;

			case OP_CONSTANT_TRUE:
			case OP_SPEC_CONSTANT_TRUE: {
				if (wc >= 3) {
					uint32_t result_id = read_word(data, len, pos + 2);
					constants.insert(result_id, 1);
					bool_ids.insert(result_id);
				}
			} break;

			case OP_CONSTANT_FALSE:
			case OP_SPEC_CONSTANT_FALSE: {
				if (wc >= 3) {
					uint32_t result_id = read_word(data, len, pos + 2);
					constants.insert(result_id, 0);
					bool_ids.insert(result_id);
				}
			} break;

			case OP_SPEC_CONSTANT_OP: {
				if (wc >= 4) {
					uint32_t type_id = read_word(data, len, pos + 1);
					uint32_t result_id = read_word(data, len, pos + 2);
					uint32_t spec_op = read_word(data, len, pos + 3);

					Vector<uint64_t> operands;
					for (uint32_t i = 4; i < wc; i++) {
						uint32_t id = read_word(data, len, pos + i);
						const uint64_t *val_ptr = constants.getptr(id);
						operands.push_back(val_ptr ? *val_ptr : 0);
					}

					uint64_t val = eval_spec_op(spec_op, operands);
					constants.insert(result_id, val);
					if (type_bool.has(type_id)) {
						bool_ids.insert(result_id);
					}
				}
			} break;

			default:
				break;
		}

		pos += wc;
	}

	// Second pass: rewrite, replacing OpSpecConstantOp with OpConstant,
	// and converting OpSpecConstant* to their non-specialization equivalents.
	Vector<uint8_t> out;
	out.resize(0);

	// Copy header (5 words = 20 bytes).
	append_bytes(out, data, 0, 20);

	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);

		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		if (op == OP_DECORATE && wc >= 3 && read_word(data, len, pos + 2) == DECO_SPEC_ID) {
			// Strip OpDecorate ... SpecId decorations -- not valid after freezing.
			pos += wc;
			continue;
		} else if (op == OP_SPEC_CONSTANT_OP) {
			// Replace OpSpecConstantOp with evaluated OpConstant.
			uint32_t type_id = read_word(data, len, pos + 1);
			uint32_t result_id = read_word(data, len, pos + 2);
			const uint64_t *val_ptr = constants.getptr(result_id);
			uint64_t val = val_ptr ? *val_ptr : 0;

			if (bool_ids.has(result_id) || type_bool.has(type_id)) {
				uint16_t bool_op = (val != 0) ? OP_CONSTANT_TRUE : OP_CONSTANT_FALSE;
				push_word(out, (3u << 16) | (uint32_t)bool_op);
				push_word(out, type_id);
				push_word(out, result_id);
			} else {
				push_word(out, (4u << 16) | (uint32_t)OP_CONSTANT);
				push_word(out, type_id);
				push_word(out, result_id);
				push_word(out, (uint32_t)val);
			}
		} else if (op == OP_SPEC_CONSTANT_TRUE) {
			// Rewrite as OpConstantTrue (same layout, different opcode).
			push_word(out, ((uint32_t)wc << 16) | (uint32_t)OP_CONSTANT_TRUE);
			for (uint32_t i = 1; i < wc; i++) {
				push_word(out, read_word(data, len, pos + i));
			}
		} else if (op == OP_SPEC_CONSTANT_FALSE) {
			push_word(out, ((uint32_t)wc << 16) | (uint32_t)OP_CONSTANT_FALSE);
			for (uint32_t i = 1; i < wc; i++) {
				push_word(out, read_word(data, len, pos + i));
			}
		} else if (op == OP_SPEC_CONSTANT) {
			push_word(out, ((uint32_t)wc << 16) | (uint32_t)OP_CONSTANT);
			for (uint32_t i = 1; i < wc; i++) {
				push_word(out, read_word(data, len, pos + i));
			}
		} else if (op == OP_SPEC_CONSTANT_COMPOSITE) {
			push_word(out, ((uint32_t)wc << 16) | (uint32_t)OP_CONSTANT_COMPOSITE);
			for (uint32_t i = 1; i < wc; i++) {
				push_word(out, read_word(data, len, pos + i));
			}
		} else {
			// Copy instruction as-is.
			append_bytes(out, data, pos * 4, wc * 4);
		}

		pos += wc;
	}

	return out;
}

// ---- negate_position_y ----

Vector<uint8_t> negate_position_y(const Vector<uint8_t> &p_bytes) {
	const uint8_t *data = p_bytes.ptr();
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);

	if (total_words < 5) {
		return p_bytes;
	}

	// --- Pass 1: Collect type info, find Position variable, find vertex entry point ---

	uint32_t bound = read_word(data, len, 3);

	// Find BuiltIn Position decoration.
	// Case A: OpDecorate %var BuiltIn Position → direct variable
	// Case B: OpMemberDecorate %struct member BuiltIn Position → struct member
	uint32_t position_var_id = 0; // Direct position variable (case A).
	uint32_t position_struct_type = 0; // Struct type with Position member (case B).
	uint32_t position_member_idx = 0; // Member index within struct.
	bool position_is_member = false;

	// Find vertex entry point function ID.
	uint32_t vertex_func_id = 0;

	// Type info.
	uint32_t float32_type_id = 0;
	HashSet<uint32_t> int_type_ids; // All integer type IDs (for filtering constants).
	HashMap<uint32_t, uint32_t> ptr_type_base; // ptr_type_id → base_type_id (for Output pointers).
	HashMap<uint32_t, uint32_t> var_ptr_type; // var_id → ptr_type_id (for Output variables).

	// Integer constants: value → result_id.
	HashMap<uint32_t, uint32_t> int_constants;

	uint32_t pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		switch (op) {
			case OP_ENTRY_POINT: {
				if (wc >= 3) {
					uint32_t exec_model = read_word(data, len, pos + 1);
					if (exec_model == EXEC_MODEL_VERTEX) {
						vertex_func_id = read_word(data, len, pos + 2);
					}
				}
			} break;

			case OP_DECORATE: {
				if (wc >= 4) {
					uint32_t target = read_word(data, len, pos + 1);
					uint32_t deco = read_word(data, len, pos + 2);
					uint32_t value = read_word(data, len, pos + 3);
					if (deco == DECO_BUILTIN && value == BUILTIN_POSITION) {
						position_var_id = target;
						position_is_member = false;
					}
				}
			} break;

			case OP_MEMBER_DECORATE: {
				if (wc >= 4) {
					uint32_t struct_type = read_word(data, len, pos + 1);
					uint32_t member = read_word(data, len, pos + 2);
					uint32_t deco = read_word(data, len, pos + 3);
					if (deco == DECO_BUILTIN && wc >= 5) {
						uint32_t value = read_word(data, len, pos + 4);
						if (value == BUILTIN_POSITION) {
							position_struct_type = struct_type;
							position_member_idx = member;
							position_is_member = true;
						}
					}
				}
			} break;

			case OP_TYPE_INT: {
				if (wc >= 3) {
					int_type_ids.insert(read_word(data, len, pos + 1));
				}
			} break;

			case OP_TYPE_FLOAT: {
				if (wc >= 3) {
					uint32_t width = read_word(data, len, pos + 2);
					if (width == 32) {
						float32_type_id = read_word(data, len, pos + 1);
					}
				}
			} break;

			case OP_TYPE_VECTOR: {
				// Tracked for potential future use (vec4<f32> identification).
			} break;

			case OP_TYPE_POINTER: {
				if (wc >= 4) {
					uint32_t id = read_word(data, len, pos + 1);
					uint32_t sc = read_word(data, len, pos + 2);
					uint32_t base = read_word(data, len, pos + 3);
					if (sc == SC_OUTPUT) {
						ptr_type_base.insert(id, base);
					}
				}
			} break;

			case OP_VARIABLE: {
				if (wc >= 4) {
					uint32_t type_id = read_word(data, len, pos + 1);
					uint32_t id = read_word(data, len, pos + 2);
					uint32_t sc = read_word(data, len, pos + 3);
					if (sc == SC_OUTPUT) {
						var_ptr_type.insert(id, type_id);
					}
				}
			} break;

			case OP_CONSTANT: {
				if (wc >= 4) {
					uint32_t type_id = read_word(data, len, pos + 1);
					uint32_t id = read_word(data, len, pos + 2);
					uint32_t value = read_word(data, len, pos + 3);
					// Only track small integer constants (for member/component indices).
					// Must verify the constant's type is actually an integer type,
					// not a float (e.g. 0.0f has bit pattern 0x00000000 which == 0).
					if (value <= 4 && int_type_ids.has(type_id)) {
						int_constants.insert(value, id);
					}
				}
			} break;

			default:
				break;
		}

		pos += wc;
	}

	// Bail if no vertex entry point or no Position found.
	if (vertex_func_id == 0) {
		return p_bytes;
	}
	if (!position_is_member && position_var_id == 0) {
		return p_bytes;
	}
	if (position_is_member && position_struct_type == 0) {
		return p_bytes;
	}

	// For case B (struct member): find the Output variable whose pointer type
	// points to the struct containing Position.
	uint32_t output_var_id = 0;
	if (position_is_member) {
		for (const KeyValue<uint32_t, uint32_t> &kv : var_ptr_type) {
			uint32_t var_id = kv.key;
			uint32_t ptr_id = kv.value;
			const uint32_t *base = ptr_type_base.getptr(ptr_id);
			if (base && *base == position_struct_type) {
				output_var_id = var_id;
				break;
			}
		}
		if (output_var_id == 0) {
			return p_bytes;
		}
	} else {
		output_var_id = position_var_id;
	}

	if (float32_type_id == 0) {
		return p_bytes;
	}

	// --- Allocate new IDs ---
	uint32_t next_id = bound;
	auto alloc_id = [&]() -> uint32_t { return next_id++; };

	// We need:
	// - A pointer type: OpTypePointer Output float32
	// - Integer constants for member index and component index 1 (Y)
	uint32_t ptr_output_float_id = alloc_id();
	uint32_t const_member_idx_id = 0;
	uint32_t const_1_id = 0;

	// Check if we already have the integer constants we need.
	if (position_is_member) {
		const uint32_t *existing = int_constants.getptr(position_member_idx);
		if (existing) {
			const_member_idx_id = *existing;
		} else {
			const_member_idx_id = alloc_id();
		}
	}
	{
		const uint32_t *existing = int_constants.getptr(1);
		if (existing) {
			const_1_id = *existing;
		} else {
			const_1_id = alloc_id();
		}
	}

	// IDs for the Y-negate sequence (per OpReturn).
	// We'll allocate fresh IDs during the rewrite pass.

	// Find an integer type for constants (32-bit signed or unsigned).
	uint32_t int32_type_id = 0;
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_TYPE_INT && wc >= 4) {
			uint32_t width = read_word(data, len, pos + 2);
			if (width == 32) {
				int32_type_id = read_word(data, len, pos + 1);
				break;
			}
		}
		pos += wc;
	}

	if (int32_type_id == 0) {
		// No 32-bit int type? Can't create constants. Bail.
		return p_bytes;
	}

	// --- Pass 2: Count OpReturn/OpReturnValue in vertex function to pre-allocate IDs ---
	uint32_t return_count = 0;
	bool in_vertex_func = false;
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_FUNCTION && wc >= 3) {
			uint32_t func_id = read_word(data, len, pos + 2);
			in_vertex_func = (func_id == vertex_func_id);
		}
		if (op == OP_FUNCTION_END) {
			in_vertex_func = false;
		}
		if (in_vertex_func && (op == OP_RETURN || op == OP_RETURN_VALUE)) {
			return_count++;
		}
		pos += wc;
	}

	if (return_count == 0) {
		return p_bytes;
	}

	// Per return site, we need 3 new IDs: access_chain_result, load_result, fnegate_result.
	Vector<uint32_t> ac_ids;
	Vector<uint32_t> load_ids;
	Vector<uint32_t> neg_ids;
	for (uint32_t i = 0; i < return_count; i++) {
		ac_ids.push_back(alloc_id());
		load_ids.push_back(alloc_id());
		neg_ids.push_back(alloc_id());
	}

	// --- Pass 3: Rewrite ---
	Vector<uint8_t> out;

	// Copy header but update bound.
	append_bytes(out, data, 0, 12);
	push_word(out, next_id); // Updated bound.
	push_word(out, read_word(data, len, 4)); // Schema.

	bool types_injected = false;
	in_vertex_func = false;
	uint32_t return_idx = 0;

	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		// Inject new types/constants before the first OpFunction.
		if (op == OP_FUNCTION && !types_injected) {
			types_injected = true;

			// OpTypePointer %ptr_output_float Output %float32.
			push_word(out, (4u << 16) | (uint32_t)OP_TYPE_POINTER);
			push_word(out, ptr_output_float_id);
			push_word(out, SC_OUTPUT);
			push_word(out, float32_type_id);

			// Integer constant for member index (if not already existing).
			if (position_is_member && !int_constants.has(position_member_idx)) {
				push_word(out, (4u << 16) | (uint32_t)OP_CONSTANT);
				push_word(out, int32_type_id);
				push_word(out, const_member_idx_id);
				push_word(out, position_member_idx);
			}

			// Integer constant for 1 (Y component index).
			if (!int_constants.has(1)) {
				push_word(out, (4u << 16) | (uint32_t)OP_CONSTANT);
				push_word(out, int32_type_id);
				push_word(out, const_1_id);
				push_word(out, 1);
			}
		}

		if (op == OP_FUNCTION && wc >= 3) {
			uint32_t func_id = read_word(data, len, pos + 2);
			in_vertex_func = (func_id == vertex_func_id);
		}
		if (op == OP_FUNCTION_END) {
			in_vertex_func = false;
		}

		// Before OpReturn in vertex function, insert Y-negate sequence.
		if (in_vertex_func && (op == OP_RETURN || op == OP_RETURN_VALUE) && return_idx < return_count) {
			uint32_t ac_id = ac_ids[return_idx];
			uint32_t ld_id = load_ids[return_idx];
			uint32_t ng_id = neg_ids[return_idx];
			return_idx++;

			if (position_is_member) {
				// OpAccessChain %ptr_output_float %output_var %member_idx %const_1
				push_word(out, (6u << 16) | (uint32_t)OP_ACCESS_CHAIN);
				push_word(out, ptr_output_float_id);
				push_word(out, ac_id);
				push_word(out, output_var_id);
				push_word(out, const_member_idx_id);
				push_word(out, const_1_id);
			} else {
				// Direct vec4 position variable:
				// OpAccessChain %ptr_output_float %output_var %const_1
				push_word(out, (5u << 16) | (uint32_t)OP_ACCESS_CHAIN);
				push_word(out, ptr_output_float_id);
				push_word(out, ac_id);
				push_word(out, output_var_id);
				push_word(out, const_1_id);
			}

			// OpLoad %float32 %ld_id %ac_id
			push_word(out, (4u << 16) | (uint32_t)OP_LOAD);
			push_word(out, float32_type_id);
			push_word(out, ld_id);
			push_word(out, ac_id);

			// OpFNegate %float32 %ng_id %ld_id
			push_word(out, (4u << 16) | (uint32_t)OP_FNEGATE);
			push_word(out, float32_type_id);
			push_word(out, ng_id);
			push_word(out, ld_id);

			// OpStore %ac_id %ng_id
			push_word(out, (3u << 16) | (uint32_t)OP_STORE);
			push_word(out, ac_id);
			push_word(out, ng_id);
		}

		// Copy original instruction.
		append_bytes(out, data, pos * 4, wc * 4);
		pos += wc;
	}

	return out;
}

// ---- lower_view_index_to_zero ----

Vector<uint8_t> lower_view_index_to_zero(const Vector<uint8_t> &p_bytes) {
	const uint8_t *data = p_bytes.ptr();
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);

	if (total_words < 5) {
		return p_bytes;
	}

	HashSet<uint32_t> view_index_vars;
	HashSet<uint64_t> view_index_members;
	HashMap<uint32_t, uint32_t> ptr_base_type;
	HashMap<uint32_t, uint32_t> var_ptr_type;
	HashMap<uint32_t, uint32_t> constant_values;
	HashMap<uint32_t, uint32_t> zero_constants_by_type;
	uint32_t int32_type = 0;

	uint32_t pos = 5;
	while (pos < total_words) {
		const uint32_t w0 = read_word(data, len, pos);
		const uint32_t wc = w0 >> 16;
		const uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		switch (op) {
			case OP_TYPE_INT: {
				if (wc >= 4 && read_word(data, len, pos + 2) == 32 && int32_type == 0) {
					int32_type = read_word(data, len, pos + 1);
				}
			} break;

			case OP_TYPE_POINTER: {
				if (wc >= 4) {
					ptr_base_type.insert(read_word(data, len, pos + 1), read_word(data, len, pos + 3));
				}
			} break;

			case OP_VARIABLE: {
				if (wc >= 4 && read_word(data, len, pos + 3) == SC_INPUT) {
					var_ptr_type.insert(read_word(data, len, pos + 2), read_word(data, len, pos + 1));
				}
			} break;

			case OP_CONSTANT: {
				if (wc >= 4) {
					const uint32_t type_id = read_word(data, len, pos + 1);
					const uint32_t result_id = read_word(data, len, pos + 2);
					const uint32_t value = read_word(data, len, pos + 3);
					constant_values.insert(result_id, value);
					if (value == 0) {
						zero_constants_by_type.insert(type_id, result_id);
					}
				}
			} break;

			case OP_DECORATE: {
				if (wc >= 4 && read_word(data, len, pos + 2) == DECO_BUILTIN && read_word(data, len, pos + 3) == BUILTIN_VIEW_INDEX) {
					view_index_vars.insert(read_word(data, len, pos + 1));
				}
			} break;

			case OP_MEMBER_DECORATE: {
				if (wc >= 5 && read_word(data, len, pos + 3) == DECO_BUILTIN && read_word(data, len, pos + 4) == BUILTIN_VIEW_INDEX) {
					const uint32_t struct_type = read_word(data, len, pos + 1);
					const uint32_t member = read_word(data, len, pos + 2);
					view_index_members.insert(((uint64_t)struct_type << 32) | member);
				}
			} break;

			default:
				break;
		}

		pos += wc;
	}

	if (view_index_vars.is_empty() && view_index_members.is_empty()) {
		return p_bytes;
	}
	if (int32_type == 0) {
		return p_bytes;
	}

	const uint32_t old_bound = read_word(data, len, 3);

	HashSet<uint32_t> view_index_access_chains;
	pos = 5;
	while (pos < total_words) {
		const uint32_t w0 = read_word(data, len, pos);
		const uint32_t wc = w0 >> 16;
		const uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && wc >= 5) {
			const uint32_t result_id = read_word(data, len, pos + 2);
			const uint32_t base_id = read_word(data, len, pos + 3);
			const uint32_t index_id = read_word(data, len, pos + 4);
			const uint32_t *member = constant_values.getptr(index_id);
			const uint32_t *ptr_type = var_ptr_type.getptr(base_id);
			const uint32_t *base_type = ptr_type ? ptr_base_type.getptr(*ptr_type) : nullptr;
			if (member && base_type && view_index_members.has(((uint64_t)*base_type << 32) | *member)) {
				view_index_access_chains.insert(result_id);
			}
		}

		pos += wc;
	}

	HashMap<uint32_t, uint32_t> replacement_zero_by_type;
	uint32_t next_id = old_bound;
	pos = 5;
	while (pos < total_words) {
		const uint32_t w0 = read_word(data, len, pos);
		const uint32_t wc = w0 >> 16;
		const uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		if (op == OP_LOAD && wc >= 4) {
			const uint32_t result_type = read_word(data, len, pos + 1);
			const uint32_t pointer_id = read_word(data, len, pos + 3);
			if ((view_index_vars.has(pointer_id) || view_index_access_chains.has(pointer_id)) && !replacement_zero_by_type.has(result_type)) {
				const uint32_t *existing_zero = zero_constants_by_type.getptr(result_type);
				replacement_zero_by_type.insert(result_type, existing_zero ? *existing_zero : next_id++);
			}
		}

		pos += wc;
	}

	const uint32_t new_bound = next_id;

	Vector<uint8_t> out;
	append_bytes(out, data, 0, 12);
	push_word(out, new_bound);
	push_word(out, read_word(data, len, 4));

	bool zero_constants_inserted = replacement_zero_by_type.is_empty();
	pos = 5;
	while (pos < total_words) {
		const uint32_t w0 = read_word(data, len, pos);
		const uint32_t wc = w0 >> 16;
		const uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		if (!zero_constants_inserted && op == OP_FUNCTION) {
			for (const KeyValue<uint32_t, uint32_t> &kv : replacement_zero_by_type) {
				if (kv.value >= old_bound) {
					push_word(out, (4u << 16) | (uint32_t)OP_CONSTANT);
					push_word(out, kv.key);
					push_word(out, kv.value);
					push_word(out, 0);
				}
			}
			zero_constants_inserted = true;
		}

		if (op == OP_DECORATE && wc >= 4 && read_word(data, len, pos + 2) == DECO_BUILTIN && read_word(data, len, pos + 3) == BUILTIN_VIEW_INDEX) {
			pos += wc;
			continue;
		}
		if (op == OP_MEMBER_DECORATE && wc >= 5 && read_word(data, len, pos + 3) == DECO_BUILTIN && read_word(data, len, pos + 4) == BUILTIN_VIEW_INDEX) {
			pos += wc;
			continue;
		}
		if (op == OP_ENTRY_POINT && wc >= 4) {
			uint32_t name_end = 3;
			while (name_end < wc) {
				const uint32_t word = read_word(data, len, pos + name_end++);
				if (((word >> 0) & 0xFF) == 0 || ((word >> 8) & 0xFF) == 0 || ((word >> 16) & 0xFF) == 0 || ((word >> 24) & 0xFF) == 0) {
					break;
				}
			}
			Vector<uint32_t> kept;
			for (uint32_t i = 0; i < name_end; i++) {
				kept.push_back(read_word(data, len, pos + i));
			}
			for (uint32_t i = name_end; i < wc; i++) {
				const uint32_t id = read_word(data, len, pos + i);
				if (!view_index_vars.has(id)) {
					kept.push_back(id);
				}
			}
			kept.write[0] = ((uint32_t)kept.size() << 16) | (uint32_t)OP_ENTRY_POINT;
			for (int i = 0; i < kept.size(); i++) {
				push_word(out, kept[i]);
			}
			pos += wc;
			continue;
		}
		if (op == OP_LOAD && wc >= 4) {
			const uint32_t result_type = read_word(data, len, pos + 1);
			const uint32_t pointer_id = read_word(data, len, pos + 3);
			if (view_index_vars.has(pointer_id) || view_index_access_chains.has(pointer_id)) {
				const uint32_t *zero_const = replacement_zero_by_type.getptr(result_type);
				push_word(out, (4u << 16) | (uint32_t)OP_COPY_OBJECT);
				push_word(out, result_type);
				push_word(out, read_word(data, len, pos + 2));
				push_word(out, zero_const ? *zero_const : 0);
				pos += wc;
				continue;
			}
		}

		append_bytes(out, data, pos * 4, wc * 4);
		pos += wc;
	}

	return out;
}

// ---- flatten_binding_arrays ----
//
// Tint rejects OpTypeArray/OpTypeRuntimeArray of handle types (image, sampler,
// sampled_image) regardless of array size. We must fully eliminate these array
// types from SPIR-V and replace them with scalar element types.
//
// Strategy: build a map of array_type_id → element_type_id, then do a
// comprehensive ID replacement across all instructions. Every operand word
// that matches an array type ID gets replaced with the element type ID.
// Then the OpTypeArray instruction itself is stripped (the ID is no longer
// referenced anywhere). OpAccessChain into handle array variables becomes
// a direct variable reference.
//
// Literal values in OpConstant/OpSpecConstant/OpSwitch are excluded from
// replacement to avoid false positives.

Vector<uint8_t> flatten_binding_arrays(const Vector<uint8_t> &p_bytes) {
	const uint8_t *data = p_bytes.ptr();
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);

	if (total_words < 5) {
		return p_bytes;
	}

	// Pass 1: Collect handle types and find arrays of handles.
	HashSet<uint32_t> handle_types;
	HashMap<uint32_t, uint32_t> array_to_elem; // array_type_id → element_type_id

	uint32_t pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		switch (op) {
			case OP_TYPE_IMAGE:
			case OP_TYPE_SAMPLER:
			case OP_TYPE_SAMPLED_IMAGE: {
				if (wc >= 2) {
					handle_types.insert(read_word(data, len, pos + 1));
				}
			} break;

			case OP_TYPE_ARRAY: {
				if (wc >= 4) {
					uint32_t result_id = read_word(data, len, pos + 1);
					uint32_t elem_type = read_word(data, len, pos + 2);
					if (handle_types.has(elem_type)) {
						array_to_elem.insert(result_id, elem_type);
					}
				}
			} break;

			case OP_TYPE_RUNTIME_ARRAY: {
				if (wc >= 3) {
					uint32_t result_id = read_word(data, len, pos + 1);
					uint32_t elem_type = read_word(data, len, pos + 2);
					if (handle_types.has(elem_type)) {
						array_to_elem.insert(result_id, elem_type);
					}
				}
			} break;

			default:
				break;
		}
		pos += wc;
	}

	if (array_to_elem.is_empty()) {
		return p_bytes;
	}

	// Pass 2: Identify pointer types for handle arrays and handle array variables,
	// needed to detect OpAccessChain targets.
	// Also collect ALL OpTypePointer for deduplication after array→elem replacement.
	HashSet<uint32_t> handle_array_ptr_types;
	HashSet<uint32_t> handle_array_vars;
	HashMap<uint32_t, uint32_t> ac_to_var; // access_chain_result → variable_id

	// For pointer type dedup: collect (storage_class, base_type) → first_ptr_id.
	// After array→elem replacement, two OpTypePointer can become identical
	// (same sc + same base type but different IDs). SPIR-V validates types by ID,
	// so we must remap duplicates to a single canonical ID.
	struct PtrTypeInfo {
		uint32_t ptr_id;
		uint32_t storage_class;
		uint32_t base_type;
	};
	Vector<PtrTypeInfo> all_ptr_types;

	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_TYPE_POINTER && wc >= 4) {
			uint32_t ptr_id = read_word(data, len, pos + 1);
			uint32_t sc = read_word(data, len, pos + 2);
			uint32_t base_type = read_word(data, len, pos + 3);
			all_ptr_types.push_back({ ptr_id, sc, base_type });
			if (array_to_elem.has(base_type)) {
				handle_array_ptr_types.insert(ptr_id);
			}
		}
		if (op == OP_VARIABLE && wc >= 4) {
			uint32_t type_id = read_word(data, len, pos + 1);
			if (handle_array_ptr_types.has(type_id)) {
				handle_array_vars.insert(read_word(data, len, pos + 2));
			}
		}
		pos += wc;
	}

	// Pointer type deduplication: after array_to_elem replacement, some
	// OpTypePointer instructions will point to the same (sc, base_type).
	// Find these duplicates and remap them to a single canonical ID.
	HashMap<uint32_t, uint32_t> ptr_remap; // duplicate_ptr_id → canonical_ptr_id
	HashSet<uint32_t> strip_ptr_ids; // duplicate OpTypePointer to remove
	{
		// Build effective (sc, base_type) for each pointer type after replacement.
		// Key: (sc << 32 | effective_base_type) → first ptr_id seen (canonical).
		HashMap<uint64_t, uint32_t> sc_base_to_canonical;
		for (int i = 0; i < all_ptr_types.size(); i++) {
			const PtrTypeInfo &info = all_ptr_types[i];
			// Compute effective base type after array→elem replacement.
			uint32_t effective_base = info.base_type;
			const uint32_t *elem = array_to_elem.getptr(effective_base);
			if (elem) {
				effective_base = *elem;
			}
			uint64_t key = ((uint64_t)info.storage_class << 32) | effective_base;
			uint32_t *canonical = sc_base_to_canonical.getptr(key);
			if (canonical) {
				// This pointer type is a duplicate. Remap to canonical.
				ptr_remap.insert(info.ptr_id, *canonical);
				strip_ptr_ids.insert(info.ptr_id);
			} else {
				sc_base_to_canonical.insert(key, info.ptr_id);
			}
		}
	}

	// Pass 3: Find OpAccessChain/OpInBoundsAccessChain on handle array vars.
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && wc >= 5) {
			uint32_t result_id = read_word(data, len, pos + 2);
			uint32_t base_id = read_word(data, len, pos + 3);
			if (handle_array_vars.has(base_id)) {
				ac_to_var.insert(result_id, base_id);
			}
		}
		pos += wc;
	}

	// Pass 4: Rewrite the SPIR-V.
	// - Strip OpTypeArray/OpTypeRuntimeArray for handle types.
	// - Strip duplicate OpTypePointer (from pointer dedup).
	// - Strip OpAccessChain for handle array vars.
	// - In all other instructions, replace array type IDs with element type IDs,
	//   access chain results with the underlying variable ID,
	//   and duplicate pointer type IDs with canonical IDs.
	Vector<uint8_t> out;
	append_bytes(out, data, 0, 20); // Copy header.

	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		// Strip OpTypeArray/OpTypeRuntimeArray for handle types.
		if ((op == OP_TYPE_ARRAY || op == OP_TYPE_RUNTIME_ARRAY) && wc >= 3) {
			uint32_t type_id = read_word(data, len, pos + 1);
			if (array_to_elem.has(type_id)) {
				pos += wc;
				continue;
			}
		}

		// Strip duplicate OpTypePointer instructions (pointer dedup).
		if (op == OP_TYPE_POINTER && wc >= 4) {
			uint32_t ptr_id = read_word(data, len, pos + 1);
			if (strip_ptr_ids.has(ptr_id)) {
				pos += wc;
				continue;
			}
		}

		// Strip OpAccessChain/OpInBoundsAccessChain for handle array vars.
		if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && wc >= 5) {
			uint32_t result_id = read_word(data, len, pos + 2);
			if (ac_to_var.has(result_id)) {
				pos += wc;
				continue;
			}
		}

		// Determine which word positions hold literal values (not IDs)
		// and should be excluded from replacement.
		// OpConstant/OpSpecConstant: words 3+ are literal values.
		// OpConstantComposite/OpSpecConstantComposite: words 3+ are constituent IDs (DO replace).
		// OpSwitch: alternating case literals starting at word 3 (word 3=literal, 4=label, 5=literal...).
		bool has_literals = false;
		uint32_t literal_start = 0;
		bool switch_alternating = false;

		if (op == OP_CONSTANT || op == OP_SPEC_CONSTANT) {
			has_literals = true;
			literal_start = 3;
		} else if (op == 251 /* OpSwitch */) {
			switch_alternating = true;
		}

		// Check if this instruction has any word that needs replacement.
		bool needs_rewrite = false;
		for (uint32_t i = 1; i < wc; i++) {
			if (has_literals && i >= literal_start) {
				continue;
			}
			if (switch_alternating && i >= 2 && ((i - 2) % 2 == 0)) {
				continue; // Case literal positions in OpSwitch.
			}
			uint32_t word = read_word(data, len, pos + i);
			if (array_to_elem.has(word) || ac_to_var.has(word) || ptr_remap.has(word)) {
				needs_rewrite = true;
				break;
			}
		}

		if (needs_rewrite) {
			for (uint32_t i = 0; i < wc; i++) {
				uint32_t word = read_word(data, len, pos + i);
				if (i == 0) {
					push_word(out, word);
					continue;
				}
				if (has_literals && i >= literal_start) {
					push_word(out, word);
					continue;
				}
				if (switch_alternating && i >= 2 && ((i - 2) % 2 == 0)) {
					push_word(out, word);
					continue;
				}
				// Replace array type ID → element type ID.
				const uint32_t *elem = array_to_elem.getptr(word);
				if (elem) {
					push_word(out, *elem);
					continue;
				}
				// Replace access chain result → variable ID.
				const uint32_t *var = ac_to_var.getptr(word);
				if (var) {
					push_word(out, *var);
					continue;
				}
				// Replace duplicate pointer type ID → canonical ID.
				const uint32_t *canonical = ptr_remap.getptr(word);
				if (canonical) {
					push_word(out, *canonical);
					continue;
				}
				push_word(out, word);
			}
		} else {
			append_bytes(out, data, pos * 4, wc * 4);
		}

		pos += wc;
	}

	return out;
}

// ---- infer_readonly_storage ----


// ---- lower_spec_constant_ops_to_runtime ----
//
// Tint accepts plain OpSpecConstant* (they become WGSL overrides) but rejects
// OpSpecConstantOp (derived expressions). Freezing them to defaults breaks
// shaders whose defaults are degenerate (Godot packs tonemap constants into
// one integer whose default is 0, freezing the luminance divisor to zero).
// Instead, convert each OpSpecConstantOp into its equivalent REGULAR
// instruction cloned into every function that uses it (with fresh result
// ids), placed after the entry block's leading OpVariables. The plain spec
// constants stay live as overrides, so runtime specialization keeps working.

static constexpr uint16_t OP_LOWER_LABEL = 248;

Vector<uint8_t> lower_spec_constant_ops_to_runtime(const Vector<uint8_t> &p_bytes) {
	const uint8_t *data = p_bytes.ptr();
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);
	if (total_words < 5) {
		return p_bytes;
	}

	// Pass 1: collect OpSpecConstantOp instructions in order.
	struct SpecOp {
		uint32_t type_id;
		uint32_t result_id;
		uint16_t opcode;
		Vector<uint32_t> operands;
	};
	Vector<SpecOp> spec_ops;
	HashMap<uint32_t, uint32_t> spec_op_index; // result_id -> index in spec_ops.
	uint32_t pos = 5;
	while (pos < total_words) {
		const uint32_t w0 = read_word(data, len, pos);
		const uint32_t wc = w0 >> 16;
		const uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_SPEC_CONSTANT_OP && wc >= 4) {
			SpecOp so;
			so.type_id = read_word(data, len, pos + 1);
			so.result_id = read_word(data, len, pos + 2);
			so.opcode = (uint16_t)read_word(data, len, pos + 3);
			for (uint32_t w = 4; w < wc; w++) {
				so.operands.push_back(read_word(data, len, pos + w));
			}
			spec_op_index.insert(so.result_id, (uint32_t)spec_ops.size());
			spec_ops.push_back(so);
		}
		pos += wc;
	}
	if (spec_ops.is_empty()) {
		return p_bytes;
	}

	// Bail if a spec-op result is used by another GLOBAL instruction that is
	// not itself a converted spec op (constant composites, array sizes,
	// variable initializers): those cannot become function-local values.
	pos = 5;
	bool in_function = false;
	while (pos < total_words) {
		const uint32_t w0 = read_word(data, len, pos);
		const uint32_t wc = w0 >> 16;
		const uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_FUNCTION) {
			in_function = true;
		} else if (op == OP_FUNCTION_END) {
			in_function = false;
		} else if (!in_function && op != OP_SPEC_CONSTANT_OP && op != 5 /* OpName */ && op != OP_DECORATE) {
			for (uint32_t w = 1; w < wc; w++) {
				if (spec_op_index.has(read_word(data, len, pos + w))) {
					// Distinguish accidental literal collisions loosely: only
					// bail for type/constant/variable definitions.
					if ((op >= 19 && op <= 46) || op == OP_VARIABLE || op == OP_SPEC_CONSTANT_COMPOSITE) {
						return p_bytes;
					}
				}
			}
		}
		pos += wc;
	}

	uint32_t id_bound = read_word(data, len, 3);

	// Pass 2: find, per function, whether it uses any spec-op result.
	struct FuncInfo {
		uint32_t start = 0; // word index of OpFunction.
		uint32_t end = 0; // word index just past OpFunctionEnd.
		bool uses = false;
	};
	Vector<FuncInfo> funcs;
	pos = 5;
	FuncInfo current;
	bool have_current = false;
	while (pos < total_words) {
		const uint32_t w0 = read_word(data, len, pos);
		const uint32_t wc = w0 >> 16;
		const uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_FUNCTION) {
			current = FuncInfo();
			current.start = pos;
			have_current = true;
		} else if (have_current) {
			for (uint32_t w = 1; w < wc && !current.uses; w++) {
				if (spec_op_index.has(read_word(data, len, pos + w))) {
					current.uses = true;
				}
			}
			if (op == OP_FUNCTION_END) {
				current.end = pos + wc;
				funcs.push_back(current);
				have_current = false;
			}
		}
		pos += wc;
	}

	// Pass 3: rebuild.
	Vector<uint8_t> out;
	out.reserve(p_bytes.size() + spec_ops.size() * 32 * funcs.size());
	for (uint32_t w = 0; w < 5; w++) {
		push_word(out, read_word(data, len, w));
	}

	HashMap<uint32_t, uint32_t> local_map; // original spec id -> clone id (per function).
	auto emit_clones = [&](Vector<uint8_t> &r_out) {
		local_map.clear();
		for (const SpecOp &so : spec_ops) {
			const uint32_t clone_id = id_bound++;
			local_map.insert(so.result_id, clone_id);
			const uint32_t wc = 3 + so.operands.size();
			push_word(r_out, (wc << 16) | so.opcode);
			push_word(r_out, so.type_id);
			push_word(r_out, clone_id);
			for (const uint32_t operand : so.operands) {
				const uint32_t *mapped = local_map.getptr(operand);
				push_word(r_out, mapped != nullptr ? *mapped : operand);
			}
		}
	};

	auto remap_word = [&](uint32_t p_word) -> uint32_t {
		const uint32_t *mapped = local_map.getptr(p_word);
		return mapped != nullptr ? *mapped : p_word;
	};

	uint32_t func_idx = 0;
	pos = 5;
	while (pos < total_words) {
		const uint32_t w0 = read_word(data, len, pos);
		const uint32_t wc = w0 >> 16;
		const uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_SPEC_CONSTANT_OP) {
			pos += wc; // Dropped from the global section.
			continue;
		}
		if (op == OP_FUNCTION) {
			while (func_idx < (uint32_t)funcs.size() && funcs[func_idx].end <= pos) {
				func_idx++;
			}
			const bool uses = func_idx < (uint32_t)funcs.size() && funcs[func_idx].start == pos && funcs[func_idx].uses;
			if (!uses) {
				// Copy the whole function untouched.
				const uint32_t fend = func_idx < (uint32_t)funcs.size() && funcs[func_idx].start == pos ? funcs[func_idx].end : total_words;
				append_bytes(out, data, pos * 4, (fend - pos) * 4);
				pos = fend;
				continue;
			}
			// Copy until after the first OpLabel plus its leading OpVariables,
			// emit clones there, then copy the rest remapping uses.
			const uint32_t fend = funcs[func_idx].end;
			uint32_t fpos = pos;
			bool label_seen = false;
			bool clones_emitted = false;
			while (fpos < fend) {
				const uint32_t fw0 = read_word(data, len, fpos);
				const uint32_t fwc = fw0 >> 16;
				const uint16_t fop = (uint16_t)(fw0 & 0xFFFF);
				if (fwc == 0) {
					break;
				}
				if (!clones_emitted && label_seen && fop != OP_VARIABLE) {
					emit_clones(out);
					clones_emitted = true;
				}
				if (fop == OP_LOWER_LABEL && !label_seen) {
					label_seen = true;
					append_bytes(out, data, fpos * 4, fwc * 4);
					fpos += fwc;
					continue;
				}
				if (!clones_emitted) {
					append_bytes(out, data, fpos * 4, fwc * 4);
					fpos += fwc;
					continue;
				}
				// Remap operands. Result slots: value ops have (type, result)
				// at +1/+2; no-result ops remap everything; literal-bearing
				// ops only remap their id operands.
				push_word(out, fw0);
				uint32_t first_operand = 3;
				uint32_t last_operand = fwc; // exclusive
				switch (fop) {
					case 62: // OpStore
					case 63: // OpCopyMemory
					case 99: // OpImageWrite
					case 249: // OpBranch
					case 250: // OpBranchConditional
					case 254: // OpReturnValue
						first_operand = 1;
						break;
					case 251: // OpSwitch: only the selector is an id here.
						first_operand = 1;
						last_operand = 2;
						break;
					case 79: // OpVectorShuffle: trailing literals.
					case 82: // OpCompositeInsert
						last_operand = MIN(fwc, 5u);
						break;
					case 81: // OpCompositeExtract: composite only.
						last_operand = MIN(fwc, 4u);
						break;
					default:
						break;
				}
				for (uint32_t w = 1; w < fwc; w++) {
					const uint32_t word = read_word(data, len, fpos + w);
					const bool in_range = w >= first_operand && w < last_operand;
					push_word(out, in_range ? remap_word(word) : word);
				}
				fpos += fwc;
			}
			pos = fend;
			continue;
		}
		append_bytes(out, data, pos * 4, wc * 4);
		pos += wc;
	}

	// Update the id bound.
	uint8_t *out_ptr = out.ptrw();
	memcpy(out_ptr + 12, &id_bound, 4);
	return out;
}

// ---- rewrite_copy_logical ----
//
// OpCopyLogical is a SPIR-V 1.4 instruction that copies between LOGICALLY
// matching but DISTINCT composite types -- glslang emits it when copying a
// Block-decorated UBO struct (explicit offsets/strides) into a plain
// function-local struct of the same shape (the clustered scene shader's
// SceneData does exactly this). Tint's reader rejects it, and OpCopyObject
// is no substitute (it requires identical types). Lower it to the SPIR-V
// 1.3 equivalent instead: a recursive per-member OpCompositeExtract /
// OpCompositeConstruct copy, converting mismatched struct/array member
// types level by level. When the two types happen to be identical, a plain
// OpCopyObject suffices. Any unexpected shape bails the whole pass,
// leaving the variant excluded exactly as it is today.

static constexpr uint16_t CL_OP_TYPE_STRUCT = 30;
static constexpr uint16_t CL_OP_UNDEF = 3;
static constexpr uint16_t CL_OP_FUNCTION_PARAMETER = 55;
static constexpr uint16_t CL_OP_FUNCTION_CALL = 57;
static constexpr uint16_t CL_OP_SELECT = 169;
static constexpr uint16_t CL_OP_PHI = 245;
static constexpr uint16_t CL_OP_COMPOSITE_CONSTRUCT = 80;
static constexpr uint16_t CL_OP_COMPOSITE_EXTRACT = 81;

struct CopyLogicalTypeInfo {
	uint16_t opcode = 0;
	LocalVector<uint32_t> operands; // Type-specific words after the result id.
};

// Emits a structural copy of p_src_id (type p_src_type) as type p_dst_type
// into r_emit. Returns the result id, or 0 on an unsupported shape.
static uint32_t _copy_logical_emit(const HashMap<uint32_t, CopyLogicalTypeInfo> &p_types, const HashMap<uint32_t, uint32_t> &p_const_values, uint32_t p_dst_type, uint32_t p_src_id, uint32_t p_src_type, uint32_t &r_next_id, Vector<uint8_t> &r_emit, uint32_t p_forced_result, int p_depth) {
	if (p_depth > 16) {
		return 0;
	}
	if (p_dst_type == p_src_type) {
		// Identical types; a plain OpCopyObject is valid.
		uint32_t result = p_forced_result ? p_forced_result : r_next_id++;
		push_word(r_emit, (4u << 16) | OP_COPY_OBJECT);
		push_word(r_emit, p_dst_type);
		push_word(r_emit, result);
		push_word(r_emit, p_src_id);
		return result;
	}
	const CopyLogicalTypeInfo *dst = p_types.getptr(p_dst_type);
	const CopyLogicalTypeInfo *src = p_types.getptr(p_src_type);
	if (!dst || !src || dst->opcode != src->opcode) {
		return 0;
	}
	// Gather the member/element types of both sides.
	LocalVector<uint32_t> dst_members;
	LocalVector<uint32_t> src_members;
	if (dst->opcode == CL_OP_TYPE_STRUCT) {
		dst_members = dst->operands;
		src_members = src->operands;
	} else if (dst->opcode == OP_TYPE_ARRAY) {
		// operands = [element type, length constant id].
		if (dst->operands.size() != 2 || src->operands.size() != 2) {
			return 0;
		}
		const uint32_t *dst_len = p_const_values.getptr(dst->operands[1]);
		const uint32_t *src_len = p_const_values.getptr(src->operands[1]);
		if (!dst_len || !src_len || *dst_len != *src_len || *dst_len == 0 || *dst_len > 4096) {
			return 0;
		}
		for (uint32_t i = 0; i < *dst_len; i++) {
			dst_members.push_back(dst->operands[0]);
			src_members.push_back(src->operands[0]);
		}
	} else {
		// Distinct ids of any non-aggregate kind cannot be logically matching.
		return 0;
	}
	if (dst_members.size() != src_members.size() || dst_members.is_empty()) {
		return 0;
	}
	LocalVector<uint32_t> member_results;
	for (uint32_t i = 0; i < src_members.size(); i++) {
		uint32_t extracted = r_next_id++;
		push_word(r_emit, (5u << 16) | CL_OP_COMPOSITE_EXTRACT);
		push_word(r_emit, src_members[i]);
		push_word(r_emit, extracted);
		push_word(r_emit, p_src_id);
		push_word(r_emit, i);
		uint32_t converted = extracted;
		if (dst_members[i] != src_members[i]) {
			converted = _copy_logical_emit(p_types, p_const_values, dst_members[i], extracted, src_members[i], r_next_id, r_emit, 0, p_depth + 1);
			if (converted == 0) {
				return 0;
			}
		}
		member_results.push_back(converted);
	}
	uint32_t result = p_forced_result ? p_forced_result : r_next_id++;
	push_word(r_emit, ((3u + member_results.size()) << 16) | CL_OP_COMPOSITE_CONSTRUCT);
	push_word(r_emit, p_dst_type);
	push_word(r_emit, result);
	for (uint32_t i = 0; i < member_results.size(); i++) {
		push_word(r_emit, member_results[i]);
	}
	return result;
}

Vector<uint8_t> rewrite_copy_logical(const Vector<uint8_t> &p_bytes) {
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);
	if (total_words < 5) {
		return p_bytes;
	}
	const uint8_t *data = p_bytes.ptr();

	// Scan: types, plain int constants (array lengths), value result types,
	// and whether any OpCopyLogical exists at all.
	HashMap<uint32_t, CopyLogicalTypeInfo> types;
	HashMap<uint32_t, uint32_t> const_values;
	HashMap<uint32_t, uint32_t> value_types; // result id -> result type id.
	bool found = false;
	uint32_t pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		switch (op) {
			case CL_OP_TYPE_STRUCT:
			case OP_TYPE_ARRAY: {
				CopyLogicalTypeInfo info;
				info.opcode = op;
				for (uint32_t i = 2; i < wc; i++) {
					info.operands.push_back(read_word(data, len, pos + i));
				}
				types.insert(read_word(data, len, pos + 1), info);
			} break;
			case OP_CONSTANT: {
				if (wc >= 4) {
					const_values.insert(read_word(data, len, pos + 2), read_word(data, len, pos + 3));
				}
			} break;
			case CL_OP_UNDEF:
			case OP_LOAD:
			case OP_COPY_OBJECT:
			case OP_COPY_LOGICAL:
			case CL_OP_COMPOSITE_CONSTRUCT:
			case CL_OP_COMPOSITE_EXTRACT:
			case CL_OP_FUNCTION_PARAMETER:
			case CL_OP_FUNCTION_CALL:
			case CL_OP_SELECT:
			case CL_OP_PHI:
			case OP_CONSTANT_COMPOSITE: {
				if (wc >= 3) {
					value_types.insert(read_word(data, len, pos + 2), read_word(data, len, pos + 1));
				}
				if (op == OP_COPY_LOGICAL) {
					found = true;
				}
			} break;
			default:
				break;
		}
		pos += wc;
	}
	if (!found) {
		return p_bytes;
	}

	uint32_t next_id = read_word(data, len, 3);
	Vector<uint8_t> out;
	out.resize(0);
	append_bytes(out, data, 0, 5 * 4);
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_COPY_LOGICAL && wc == 4) {
			uint32_t dst_type = read_word(data, len, pos + 1);
			uint32_t result_id = read_word(data, len, pos + 2);
			uint32_t src_id = read_word(data, len, pos + 3);
			const uint32_t *src_type = value_types.getptr(src_id);
			if (!src_type) {
				return p_bytes; // Untracked operand producer; leave for exclusion.
			}
			Vector<uint8_t> emitted;
			uint32_t got = _copy_logical_emit(types, const_values, dst_type, src_id, *src_type, next_id, emitted, result_id, 0);
			if (got != result_id) {
				return p_bytes; // Unsupported shape; leave for exclusion.
			}
			append_bytes(out, emitted.ptr(), 0, emitted.size());
			// The lowered results are plain values; record the copy's type for
			// any later OpCopyLogical chained off this one.
			value_types.insert(result_id, dst_type);
		} else {
			append_bytes(out, data, (int64_t)pos * 4, (int64_t)wc * 4);
		}
		pos += wc;
	}
	// Patch the id bound for the fresh intermediate ids.
	uint8_t *out_data = out.ptrw();
	memcpy(out_data + 3 * 4, &next_id, 4);
	return out;
}

// ---- strip_nonwritable_on_function_vars ----
//
// glslang decorates the function-local "indexable" copies of const arrays
// with NonWritable, which SPIR-V 1.4 allows on any variable but 1.3 (and
// Tint's reader) restricts to storage images, uniform blocks and storage
// buffers. The decoration is a pure optimization hint; drop it when the
// target is a Function-storage variable.

static constexpr uint32_t SNW_DECO_NONWRITABLE = 24;
static constexpr uint32_t SNW_SC_FUNCTION = 7;

Vector<uint8_t> strip_nonwritable_on_function_vars(const Vector<uint8_t> &p_bytes) {
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);
	if (total_words < 5) {
		return p_bytes;
	}
	const uint8_t *data = p_bytes.ptr();

	// Prescan: NonWritable decoration targets + Function-storage variables.
	HashSet<uint32_t> nonwritable_targets;
	HashSet<uint32_t> function_vars;
	uint32_t pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_DECORATE && wc == 3 && read_word(data, len, pos + 2) == SNW_DECO_NONWRITABLE) {
			nonwritable_targets.insert(read_word(data, len, pos + 1));
		} else if (op == OP_VARIABLE && wc >= 4 && read_word(data, len, pos + 3) == SNW_SC_FUNCTION) {
			function_vars.insert(read_word(data, len, pos + 2));
		}
		pos += wc;
	}
	bool any = false;
	for (const uint32_t target : nonwritable_targets) {
		if (function_vars.has(target)) {
			any = true;
			break;
		}
	}
	if (!any) {
		return p_bytes;
	}

	Vector<uint8_t> out;
	append_bytes(out, data, 0, 5 * 4);
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		bool skip = op == OP_DECORATE && wc == 3 && read_word(data, len, pos + 2) == SNW_DECO_NONWRITABLE && function_vars.has(read_word(data, len, pos + 1));
		if (!skip) {
			append_bytes(out, data, (int64_t)pos * 4, (int64_t)wc * 4);
		}
		pos += wc;
	}
	return out;
}

// ---- lower_subgroup_ops_to_single_invocation ----
//
// The Forward+ clustered shaders use subgroup operations unconditionally
// (upstream assumes Vulkan 1.1), but only as a wave-coherence optimization
// over idempotent merges: reductions feed loop bounds and masks that each
// invocation re-filters, and the cluster writer's ballot dedup guards an
// atomicOr whose repeated execution is a no-op. Tint's SPIR-V reader
// predates WGSL subgroups and rejects the opcodes, so lower them to exact
// wave-of-1 semantics instead:
//
//   Reduce min/max/and/or/xor/add, Broadcast(First), All, Any -> the value
//   Ballot(pred)               -> (pred ? 1u : 0u, 0, 0, 0)
//   BallotBitCount Exclusive   -> 0u          (lane 0 has no lower lanes)
//   BallotBitCount Reduce      -> mask.x & 1u (one possible active lane)
//
// Every invocation then does its own full (correct) work; only the wave
// bandwidth sharing is lost. Any other subgroup instruction bails the pass,
// leaving the variant excluded as it is today.

static constexpr uint16_t SG_OP_CAPABILITY = 17;
static constexpr uint16_t SG_OP_COMPOSITE_CONSTRUCT = 80;
static constexpr uint16_t SG_OP_COMPOSITE_EXTRACT = 81;
static constexpr uint16_t SG_OP_SELECT = 169;
static constexpr uint16_t SG_OP_BITWISE_AND = 199;
static constexpr uint16_t SG_OP_ALL = 334;
static constexpr uint16_t SG_OP_ANY = 335;
static constexpr uint16_t SG_OP_BROADCAST = 337;
static constexpr uint16_t SG_OP_BROADCAST_FIRST = 338;
static constexpr uint16_t SG_OP_BALLOT = 339;
static constexpr uint16_t SG_OP_BALLOT_BIT_COUNT = 342;
static constexpr uint16_t SG_OP_ARITH_FIRST = 349; // IAdd.
static constexpr uint16_t SG_OP_ARITH_LAST = 361; // BitwiseXor.
static constexpr uint16_t SG_GROUP_NONUNIFORM_FIRST = 333;
static constexpr uint16_t SG_GROUP_NONUNIFORM_LAST = 363;
static constexpr uint32_t SG_GROUP_OP_REDUCE = 0;
static constexpr uint32_t SG_GROUP_OP_EXCLUSIVE_SCAN = 2;
static constexpr uint32_t SG_CAP_GROUP_NONUNIFORM_FIRST = 61;
static constexpr uint32_t SG_CAP_GROUP_NONUNIFORM_LAST = 68;

Vector<uint8_t> lower_subgroup_ops_to_single_invocation(const Vector<uint8_t> &p_bytes) {
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);
	if (total_words < 5) {
		return p_bytes;
	}
	const uint8_t *data = p_bytes.ptr();

	// Prescan: find subgroup ops, validate every one is a supported shape,
	// and locate the uint type for constant synthesis.
	bool found = false;
	bool needs_uint_consts = false;
	uint32_t uint_type = 0;
	uint32_t uint_type_def_pos = 0;
	uint32_t pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_TYPE_INT && wc == 4 && read_word(data, len, pos + 2) == 32 && read_word(data, len, pos + 3) == 0) {
			uint_type = read_word(data, len, pos + 1);
			uint_type_def_pos = pos;
		} else if (op >= SG_GROUP_NONUNIFORM_FIRST && op <= SG_GROUP_NONUNIFORM_LAST) {
			found = true;
			bool supported = false;
			if ((op == SG_OP_ALL || op == SG_OP_ANY || op == SG_OP_BROADCAST_FIRST) && wc == 5) {
				supported = true;
			} else if (op == SG_OP_BROADCAST && wc == 6) {
				supported = true;
			} else if (op == SG_OP_BALLOT && wc == 5) {
				supported = true;
				needs_uint_consts = true;
			} else if (op == SG_OP_BALLOT_BIT_COUNT && wc == 6) {
				uint32_t group_op = read_word(data, len, pos + 4);
				supported = group_op == SG_GROUP_OP_REDUCE || group_op == SG_GROUP_OP_EXCLUSIVE_SCAN;
				needs_uint_consts = true;
			} else if (op >= SG_OP_ARITH_FIRST && op <= SG_OP_ARITH_LAST && wc == 6) {
				supported = read_word(data, len, pos + 4) == SG_GROUP_OP_REDUCE;
			}
			if (!supported) {
				return p_bytes; // Unsupported subgroup use; leave for exclusion.
			}
		}
		pos += wc;
	}
	if (!found) {
		return p_bytes;
	}
	if (needs_uint_consts && uint_type == 0) {
		return p_bytes;
	}

	uint32_t next_id = read_word(data, len, 3);
	uint32_t uint_0 = 0;
	uint32_t uint_1 = 0;
	if (needs_uint_consts) {
		uint_0 = next_id++;
		uint_1 = next_id++;
	}

	Vector<uint8_t> out;
	append_bytes(out, data, 0, 5 * 4);
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == SG_OP_CAPABILITY && wc == 2 && read_word(data, len, pos + 1) >= SG_CAP_GROUP_NONUNIFORM_FIRST && read_word(data, len, pos + 1) <= SG_CAP_GROUP_NONUNIFORM_LAST) {
			// Dropped: nothing references subgroup capabilities anymore.
		} else if (op >= SG_GROUP_NONUNIFORM_FIRST && op <= SG_GROUP_NONUNIFORM_LAST) {
			uint32_t result_type = read_word(data, len, pos + 1);
			uint32_t result_id = read_word(data, len, pos + 2);
			if (op == SG_OP_BALLOT) {
				// (pred ? 1u : 0u, 0, 0, 0).
				uint32_t pred = read_word(data, len, pos + 4);
				uint32_t sel = next_id++;
				push_word(out, (6u << 16) | SG_OP_SELECT);
				push_word(out, uint_type);
				push_word(out, sel);
				push_word(out, pred);
				push_word(out, uint_1);
				push_word(out, uint_0);
				push_word(out, (7u << 16) | SG_OP_COMPOSITE_CONSTRUCT);
				push_word(out, result_type);
				push_word(out, result_id);
				push_word(out, sel);
				push_word(out, uint_0);
				push_word(out, uint_0);
				push_word(out, uint_0);
			} else if (op == SG_OP_BALLOT_BIT_COUNT) {
				uint32_t group_op = read_word(data, len, pos + 4);
				uint32_t value = read_word(data, len, pos + 5);
				if (group_op == SG_GROUP_OP_EXCLUSIVE_SCAN) {
					push_word(out, (4u << 16) | OP_COPY_OBJECT);
					push_word(out, result_type);
					push_word(out, result_id);
					push_word(out, uint_0);
				} else {
					uint32_t extracted = next_id++;
					push_word(out, (5u << 16) | SG_OP_COMPOSITE_EXTRACT);
					push_word(out, result_type);
					push_word(out, extracted);
					push_word(out, value);
					push_word(out, 0);
					push_word(out, (5u << 16) | SG_OP_BITWISE_AND);
					push_word(out, result_type);
					push_word(out, result_id);
					push_word(out, extracted);
					push_word(out, uint_1);
				}
			} else {
				// Broadcast(First)/All/Any/Reduce arithmetic: the value operand
				// sits right after the execution scope in every layout.
				uint32_t value = (op >= SG_OP_ARITH_FIRST && op <= SG_OP_ARITH_LAST) ? read_word(data, len, pos + 5) : read_word(data, len, pos + 4);
				push_word(out, (4u << 16) | OP_COPY_OBJECT);
				push_word(out, result_type);
				push_word(out, result_id);
				push_word(out, value);
			}
		} else {
			append_bytes(out, data, (int64_t)pos * 4, (int64_t)wc * 4);
			if (needs_uint_consts && pos == uint_type_def_pos) {
				push_word(out, (4u << 16) | OP_CONSTANT);
				push_word(out, uint_type);
				push_word(out, uint_0);
				push_word(out, 0);
				push_word(out, (4u << 16) | OP_CONSTANT);
				push_word(out, uint_type);
				push_word(out, uint_1);
				push_word(out, 1);
			}
		}
		pos += wc;
	}
	uint8_t *out_data = out.ptrw();
	memcpy(out_data + 3 * 4, &next_id, 4);
	return out;
}

// ---- lower_helper_invocation_to_false ----
//
// Tint's reader rejects the HelperInvocation builtin (no WGSL equivalent in
// its era). The cluster writer only reads it to skip idempotent atomicOr
// merges from helper invocations -- and upstream already ships a mode that
// disables that check entirely (sc_use_helper_check, off on Apple where the
// builtin is unreliable). Reproduce that mode structurally: turn the
// builtin input into a Private variable initialized to false, so every
// read sees "not a helper invocation".

static constexpr uint32_t HI_BUILTIN_HELPER_INVOCATION = 23;
static constexpr uint32_t HI_SC_PRIVATE = 6;

Vector<uint8_t> lower_helper_invocation_to_false(const Vector<uint8_t> &p_bytes) {
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);
	if (total_words < 5) {
		return p_bytes;
	}
	const uint8_t *data = p_bytes.ptr();

	// Prescan: the decorated variable, its pointer type and the pointee bool.
	uint32_t helper_var = 0;
	uint32_t pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_DECORATE && wc == 4 && read_word(data, len, pos + 2) == DECO_BUILTIN && read_word(data, len, pos + 3) == HI_BUILTIN_HELPER_INVOCATION) {
			helper_var = read_word(data, len, pos + 1);
			break;
		}
		pos += wc;
	}
	if (helper_var == 0) {
		return p_bytes;
	}

	// Find the variable's pointer type and that type's pointee.
	uint32_t old_ptr_type = 0;
	uint32_t bool_type = 0;
	uint32_t ptr_def_pos = 0;
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_VARIABLE && wc >= 4 && read_word(data, len, pos + 2) == helper_var) {
			old_ptr_type = read_word(data, len, pos + 1);
		}
		pos += wc;
	}
	if (old_ptr_type == 0) {
		return p_bytes;
	}
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_TYPE_POINTER && wc == 4 && read_word(data, len, pos + 1) == old_ptr_type) {
			bool_type = read_word(data, len, pos + 3);
			ptr_def_pos = pos;
		}
		pos += wc;
	}
	if (bool_type == 0) {
		return p_bytes;
	}

	uint32_t next_id = read_word(data, len, 3);
	uint32_t private_ptr_type = next_id++;
	uint32_t false_const = next_id++;

	Vector<uint8_t> out;
	append_bytes(out, data, 0, 5 * 4);
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = read_word(data, len, pos);
		uint32_t wc = (w0 >> 16);
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		if (op == OP_DECORATE && wc == 4 && read_word(data, len, pos + 1) == helper_var && read_word(data, len, pos + 2) == DECO_BUILTIN) {
			// Dropped builtin decoration.
		} else if (op == OP_VARIABLE && wc >= 4 && read_word(data, len, pos + 2) == helper_var) {
			push_word(out, (5u << 16) | OP_VARIABLE);
			push_word(out, private_ptr_type);
			push_word(out, helper_var);
			push_word(out, HI_SC_PRIVATE);
			push_word(out, false_const);
		} else if (op == OP_ENTRY_POINT) {
			// Interface must list only Input/Output under 1.3; drop the id.
			// Words: [w0, exec model, entry id, name string..., interface...].
			uint32_t name_end = 3;
			while (name_end < wc) {
				uint32_t nw = read_word(data, len, pos + name_end);
				name_end++;
				if ((nw & 0xFF000000) == 0 || (nw & 0x00FF0000) == 0 || (nw & 0x0000FF00) == 0 || (nw & 0x000000FF) == 0) {
					break;
				}
			}
			LocalVector<uint32_t> kept;
			for (uint32_t i = name_end; i < wc; i++) {
				uint32_t iface = read_word(data, len, pos + i);
				if (iface != helper_var) {
					kept.push_back(iface);
				}
			}
			uint32_t new_wc = name_end + kept.size();
			push_word(out, (new_wc << 16) | OP_ENTRY_POINT);
			for (uint32_t i = 1; i < name_end; i++) {
				push_word(out, read_word(data, len, pos + i));
			}
			for (uint32_t i = 0; i < kept.size(); i++) {
				push_word(out, kept[i]);
			}
		} else {
			append_bytes(out, data, (int64_t)pos * 4, (int64_t)wc * 4);
			if (pos == ptr_def_pos) {
				push_word(out, (4u << 16) | OP_TYPE_POINTER);
				push_word(out, private_ptr_type);
				push_word(out, HI_SC_PRIVATE);
				push_word(out, bool_type);
				push_word(out, (3u << 16) | OP_CONSTANT_FALSE);
				push_word(out, bool_type);
				push_word(out, false_const);
			}
		}
		pos += wc;
	}
	uint8_t *out_data = out.ptrw();
	memcpy(out_data + 3 * 4, &next_id, 4);
	return out;
}

// ---- fan_out_binding_arrays ----
//
// Tint rejects arrays of handle types, and the flatten_binding_arrays
// fallback collapses them to their first element -- only correct for shaders
// that never index past zero. This pass performs the real lowering, emitting
// the per-element binding layout the driver already fans out for arrayed
// uniforms (ARRAY_BINDING_BASE / ARRAY_BINDING_STRIDE):
//
//   element binding = p_binding_base + original_binding * p_binding_stride + i
//
// Constant-index accesses substitute the element variable everywhere the
// chain result was used (loads, helper-call arguments). Dynamic-index
// accesses lower to a structured OpSwitch that clones the consuming
// instructions per element and merges the produced value with OpPhi -- the
// lowering the WebGPU sized-binding-arrays proposal endorses. Supported
// dynamic consumer shapes:
//
//   chain -> OpLoad -> image op                      (sampled-image arrays)
//   chain -> OpLoad -> OpSampledImage -> image op    (texture + separate sampler)
//   chain -> OpFunctionCall                          (texture passed to a helper)
//
// Arrays with any unsupported use are left untouched for the truncation
// fallback, keeping today's behavior as the worst case.

static constexpr uint16_t FAN_OP_NAME = 5;
static constexpr uint16_t FAN_OP_FUNCTION_CALL = 57;
static constexpr uint16_t FAN_OP_SAMPLED_IMAGE = 86;
static constexpr uint16_t FAN_OP_PHI = 245;
static constexpr uint16_t FAN_OP_SELECTION_MERGE = 247;
static constexpr uint16_t FAN_OP_LABEL = 248;
static constexpr uint16_t FAN_OP_BRANCH = 249;
static constexpr uint16_t FAN_OP_SWITCH = 251;
static constexpr uint32_t FAN_DECO_BINDING = 33;
static constexpr uint32_t FAN_DECO_DESCRIPTOR_SET = 34;
static constexpr uint32_t FAN_SC_UNIFORM_CONSTANT = 0;

// Image instructions that consume a handle and produce a plain value we can
// merge with OpPhi: OpImageSample* / *Dref* / Fetch / Gather (87-97),
// OpImageRead (98), and the query ops (101-107). OpImageWrite (99, no
// result) and OpImage (100, produces another handle) are excluded.
static inline bool fan_is_value_image_op(uint16_t p_op) {
	return (p_op >= 87 && p_op <= 98) || (p_op >= 101 && p_op <= 107);
}

Vector<uint8_t> fan_out_binding_arrays(const Vector<uint8_t> &p_bytes, uint32_t p_binding_base, uint32_t p_binding_stride, uint32_t p_max_binding) {
	const uint8_t *data = p_bytes.ptr();
	const int64_t len = p_bytes.size();
	const uint32_t total_words = (uint32_t)(len / 4);
	if (total_words < 5) {
		return p_bytes;
	}
	// Header: magic, version, generator, id bound, schema. The pass only
	// understands SPIR-V <= 1.3 entry-point interface rules (UniformConstant
	// globals need not be listed); the container downgrades to 1.3 before
	// this chain runs, so anything newer just passes through.
	if (read_word(data, len, 0) != 0x07230203u || read_word(data, len, 1) > 0x00010300u) {
		return p_bytes;
	}
	uint32_t next_id = read_word(data, len, 3);

	// ---- Sweep 1: index instructions, types, constants, variables. ----
	struct Instr {
		uint32_t pos;
		uint32_t wc;
		uint16_t op;
		int32_t block; // Block ordinal inside the function section, -1 outside.
	};
	LocalVector<Instr> instrs;
	HashSet<uint32_t> handle_types;
	HashSet<uint32_t> int_types32;
	HashMap<uint32_t, uint32_t> int_const; // id -> 32-bit value
	struct ArrayInfo {
		uint32_t elem = 0;
		uint32_t length_id = 0;
		uint32_t instr_idx = 0;
	};
	HashMap<uint32_t, ArrayInfo> handle_arrays; // array type id -> info
	struct PtrInfo {
		uint32_t sc = 0;
		uint32_t base = 0;
		uint32_t instr_idx = 0;
	};
	HashMap<uint32_t, PtrInfo> ptr_types;
	HashMap<uint64_t, uint32_t> ptr_lookup; // (sc<<32)|base -> ptr id
	struct VarInfo {
		uint32_t ptr_type = 0;
		uint32_t sc = 0;
		uint32_t instr_idx = 0;
		uint32_t set = UINT32_MAX;
		uint32_t binding = UINT32_MAX;
		uint32_t binding_deco_idx = UINT32_MAX;
	};
	HashMap<uint32_t, VarInfo> vars;
	HashMap<uint32_t, LocalVector<uint32_t>> id_meta_instrs; // target id -> OpName/OpDecorate instr indices
	uint32_t first_function_instr = UINT32_MAX;
	int32_t block_ord = -1;
	{
		uint32_t pos = 5;
		while (pos < total_words) {
			uint32_t w0 = read_word(data, len, pos);
			uint32_t wc = w0 >> 16;
			uint16_t op = (uint16_t)(w0 & 0xFFFF);
			if (wc == 0 || pos + wc > total_words) {
				return p_bytes; // Malformed.
			}
			if (op == FAN_OP_LABEL) {
				block_ord++;
			}
			const uint32_t instr_idx = instrs.size();
			instrs.push_back({ pos, wc, op, block_ord });
			switch (op) {
				case OP_TYPE_IMAGE:
				case OP_TYPE_SAMPLER:
				case OP_TYPE_SAMPLED_IMAGE: {
					handle_types.insert(read_word(data, len, pos + 1));
				} break;
				case OP_TYPE_INT: {
					if (wc >= 4 && read_word(data, len, pos + 2) == 32) {
						int_types32.insert(read_word(data, len, pos + 1));
					}
				} break;
				case OP_CONSTANT: {
					if (wc >= 4 && int_types32.has(read_word(data, len, pos + 1))) {
						int_const.insert(read_word(data, len, pos + 2), read_word(data, len, pos + 3));
					}
				} break;
				case OP_TYPE_ARRAY: {
					if (wc >= 4 && handle_types.has(read_word(data, len, pos + 2))) {
						handle_arrays.insert(read_word(data, len, pos + 1), { read_word(data, len, pos + 2), read_word(data, len, pos + 3), instr_idx });
					}
				} break;
				case OP_TYPE_POINTER: {
					if (wc >= 4) {
						uint32_t id = read_word(data, len, pos + 1);
						uint32_t sc = read_word(data, len, pos + 2);
						uint32_t base = read_word(data, len, pos + 3);
						ptr_types.insert(id, { sc, base, instr_idx });
						uint64_t key = ((uint64_t)sc << 32) | base;
						if (!ptr_lookup.has(key)) {
							ptr_lookup.insert(key, id);
						}
					}
				} break;
				case OP_VARIABLE: {
					if (wc >= 4) {
						vars.insert(read_word(data, len, pos + 2), { read_word(data, len, pos + 1), read_word(data, len, pos + 3), instr_idx, UINT32_MAX, UINT32_MAX, UINT32_MAX });
					}
				} break;
				case OP_DECORATE: {
					if (wc >= 3) {
						id_meta_instrs[read_word(data, len, pos + 1)].push_back(instr_idx);
					}
				} break;
				case FAN_OP_NAME: {
					if (wc >= 2) {
						id_meta_instrs[read_word(data, len, pos + 1)].push_back(instr_idx);
					}
				} break;
				case OP_FUNCTION: {
					if (first_function_instr == UINT32_MAX) {
						first_function_instr = instr_idx;
					}
				} break;
				default:
					break;
			}
			pos += wc;
		}
	}
	if (handle_arrays.is_empty() || first_function_instr == UINT32_MAX) {
		return p_bytes;
	}
	// Fill set/binding decorations on variables.
	for (uint32_t i = 0; i < instrs.size(); i++) {
		const Instr &ins = instrs[i];
		if (ins.op != OP_DECORATE || ins.wc < 4) {
			continue;
		}
		uint32_t target = read_word(data, len, ins.pos + 1);
		VarInfo *vi = vars.getptr(target);
		if (!vi) {
			continue;
		}
		uint32_t deco = read_word(data, len, ins.pos + 2);
		if (deco == FAN_DECO_DESCRIPTOR_SET) {
			vi->set = read_word(data, len, ins.pos + 3);
		} else if (deco == FAN_DECO_BINDING) {
			vi->binding = read_word(data, len, ins.pos + 3);
			vi->binding_deco_idx = i;
		}
	}

	// ---- Sweep 2: candidate variables and their access chains. ----
	struct ChainInfo {
		uint32_t instr_idx = 0;
		uint32_t result = 0;
		uint32_t result_type = 0;
		uint32_t index_id = 0;
	};
	struct CandidateVar {
		uint32_t var_id = 0;
		const VarInfo *info = nullptr;
		const ArrayInfo *arr = nullptr;
		uint32_t length = 0;
		LocalVector<ChainInfo> chains;
		bool bailed = false;
	};
	LocalVector<CandidateVar> candidates;
	HashMap<uint32_t, uint32_t> var_to_candidate; // var id -> candidates index
	for (const KeyValue<uint32_t, VarInfo> &kv : vars) {
		const VarInfo &vi = kv.value;
		if (vi.sc != FAN_SC_UNIFORM_CONSTANT || vi.set == UINT32_MAX || vi.binding == UINT32_MAX || vi.binding_deco_idx == UINT32_MAX) {
			continue;
		}
		const PtrInfo *pt = ptr_types.getptr(vi.ptr_type);
		if (!pt) {
			continue;
		}
		const ArrayInfo *arr = handle_arrays.getptr(pt->base);
		if (!arr) {
			continue;
		}
		const uint32_t *len_val = int_const.getptr(arr->length_id);
		if (!len_val || *len_val == 0 || *len_val > p_binding_stride) {
			continue;
		}
		if (p_binding_base + vi.binding * p_binding_stride + (*len_val - 1) >= p_max_binding) {
			continue;
		}
		CandidateVar cand;
		cand.var_id = kv.key;
		cand.info = &kv.value;
		cand.arr = arr;
		cand.length = *len_val;
		var_to_candidate.insert(kv.key, candidates.size());
		candidates.push_back(cand);
	}
	if (candidates.is_empty()) {
		return p_bytes;
	}
	for (uint32_t i = first_function_instr; i < instrs.size(); i++) {
		const Instr &ins = instrs[i];
		if ((ins.op != OP_ACCESS_CHAIN && ins.op != OP_IN_BOUNDS_ACCESS_CHAIN) || ins.wc < 5) {
			continue;
		}
		uint32_t base = read_word(data, len, ins.pos + 3);
		const uint32_t *cand_idx = var_to_candidate.getptr(base);
		if (!cand_idx) {
			continue;
		}
		if (ins.wc != 5) {
			candidates[*cand_idx].bailed = true; // Multi-index chain: not a plain element access.
			continue;
		}
		candidates[*cand_idx].chains.push_back({ i, read_word(data, len, ins.pos + 2), read_word(data, len, ins.pos + 1), read_word(data, len, ins.pos + 4) });
	}

	// Conservative use counting inside the function section: every word of
	// every instruction except the leading opcode word and the instruction's
	// own result slot. The slot heuristic also skips OpStore's value operand,
	// which is safe for the pointer/handle ids audited here -- logical
	// addressing forbids storing either. Literal collisions only ever inflate
	// the count, which bails (safe direction). Debug/annotation instructions
	// live before the function section, so decorations do not inflate counts.
	HashMap<uint32_t, uint32_t> use_count;
	auto count_uses_of = [&](uint32_t p_id) -> uint32_t {
		const uint32_t *cached = use_count.getptr(p_id);
		if (cached) {
			return *cached;
		}
		uint32_t count = 0;
		for (uint32_t i = first_function_instr; i < instrs.size(); i++) {
			const Instr &ins = instrs[i];
			for (uint32_t w = 1; w < ins.wc; w++) {
				const bool is_result_slot = (w == 2 && ins.wc >= 3) || (w == 1 && ins.op == FAN_OP_LABEL);
				if (!is_result_slot && read_word(data, len, ins.pos + w) == p_id) {
					count++;
				}
			}
		}
		use_count.insert(p_id, count);
		return count;
	};
	// Find the single consumer instruction of an id, scanning operand words
	// only (word 3 onward covers every consumer shape we accept).
	auto find_consumers = [&](uint32_t p_id, LocalVector<uint32_t> &r_instr_indices) {
		for (uint32_t i = first_function_instr; i < instrs.size(); i++) {
			const Instr &ins = instrs[i];
			for (uint32_t w = 1; w < ins.wc; w++) {
				if (read_word(data, len, ins.pos + w) == p_id) {
					// Skip the instruction's own result slot.
					bool is_result_slot = (w == 2 && ins.wc >= 3) || (w == 1 && ins.op == FAN_OP_LABEL);
					if (!is_result_slot) {
						r_instr_indices.push_back(i);
					}
					break;
				}
			}
		}
	};

	// ---- Plan rewrites. ----
	HashSet<uint32_t> skip_instr; // instr indices dropped from the output
	HashMap<uint32_t, LocalVector<uint32_t>> emit_before; // instr idx -> words
	HashMap<uint32_t, LocalVector<uint32_t>> emit_after; // instr idx -> words
	HashMap<uint32_t, HashMap<uint32_t, uint32_t>> operand_patch; // instr idx -> word offset -> value
	HashSet<uint32_t> removed_value_ids; // strip their names/decorations

	struct DynamicSite {
		uint32_t idx_id = 0;
		uint32_t clone_instrs[3] = { UINT32_MAX, UINT32_MAX, UINT32_MAX }; // in order
		uint32_t clone_count = 0;
		uint32_t var_operand_instr = 0; // which clone consumes the element variable
		uint32_t var_operand_word = 0; // ... at which word offset
		uint32_t term_type = 0;
		uint32_t term_result = 0;
	};

	uint32_t fanned_count = 0;
	HashMap<uint32_t, uint32_t> synth_ptr_def; // synthesized elem ptr id -> emit anchor instr idx
	for (CandidateVar &cand : candidates) {
		if (cand.bailed) {
			continue;
		}
		const uint32_t N = cand.length;
		// Validate every chain first; any failure bails the whole variable.
		LocalVector<DynamicSite> dynamic_sites;
		LocalVector<uint32_t> dynamic_chain_instrs;
		LocalVector<uint32_t> const_chain_instrs;
		LocalVector<uint32_t> const_chain_elements;
		LocalVector<uint32_t> const_chain_results;
		// Exact operand patches for constant chains: (instr idx, word offset).
		LocalVector<uint32_t> const_patch_instrs;
		LocalVector<uint32_t> const_patch_words;
		LocalVector<uint32_t> const_patch_elements;
		bool ok = true;
		for (const ChainInfo &chain : cand.chains) {
			const uint32_t *const_val = int_const.getptr(chain.index_id);
			if (const_val) {
				// Substitute the element variable at every consumer. Only
				// loads and helper-call arguments are recognized; anything
				// else (or a literal collision inside an unexpected opcode)
				// bails the variable to the truncation fallback.
				const uint32_t element = MIN(*const_val, N - 1);
				LocalVector<uint32_t> consumers;
				find_consumers(chain.result, consumers);
				if (consumers.is_empty() || count_uses_of(chain.result) != consumers.size()) {
					ok = false;
					break;
				}
				for (uint32_t ci : consumers) {
					const Instr &consumer = instrs[ci];
					if (consumer.op == OP_LOAD && read_word(data, len, consumer.pos + 3) == chain.result) {
						const_patch_instrs.push_back(ci);
						const_patch_words.push_back(3);
						const_patch_elements.push_back(element);
					} else if (consumer.op == FAN_OP_FUNCTION_CALL) {
						uint32_t hits = 0;
						for (uint32_t w = 4; w < consumer.wc; w++) {
							if (read_word(data, len, consumer.pos + w) == chain.result) {
								const_patch_instrs.push_back(ci);
								const_patch_words.push_back(w);
								const_patch_elements.push_back(element);
								hits++;
							}
						}
						if (hits == 0) {
							ok = false;
							break;
						}
					} else {
						ok = false;
						break;
					}
				}
				if (!ok) {
					break;
				}
				const_chain_instrs.push_back(chain.instr_idx);
				const_chain_elements.push_back(element);
				const_chain_results.push_back(chain.result);
				continue;
			}
			// Dynamic index: consumers must match a supported shape, and
			// every use of the chain result must be accounted for.
			LocalVector<uint32_t> consumers;
			find_consumers(chain.result, consumers);
			if (consumers.is_empty() || count_uses_of(chain.result) != consumers.size()) {
				ok = false;
				break;
			}
			for (uint32_t ci : consumers) {
				const Instr &consumer = instrs[ci];
				DynamicSite site;
				site.idx_id = chain.index_id;
				if (consumer.op == FAN_OP_FUNCTION_CALL) {
					// chain -> helper call. Clone the call per element.
					uint32_t arg_word = 0;
					uint32_t arg_hits = 0;
					for (uint32_t w = 4; w < consumer.wc; w++) {
						if (read_word(data, len, consumer.pos + w) == chain.result) {
							arg_word = w;
							arg_hits++;
						}
					}
					uint32_t result_type = read_word(data, len, consumer.pos + 1);
					if (arg_hits != 1 || consumer.block != instrs[chain.instr_idx].block || count_uses_of(chain.result) != consumers.size()) {
						ok = false;
						break;
					}
					site.clone_instrs[0] = ci;
					site.clone_count = 1;
					site.var_operand_instr = 0;
					site.var_operand_word = arg_word;
					site.term_type = result_type;
					site.term_result = read_word(data, len, consumer.pos + 2);
					dynamic_sites.push_back(site);
				} else if (consumer.op == OP_LOAD) {
					uint32_t load_result = read_word(data, len, consumer.pos + 2);
					LocalVector<uint32_t> load_consumers;
					find_consumers(load_result, load_consumers);
					if (load_consumers.size() != 1 || count_uses_of(load_result) != 1) {
						ok = false;
						break;
					}
					const Instr &lc = instrs[load_consumers[0]];
					site.clone_instrs[0] = ci;
					site.var_operand_instr = 0;
					site.var_operand_word = 3;
					if (fan_is_value_image_op(lc.op)) {
						site.clone_instrs[1] = load_consumers[0];
						site.clone_count = 2;
						site.term_type = read_word(data, len, lc.pos + 1);
						site.term_result = read_word(data, len, lc.pos + 2);
					} else if (lc.op == FAN_OP_SAMPLED_IMAGE) {
						uint32_t si_result = read_word(data, len, lc.pos + 2);
						LocalVector<uint32_t> si_consumers;
						find_consumers(si_result, si_consumers);
						if (si_consumers.size() != 1 || count_uses_of(si_result) != 1 || !fan_is_value_image_op(instrs[si_consumers[0]].op)) {
							ok = false;
							break;
						}
						const Instr &term = instrs[si_consumers[0]];
						site.clone_instrs[1] = load_consumers[0];
						site.clone_instrs[2] = si_consumers[0];
						site.clone_count = 3;
						site.term_type = read_word(data, len, term.pos + 1);
						site.term_result = read_word(data, len, term.pos + 2);
						if (term.block != consumer.block) {
							ok = false;
							break;
						}
					} else {
						ok = false;
						break;
					}
					// All pieces must share a block for the split to preserve dominance.
					if (instrs[site.clone_instrs[site.clone_count - 1]].block != instrs[chain.instr_idx].block || consumer.block != instrs[chain.instr_idx].block) {
						ok = false;
						break;
					}
					dynamic_sites.push_back(site);
				} else {
					ok = false;
					break;
				}
			}
			if (!ok) {
				break;
			}
			dynamic_chain_instrs.push_back(chain.instr_idx);
		}
		if (!ok || (dynamic_sites.is_empty() && const_chain_instrs.is_empty())) {
			continue; // Leave for the truncation fallback.
		}

		// Element variables. Constant-only arrays materialize just the
		// referenced elements; any dynamic access needs all of them.
		LocalVector<uint32_t> element_vars;
		element_vars.resize(N);
		LocalVector<bool> element_used;
		element_used.resize(N);
		for (uint32_t i = 0; i < N; i++) {
			element_vars[i] = 0;
			element_used[i] = !dynamic_sites.is_empty();
		}
		for (uint32_t k : const_chain_elements) {
			element_used[k] = true;
		}
		// Pointer type to the element under UniformConstant. An existing
		// pointer type may be defined later in the global section than the
		// array variable; the new variables must then be emitted after the
		// pointer type's definition, not after the variable they replace.
		uint32_t elem_ptr = 0;
		uint32_t elem_ptr_def_idx = 0;
		{
			uint64_t key = ((uint64_t)FAN_SC_UNIFORM_CONSTANT << 32) | cand.arr->elem;
			const uint32_t *existing = ptr_lookup.getptr(key);
			if (existing) {
				elem_ptr = *existing;
				const PtrInfo *pi = ptr_types.getptr(elem_ptr);
				if (pi) {
					elem_ptr_def_idx = pi->instr_idx;
				} else {
					const uint32_t *synth = synth_ptr_def.getptr(elem_ptr);
					elem_ptr_def_idx = synth ? *synth : 0;
				}
			} else {
				elem_ptr = next_id++;
				elem_ptr_def_idx = ptr_types[cand.info->ptr_type].instr_idx;
				ptr_lookup.insert(key, elem_ptr);
				synth_ptr_def.insert(elem_ptr, elem_ptr_def_idx);
				LocalVector<uint32_t> &words = emit_after[elem_ptr_def_idx];
				words.push_back((4u << 16) | OP_TYPE_POINTER);
				words.push_back(elem_ptr);
				words.push_back(FAN_SC_UNIFORM_CONSTANT);
				words.push_back(cand.arr->elem);
			}
		}
		{
			LocalVector<uint32_t> &var_words = emit_after[MAX(cand.info->instr_idx, elem_ptr_def_idx)];
			LocalVector<uint32_t> &deco_words = emit_after[cand.info->binding_deco_idx];
			for (uint32_t i = 0; i < N; i++) {
				if (!element_used[i]) {
					continue;
				}
				element_vars[i] = next_id++;
				var_words.push_back((4u << 16) | OP_VARIABLE);
				var_words.push_back(elem_ptr);
				var_words.push_back(element_vars[i]);
				var_words.push_back(FAN_SC_UNIFORM_CONSTANT);
				deco_words.push_back((4u << 16) | OP_DECORATE);
				deco_words.push_back(element_vars[i]);
				deco_words.push_back(FAN_DECO_DESCRIPTOR_SET);
				deco_words.push_back(cand.info->set);
				deco_words.push_back((4u << 16) | OP_DECORATE);
				deco_words.push_back(element_vars[i]);
				deco_words.push_back(FAN_DECO_BINDING);
				deco_words.push_back(p_binding_base + cand.info->binding * p_binding_stride + i);
			}
		}
		// Constant chains: drop the chain, point each consumer's operand at
		// the element variable directly.
		for (uint32_t c = 0; c < const_chain_instrs.size(); c++) {
			skip_instr.insert(const_chain_instrs[c]);
			removed_value_ids.insert(const_chain_results[c]);
		}
		for (uint32_t p = 0; p < const_patch_instrs.size(); p++) {
			operand_patch[const_patch_instrs[p]].insert(const_patch_words[p], element_vars[const_patch_elements[p]]);
		}
		// Dynamic chains and their switch constructs.
		for (uint32_t ci : dynamic_chain_instrs) {
			skip_instr.insert(ci);
		}
		for (const ChainInfo &chain : cand.chains) {
			if (int_const.has(chain.index_id)) {
				continue;
			}
			removed_value_ids.insert(chain.result);
		}
		for (const DynamicSite &site : dynamic_sites) {
			const uint32_t term_instr = site.clone_instrs[site.clone_count - 1];
			for (uint32_t c = 0; c < site.clone_count; c++) {
				skip_instr.insert(site.clone_instrs[c]);
				uint32_t result = read_word(data, len, instrs[site.clone_instrs[c]].pos + 2);
				if (result != site.term_result) {
					removed_value_ids.insert(result);
				}
			}
			LocalVector<uint32_t> &out = emit_before[term_instr];
			const uint32_t merge_label = next_id++;
			const uint32_t default_label = next_id++;
			LocalVector<uint32_t> case_labels;
			LocalVector<uint32_t> case_values; // Phi operand per case
			case_labels.resize(N);
			for (uint32_t i = 0; i < N; i++) {
				case_labels[i] = next_id++;
			}
			out.push_back((3u << 16) | FAN_OP_SELECTION_MERGE);
			out.push_back(merge_label);
			out.push_back(0); // No selection control.
			out.push_back(((3u + 2u * N) << 16) | FAN_OP_SWITCH);
			out.push_back(site.idx_id);
			out.push_back(default_label);
			for (uint32_t i = 0; i < N; i++) {
				out.push_back(i);
				out.push_back(case_labels[i]);
			}
			auto emit_case = [&](uint32_t p_label, uint32_t p_element, uint32_t &r_value) {
				out.push_back((2u << 16) | FAN_OP_LABEL);
				out.push_back(p_label);
				uint32_t prev_result = 0;
				uint32_t new_result = 0;
				for (uint32_t c = 0; c < site.clone_count; c++) {
					const Instr &src = instrs[site.clone_instrs[c]];
					uint32_t old_result = read_word(data, len, src.pos + 2);
					new_result = next_id++;
					for (uint32_t w = 0; w < src.wc; w++) {
						uint32_t word = read_word(data, len, src.pos + w);
						if (w == 2) {
							word = new_result;
						} else if (w >= 3) {
							if (c == site.var_operand_instr && w == site.var_operand_word) {
								word = element_vars[p_element];
							} else if (c > 0 && word == prev_result) {
								word = r_value; // Reuse the previous clone's result id.
							}
						}
						out.push_back(word);
					}
					prev_result = old_result;
					r_value = new_result;
				}
				out.push_back((2u << 16) | FAN_OP_BRANCH);
				out.push_back(merge_label);
			};
			for (uint32_t i = 0; i < N; i++) {
				uint32_t value = 0;
				emit_case(case_labels[i], i, value);
				case_values.push_back(value);
			}
			uint32_t default_value = 0;
			emit_case(default_label, 0, default_value);
			out.push_back((2u << 16) | FAN_OP_LABEL);
			out.push_back(merge_label);
			out.push_back(((3u + 2u * (N + 1)) << 16) | FAN_OP_PHI);
			out.push_back(site.term_type);
			out.push_back(site.term_result); // Downstream uses keep working untouched.
			for (uint32_t i = 0; i < N; i++) {
				out.push_back(case_values[i]);
				out.push_back(case_labels[i]);
			}
			out.push_back(default_value);
			out.push_back(default_label);
		}
		// Remove the original variable and its metadata.
		skip_instr.insert(cand.info->instr_idx);
		removed_value_ids.insert(cand.var_id);
		fanned_count++;
	}
	if (fanned_count == 0) {
		return p_bytes;
	}
	// Metadata cleanup for every removed id.
	for (const uint32_t removed : removed_value_ids) {
		const LocalVector<uint32_t> *meta = id_meta_instrs.getptr(removed);
		if (meta) {
			for (uint32_t idx : *meta) {
				skip_instr.insert(idx);
			}
		}
	}
	// Type GC: remove array types (and their pointer types) once no variable
	// uses them anymore -- Tint rejects even an unused handle-array type.
	for (const KeyValue<uint32_t, ArrayInfo> &arr : handle_arrays) {
		bool still_used = false;
		for (const KeyValue<uint32_t, VarInfo> &kv : vars) {
			if (removed_value_ids.has(kv.key)) {
				continue;
			}
			const PtrInfo *pt = ptr_types.getptr(kv.value.ptr_type);
			if (pt && pt->base == arr.key) {
				still_used = true;
				break;
			}
		}
		if (still_used) {
			continue;
		}
		skip_instr.insert(arr.value.instr_idx);
		const LocalVector<uint32_t> *meta = id_meta_instrs.getptr(arr.key);
		if (meta) {
			for (uint32_t idx : *meta) {
				skip_instr.insert(idx);
			}
		}
		for (const KeyValue<uint32_t, PtrInfo> &pt : ptr_types) {
			if (pt.value.base == arr.key) {
				skip_instr.insert(pt.value.instr_idx);
			}
		}
	}

	// ---- Rebuild. ----
	Vector<uint8_t> out;
	out.resize(0);
	for (uint32_t w = 0; w < 5; w++) {
		push_word(out, w == 3 ? next_id : read_word(data, len, w));
	}
	for (uint32_t i = 0; i < instrs.size(); i++) {
		const Instr &ins = instrs[i];
		const LocalVector<uint32_t> *before = emit_before.getptr(i);
		if (before) {
			for (uint32_t word : *before) {
				push_word(out, word);
			}
		}
		if (!skip_instr.has(i)) {
			const HashMap<uint32_t, uint32_t> *patches = operand_patch.getptr(i);
			for (uint32_t w = 0; w < ins.wc; w++) {
				uint32_t word = read_word(data, len, ins.pos + w);
				if (patches) {
					const uint32_t *p = patches->getptr(w);
					if (p) {
						word = *p;
					}
				}
				push_word(out, word);
			}
		}
		const LocalVector<uint32_t> *after = emit_after.getptr(i);
		if (after) {
			for (uint32_t word : *after) {
				push_word(out, word);
			}
		}
	}
	return out;
}

} // namespace spirv_preprocess
