/**************************************************************************/
/*  webgpu_probe.cpp                                                      */
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

#include "webgpu_probe.h"

#include "rendering_context_driver_webgpu.h"
#include "rendering_device_driver_webgpu.h"

#include <emscripten.h>

#include <stdio.h>

// See platform/web/js/libs/library_godot_webgpu.js.
extern "C" {
void godot_js_webgpu_probe_canvas_create();
}

static const Color PROBE_CLEAR_COLOR = Color(0.2f, 0.6f, 0.9f, 1.0f);
// The pattern uploaded through the staging path and copied over the cleared
// frame; this is the color the CI harness asserts
// (misc/webgpu_scripts/loader-smoke.mjs).
static const uint8_t PROBE_PATTERN_R = 230;
static const uint8_t PROBE_PATTERN_G = 102;
static const uint8_t PROBE_PATTERN_B = 26;
static const uint32_t PROBE_SIZE = 64;
static const uint8_t PROBE_TRIANGLE_R = 230;
static const uint8_t PROBE_TRIANGLE_G = 51;
static const uint8_t PROBE_TRIANGLE_B = 230;
static const uint32_t PROBE_ARRAY_TEXTURES = 4;
// Instanced-quad stage: replicates the canvas renderer's pattern (indexed
// unit quad, per-instance rect + color pulled from a storage buffer).
static const uint8_t PROBE_INST0_RGB[3] = { 51, 230, 76 };
static const uint8_t PROBE_INST1_RGB[3] = { 242, 217, 25 };

// Feeds hand-written WGSL through the same container the baked-shader path
// uses, so the probe exercises shader_create_from_container and
// render_pipeline_create exactly like an exported project would.
class ProbeShaderContainer : public RenderingShaderContainerWebGPU {
	GDSOFTCLASS(ProbeShaderContainer, RenderingShaderContainerWebGPU);

public:
	bool set_from_wgsl_with_storage(const char *p_vertex_wgsl, const char *p_fragment_wgsl) {
		reflection_data.stage_count = 2;
		reflection_data.set_count = 1;
		reflection_binding_set_uniforms_count.push_back(1);
		ReflectionBindingData instance_uniform;
		instance_uniform.type = RDC::UNIFORM_TYPE_STORAGE_BUFFER;
		instance_uniform.binding = 0;
		instance_uniform.stages = 1 << RDC::SHADER_STAGE_VERTEX;
		instance_uniform.length = 0;
		reflection_binding_set_uniforms_data.push_back(instance_uniform);
		reflection_shader_stages.push_back(RDC::SHADER_STAGE_VERTEX);
		reflection_shader_stages.push_back(RDC::SHADER_STAGE_FRAGMENT);
		return _compress_stages(p_vertex_wgsl, p_fragment_wgsl);
	}

	bool set_from_wgsl(const char *p_vertex_wgsl, const char *p_fragment_wgsl) {
		reflection_data.stage_count = 2;
		reflection_data.push_constant_size = 16; // One vec4 color.
		reflection_data.push_constant_stages_mask = (1 << RDC::SHADER_STAGE_VERTEX) | (1 << RDC::SHADER_STAGE_FRAGMENT);
		// An arrayed texture uniform the WGSL never references: it exercises
		// the driver's binding fan-out (layout + bind group), which WebGPU
		// validates even for bindings the shader does not use.
		reflection_data.set_count = 1;
		reflection_binding_set_uniforms_count.push_back(1);
		ReflectionBindingData array_uniform;
		array_uniform.type = RDC::UNIFORM_TYPE_TEXTURE;
		array_uniform.binding = 0;
		array_uniform.stages = 1 << RDC::SHADER_STAGE_FRAGMENT;
		array_uniform.length = PROBE_ARRAY_TEXTURES;
		array_uniform.texture_type = RDC::TEXTURE_TYPE_2D;
		array_uniform.texture_format = RDC::DATA_FORMAT_R8G8B8A8_UNORM;
		reflection_binding_set_uniforms_data.push_back(array_uniform);
		// A dynamic uniform buffer: the triangle's tint is read from the
		// per-frame slice the dynamic offset selects.
		reflection_binding_set_uniforms_count.ptrw()[0] = 2;
		ReflectionBindingData tint_uniform;
		tint_uniform.type = RDC::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC;
		tint_uniform.binding = 1;
		tint_uniform.stages = 1 << RDC::SHADER_STAGE_FRAGMENT;
		tint_uniform.length = 16;
		reflection_binding_set_uniforms_data.push_back(tint_uniform);
		reflection_shader_stages.push_back(RDC::SHADER_STAGE_VERTEX);
		reflection_shader_stages.push_back(RDC::SHADER_STAGE_FRAGMENT);

		return _compress_stages(p_vertex_wgsl, p_fragment_wgsl);
	}

