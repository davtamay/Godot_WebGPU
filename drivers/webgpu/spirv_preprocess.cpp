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

} // namespace spirv_preprocess
