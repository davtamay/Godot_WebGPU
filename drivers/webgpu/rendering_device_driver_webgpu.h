/**************************************************************************/
/*  rendering_device_driver_webgpu.h                                      */
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

#include "rendering_context_driver_webgpu.h"
#include "rendering_shader_container_webgpu.h"

#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_device_driver.h"

#include <webgpu/webgpu.h>

// WebGPU rendering device driver. The command recording, swap chain, and
// presentation paths are implemented; everything else still fails with
// ERR_UNAVAILABLE-style errors (inline stubs below) and is implemented patch
// by patch (see docs/webgpu-patch-stack.md).
class RenderingDeviceDriverWebGPU : public RenderingDeviceDriver {
private:
	static constexpr const char *UNIMPLEMENTED = "The WebGPU rendering device driver is not implemented yet.";

	struct BufferInfo {
		WGPUBuffer buffer = nullptr;
		uint64_t size = 0;
		// Dynamic (persistently mapped) buffers hold one slice per frame in
		// flight; binds select the slice with a dynamic offset of
		// frame_idx * slice_stride (stride is alignment-padded, the binding
		// window keeps the requested size).
		bool dynamic = false;
		// Set by buffer_persistent_map_advance; the CPU writes that follow
		// have no flush call on coherent-memory platforms, so the driver
		// flushes dirty slices itself at submission.
		bool dirty = false;
		uint64_t slice_size = 0;
		uint64_t slice_stride = 0;
		uint32_t slice_count = 1;
		uint32_t frame_idx = 0;
		// CPU-visible buffers keep a malloc'd shadow: WebGPU has no
		// synchronous map, so buffer_map() returns the shadow and
		// buffer_unmap() flushes it with wgpuQueueWriteBuffer (which executes
		// before any subsequently submitted command buffer, matching the
		// engine's staging semantics). Downloads are not supported yet.
		uint8_t *shadow = nullptr;
	};

	struct TextureInfo {
		// WebGPU has no view component swizzle; the driver emulates the
		// patterns the engine uses (LA8/L8/A8 images) by promoting the
		// texture to RGBA8 and expanding texels at upload time.
		enum SwizzleExpand : uint8_t {
			SWIZZLE_EXPAND_NONE,
			SWIZZLE_EXPAND_RG_TO_RRRG,
			SWIZZLE_EXPAND_R_TO_RRR1,
			SWIZZLE_EXPAND_R_TO_000R,
		};
		WGPUTexture texture = nullptr;
		WGPUTextureView view = nullptr;
		WGPUTextureViewDimension view_dimension = WGPUTextureViewDimension_2D;
		WGPUTextureUsage usage = WGPUTextureUsage_None;
		WGPUTextureFormat wgpu_format = WGPUTextureFormat_Undefined;
		DataFormat format = DATA_FORMAT_MAX;
		uint64_t allocation_size = 0;
		bool owned = true; // False for textures wrapped from external handles.
		SwizzleExpand swizzle_expand = SWIZZLE_EXPAND_NONE;
	};

	struct ShaderInfo {
		LocalVector<WGPUShaderModule> modules;
		LocalVector<ShaderStage> module_stages;
		LocalVector<WGPUBindGroupLayout> bind_group_layouts;
		WGPUPipelineLayout pipeline_layout = nullptr;
		// Constant IDs for WGSL `override` expressions, in reflection order;
		// pipeline creation keys WGPUConstantEntry by their decimal strings.
		LocalVector<uint32_t> specialization_constant_ids;
		uint32_t push_constant_size = 0;
		// Created for shaders whose set 0 holds nothing but the reserved
		// push-constant binding, so draws can still bind the ring buffer.
		WGPUBindGroup push_constant_bind_group = nullptr;
	};

	static const uint32_t MAX_BIND_GROUPS = 4;
	static const uint32_t PUSH_CONSTANT_SLOT_SIZE = 256; // Matches the dynamic offset alignment limit.
	static const uint32_t PUSH_CONSTANT_RING_SIZE = 256 * 1024;

	struct UniformSetInfo;