	bool _compress_stages(const char *p_vertex_wgsl, const char *p_fragment_wgsl) {
		const char *sources[2] = { p_vertex_wgsl, p_fragment_wgsl };
		const RDC::ShaderStage stages[2] = { RDC::SHADER_STAGE_VERTEX, RDC::SHADER_STAGE_FRAGMENT };
		shaders.resize(2);
		for (int i = 0; i < 2; i++) {
			Shader &shader = shaders.ptrw()[i];
			shader.shader_stage = stages[i];
			const uint32_t size = (uint32_t)strlen(sources[i]);
			shader.code_decompressed_size = size;
			shader.code_compressed_bytes.resize(size);
			uint32_t compressed_size = 0;
			if (!compress_code((const uint8_t *)sources[i], size, shader.code_compressed_bytes.ptrw(), &compressed_size, &shader.code_compression_flags)) {
				return false;
			}
			shader.code_compressed_bytes.resize(compressed_size);
		}
		return true;
	}
};

#define PROBE_FAIL(m_stage, m_msg)                        \
	{                                                     \
		printf("WebGPU probe: FAILED (%s)\n", m_msg);     \
		fflush(stdout);                                   \
		return m_stage;                                   \
	}

extern "C" EMSCRIPTEN_KEEPALIVE int godot_webgpu_probe() {
	godot_js_webgpu_probe_canvas_create();

	// Heap-allocated: on success the context (and the device it imported)
	// stays alive with the swap chain, both because the presented frame must
	// remain on the probe canvas and because releasing the last device
	// reference destroys the page's GPUDevice in the emdawnwebgpu bindings.
	RenderingContextDriverWebGPU *context = memnew(RenderingContextDriverWebGPU);
	if (context->initialize() != OK) {
		memdelete(context);
		PROBE_FAIL(1, "no pre-initialized WebGPU device");
	}

	RenderingContextDriverWebGPU::WindowPlatformData wpd;
	wpd.canvas_selector = "#godot-webgpu-probe";
	const DisplayServerEnums::WindowID window_id = DisplayServerEnums::WindowID(0);
	if (context->window_create(window_id, &wpd) != OK) {
		memdelete(context);
		PROBE_FAIL(2, "surface creation failed");
	}
	context->window_set_size(window_id, PROBE_SIZE, PROBE_SIZE);
	RenderingContextDriver::SurfaceID surface = context->surface_get_from_window(window_id);

	RenderingDeviceDriverWebGPU *driver = (RenderingDeviceDriverWebGPU *)context->driver_create();
	if (driver->initialize(0, 2) != OK) {
		context->driver_free(driver);
		context->window_destroy(window_id);
		memdelete(context);
		PROBE_FAIL(3, "driver initialization failed");
	}

	int stage = 0;
	RenderingDeviceDriver::CommandQueueFamilyID family = driver->command_queue_family_get(RenderingDeviceDriver::COMMAND_QUEUE_FAMILY_GRAPHICS_BIT, surface);
	RenderingDeviceDriver::CommandQueueID cmd_queue = driver->command_queue_create(family, true);
	RenderingDeviceDriver::SwapChainID swap_chain = driver->swap_chain_create(surface);
	RenderingDeviceDriver::CommandPoolID cmd_pool = driver->command_pool_create(family, RenderingDeviceDriver::COMMAND_BUFFER_TYPE_PRIMARY);
	RenderingDeviceDriver::CommandBufferID cmd_buffer = driver->command_buffer_create(cmd_pool);

	if (!cmd_queue || !swap_chain || !cmd_pool || !cmd_buffer) {
		stage = 4;
	}

	if (stage == 0 && driver->swap_chain_resize(cmd_queue, swap_chain, 2) != OK) {
		stage = 5;
	}

	RenderingDeviceDriver::FramebufferID framebuffer;
	if (stage == 0) {
		bool resize_required = false;
		framebuffer = driver->swap_chain_acquire_framebuffer(cmd_queue, swap_chain, resize_required);
		if (!framebuffer) {
			stage = 6;
		}
	}

	if (stage == 0 && !driver->command_buffer_begin(cmd_buffer)) {
		stage = 7;
	}

	if (stage == 0) {
		RenderingDeviceDriver::RenderPassID render_pass = driver->swap_chain_get_render_pass(swap_chain);
		RenderingDeviceDriver::RenderPassClearValue clear_value;
		clear_value.color = PROBE_CLEAR_COLOR;
		driver->command_begin_render_pass(cmd_buffer, render_pass, framebuffer, RenderingDeviceDriver::COMMAND_BUFFER_TYPE_PRIMARY, Rect2i(0, 0, PROBE_SIZE, PROBE_SIZE), clear_value);
		driver->command_end_render_pass(cmd_buffer);
	}

	// Stage the pattern through the full upload path: shadow map, queue
	// write, buffer-to-texture copy, then texture-to-texture copy over the
	// cleared frame. The copies record after the render pass and execute in
	// submission order.
	RenderingDeviceDriver::BufferID staging;
	RenderingDeviceDriver::TextureID pattern_texture;
	RenderingDeviceDriver::TextureID swap_chain_texture;
	if (stage == 0) {
		const RenderingDeviceDriver::DataFormat format = driver->swap_chain_get_format(swap_chain);
		staging = driver->buffer_create(PROBE_SIZE * PROBE_SIZE * 4, RenderingDeviceDriver::BUFFER_USAGE_TRANSFER_FROM_BIT, RenderingDeviceDriver::MEMORY_ALLOCATION_TYPE_CPU, 0);
		uint8_t *pixels = staging ? driver->buffer_map(staging) : nullptr;
		if (pixels == nullptr) {
			stage = 9;
		} else {
			const bool bgra = format == RenderingDeviceDriver::DATA_FORMAT_B8G8R8A8_UNORM;
			for (uint32_t i = 0; i < PROBE_SIZE * PROBE_SIZE; i++) {
				pixels[i * 4 + 0] = bgra ? PROBE_PATTERN_B : PROBE_PATTERN_R;
				pixels[i * 4 + 1] = PROBE_PATTERN_G;
				pixels[i * 4 + 2] = bgra ? PROBE_PATTERN_R : PROBE_PATTERN_B;
				pixels[i * 4 + 3] = 255;
			}
			driver->buffer_unmap(staging);

			RenderingDeviceDriver::TextureFormat texture_format;
			texture_format.format = format;
			texture_format.width = PROBE_SIZE;
			texture_format.height = PROBE_SIZE;
			texture_format.usage_bits = RenderingDeviceDriver::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDeviceDriver::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
			RenderingDeviceDriver::TextureView texture_view;
			texture_view.format = format;
			pattern_texture = driver->texture_create(texture_format, texture_view);
			swap_chain_texture = driver->texture_create_from_extension(driver->swap_chain_get_current_texture_handle(swap_chain), RenderingDeviceDriver::TEXTURE_TYPE_2D, format, 1, false, 1);
			if (!pattern_texture || !swap_chain_texture) {
				stage = 10;
			} else {
				RenderingDeviceDriver::BufferTextureCopyRegion upload_region;
				upload_region.row_pitch = PROBE_SIZE * 4; // 256-byte aligned at PROBE_SIZE = 64.
				upload_region.texture_region_size = Vector3i(PROBE_SIZE, PROBE_SIZE, 1);
				driver->command_copy_buffer_to_texture(cmd_buffer, staging, pattern_texture, RenderingDeviceDriver::TEXTURE_LAYOUT_COPY_DST_OPTIMAL, upload_region);

				RenderingDeviceDriver::TextureCopyRegion blit_region;
				blit_region.size = Vector3i(PROBE_SIZE, PROBE_SIZE, 1);
				driver->command_copy_texture(cmd_buffer, pattern_texture, RenderingDeviceDriver::TEXTURE_LAYOUT_COPY_SRC_OPTIMAL, swap_chain_texture, RenderingDeviceDriver::TEXTURE_LAYOUT_COPY_DST_OPTIMAL, blit_region);
			}
		}
	}

	// Drive a full draw through the same path exported projects use: WGSL
	// container -> shader -> pipeline -> push constants -> draw, composited
	// over the pattern so the copy checks above stay verifiable at (0,0)
	// while the triangle owns the canvas center.
	RenderingDeviceDriver::ShaderID triangle_shader;
	RenderingDeviceDriver::PipelineID triangle_pipeline;
	if (stage == 0) {
		static const char *vertex_wgsl =
				"struct PC { color : vec4f, }\n"
				"@group(0) @binding(510) var<storage, read> pc : PC;\n"
				"@vertex fn main(@builtin(vertex_index) vi : u32) -> @builtin(position) vec4f {\n"
				"    var positions = array<vec2f, 3>(vec2f(0.0, 0.6), vec2f(-0.6, -0.6), vec2f(0.6, -0.6));\n"
				"    // Scale by the push constant's alpha (1.0): a garbage read\n"
				"    // collapses the triangle and fails the strict pixel check.\n"
				"    return vec4f(positions[vi] * pc.color.a, 0.0, 1.0);\n"
				"}\n";
		static const char *fragment_wgsl =
				"struct PC { color : vec4f, }\n"
				"@group(0) @binding(510) var<storage, read> pc : PC;\n"
				"struct Tint { color : vec4f, }\n"
				"@group(0) @binding(1) var<uniform> tint : Tint;\n"
				"@fragment fn main() -> @location(0) vec4f {\n"
				"    return pc.color * tint.color;\n"
				"}\n";
		Ref<ProbeShaderContainer> container;
		container.instantiate();
		if (!container->set_from_wgsl(vertex_wgsl, fragment_wgsl)) {
			stage = 12;
		} else {
			triangle_shader = driver->shader_create_from_container(container, Vector<RenderingDeviceDriver::ImmutableSampler>());
			if (!triangle_shader) {
				stage = 12;
			}
		}
	}

	if (stage == 0) {
		const int32_t color_attachment = 0;
		triangle_pipeline = driver->render_pipeline_create(
				triangle_shader,
				RenderingDeviceDriver::VertexFormatID(),
				RenderingDeviceDriver::RENDER_PRIMITIVE_TRIANGLES,
				RenderingDeviceDriver::PipelineRasterizationState(),
				RenderingDeviceDriver::PipelineMultisampleState(),
				RenderingDeviceDriver::PipelineDepthStencilState(),
				RenderingDeviceDriver::PipelineColorBlendState::create_disabled(1),
				color_attachment,
				BitField<RenderingDeviceDriver::PipelineDynamicStateFlags>(),
				driver->swap_chain_get_render_pass(swap_chain),
				0,
				VectorView<RenderingDeviceDriver::PipelineSpecializationConstant>());
		if (!triangle_pipeline) {
			stage = 13;
		}
	}

	// The triangle's set 0 carries the unreferenced texture array (fan-out
	// coverage) and the push-constant ring entry.
	RenderingDeviceDriver::TextureID array_textures[PROBE_ARRAY_TEXTURES];
	RenderingDeviceDriver::UniformSetID triangle_uniform_set;
	RenderingDeviceDriver::BufferID tint_buffer;
	uint32_t triangle_dynamic_offsets = 0;
	if (stage == 0) {
		// The advanced slice gets an opaque white tint; the slice at offset 0
		// stays zeroed, so selecting the wrong slice blacks out the triangle
		// and fails the strict pixel check.
		tint_buffer = driver->buffer_create(16, BitField<RenderingDeviceDriver::BufferUsageBits>(RenderingDeviceDriver::BUFFER_USAGE_UNIFORM_BIT | RenderingDeviceDriver::BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT), RenderingDeviceDriver::MEMORY_ALLOCATION_TYPE_CPU, 0);
		float *tint_values = tint_buffer ? (float *)driver->buffer_persistent_map_advance(tint_buffer, 0) : nullptr;
		if (tint_values == nullptr) {
			stage = 16;
		} else {
			tint_values[0] = 1.0f;
			tint_values[1] = 1.0f;
			tint_values[2] = 1.0f;
			tint_values[3] = 1.0f;
			driver->buffer_flush(tint_buffer);
			printf("WebGPU probe: dynamic buffer OK\n");
			fflush(stdout);
		}
	}
	if (stage == 0) {
		RenderingDeviceDriver::TextureFormat array_format;
		array_format.format = RenderingDeviceDriver::DATA_FORMAT_R8G8B8A8_UNORM;
		array_format.width = 4;
		array_format.height = 4;
		array_format.usage_bits = RenderingDeviceDriver::TEXTURE_USAGE_SAMPLING_BIT;
		RenderingDeviceDriver::TextureView array_view;
		array_view.format = array_format.format;
		RenderingDeviceDriver::BoundUniform array_uniform;
		array_uniform.type = RenderingDeviceDriver::UNIFORM_TYPE_TEXTURE;
		array_uniform.binding = 0;
		for (uint32_t t = 0; t < PROBE_ARRAY_TEXTURES; t++) {
			array_textures[t] = driver->texture_create(array_format, array_view);
			if (!array_textures[t]) {
				stage = 14;
				break;
			}
			array_uniform.ids.push_back(array_textures[t]);
		}
		if (stage == 0) {
			RenderingDeviceDriver::BoundUniform tint_bound;
			tint_bound.type = RenderingDeviceDriver::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC;
			tint_bound.binding = 1;
			tint_bound.ids.push_back(tint_buffer);
			RenderingDeviceDriver::BoundUniform set_uniforms[2] = { array_uniform, tint_bound };
			triangle_uniform_set = driver->uniform_set_create(VectorView<RenderingDeviceDriver::BoundUniform>(set_uniforms, 2), triangle_shader, 0, -1);
			if (!triangle_uniform_set) {
				stage = 15;
			} else {
				triangle_dynamic_offsets = driver->uniform_sets_get_dynamic_offsets(triangle_uniform_set, triangle_shader, 0, 1);
				printf("WebGPU probe: uniform set OK\n");
				fflush(stdout);
			}
		}
	}

	// Instanced-quad shader: indexed draw, per-instance data from an SSBO.
	RenderingDeviceDriver::ShaderID quad_shader;
	RenderingDeviceDriver::PipelineID quad_pipeline;
	RenderingDeviceDriver::BufferID quad_index_buffer;
	RenderingDeviceDriver::BufferID quad_instance_buffer;
	RenderingDeviceDriver::UniformSetID quad_uniform_set;
	if (stage == 0) {
		static const char *quad_vertex_wgsl =
				"struct Inst { rect : vec4f, color : vec4f, }\n"
				"struct Insts { data : array<Inst>, }\n"
				"@group(0) @binding(0) var<storage, read> insts : Insts;\n"
				"struct VOut { @builtin(position) pos : vec4f, @location(0) color : vec4f, }\n"
				"@vertex fn main(@builtin(vertex_index) vi : u32, @builtin(instance_index) ii : u32) -> VOut {\n"
				"    let r = insts.data[ii].rect;\n"
				"    var corners = array<vec2f, 4>(vec2f(r.x, r.y), vec2f(r.z, r.y), vec2f(r.z, r.w), vec2f(r.x, r.w));\n"
				"    var out : VOut;\n"
				"    out.pos = vec4f(corners[vi], 0.0, 1.0);\n"
				"    out.color = insts.data[ii].color;\n"
				"    return out;\n"
				"}\n";
		static const char *quad_fragment_wgsl =
				"@fragment fn main(@location(0) color : vec4f) -> @location(0) vec4f {\n"
				"    return color;\n"
				"}\n";
		Ref<ProbeShaderContainer> quad_container;
		quad_container.instantiate();
		if (!quad_container->set_from_wgsl_with_storage(quad_vertex_wgsl, quad_fragment_wgsl)) {
			stage = 17;
		} else {
			quad_shader = driver->shader_create_from_container(quad_container, Vector<RenderingDeviceDriver::ImmutableSampler>());
			if (!quad_shader) {
				stage = 17;
			}
		}
	}
	if (stage == 0) {
		const int32_t color_attachment = 0;
		quad_pipeline = driver->render_pipeline_create(
				quad_shader,
				RenderingDeviceDriver::VertexFormatID(),
				RenderingDeviceDriver::RENDER_PRIMITIVE_TRIANGLES,
				RenderingDeviceDriver::PipelineRasterizationState(),
				RenderingDeviceDriver::PipelineMultisampleState(),
				RenderingDeviceDriver::PipelineDepthStencilState(),
				RenderingDeviceDriver::PipelineColorBlendState::create_disabled(1),
				color_attachment,
				BitField<RenderingDeviceDriver::PipelineDynamicStateFlags>(),
				driver->swap_chain_get_render_pass(swap_chain),
				0,
				VectorView<RenderingDeviceDriver::PipelineSpecializationConstant>());
		if (!quad_pipeline) {
			stage = 18;
		}
	}
	if (stage == 0) {
		quad_index_buffer = driver->buffer_create(6 * sizeof(uint16_t), RenderingDeviceDriver::BUFFER_USAGE_INDEX_BIT, RenderingDeviceDriver::MEMORY_ALLOCATION_TYPE_CPU, 0);
		quad_instance_buffer = driver->buffer_create(2 * 8 * sizeof(float), RenderingDeviceDriver::BUFFER_USAGE_STORAGE_BIT, RenderingDeviceDriver::MEMORY_ALLOCATION_TYPE_CPU, 0);
		uint16_t *indices = quad_index_buffer ? (uint16_t *)driver->buffer_map(quad_index_buffer) : nullptr;
		float *instances = quad_instance_buffer ? (float *)driver->buffer_map(quad_instance_buffer) : nullptr;
		if (indices == nullptr || instances == nullptr) {
			stage = 19;
		} else {
			const uint16_t quad_indices[6] = { 0, 1, 2, 0, 2, 3 };
			memcpy(indices, quad_indices, sizeof(quad_indices));
			driver->buffer_unmap(quad_index_buffer);
			const float quad_instances[16] = {
				-0.9f, -0.2f, -0.5f, 0.2f, PROBE_INST0_RGB[0] / 255.0f, PROBE_INST0_RGB[1] / 255.0f, PROBE_INST0_RGB[2] / 255.0f, 1.0f, // Left rect, green.
				0.5f, -0.2f, 0.9f, 0.2f, PROBE_INST1_RGB[0] / 255.0f, PROBE_INST1_RGB[1] / 255.0f, PROBE_INST1_RGB[2] / 255.0f, 1.0f, // Right rect, yellow.
			};
			memcpy(instances, quad_instances, sizeof(quad_instances));
			driver->buffer_unmap(quad_instance_buffer);
			RenderingDeviceDriver::BoundUniform instance_uniform;
			instance_uniform.type = RenderingDeviceDriver::UNIFORM_TYPE_STORAGE_BUFFER;
			instance_uniform.binding = 0;
			instance_uniform.ids.push_back(quad_instance_buffer);
			quad_uniform_set = driver->uniform_set_create(VectorView<RenderingDeviceDriver::BoundUniform>(&instance_uniform, 1), quad_shader, 0, -1);
			if (!quad_uniform_set) {
				stage = 19;
			} else {
				printf("WebGPU probe: instanced quads ready\n");
				fflush(stdout);
			}
		}
	}

	if (stage == 0) {
		// No clear values: the swap chain pass loads the pattern frame.
		driver->command_begin_render_pass(cmd_buffer, driver->swap_chain_get_render_pass(swap_chain), framebuffer, RenderingDeviceDriver::COMMAND_BUFFER_TYPE_PRIMARY, Rect2i(0, 0, PROBE_SIZE, PROBE_SIZE), VectorView<RenderingDeviceDriver::RenderPassClearValue>());
		driver->command_bind_render_pipeline(cmd_buffer, triangle_pipeline);
		driver->command_bind_render_uniform_sets(cmd_buffer, triangle_uniform_set, triangle_shader, 0, 1, triangle_dynamic_offsets);
		const float triangle_color[4] = { PROBE_TRIANGLE_R / 255.0f, PROBE_TRIANGLE_G / 255.0f, PROBE_TRIANGLE_B / 255.0f, 1.0f };
		driver->command_bind_push_constants(cmd_buffer, triangle_shader, 0, VectorView<uint32_t>((const uint32_t *)triangle_color, 4));
		driver->command_render_draw(cmd_buffer, 3, 1, 0, 0);
		driver->command_bind_render_pipeline(cmd_buffer, quad_pipeline);
		driver->command_bind_render_uniform_sets(cmd_buffer, quad_uniform_set, quad_shader, 0, 1, 0);
		driver->command_render_bind_index_buffer(cmd_buffer, quad_index_buffer, RenderingDeviceDriver::INDEX_BUFFER_FORMAT_UINT16, 0);
		driver->command_render_draw_indexed(cmd_buffer, 6, 2, 0, 0, 0);
		printf("WebGPU probe: instanced quads OK\n");
		fflush(stdout);
		driver->command_end_render_pass(cmd_buffer);
		printf("WebGPU probe: triangle OK\n");
		fflush(stdout);
	}

	if (stage == 0) {
		driver->command_buffer_end(cmd_buffer);
		if (driver->command_queue_execute_and_present(cmd_queue, VectorView<RenderingDeviceDriver::SemaphoreID>(), cmd_buffer, VectorView<RenderingDeviceDriver::SemaphoreID>(), RenderingDeviceDriver::FenceID(), swap_chain) != OK) {
			stage = 8;
		}
	}

	// Validate WGSL shader module creation (including an override, the WGSL
	// form of specialization constants). Uses the WebGPU API directly: the
	// driver-level path needs a baked shader container, which only exists in
	// exported projects (see docs/webgpu-testing.md).
	if (stage == 0) {
		static const char *probe_wgsl =
				"@id(0) override probe_flag : bool = false;\n"
				"@fragment fn main() -> @location(0) vec4f {\n"
				"    return select(vec4f(0.0), vec4f(1.0), probe_flag);\n"
				"}\n";
		WGPUShaderSourceWGSL wgsl_source = WGPU_SHADER_SOURCE_WGSL_INIT;
		wgsl_source.code = { probe_wgsl, WGPU_STRLEN };
		WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		module_desc.nextInChain = &wgsl_source.chain;
		WGPUShaderModule module = wgpuDeviceCreateShaderModule(context->get_device(), &module_desc);
		if (module == nullptr) {
			stage = 11;
		} else {
			wgpuShaderModuleRelease(module);
			printf("WebGPU probe: shader module OK\n");
			fflush(stdout);
		}
	}

	if (triangle_uniform_set) {
		driver->uniform_set_free(triangle_uniform_set);
	}
	if (tint_buffer) {
		driver->buffer_free(tint_buffer);
	}
	if (quad_uniform_set) {
		driver->uniform_set_free(quad_uniform_set);
	}
	if (quad_index_buffer) {
		driver->buffer_free(quad_index_buffer);
	}
	if (quad_instance_buffer) {
		driver->buffer_free(quad_instance_buffer);
	}
	if (quad_pipeline) {
		driver->pipeline_free(quad_pipeline);
	}
	if (quad_shader) {
		driver->shader_free(quad_shader);
	}
	for (uint32_t t = 0; t < PROBE_ARRAY_TEXTURES; t++) {
		if (array_textures[t]) {
			driver->texture_free(array_textures[t]);
		}
	}
	if (triangle_pipeline) {
		driver->pipeline_free(triangle_pipeline);
	}
	if (triangle_shader) {
		driver->shader_free(triangle_shader);
	}
	if (staging) {
		driver->buffer_free(staging);
	}
	if (pattern_texture) {
		driver->texture_free(pattern_texture);
	}
	if (swap_chain_texture) {
		driver->texture_free(swap_chain_texture);
	}

	if (cmd_pool) {
		driver->command_pool_free(cmd_pool);
	}

	if (stage != 0) {
		if (swap_chain) {
			driver->swap_chain_free(swap_chain);
		}
		context->window_destroy(window_id);
		context->driver_free(driver);
		memdelete(context);
		PROBE_FAIL(stage, "swap chain presentation failed");
	}

	// On success the context, device, swap chain, and surface are
	// deliberately kept alive for the lifetime of the page: unconfiguring the
	// canvas context or destroying the device erases the just-presented frame
	// (and the device belongs to the page), and the probe canvas is meant to
	// keep displaying it (CI reads the pixels back).
	context->driver_free(driver);
	printf("WebGPU probe: OK (presented clear frame)\n");
	fflush(stdout);
	return 0;
}
