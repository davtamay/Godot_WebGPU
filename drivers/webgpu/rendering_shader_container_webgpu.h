/**************************************************************************/
/*  rendering_shader_container_webgpu.h                                   */
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

#include "servers/rendering/rendering_shader_container.h"

// Serializes shaders as WGSL translated from SPIR-V by an external Tint
// binary at bake time (see docs/webgpu-testing.md). No webgpu.h dependency:
// this compiles into editor builds on every platform so any editor can bake
// web exports, mirroring the Metal/D3D12 shader containers. At runtime only
// the deserialization path is used; translating on the web platform itself
// is not possible.
class RenderingShaderContainerWebGPU : public RenderingShaderContainer {
	GDSOFTCLASS(RenderingShaderContainerWebGPU, RenderingShaderContainer);

public:
	static const uint32_t FORMAT_VERSION;

	// Fixed binding remaps agreed between the SPIR-V pre-transform applied
	// before translation and the runtime driver's bind group layouts:
	// - Push constants become a read-only storage buffer at this group and
	//   binding (Tint rejects the PushConstant storage class, and their std430
	//   packing is not legal in a uniform buffer).
	// - Combined image samplers are split by Tint: within each group, a
	//   binding moves up by the number of combined samplers at lower
	//   bindings, and each synthesized sampler lands right after its texture.
	static const uint32_t PUSH_CONSTANT_GROUP = 0;
	static const uint32_t PUSH_CONSTANT_BINDING = 510;
	// Arrays of textures/samplers cannot be expressed in WGSL; each element
	// becomes its own binding in a reserved high range (Godot's binding
	// numbering is dense, so consecutive slots after the array's binding
	// would collide with its neighbors). The stride bounds the largest
	// supported array; the scene shader's lightmap_textures spans
	// MAX_LIGHTMAP_TEXTURES * 2 = 32 elements.
	static const uint32_t ARRAY_BINDING_BASE = 512;
	static const uint32_t ARRAY_BINDING_STRIDE = 32;
	// WebGPU's default maxBindingsPerBindGroup; fan-outs that would land a
	// binding number at or above this stay on the truncation fallback.
	static const uint32_t MAX_FANNED_BINDING = 1000;

	// Bake-time only; when empty (runtime), only from_bytes() works.
	String tint_path;

protected:
	virtual uint32_t _format() const override;
	virtual uint32_t _format_version() const override;
	virtual bool _set_code_from_spirv(const ReflectShader &p_shader) override;

private:
	bool _transform_spirv(Vector<uint8_t> &r_spirv) const;
};

class RenderingShaderContainerFormatWebGPU : public RenderingShaderContainerFormat {
	String tint_path;

public:
	void set_tint_path(const String &p_tint_path);

	virtual Ref<RenderingShaderContainer> create_container() const override;
	virtual ShaderLanguageVersion get_shader_language_version() const override;
	virtual ShaderSpirvVersion get_shader_spirv_version() const override;
};