	struct CommandBufferInfo {
		WGPUCommandEncoder encoder = nullptr;
		WGPUCommandBuffer command_buffer = nullptr;
		WGPURenderPassEncoder render_pass_encoder = nullptr;
		WGPUComputePassEncoder compute_pass_encoder = nullptr;
		// Bind groups are deferred to draw/dispatch time: the group that
		// carries the push-constant ring entry needs a dynamic offset that
		// only arrives with command_bind_push_constants, which the engine
		// records after binding uniform sets.
		const ShaderInfo *current_shader = nullptr;
		UniformSetInfo *pending_bind_groups[MAX_BIND_GROUPS] = {};
		LocalVector<uint32_t> pending_dynamic_offsets[MAX_BIND_GROUPS];
		uint32_t push_constant_offset = 0;
		bool bind_groups_dirty = false;
	};

	struct CommandPoolInfo {
		CommandBufferType buffer_type = COMMAND_BUFFER_TYPE_PRIMARY;
		LocalVector<CommandBufferInfo *> command_buffers;
	};

	struct RenderPassAttachment {
		WGPUTextureFormat format = WGPUTextureFormat_Undefined;
		WGPULoadOp load_op = WGPULoadOp_Clear;
		WGPUStoreOp store_op = WGPUStoreOp_Store;
		WGPULoadOp stencil_load_op = WGPULoadOp_Clear;
		WGPUStoreOp stencil_store_op = WGPUStoreOp_Store;
		bool is_depth_stencil = false;
	};

	struct RenderPassInfo {
		LocalVector<RenderPassAttachment> attachments;
		bool from_swap_chain = false;
	};

	struct FramebufferInfo {
		LocalVector<WGPUTextureView> views;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct VertexFormatInfo {
		LocalVector<LocalVector<WGPUVertexAttribute>> attributes;
		LocalVector<WGPUVertexBufferLayout> buffer_layouts;
	};

	struct UniformSetInfo {
		WGPUBindGroup bind_group = nullptr;
		// Dynamic buffers in binding order; their offsets are decoded from
		// the packed mask at bind time.
		LocalVector<BufferInfo *> dynamic_buffers;
		// True when the group carries the push-constant ring buffer entry and
		// therefore needs the current dynamic offset when bound.
		bool has_push_constant_offset = false;
	};

	struct PipelineInfo {
		WGPURenderPipeline render_pipeline = nullptr;
		WGPUComputePipeline compute_pipeline = nullptr;
		const ShaderInfo *shader = nullptr;
	};

	struct SwapChainInfo {
		RenderingContextDriver::SurfaceID surface = 0;
		WGPUTextureFormat wgpu_format = WGPUTextureFormat_BGRA8Unorm;
		DataFormat data_format = DATA_FORMAT_B8G8R8A8_UNORM;
		RenderPassInfo render_pass;
		FramebufferInfo framebuffer;
		WGPUTexture current_texture = nullptr;
		bool configured = false;
	};

	RenderingContextDriverWebGPU *context = nullptr;
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPULimits device_limits = {};
	uint32_t frame_count = 1;
	LocalVector<BufferInfo *> dynamic_buffers_all;

	// Push-constant ring: values are written into a shadow at 256-aligned
	// offsets and flushed with one wgpuQueueWriteBuffer before submission;
	// bind groups reference the buffer with a dynamic offset.
	WGPUBuffer push_constant_buffer = nullptr;
	uint8_t *push_constant_shadow = nullptr;
	uint32_t push_constant_capacity = 0;
	uint32_t push_constant_used = 0;

	void _flush_bind_groups(CommandBufferInfo *p_cb_info);
	void _end_compute_pass(CommandBufferInfo *p_cb_info);

