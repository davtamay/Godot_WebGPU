/**************************************************************************/
/*  spirv_preprocess.h                                                    */
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

#pragma once

#include "core/templates/vector.h"

#include <cstdint>

namespace spirv_preprocess {

// Evaluate OpSpecConstantOp instructions with default values and replace
// them with regular OpConstant instructions. Also converts OpSpecConstant*
// to their non-specialization equivalents and strips SpecId decorations.
Vector<uint8_t> freeze_spec_constant_ops(const Vector<uint8_t> &p_bytes);

// Negate the Y component of gl_Position in vertex shaders.
// Compensates for the difference between Vulkan's Y-down NDC (which
// Godot's GLSL shaders target) and WebGPU's Y-up NDC.
// Without this, all rendered content appears flipped vertically.
// Tint has no built-in coordinate space adjustment option, so this
// is done as a SPIR-V preprocessing pass instead.
Vector<uint8_t> negate_position_y(const Vector<uint8_t> &p_bytes);

// Lower gl_ViewIndex to constant zero. WebGPU multiview is not supported by
// this backend yet, but generic shader variant baking can still produce
// multiview variants. Tint rejects the SPIR-V ViewIndex builtin, so bake those
// variants as single-view equivalents instead.
Vector<uint8_t> lower_view_index_to_zero(const Vector<uint8_t> &p_bytes);

// Unwrap arrays of handle types (images, samplers, sampled images)
// into single variables. Tint does not support arrays of handle types.
// Rewrites pointer types, removes access chains, and updates loads.
Vector<uint8_t> flatten_binding_arrays(const Vector<uint8_t> &p_bytes);

// Replace OpCopyLogical (SPIR-V 1.4, rejected by Tint) with OpCopyObject,
// which has the identical word layout. Emitted by glslang for copies between
// structurally-identical struct types (clustered scene shader vertex stages).
Vector<uint8_t> rewrite_copy_logical(const Vector<uint8_t> &p_bytes);

// Drop NonWritable decorations targeting Function-storage variables
// (a SPIR-V 1.4-legal glslang hint that Tint's 1.3-era reader rejects).
Vector<uint8_t> strip_nonwritable_on_function_vars(const Vector<uint8_t> &p_bytes);

// Lower subgroup (GroupNonUniform) operations to exact single-invocation
// semantics: reductions/broadcasts become the value itself, ballots become
// a one-lane mask. Correct for the clustered renderer's use of subgroups as
// a wave-coherence optimization over idempotent merges. Unsupported subgroup
// instructions bail the pass, leaving the variant for bake exclusion.
Vector<uint8_t> lower_subgroup_ops_to_single_invocation(const Vector<uint8_t> &p_bytes);

// Replace the HelperInvocation builtin input with a Private variable
// initialized to false (reproduces upstream's sc_use_helper_check-off mode).
Vector<uint8_t> lower_helper_invocation_to_false(const Vector<uint8_t> &p_bytes);

// Fan arrays of handle types out into one standalone variable per element at
// binding p_binding_base + original_binding * p_binding_stride + element,
// matching the layout the WebGPU driver builds for arrayed uniforms.
// Constant indices rewrite to the element variable directly; dynamic indices
// lower to a structured OpSwitch cloning the consuming instructions per
// element. Arrays whose uses do not match a supported shape are left intact
// for the flatten_binding_arrays fallback.
Vector<uint8_t> fan_out_binding_arrays(const Vector<uint8_t> &p_bytes, uint32_t p_binding_base, uint32_t p_binding_stride, uint32_t p_max_binding);

// Convert OpSpecConstantOp expressions into regular instructions cloned into
// each using function, keeping plain OpSpecConstant as live WGSL overrides.
Vector<uint8_t> lower_spec_constant_ops_to_runtime(const Vector<uint8_t> &p_bytes);

} // namespace spirv_preprocess