	MultiviewCapabilities multiview_capabilities;
	FragmentShadingRateCapabilities fragment_shading_rate_capabilities;
	FragmentDensityMapCapabilities fragment_density_map_capabilities;
	Capabilities capabilities;
	RenderingShaderContainerFormatWebGPU shader_container_format;

public:
	virtual Error initialize(uint32_t p_device_index, uint32_t p_frame_count) override;
	virtual BufferID buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) override;
	// Texel buffers have no WGSL equivalent; they exist only as placeholder
	// storage bindings (shaders never reference them: the transform removes
	// the unused declarations), so the texel format is irrelevant.
	virtual bool buffer_set_texel_format(BufferID p_buffer, DataFormat p_format) override { return true; }
	virtual void buffer_free(BufferID p_buffer) override;
	virtual uint64_t buffer_get_allocation_size(BufferID p_buffer) override;
	virtual uint8_t *buffer_map(BufferID p_buffer) override;
	virtual void buffer_unmap(BufferID p_buffer) override;
	virtual uint8_t *buffer_persistent_map_advance(BufferID p_buffer, uint64_t p_frames_drawn) override;
	virtual void buffer_flush(BufferID p_buffer) override;
	virtual uint64_t buffer_get_dynamic_offsets(Span<BufferID> p_buffers) override;
	virtual uint64_t buffer_get_device_address(BufferID p_buffer) override { ERR_FAIL_V_MSG(0, UNIMPLEMENTED); }
	virtual TextureID texture_create(const TextureFormat &p_format, const TextureView &p_view) override;
	virtual TextureID texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool p_depth_stencil, uint32_t p_mipmaps) override;
	virtual TextureID texture_create_shared(TextureID p_original_texture, const TextureView &p_view) override;
	virtual TextureID texture_create_shared_from_slice(TextureID p_original_texture, const TextureView &p_view, TextureSliceType p_slice_type, uint32_t p_layer, uint32_t p_layers, uint32_t p_mipmap, uint32_t p_mipmaps) override;
	virtual void texture_free(TextureID p_texture) override;
	virtual uint64_t texture_get_allocation_size(TextureID p_texture) override;
	virtual void texture_get_copyable_layout(TextureID p_texture, const TextureSubresource &p_subresource, TextureCopyableLayout *r_layout) override;
	virtual Vector<uint8_t> texture_get_data(TextureID p_texture, uint32_t p_layer) override { ERR_FAIL_V_MSG((Vector<uint8_t>()), UNIMPLEMENTED); }
	virtual BitField<TextureUsageBits> texture_get_usages_supported_by_format(DataFormat p_format, bool p_cpu_readable) override;
	virtual bool texture_can_make_shared_with_format(TextureID p_texture, DataFormat p_format, bool &r_raw_reinterpretation) override {
		r_raw_reinterpretation = false;
		return true; // Views are created directly from the texture; formats resolve at view creation.
	}
	virtual SamplerID sampler_create(const SamplerState &p_state) override;
	virtual void sampler_free(SamplerID p_sampler) override;
	virtual bool sampler_is_format_supported_for_filter(DataFormat p_format, SamplerFilter p_filter) override;
	virtual VertexFormatID vertex_format_create(Span<VertexAttribute> p_vertex_attribs, const VertexAttributeBindingsMap &p_vertex_bindings) override;
	virtual void vertex_format_free(VertexFormatID p_vertex_format) override;
	virtual void command_pipeline_barrier(CommandBufferID p_cmd_buffer, BitField<PipelineStageBits> p_src_stages, BitField<PipelineStageBits> p_dst_stages, VectorView<MemoryAccessBarrier> p_memory_barriers, VectorView<BufferBarrier> p_buffer_barriers, VectorView<TextureBarrier> p_texture_barriers, VectorView<AccelerationStructureBarrier> p_acceleration_structure_barriers) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual FenceID fence_create() override;
	virtual Error fence_wait(FenceID p_fence) override;
	virtual void fence_free(FenceID p_fence) override;
	virtual SemaphoreID semaphore_create() override;
	virtual void semaphore_free(SemaphoreID p_semaphore) override;
	virtual CommandQueueFamilyID command_queue_family_get(BitField<CommandQueueFamilyBits> p_cmd_queue_family_bits, RenderingContextDriver::SurfaceID p_surface = 0) override;
	virtual CommandQueueID command_queue_create(CommandQueueFamilyID p_cmd_queue_family, bool p_identify_as_main_queue = false) override;
	virtual Error command_queue_execute_and_present(CommandQueueID p_cmd_queue, VectorView<SemaphoreID> p_wait_semaphores, VectorView<CommandBufferID> p_cmd_buffers, VectorView<SemaphoreID> p_cmd_semaphores, FenceID p_cmd_fence, VectorView<SwapChainID> p_swap_chains) override;
	virtual void command_queue_free(CommandQueueID p_cmd_queue) override;
	virtual CommandPoolID command_pool_create(CommandQueueFamilyID p_cmd_queue_family, CommandBufferType p_cmd_buffer_type) override;
	// Encoders are transient in WebGPU; returning false makes the engine skip
	// pool reuse and simply create new command buffers.
	// Encoders and command buffers are released at submission; nothing
	// lingers, so a reset only has to succeed (returning false aborts the
	// engine's frame setup before begin_segment runs).
	virtual bool command_pool_reset(CommandPoolID p_cmd_pool) override { return true; }
	virtual void command_pool_free(CommandPoolID p_cmd_pool) override;
	virtual CommandBufferID command_buffer_create(CommandPoolID p_cmd_pool) override;
	virtual bool command_buffer_begin(CommandBufferID p_cmd_buffer) override;
	virtual bool command_buffer_begin_secondary(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, uint32_t p_subpass, FramebufferID p_framebuffer) override { ERR_FAIL_V_MSG(false, UNIMPLEMENTED); }
	virtual void command_buffer_end(CommandBufferID p_cmd_buffer) override;
	virtual void command_buffer_execute_secondary(CommandBufferID p_cmd_buffer, VectorView<CommandBufferID> p_secondary_cmd_buffers) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual SwapChainID swap_chain_create(RenderingContextDriver::SurfaceID p_surface) override;
	virtual Error swap_chain_resize(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, uint32_t p_desired_framebuffer_count) override;
	virtual FramebufferID swap_chain_acquire_framebuffer(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, bool &r_resize_required) override;
	virtual RenderPassID swap_chain_get_render_pass(SwapChainID p_swap_chain) override;
	virtual DataFormat swap_chain_get_format(SwapChainID p_swap_chain) override;
	virtual ColorSpace swap_chain_get_color_space(SwapChainID p_swap_chain) override { return COLOR_SPACE_REC709_NONLINEAR_SRGB; }
	virtual bool swap_chain_get_hdr_output_supported(SwapChainID p_swap_chain) override { ERR_FAIL_V_MSG(false, UNIMPLEMENTED); }
	virtual void swap_chain_free(SwapChainID p_swap_chain) override;
	virtual FramebufferID framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) override;
	virtual void framebuffer_free(FramebufferID p_framebuffer) override;
	virtual ShaderID shader_create_from_container(const Ref<RenderingShaderContainer> &p_shader_container, const Vector<ImmutableSampler> &p_immutable_samplers) override;
	virtual void shader_free(ShaderID p_shader) override;
	virtual void shader_destroy_modules(ShaderID p_shader) override;
	virtual UniformSetID uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader, uint32_t p_set_index, int p_linear_pool_index) override;
	virtual void uniform_set_free(UniformSetID p_uniform_set) override;
	virtual uint32_t uniform_sets_get_dynamic_offsets(VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count) const override;
	virtual void command_uniform_set_prepare_for_use(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) override {}
	virtual void command_clear_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, uint64_t p_offset, uint64_t p_size) override;
	virtual void command_copy_buffer(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, BufferID p_dst_buffer, VectorView<BufferCopyRegion> p_regions) override;
	virtual void command_copy_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<TextureCopyRegion> p_regions) override;
	virtual void command_resolve_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_clear_color_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, const Color &p_color, const TextureSubresourceRange &p_subresources) override;
	virtual void command_clear_depth_stencil_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, float p_depth, uint8_t p_stencil, const TextureSubresourceRange &p_subresources) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_copy_buffer_to_texture(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<BufferTextureCopyRegion> p_regions) override;
	virtual void command_copy_texture_to_buffer(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, BufferID p_dst_buffer, VectorView<BufferTextureCopyRegion> p_regions) override;
	virtual void pipeline_free(PipelineID p_pipeline) override;
	virtual void command_bind_push_constants(CommandBufferID p_cmd_buffer, ShaderID p_shader, uint32_t p_first_index, VectorView<uint32_t> p_data) override;
	// The browser manages pipeline caching; declining makes the engine skip it.
	virtual bool pipeline_cache_create(const Vector<uint8_t> &p_data) override { return false; }
	virtual void pipeline_cache_free() override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual size_t pipeline_cache_query_size() override { ERR_FAIL_V_MSG((size_t()), UNIMPLEMENTED); }
	virtual Vector<uint8_t> pipeline_cache_serialize() override { ERR_FAIL_V_MSG((Vector<uint8_t>()), UNIMPLEMENTED); }
	virtual RenderPassID render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses, VectorView<SubpassDependency> p_subpass_dependencies, uint32_t p_view_count, AttachmentReference p_fragment_density_map_attachment) override;
	virtual void render_pass_free(RenderPassID p_render_pass) override;
	virtual void command_begin_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RenderPassClearValue> p_clear_values) override;
	virtual void command_end_render_pass(CommandBufferID p_cmd_buffer) override;
	virtual void command_next_render_subpass(CommandBufferID p_cmd_buffer, CommandBufferType p_cmd_buffer_type) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_render_set_viewport(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_viewports) override;
	virtual void command_render_set_scissor(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_scissors) override;
	virtual void command_render_clear_attachments(CommandBufferID p_cmd_buffer, VectorView<AttachmentClear> p_attachment_clears, VectorView<Rect2i> p_rects) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_bind_render_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) override;
	virtual void command_bind_render_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) override;
	virtual void command_render_draw(CommandBufferID p_cmd_buffer, uint32_t p_vertex_count, uint32_t p_instance_count, uint32_t p_base_vertex, uint32_t p_first_instance) override;
	virtual void command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) override;
	virtual void command_render_draw_indexed_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_render_draw_indexed_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_render_draw_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) override;
	virtual void command_render_draw_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_render_bind_vertex_buffers(CommandBufferID p_cmd_buffer, uint32_t p_binding_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) override;
	virtual void command_render_bind_index_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, IndexBufferFormat p_format, uint64_t p_offset) override;
	virtual void command_render_set_blend_constants(CommandBufferID p_cmd_buffer, const Color &p_constants) override;
	virtual void command_render_set_line_width(CommandBufferID p_cmd_buffer, float p_width) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual PipelineID render_pipeline_create(ShaderID p_shader, VertexFormatID p_vertex_format, RenderPrimitive p_render_primitive, PipelineRasterizationState p_rasterization_state, PipelineMultisampleState p_multisample_state, PipelineDepthStencilState p_depth_stencil_state, PipelineColorBlendState p_blend_state, VectorView<int32_t> p_color_attachments, BitField<PipelineDynamicStateFlags> p_dynamic_state, RenderPassID p_render_pass, uint32_t p_render_subpass, VectorView<PipelineSpecializationConstant> p_specialization_constants) override;
	virtual void command_bind_compute_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) override;
	virtual void command_bind_compute_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) override;
	virtual void command_compute_dispatch(CommandBufferID p_cmd_buffer, uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) override;
	virtual void command_compute_dispatch_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset) override;
	virtual PipelineID compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants) override;
	virtual AccelerationStructureID blas_create(VectorView<AccelerationStructureGeometry> p_geometries, BitField<AccelerationStructureFlagBits> p_flags) override { ERR_FAIL_V_MSG((AccelerationStructureID()), UNIMPLEMENTED); }
	virtual AccelerationStructureID tlas_create(uint32_t p_max_instance_count, BitField<AccelerationStructureFlagBits> p_flags) override { ERR_FAIL_V_MSG((AccelerationStructureID()), UNIMPLEMENTED); }
	virtual void acceleration_structure_instance_write(uint8_t *r_driver_instance, const AccelerationStructureInstance &p_instance) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void acceleration_structure_free(AccelerationStructureID p_acceleration_structure) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual uint32_t acceleration_structure_get_scratch_size_bytes(AccelerationStructureID p_acceleration_structure) override { ERR_FAIL_V_MSG(0, UNIMPLEMENTED); }
	virtual RaytracingPipelineID raytracing_pipeline_create(VectorView<PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, ShaderID p_layout_defining_shader) override { ERR_FAIL_V_MSG((RaytracingPipelineID()), UNIMPLEMENTED); }
	virtual void raytracing_pipeline_free(RaytracingPipelineID p_pipeline) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual bool raytracing_pipeline_get_shader_group_handles(RaytracingPipelineID p_pipeline, uint32_t p_group_index_offset, VectorView<uint32_t> p_group_indices, uint8_t *r_data, uint32_t p_data_stride_bytes) override { ERR_FAIL_V_MSG(false, UNIMPLEMENTED); }
	virtual void command_build_blas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_build_tlas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer, BufferID p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_bind_raytracing_pipeline(CommandBufferID p_cmd_buffer, RaytracingPipelineID p_pipeline) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_bind_raytracing_uniform_set(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	virtual void command_trace_rays(CommandBufferID p_cmd_buffer, const ShaderBindingTable &p_raygen_sbt, const ShaderBindingTable &p_miss_sbt, const ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth) override { ERR_FAIL_MSG(UNIMPLEMENTED); }
	// Timestamp queries are unsupported so far (WebGPU gates them behind the
	// 'timestamp-query' feature); benign tokens and zeroed results keep the
	// engine's per-frame profiling scaffolding alive.
	virtual QueryPoolID timestamp_query_pool_create(uint32_t p_query_count) override { return QueryPoolID(1); }
	virtual void timestamp_query_pool_free(QueryPoolID p_pool_id) override {}
	virtual void timestamp_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) override {
		for (uint32_t i = 0; i < p_query_count; i++) {
			r_results[i] = 0;
		}
	}
	virtual uint64_t timestamp_query_result_to_time(uint64_t p_result) override { return 0; }
	virtual void command_timestamp_query_pool_reset(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_query_count) override {}
	virtual void command_timestamp_write(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_index) override {}
	// Debug labels and breadcrumbs are diagnostics, not functionality:
	// no-ops rather than errors so engine paths that always emit them
	// (object naming, RenderDoc-style regions) run silently.
	virtual void command_begin_label(CommandBufferID p_cmd_buffer, const char *p_label_name, const Color &p_color) override {}
	virtual void command_end_label(CommandBufferID p_cmd_buffer) override {}
	virtual void command_insert_breadcrumb(CommandBufferID p_cmd_buffer, uint32_t p_data) override {}
	virtual void begin_segment(uint32_t p_frame_index, uint32_t p_frames_drawn) override;
	virtual void end_segment() override;
	virtual void set_object_name(ObjectType p_type, ID p_driver_id, const String &p_name) override {}
	virtual uint64_t get_resource_native_handle(DriverResource p_type, ID p_driver_id) override { ERR_FAIL_V_MSG(0, UNIMPLEMENTED); }
	// WebGPU exposes no memory statistics.
	virtual uint64_t get_total_memory_used() override { return 0; }
	virtual uint64_t get_lazily_memory_used() override { return 0; }
	virtual uint64_t limit_get(Limit p_limit) override;
	virtual bool has_feature(Features p_feature) override {
		switch (p_feature) {
			// Half float needs the shader-f16 device feature (tint emits
			// enable f16), which the loader does not request yet.
			case SUPPORTS_FRAGMENT_SHADER_WITH_ONLY_SIDE_EFFECTS:
				return true;
			default:
				return false;
		}
	}
	virtual const MultiviewCapabilities &get_multiview_capabilities() override { return multiview_capabilities; }
	virtual const FragmentShadingRateCapabilities &get_fragment_shading_rate_capabilities() override { return fragment_shading_rate_capabilities; }
	virtual const FragmentDensityMapCapabilities &get_fragment_density_map_capabilities() override { return fragment_density_map_capabilities; }
	virtual String get_api_name() const override { return "WebGPU"; }
	virtual String get_api_version() const override { return "1.0"; }
	virtual String get_pipeline_cache_uuid() const override { ERR_FAIL_V_MSG((String()), UNIMPLEMENTED); }
	virtual const Capabilities &get_capabilities() const override { return capabilities; }
	virtual const RenderingShaderContainerFormat &get_shader_container_format() const override { return shader_container_format; }
	virtual uint64_t api_trait_get(ApiTrait p_trait) override;

	// Driver-internal (non-RDD) helper: the current acquired texture of a swap
	// chain, as a native handle for texture_create_from_extension. Used by the
	// probe (drivers/webgpu/webgpu_probe.cpp) to copy into the presented frame
	// without a render pipeline.
	uint64_t swap_chain_get_current_texture_handle(SwapChainID p_swap_chain) const {
		return (uint64_t)((SwapChainInfo *)p_swap_chain.id)->current_texture;
	}

	explicit RenderingDeviceDriverWebGPU(RenderingContextDriverWebGPU *p_context);
};
