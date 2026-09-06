/**************************************************************************/
/*  rendering_device_driver_webgpu.cpp                                    */
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

#include "rendering_device_driver_webgpu.h"



#include "core/string/print_string.h"

// See platform/web/js/libs/library_godot_webgpu.js.
extern "C" {
// Returns 1 for bgra8unorm, 2 for rgba8unorm.
int godot_js_webgpu_preferred_format();
// 0 when the page URL carries ?nobundles (benchmark A/B switch).
int godot_js_webgpu_use_bundles();
// 1 when the browser understands GPUTextureUsage.TRANSIENT_ATTACHMENT
// (core in Chromium 149+, no adapter feature to request).
int godot_js_webgpu_has_transient_attachments();
// 0 when the page URL carries ?noswizzle (benchmark A/B switch for the
// native view-swizzle path).
int godot_js_webgpu_use_native_swizzle();
// 0 when the page URL carries ?nobc (benchmark A/B switch for
// block-compressed texture support).
int godot_js_webgpu_use_bc();
// 0 when the page URL carries ?noetc (benchmark A/B switch for the mobile
// compressed-texture families, ETC2/EAC and ASTC together).
int godot_js_webgpu_use_etc();
// Publishes the pipeline-warm backlog (queued + compiles in flight) to the
// page, so a loading overlay can hold until the first-visit compile settles.
void godot_js_webgpu_report_warm_pending(int p_pending);
// 0 when the page URL carries ?nowarm (benchmark A/B switch for the
// background pipeline warm).
int godot_js_webgpu_use_warm();
// 0 when the page URL carries ?nofastpath (benchmark A/B switch for the
// identity bundle fast path).
int godot_js_webgpu_use_fastpath();
// 0 when the page URL carries ?noindirect (benchmark A/B switch for
// indirect draw parameters in cached bundles).
int godot_js_webgpu_use_indirect();
// 0 when the page URL carries ?nodiff (benchmark A/B switch for the
// range-diffed dynamic slice flush).
int godot_js_webgpu_use_diff_flush();
// 1 when the page URL carries ?perfcounters (periodic driver counter print).
int godot_js_webgpu_perf_counters();
int godot_js_webgpu_use_subgroups();
// 0 when the page URL carries ?notiers (benchmark A/B switch for the
// texture-format-tier paths).
int godot_js_webgpu_use_tiers();
}

static uint32_t _data_format_block_info(RenderingDeviceCommons::DataFormat p_format, uint32_t &r_block_w, uint32_t &r_block_h);

// WebGPU exposes a single, implicitly synchronized queue: queue family and
// queue handles are opaque non-zero tokens, submission order is the only
// ordering guarantee, and fences/semaphores are recorded but never waited on
// (a blocking wait on the browser's main thread can never make progress).
static const uint64_t TOKEN_ID = 1;

// Warm compiles handed to createRenderPipelineAsync/createComputePipelineAsync
// whose callbacks have not fired yet (main-thread only - no atomics needed).
static uint32_t g_warm_inflight = 0;

Error RenderingDeviceDriverWebGPU::initialize(uint32_t p_device_index, uint32_t p_frame_count) {
	ERR_FAIL_COND_V(p_device_index != 0, ERR_INVALID_PARAMETER);
	device = context->get_device();
	ERR_FAIL_NULL_V(device, ERR_UNAVAILABLE);
	queue = wgpuDeviceGetQueue(device);
	ERR_FAIL_NULL_V(queue, ERR_CANT_CREATE);
	use_render_bundles = godot_js_webgpu_use_bundles() != 0;
	if (!use_render_bundles) {
		print_line("WebGPU: render-bundle caching disabled (?nobundles).");
	}
	use_pipeline_warm = godot_js_webgpu_use_warm() != 0;
	if (!use_pipeline_warm) {
		print_line("WebGPU: background pipeline warming disabled (?nowarm).");
	}
	use_bundle_fastpath = godot_js_webgpu_use_fastpath() != 0;
	if (!use_bundle_fastpath) {
		print_line("WebGPU: identity bundle fast path disabled (?nofastpath).");
	}
	use_indirect_params = godot_js_webgpu_use_indirect() != 0;
	if (!use_indirect_params) {
		print_line("WebGPU: indirect bundle draw parameters disabled (?noindirect).");
	}
	use_diff_flush = godot_js_webgpu_use_diff_flush() != 0;
	if (!use_diff_flush) {
		print_line("WebGPU: range-diffed dynamic flushes disabled (?nodiff).");
	}
	perf_counters_enabled = godot_js_webgpu_perf_counters() != 0;
	if (perf_counters_enabled) {
		print_line("WebGPU: driver perf counters enabled (?perfcounters).");
	}
	device_has_transient_attachments = godot_js_webgpu_has_transient_attachments() != 0;
	const bool use_tiers = godot_js_webgpu_use_tiers() != 0;
	device_has_texture_formats_tier1 = use_tiers && wgpuDeviceHasFeature(device, WGPUFeatureName_TextureFormatsTier1);
	device_has_texture_formats_tier2 = use_tiers && wgpuDeviceHasFeature(device, WGPUFeatureName_TextureFormatsTier2);
	if (device_has_texture_formats_tier1 || device_has_texture_formats_tier2) {
		print_verbose(vformat("WebGPU: texture format tiers granted: tier1=%s tier2=%s.", device_has_texture_formats_tier1 ? "yes" : "no", device_has_texture_formats_tier2 ? "yes" : "no"));
	}

	if (wgpuDeviceGetLimits(device, &device_limits) != WGPUStatus_Success) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "Failed to query WebGPU device limits.");
	}
	device_has_shader_f16 = wgpuDeviceHasFeature(device, WGPUFeatureName_ShaderF16);
	device_has_subgroups = wgpuDeviceHasFeature(device, WGPUFeatureName_Subgroups) && godot_js_webgpu_use_subgroups() != 0;
	device_has_depth_clip_control = wgpuDeviceHasFeature(device, WGPUFeatureName_DepthClipControl);
	device_has_timestamp_query = wgpuDeviceHasFeature(device, WGPUFeatureName_TimestampQuery);
	device_has_texture_swizzle = wgpuDeviceHasFeature(device, WGPUFeatureName_TextureComponentSwizzle) && godot_js_webgpu_use_native_swizzle() != 0;
	print_verbose(device_has_texture_swizzle ? "WebGPU: texture swizzles served by native view swizzles." : "WebGPU: texture swizzles emulated by texel expansion at upload.");
	device_has_texture_compression_bc = wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionBC) && godot_js_webgpu_use_bc() != 0;
	const bool use_etc = godot_js_webgpu_use_etc() != 0;
	device_has_texture_compression_etc2 = use_etc && wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionETC2);
	device_has_texture_compression_astc = use_etc && wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionASTC);
	device_has_indirect_first_instance = wgpuDeviceHasFeature(device, WGPUFeatureName_IndirectFirstInstance);

	frame_count = MAX(1u, p_frame_count);

	ERR_FAIL_COND_V(!_push_constant_ring_allocate(PUSH_CONSTANT_RING_SIZE), ERR_CANT_CREATE);

	capabilities.device_family = DEVICE_UNKNOWN;
	capabilities.version_major = 1;
	capabilities.version_minor = 0;
	return OK;
}

/******************/
/**** COMMANDS ****/
/******************/

RenderingDeviceDriver::CommandQueueFamilyID RenderingDeviceDriverWebGPU::command_queue_family_get(BitField<CommandQueueFamilyBits> p_cmd_queue_family_bits, RenderingContextDriver::SurfaceID p_surface) {
	// The single WebGPU queue supports graphics, compute, transfer, and present.
	return CommandQueueFamilyID(TOKEN_ID);
}

RenderingDeviceDriver::CommandQueueID RenderingDeviceDriverWebGPU::command_queue_create(CommandQueueFamilyID p_cmd_queue_family, bool p_identify_as_main_queue) {
	ERR_FAIL_COND_V(!p_cmd_queue_family, CommandQueueID());
	return CommandQueueID(TOKEN_ID);
}

Error RenderingDeviceDriverWebGPU::command_queue_execute_and_present(CommandQueueID p_cmd_queue, VectorView<SemaphoreID> p_wait_semaphores, VectorView<CommandBufferID> p_cmd_buffers, VectorView<SemaphoreID> p_cmd_semaphores, FenceID p_cmd_fence, VectorView<SwapChainID> p_swap_chains) {
	if (p_cmd_buffers.size() > 0) {
		LocalVector<WGPUCommandBuffer> wgpu_buffers;
		wgpu_buffers.reserve(p_cmd_buffers.size());
		for (uint32_t i = 0; i < p_cmd_buffers.size(); i++) {
			CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffers[i].id;
			ERR_FAIL_NULL_V_MSG(cb_info->command_buffer, ERR_INVALID_PARAMETER, "Command buffer was not ended before submission.");
			wgpu_buffers.push_back(cb_info->command_buffer);
		}
		if (push_constant_used > 0) {
			_counted_write_buffer(push_constant_buffer, 0, push_constant_shadow, push_constant_used);
		}
		// Persistent-map writes have no flush on coherent-memory platforms;
		// push dirty slices before their commands execute.
		for (BufferInfo *dynamic_buffer : dynamic_buffers_all) {
			if (dynamic_buffer->dirty) {
				_flush_dynamic_slice(dynamic_buffer);
			}
		}
		wgpuQueueSubmit(queue, wgpu_buffers.size(), wgpu_buffers.ptr());
		// Request the timestamp readback only after submission: mapping a
		// buffer the queue is still writing is invalid.
		_resolve_timestamp_pools(nullptr);
		_map_timestamp_pools();
		for (uint32_t i = 0; i < p_cmd_buffers.size(); i++) {
			CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffers[i].id;
			wgpuCommandBufferRelease(cb_info->command_buffer);
			cb_info->command_buffer = nullptr;
		}
	}

	for (uint32_t i = 0; i < p_swap_chains.size(); i++) {
		SwapChainInfo *swap_chain = (SwapChainInfo *)p_swap_chains[i].id;
		if (!swap_chain->framebuffer.views.is_empty()) {
			wgpuTextureViewRelease(swap_chain->framebuffer.views[0]);
			swap_chain->framebuffer.views.clear();
		}
		if (swap_chain->current_texture != nullptr) {
			wgpuTextureRelease(swap_chain->current_texture);
			swap_chain->current_texture = nullptr;
		}
		// No wgpuSurfacePresent here: the browser presents when control
		// returns to the event loop, and emdawnwebgpu aborts on the call.
	}

	// After the frame's submission, hand a few deferred pipelines to the
	// browser for background compilation (runs on the main thread, where
	// the spontaneous completion callbacks also fire).
	_pump_pipeline_warm_queue();
	present_count++;
	if (perf_counters_enabled) {
		perf.frames++;
		const uint32_t PERF_WINDOW = 120;
		if (perf.frames >= PERF_WINDOW) {
			print_line(vformat("WGPUPERF frames=%d wb_calls=%d wb_kb=%d fastpath=%d hash_hit=%d builds=%d direct=%d param_writes=%d bg_new=%d pipe_new=%d sg_new=%d",
					perf.frames, perf.write_buffer_calls, (int)(perf.write_buffer_bytes / 1024),
					perf.bundle_fastpath_hits, perf.bundle_hash_hits, perf.bundle_builds, perf.bundle_direct,
					perf.indirect_param_writes, perf.bind_groups_created, perf.pipelines_created, perf.native_subgroup_modules));
			perf = PerfCounters();
		}
	}
	return OK;
}

void RenderingDeviceDriverWebGPU::command_queue_free(CommandQueueID p_cmd_queue) {
	// Token handle; the queue belongs to the device.
}

RenderingDeviceDriver::CommandPoolID RenderingDeviceDriverWebGPU::command_pool_create(CommandQueueFamilyID p_cmd_queue_family, CommandBufferType p_cmd_buffer_type) {
	ERR_FAIL_COND_V_MSG(p_cmd_buffer_type != COMMAND_BUFFER_TYPE_PRIMARY, CommandPoolID(), "Secondary command buffers are not supported by the WebGPU driver.");
	CommandPoolInfo *pool = memnew(CommandPoolInfo);
	pool->buffer_type = p_cmd_buffer_type;
	return CommandPoolID(pool);
}

void RenderingDeviceDriverWebGPU::command_pool_free(CommandPoolID p_cmd_pool) {
	CommandPoolInfo *pool = (CommandPoolInfo *)p_cmd_pool.id;
	for (CommandBufferInfo *cb_info : pool->command_buffers) {
		if (cb_info->render_pass_encoder != nullptr) {
			wgpuRenderPassEncoderRelease(cb_info->render_pass_encoder);
		}
		if (cb_info->encoder != nullptr) {
			wgpuCommandEncoderRelease(cb_info->encoder);
		}
		if (cb_info->command_buffer != nullptr) {
			wgpuCommandBufferRelease(cb_info->command_buffer);
		}
		memdelete(cb_info);
	}
	memdelete(pool);
}

RenderingDeviceDriver::CommandBufferID RenderingDeviceDriverWebGPU::command_buffer_create(CommandPoolID p_cmd_pool) {
	CommandPoolInfo *pool = (CommandPoolInfo *)p_cmd_pool.id;
	CommandBufferInfo *cb_info = memnew(CommandBufferInfo);
	pool->command_buffers.push_back(cb_info);
	return CommandBufferID(cb_info);
}

bool RenderingDeviceDriverWebGPU::command_buffer_begin(CommandBufferID p_cmd_buffer) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	if (cb_info->command_buffer != nullptr) {
		wgpuCommandBufferRelease(cb_info->command_buffer);
		cb_info->command_buffer = nullptr;
	}
	cb_info->encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
	ERR_FAIL_NULL_V(cb_info->encoder, false);
	cb_info->current_shader = nullptr;
	cb_info->bind_group_dirty_mask = 0;
	cb_info->push_constant_dirty = false;
	cb_info->push_constant_offset = 0;
	for (uint32_t i = 0; i < MAX_BIND_GROUPS; i++) {
		cb_info->pending_bind_groups[i] = nullptr;
		cb_info->pending_dynamic_offsets[i].clear();
	}
	return true;
}

void RenderingDeviceDriverWebGPU::command_clear_color_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, const Color &p_color, const TextureSubresourceRange &p_subresources) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	const TextureInfo *texture = (const TextureInfo *)p_texture.id;
	ERR_FAIL_NULL(cb_info->encoder);
	_end_compute_pass(cb_info);
	uint32_t clear_block_w = 1, clear_block_h = 1;
	if (_data_format_block_info(texture->format, clear_block_w, clear_block_h) != 0) {
		// Block-compressed textures are sampled-only content; nothing clears
		// them, and the texel zero-fill below cannot express block rows.
		return;
	}
	if ((texture->usage & WGPUTextureUsage_RenderAttachment) == 0 || wgpuTextureGetDimension(texture->texture) == WGPUTextureDimension_3D) {
		// Non-renderable (e.g. storage-only fog maps) and 3D textures cannot
		// take the render-pass clear below; zero them through writeTexture
		// (queue-ordered before this command buffer's later submission --
		// clears happen at resource init, before any same-buffer writes).
		if (p_color != Color(0, 0, 0, 0)) {
			print_verbose("WebGPU: clearing a non-renderable texture to a non-zero color is unsupported; clearing to zero instead.");
		}
		const bool is_3d = wgpuTextureGetDimension(texture->texture) == WGPUTextureDimension_3D;
		// The PHYSICAL texel size: storage-format substitution can promote the
		// texture to a wider format than the engine-side one (r16f/r8 -> r32f
		// and friends), and a zero-fill sized from the engine format then
		// passes a bytesPerRow smaller than the real row - Dawn rejects the
		// write and the texture keeps its previous contents (SDFGI's cascade
		// scratch was the first caught doing it).
		uint32_t pixel_size = get_image_format_pixel_size(texture->format);
		switch (texture->wgpu_format) {
			case WGPUTextureFormat_RGBA8Unorm:
			case WGPUTextureFormat_R32Float:
			case WGPUTextureFormat_R32Uint:
			case WGPUTextureFormat_R32Sint:
				pixel_size = 4;
				break;
			case WGPUTextureFormat_RGBA16Float:
			case WGPUTextureFormat_RG32Uint:
			case WGPUTextureFormat_RG32Sint:
				pixel_size = 8;
				break;
			default:
				break;
		}
		for (uint32_t mip = 0; mip < p_subresources.mipmap_count; mip++) {
			const uint32_t mip_level = p_subresources.base_mipmap + mip;
			const uint32_t w = MAX(1u, wgpuTextureGetWidth(texture->texture) >> mip_level);
			const uint32_t h = MAX(1u, wgpuTextureGetHeight(texture->texture) >> mip_level);
			const uint32_t d = is_3d ? MAX(1u, wgpuTextureGetDepthOrArrayLayers(texture->texture) >> mip_level) : p_subresources.layer_count;
			LocalVector<uint8_t> zeros;
			zeros.resize(w * h * d * pixel_size);
			memset(zeros.ptr(), 0, zeros.size());
			WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			dst.texture = texture->texture;
			dst.mipLevel = mip_level;
			dst.origin = { 0, 0, is_3d ? 0 : p_subresources.base_layer };
			WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
			layout.offset = 0;
			layout.bytesPerRow = w * pixel_size;
			layout.rowsPerImage = h;
			WGPUExtent3D extent = { w, h, d };
			wgpuQueueWriteTexture(queue, &dst, zeros.ptr(), zeros.size(), &layout, &extent);
		}
		return;
	}
	// WebGPU has no texture clear command; run an empty render pass with a
	// clear load op per mip/layer.
	for (uint32_t mip = 0; mip < p_subresources.mipmap_count; mip++) {
		for (uint32_t layer = 0; layer < p_subresources.layer_count; layer++) {
			WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
			view_desc.format = texture->wgpu_format;
			view_desc.dimension = WGPUTextureViewDimension_2D;
			view_desc.baseMipLevel = p_subresources.base_mipmap + mip;
			view_desc.mipLevelCount = 1;
			view_desc.baseArrayLayer = p_subresources.base_layer + layer;
			view_desc.arrayLayerCount = 1;
			WGPUTextureView view = wgpuTextureCreateView(texture->texture, &view_desc);
			ERR_FAIL_NULL(view);
			WGPURenderPassColorAttachment color_attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			color_attachment.view = view;
			color_attachment.loadOp = WGPULoadOp_Clear;
			color_attachment.storeOp = WGPUStoreOp_Store;
			color_attachment.clearValue = { p_color.r, p_color.g, p_color.b, p_color.a };
			WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
			pass_desc.colorAttachmentCount = 1;
			pass_desc.colorAttachments = &color_attachment;
			WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(cb_info->encoder, &pass_desc);
			ERR_FAIL_NULL(pass);
			wgpuRenderPassEncoderEnd(pass);
			wgpuRenderPassEncoderRelease(pass);
			wgpuTextureViewRelease(view);
		}
	}
}

void RenderingDeviceDriverWebGPU::timestamp_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) {
	const TimestampPoolInfo *pool = (const TimestampPoolInfo *)p_pool_id.id;
	for (uint32_t i = 0; i < p_query_count; i++) {
		r_results[i] = (pool && i < pool->values.size()) ? pool->values[i] : 0;
	}
}

void RenderingDeviceDriverWebGPU::command_timestamp_write(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_index) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	TimestampPoolInfo *pool = (TimestampPoolInfo *)p_pool_id.id;
	if (!cb_info || !cb_info->encoder || !pool || !pool->query_set || p_index >= pool->count) {
		return;
	}
	// An empty compute pass exists purely to carry the timestamp: WebGPU can
	// only write one at a pass boundary, and a pass with no dispatches costs
	// nothing. Any open pass must be closed first, or the encoder rejects it.
	_end_compute_pass(cb_info);
	WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
	writes.querySet = pool->query_set;
	writes.beginningOfPassWriteIndex = p_index;
	writes.endOfPassWriteIndex = pool->count; // scratch slot, never read
	WGPUComputePassDescriptor desc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
	desc.timestampWrites = &writes;
	WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(cb_info->encoder, &desc);
	if (pass) {
		wgpuComputePassEncoderEnd(pass);
		wgpuComputePassEncoderRelease(pass);
		pool->written = MAX(pool->written, p_index + 1);
	}
}

// Resolve every pool that was written this frame onto the same encoder that
// wrote it, then stage it for readback.
void RenderingDeviceDriverWebGPU::_resolve_timestamp_pools(CommandBufferInfo *p_cb_info) {
	// Resolving must happen after EVERY write for the frame, and the engine
	// submits several command buffers per frame - resolving on each one's end
	// captured queries that had not been written yet, and read back zeros.
	// One dedicated encoder submitted after the frame's work is unambiguous.
	WGPUCommandEncoder enc = nullptr;
	for (TimestampPoolInfo *pool : timestamp_pools) {
		if (!pool->query_set || pool->written == 0 || pool->map_in_flight) {
			continue;
		}
		if (!enc) {
			enc = wgpuDeviceCreateCommandEncoder(device, nullptr);
			ERR_FAIL_NULL(enc);
		}
		wgpuCommandEncoderResolveQuerySet(enc, pool->query_set, 0, pool->written, pool->resolve, 0);
		wgpuCommandEncoderCopyBufferToBuffer(enc, pool->resolve, 0, pool->readback, 0,
				(uint64_t)pool->written * sizeof(uint64_t));
	}
	if (enc) {
		WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
		wgpuQueueSubmit(queue, 1, &cb);
		wgpuCommandBufferRelease(cb);
		wgpuCommandEncoderRelease(enc);
	}
}

// Kick the async readback after submission. There is no synchronous map on the
// web, so results land a frame or two later; the engine only displays them.
void RenderingDeviceDriverWebGPU::_map_timestamp_pools() {
	for (TimestampPoolInfo *pool : timestamp_pools) {
		if (!pool->query_set || pool->written == 0 || pool->map_in_flight) {
			continue;
		}
		pool->map_in_flight = true;
		WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
		cb.mode = WGPUCallbackMode_AllowSpontaneous;
		cb.userdata1 = pool;
		cb.callback = [](WGPUMapAsyncStatus p_status, WGPUStringView, void *p_user1, void *) {
			TimestampPoolInfo *p = (TimestampPoolInfo *)p_user1;
			if (p_status == WGPUMapAsyncStatus_Success) {
				const uint64_t *src = (const uint64_t *)wgpuBufferGetConstMappedRange(p->readback, 0, (size_t)p->written * sizeof(uint64_t));
				if (src) {
					for (uint32_t i = 0; i < p->written && i < p->values.size(); i++) {
						p->values[i] = src[i];
					}
				}
				wgpuBufferUnmap(p->readback);
			}
			p->map_in_flight = false;
		};
		wgpuBufferMapAsync(pool->readback, WGPUMapMode_Read, 0, (size_t)pool->written * sizeof(uint64_t), cb);
	}
}

void RenderingDeviceDriverWebGPU::command_buffer_end(CommandBufferID p_cmd_buffer) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->encoder);
	_end_compute_pass(cb_info);
	cb_info->command_buffer = wgpuCommandEncoderFinish(cb_info->encoder, nullptr);
	wgpuCommandEncoderRelease(cb_info->encoder);
	cb_info->encoder = nullptr;
}

void RenderingDeviceDriverWebGPU::command_begin_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RenderPassClearValue> p_clear_values) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	RenderPassInfo *pass = (RenderPassInfo *)p_render_pass.id;
	FramebufferInfo *framebuffer = (FramebufferInfo *)p_framebuffer.id;
	ERR_FAIL_NULL(cb_info->encoder);
	_end_compute_pass(cb_info);
	ERR_FAIL_COND(framebuffer->views.size() < pass->attachments.size());

	// WebGPU allows at most 8 color attachments; fixed stack storage keeps
	// this per-pass path allocation-free.
	WGPURenderPassColorAttachment color_attachments[8];
	uint32_t color_attachment_count = 0;
	WGPURenderPassDepthStencilAttachment depth_attachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	bool has_depth = false;
	for (uint32_t i = 0; i < pass->attachments.size(); i++) {
		const RenderPassAttachment &attachment = pass->attachments[i];
		if (attachment.is_resolve_target) {
			continue;
		}
		if (attachment.is_depth_stencil) {
			depth_attachment.view = framebuffer->views[i];
			depth_attachment.depthLoadOp = attachment.load_op;
			depth_attachment.depthStoreOp = attachment.store_op;
			// Stencil ops are only valid on formats with a stencil aspect;
			// leaving them Undefined is required for depth-only attachments.
			const bool has_stencil_aspect = attachment.format == WGPUTextureFormat_Depth24PlusStencil8 || attachment.format == WGPUTextureFormat_Depth32FloatStencil8 || attachment.format == WGPUTextureFormat_Stencil8;
			if (has_stencil_aspect) {
				depth_attachment.stencilLoadOp = attachment.stencil_load_op;
				depth_attachment.stencilStoreOp = attachment.stencil_store_op;
			}
			if (i < p_clear_values.size()) {
				depth_attachment.depthClearValue = p_clear_values[i].depth;
				if (has_stencil_aspect) {
					depth_attachment.stencilClearValue = p_clear_values[i].stencil;
				}
			}
			has_depth = true;
		} else {
			WGPURenderPassColorAttachment color_attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			color_attachment.view = framebuffer->views[i];
			// The swap chain pass predates render_pass_create and decides its
			// load op from the presence of clear values.
			color_attachment.loadOp = pass->from_swap_chain ? (p_clear_values.size() > 0 ? WGPULoadOp_Clear : WGPULoadOp_Load) : attachment.load_op;
			color_attachment.storeOp = attachment.store_op;
			if (attachment.resolve_attachment >= 0) {
				color_attachment.resolveTarget = framebuffer->views[attachment.resolve_attachment];
			}
			if (i < p_clear_values.size()) {
				const Color &color = p_clear_values[i].color;
				color_attachment.clearValue = { color.r, color.g, color.b, color.a };
			}
			ERR_FAIL_COND(color_attachment_count >= 8);
			color_attachments[color_attachment_count++] = color_attachment;
		}
	}

	WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	pass_desc.colorAttachmentCount = color_attachment_count;
	pass_desc.colorAttachments = color_attachments;
	if (has_depth) {
		pass_desc.depthStencilAttachment = &depth_attachment;
	}

	cb_info->render_pass_encoder = wgpuCommandEncoderBeginRenderPass(cb_info->encoder, &pass_desc);
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	cb_info->current_shader = nullptr;
	cb_info->bind_group_dirty_mask = 0;
	for (uint32_t i = 0; i < MAX_BIND_GROUPS; i++) {
		cb_info->pending_bind_groups[i] = nullptr;
		cb_info->pending_dynamic_offsets[i].clear();
	}
	// The swap-chain pass builds its pass info outside render_pass_create;
	// its single blit draw is not worth the bundle bookkeeping.
	cb_info->render_bundleable = use_render_bundles && !pass->from_swap_chain;
	cb_info->render_records.clear();
	cb_info->render_draw_params.clear();
	cb_info->render_pass_info = pass;
	cb_info->render_framebuffer = framebuffer;
}

static _FORCE_INLINE_ uint64_t _fnv1a64_append(uint64_t p_hash, const void *p_data, size_t p_size) {
	const uint8_t *bytes = (const uint8_t *)p_data;
	for (size_t i = 0; i < p_size; i++) {
		p_hash = (p_hash ^ bytes[i]) * 0x100000001b3ULL;
	}
	return p_hash;
}

void RenderingDeviceDriverWebGPU::_record_render_command(CommandBufferInfo *p_cb_info, const RenderCommandRecord &p_record) {
	// Hashing is deferred to command_end_render_pass: the steady state
	// resolves through the identity fast path there and never hashes at all.
	p_cb_info->render_records.push_back(p_record);
}

void RenderingDeviceDriverWebGPU::_render_pass_flush_records(CommandBufferInfo *p_cb_info) {
	// Pass-encoder-only state (viewport/scissor/blend constant) arrived
	// after draw-side commands: bundles cannot express it, so the capture is
	// encoded directly and bundling ends for this pass.
	_replay_render_records(p_cb_info, p_cb_info->render_pass_encoder, nullptr);
	p_cb_info->render_records.clear();
	p_cb_info->render_draw_params.clear();
	p_cb_info->render_bundleable = false;
}

void RenderingDeviceDriverWebGPU::_replay_render_records(const CommandBufferInfo *p_cb_info, WGPURenderPassEncoder p_pass_encoder, WGPURenderBundleEncoder p_bundle_encoder, WGPUBuffer p_indirect_params) {
	// With p_indirect_params (bundle recording only), draws read their
	// parameters from that buffer at 20-byte slots in draw order instead of
	// baking them into the bundle - a later parameter change is then a queue
	// write, not a bundle rebuild.
	uint32_t draw_slot = 0;
	for (const RenderCommandRecord &rec : p_cb_info->render_records) {
		switch (rec.type) {
			case RenderCommandRecord::TYPE_SET_PIPELINE: {
				if (p_bundle_encoder != nullptr) {
					wgpuRenderBundleEncoderSetPipeline(p_bundle_encoder, rec.set_pipeline.pipeline);
				} else {
					wgpuRenderPassEncoderSetPipeline(p_pass_encoder, rec.set_pipeline.pipeline);
				}
			} break;
			case RenderCommandRecord::TYPE_SET_BIND_GROUP: {
				const uint32_t *offsets = rec.set_bind_group.offset_count == 0 ? nullptr : rec.set_bind_group.offsets;
				if (p_bundle_encoder != nullptr) {
					wgpuRenderBundleEncoderSetBindGroup(p_bundle_encoder, rec.set_bind_group.index, rec.set_bind_group.group, rec.set_bind_group.offset_count, offsets);
				} else {
					wgpuRenderPassEncoderSetBindGroup(p_pass_encoder, rec.set_bind_group.index, rec.set_bind_group.group, rec.set_bind_group.offset_count, offsets);
				}
			} break;
			case RenderCommandRecord::TYPE_SET_VERTEX_BUFFER: {
				if (p_bundle_encoder != nullptr) {
					wgpuRenderBundleEncoderSetVertexBuffer(p_bundle_encoder, rec.set_vertex_buffer.slot, rec.set_vertex_buffer.buffer, rec.set_vertex_buffer.offset, WGPU_WHOLE_SIZE);
				} else {
					wgpuRenderPassEncoderSetVertexBuffer(p_pass_encoder, rec.set_vertex_buffer.slot, rec.set_vertex_buffer.buffer, rec.set_vertex_buffer.offset, WGPU_WHOLE_SIZE);
				}
			} break;
			case RenderCommandRecord::TYPE_SET_INDEX_BUFFER: {
				if (p_bundle_encoder != nullptr) {
					wgpuRenderBundleEncoderSetIndexBuffer(p_bundle_encoder, rec.set_index_buffer.buffer, rec.set_index_buffer.format, rec.set_index_buffer.offset, WGPU_WHOLE_SIZE);
				} else {
					wgpuRenderPassEncoderSetIndexBuffer(p_pass_encoder, rec.set_index_buffer.buffer, rec.set_index_buffer.format, rec.set_index_buffer.offset, WGPU_WHOLE_SIZE);
				}
			} break;
			case RenderCommandRecord::TYPE_DRAW: {
				const DrawParams &params = p_cb_info->render_draw_params[rec.draw.param_index];
				if (p_bundle_encoder != nullptr) {
					if (p_indirect_params != nullptr) {
						wgpuRenderBundleEncoderDrawIndirect(p_bundle_encoder, p_indirect_params, (uint64_t)draw_slot * sizeof(DrawParams));
					} else {
						wgpuRenderBundleEncoderDraw(p_bundle_encoder, params.args[0], params.args[1], params.args[2], params.args[3]);
					}
				} else {
					wgpuRenderPassEncoderDraw(p_pass_encoder, params.args[0], params.args[1], params.args[2], params.args[3]);
				}
				draw_slot++;
			} break;
			case RenderCommandRecord::TYPE_DRAW_INDEXED: {
				const DrawParams &params = p_cb_info->render_draw_params[rec.draw_indexed.param_index];
				if (p_bundle_encoder != nullptr) {
					if (p_indirect_params != nullptr) {
						wgpuRenderBundleEncoderDrawIndexedIndirect(p_bundle_encoder, p_indirect_params, (uint64_t)draw_slot * sizeof(DrawParams));
					} else {
						wgpuRenderBundleEncoderDrawIndexed(p_bundle_encoder, params.args[0], params.args[1], params.args[2], (int32_t)params.args[3], params.args[4]);
					}
				} else {
					wgpuRenderPassEncoderDrawIndexed(p_pass_encoder, params.args[0], params.args[1], params.args[2], (int32_t)params.args[3], params.args[4]);
				}
				draw_slot++;
			} break;
			case RenderCommandRecord::TYPE_DRAW_INDIRECT: {
				if (p_bundle_encoder != nullptr) {
					wgpuRenderBundleEncoderDrawIndirect(p_bundle_encoder, rec.draw_indirect.buffer, rec.draw_indirect.offset);
				} else {
					wgpuRenderPassEncoderDrawIndirect(p_pass_encoder, rec.draw_indirect.buffer, rec.draw_indirect.offset);
				}
			} break;
			case RenderCommandRecord::TYPE_DRAW_INDEXED_INDIRECT: {
				if (p_bundle_encoder != nullptr) {
					wgpuRenderBundleEncoderDrawIndexedIndirect(p_bundle_encoder, rec.draw_indirect.buffer, rec.draw_indirect.offset);
				} else {
					wgpuRenderPassEncoderDrawIndexedIndirect(p_pass_encoder, rec.draw_indirect.buffer, rec.draw_indirect.offset);
				}
			} break;
		}
	}
}

void RenderingDeviceDriverWebGPU::command_end_render_pass(CommandBufferID p_cmd_buffer) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	if (cb_info->render_bundleable && cb_info->render_records.size() > 0) {
		FramebufferInfo *framebuffer = cb_info->render_framebuffer;
		const uint64_t record_bytes = (uint64_t)cb_info->render_records.size() * sizeof(RenderCommandRecord);
		const uint64_t param_bytes = (uint64_t)cb_info->render_draw_params.size() * sizeof(DrawParams);
		BundleCacheEntry *hit = nullptr;
		uint64_t stream_hash = 0;
		if (use_bundle_fastpath) {
			// Identity fast path: an exact memcmp against each cached entry's
			// record stream. Vectorized compares beat the byte-serial FNV this
			// replaces, mismatches exit at the first differing byte (usually a
			// dynamic offset in the first bind-group record), and a match
			// needs no hashing at all.
			for (BundleCacheEntry &entry : framebuffer->bundle_cache) {
				if (entry.bundle != nullptr && entry.epoch == resource_epoch &&
						entry.records.size() == cb_info->render_records.size() &&
						memcmp(entry.records.ptr(), cb_info->render_records.ptr(), record_bytes) == 0) {
					hit = &entry;
					perf.bundle_fastpath_hits++;
					break;
				}
			}
		}
		if (hit == nullptr) {
			stream_hash = _fnv1a64_append(0xcbf29ce484222325ULL, cb_info->render_records.ptr(), record_bytes);
			for (BundleCacheEntry &entry : framebuffer->bundle_cache) {
				if (entry.bundle != nullptr && entry.hash == stream_hash && entry.epoch == resource_epoch) {
					hit = &entry;
					perf.bundle_hash_hits++;
					break;
				}
			}
		}
		WGPURenderBundle bundle = nullptr;
		if (hit != nullptr) {
			// Records match; reconcile the draw parameters (excluded from both
			// the identity compare and the hash by construction).
			bool usable = hit->params.size() == cb_info->render_draw_params.size();
			if (usable && param_bytes > 0 && memcmp(hit->params.ptr(), cb_info->render_draw_params.ptr(), param_bytes) != 0) {
				if (hit->params_baked || hit->last_param_frame == present_count) {
					// Literal-draw bundle, or this entry already executed this
					// frame with other parameters (queue writes land before the
					// whole submit): the capture must encode directly.
					usable = false;
				} else {
					_counted_write_buffer(hit->params_buffer, 0, cb_info->render_draw_params.ptr(), param_bytes);
					memcpy(hit->params.ptr(), cb_info->render_draw_params.ptr(), param_bytes);
					perf.indirect_param_writes++;
				}
			}
			if (usable) {
				bundle = hit->bundle;
				hit->last_param_frame = present_count;
			}
		} else {
			// Build a bundle only for a stream seen before: a first sighting
			// encodes directly and just notes the hash, so passes that never
			// repeat (one-off passes, structure churn) cost nothing beyond
			// the capture itself.
			bool build_bundle = false;
			for (const uint64_t recent : framebuffer->recent_hashes) {
				if (recent == stream_hash) {
					build_bundle = true;
					break;
				}
			}
			if (!build_bundle) {
				framebuffer->recent_hashes[framebuffer->recent_hash_next] = stream_hash;
				framebuffer->recent_hash_next = (framebuffer->recent_hash_next + 1) % (sizeof(framebuffer->recent_hashes) / sizeof(uint64_t));
			}
			if (build_bundle) {
				// Second sighting: record it into a bundle so subsequent
				// structurally identical frames replay it with a single call.
				// Draws go through an indirect-args buffer where legal, so
				// parameter-only changes keep hitting this entry.
				bool indirect_ok = use_indirect_params && param_bytes > 0;
				if (indirect_ok && !device_has_indirect_first_instance) {
					for (const RenderCommandRecord &rec : cb_info->render_records) {
						if ((rec.type == RenderCommandRecord::TYPE_DRAW && cb_info->render_draw_params[rec.draw.param_index].args[3] != 0) ||
								(rec.type == RenderCommandRecord::TYPE_DRAW_INDEXED && cb_info->render_draw_params[rec.draw_indexed.param_index].args[4] != 0)) {
							indirect_ok = false;
							break;
						}
					}
				}
				WGPUBuffer params_buffer = nullptr;
				if (indirect_ok) {
					WGPUBufferDescriptor params_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
					params_desc.usage = WGPUBufferUsage_Indirect | WGPUBufferUsage_CopyDst;
					params_desc.size = param_bytes;
					params_buffer = wgpuDeviceCreateBuffer(device, &params_desc);
					if (params_buffer != nullptr) {
						_counted_write_buffer(params_buffer, 0, cb_info->render_draw_params.ptr(), param_bytes);
					} else {
						indirect_ok = false;
					}
				}
				WGPUTextureFormat color_formats[8];
				uint32_t color_format_count = 0;
				WGPURenderBundleEncoderDescriptor bundle_desc = WGPU_RENDER_BUNDLE_ENCODER_DESCRIPTOR_INIT;
				for (const RenderPassAttachment &attachment : cb_info->render_pass_info->attachments) {
					if (attachment.is_resolve_target) {
						continue;
					}
					bundle_desc.sampleCount = attachment.texture_samples;
					if (attachment.is_depth_stencil) {
						bundle_desc.depthStencilFormat = attachment.format;
					} else if (color_format_count < 8) {
						color_formats[color_format_count++] = attachment.format;
					}
				}
				bundle_desc.colorFormatCount = color_format_count;
				bundle_desc.colorFormats = color_formats;
				WGPURenderBundleEncoder bundle_encoder = wgpuDeviceCreateRenderBundleEncoder(device, &bundle_desc);
				if (bundle_encoder != nullptr) {
					_replay_render_records(cb_info, nullptr, bundle_encoder, indirect_ok ? params_buffer : nullptr);
					bundle = wgpuRenderBundleEncoderFinish(bundle_encoder, nullptr);
					wgpuRenderBundleEncoderRelease(bundle_encoder);
				}
				if (bundle != nullptr) {
					perf.bundle_builds++;
					BundleCacheEntry &evict = framebuffer->bundle_cache[framebuffer->bundle_cache_evict];
					framebuffer->bundle_cache_evict = (framebuffer->bundle_cache_evict + 1) % (sizeof(framebuffer->bundle_cache) / sizeof(BundleCacheEntry));
					if (evict.bundle != nullptr) {
						wgpuRenderBundleRelease(evict.bundle);
					}
					if (evict.params_buffer != nullptr) {
						wgpuBufferRelease(evict.params_buffer);
					}
					evict.hash = stream_hash;
					evict.epoch = resource_epoch;
					evict.bundle = bundle;
					evict.params_buffer = indirect_ok ? params_buffer : nullptr;
					evict.params_baked = !indirect_ok;
					evict.last_param_frame = present_count;
					evict.records.clear();
					evict.records.resize(cb_info->render_records.size());
					memcpy(evict.records.ptr(), cb_info->render_records.ptr(), record_bytes);
					evict.params.clear();
					evict.params.resize(cb_info->render_draw_params.size());
					if (param_bytes > 0) {
						memcpy(evict.params.ptr(), cb_info->render_draw_params.ptr(), param_bytes);
					}
				} else if (params_buffer != nullptr) {
					wgpuBufferRelease(params_buffer);
				}
			}
		}
		if (bundle != nullptr) {
			wgpuRenderPassEncoderExecuteBundles(cb_info->render_pass_encoder, 1, &bundle);
		} else {
			// First sighting of this stream (or bundle creation failed, or an
			// unusable parameter mismatch): encode the capture directly - the
			// cost of the old path.
			perf.bundle_direct++;
			_replay_render_records(cb_info, cb_info->render_pass_encoder, nullptr);
		}
	}
	wgpuRenderPassEncoderEnd(cb_info->render_pass_encoder);
	wgpuRenderPassEncoderRelease(cb_info->render_pass_encoder);
	cb_info->render_pass_encoder = nullptr;
	cb_info->render_bundleable = false;
	cb_info->render_records.clear();
	cb_info->render_draw_params.clear();
	cb_info->render_pass_info = nullptr;
	cb_info->render_framebuffer = nullptr;
}

/*******************/
/**** SWAP CHAIN ***/
/*******************/

RenderingDeviceDriver::SwapChainID RenderingDeviceDriverWebGPU::swap_chain_create(RenderingContextDriver::SurfaceID p_surface) {
	SwapChainInfo *swap_chain = memnew(SwapChainInfo);
	swap_chain->surface = p_surface;
	if (godot_js_webgpu_preferred_format() == 2) {
		swap_chain->wgpu_format = WGPUTextureFormat_RGBA8Unorm;
		swap_chain->data_format = DATA_FORMAT_R8G8B8A8_UNORM;
	} else {
		swap_chain->wgpu_format = WGPUTextureFormat_BGRA8Unorm;
		swap_chain->data_format = DATA_FORMAT_B8G8R8A8_UNORM;
	}
	RenderPassAttachment color_attachment;
	color_attachment.format = swap_chain->wgpu_format;
	swap_chain->render_pass.attachments.push_back(color_attachment);
	swap_chain->render_pass.from_swap_chain = true;
	return SwapChainID(swap_chain);
}

Error RenderingDeviceDriverWebGPU::swap_chain_resize(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, uint32_t p_desired_framebuffer_count) {
	SwapChainInfo *swap_chain = (SwapChainInfo *)p_swap_chain.id;
	const uint32_t width = context->surface_get_width(swap_chain->surface);
	const uint32_t height = context->surface_get_height(swap_chain->surface);
	ERR_FAIL_COND_V_MSG(width == 0 || height == 0, ERR_SKIP, "Surface size is not set.");

	WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
	config.device = device;
	config.format = swap_chain->wgpu_format;
	// CopyDst on top of the default RenderAttachment lets blit-style paths
	// copy directly into the acquired texture.
	config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
	config.width = width;
	config.height = height;
	wgpuSurfaceConfigure(context->get_wgpu_surface(swap_chain->surface), &config);

	swap_chain->framebuffer.width = width;
	swap_chain->framebuffer.height = height;
	swap_chain->configured = true;
	context->surface_set_needs_resize(swap_chain->surface, false);
	return OK;
}

RenderingDeviceDriver::FramebufferID RenderingDeviceDriverWebGPU::swap_chain_acquire_framebuffer(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, bool &r_resize_required) {
	SwapChainInfo *swap_chain = (SwapChainInfo *)p_swap_chain.id;
	if (!swap_chain->configured || context->surface_get_needs_resize(swap_chain->surface)) {
		r_resize_required = true;
		return FramebufferID();
	}

	WGPUSurfaceTexture surface_texture = WGPU_SURFACE_TEXTURE_INIT;
	wgpuSurfaceGetCurrentTexture(context->get_wgpu_surface(swap_chain->surface), &surface_texture);
	switch (surface_texture.status) {
		case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
		case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
			break;
		case WGPUSurfaceGetCurrentTextureStatus_Outdated:
		case WGPUSurfaceGetCurrentTextureStatus_Lost:
			r_resize_required = true;
			return FramebufferID();
		default:
			// The canvas can legitimately have no current texture, e.g.
			// while an immersive WebXR session owns the compositor; skip
			// presenting this frame (the caller tolerates a null
			// framebuffer without a resize).
			print_verbose("WebGPU: no current surface texture; skipping presentation this frame.");
			return FramebufferID();
	}

	swap_chain->current_texture = surface_texture.texture;
	WGPUTextureView view = wgpuTextureCreateView(surface_texture.texture, nullptr);
	ERR_FAIL_NULL_V(view, FramebufferID());
	swap_chain->framebuffer.views.clear();
	swap_chain->framebuffer.views.push_back(view);
	return FramebufferID(&swap_chain->framebuffer);
}

RenderingDeviceDriver::RenderPassID RenderingDeviceDriverWebGPU::swap_chain_get_render_pass(SwapChainID p_swap_chain) {
	SwapChainInfo *swap_chain = (SwapChainInfo *)p_swap_chain.id;
	return RenderPassID(&swap_chain->render_pass);
}

RenderingDeviceDriver::DataFormat RenderingDeviceDriverWebGPU::swap_chain_get_format(SwapChainID p_swap_chain) {
	return ((SwapChainInfo *)p_swap_chain.id)->data_format;
}

void RenderingDeviceDriverWebGPU::swap_chain_free(SwapChainID p_swap_chain) {
	SwapChainInfo *swap_chain = (SwapChainInfo *)p_swap_chain.id;
	if (!swap_chain->framebuffer.views.is_empty()) {
		wgpuTextureViewRelease(swap_chain->framebuffer.views[0]);
	}
	if (swap_chain->current_texture != nullptr) {
		wgpuTextureRelease(swap_chain->current_texture);
	}
	if (swap_chain->configured) {
		wgpuSurfaceUnconfigure(context->get_wgpu_surface(swap_chain->surface));
	}
	memdelete(swap_chain);
}

/*******************/
/**** RESOURCES ****/
/*******************/

// Uncompressed color, packed, and depth/stencil formats plus the BC family.
// ETC2/EAC and ASTC (mobile-web) are deferred until a consumer exists.
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <type_traits>


static bool memmem_compat(const char *p_haystack, size_t p_len, const char *p_needle, size_t p_needle_len) {
	if (p_needle_len > p_len) {
		return false;
	}
	for (size_t i = 0; i + p_needle_len <= p_len; i++) {
		if (memcmp(p_haystack + i, p_needle, p_needle_len) == 0) {
			return true;
		}
	}
	return false;
}


// The emdawnwebgpu bindings keep their JS object tables on the thread that
// imported the device, and WebGPU objects cannot be shared across workers.
// The engine creates resources from worker threads (ShaderRD variant loads,
// resource loading), so creation and destruction proxy synchronously to the
// main thread. The main thread processes the system proxying queue while it
// is blocked on futexes (e.g. waiting on WorkerThreadPool group tasks), so
// this cannot deadlock. Command recording and submission stay unproxied:
// they happen on the main thread's render loop.
template <typename F>
static auto _webgpu_run_on_main(F p_func) -> decltype(p_func()) {
	using ReturnType = decltype(p_func());
	if constexpr (std::is_void_v<ReturnType>) {
		struct Context {
			F *func;
		} context = { &p_func };
		emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), [](void *p_context) { (*((Context *)p_context)->func)(); }, &context);
	} else {
		ReturnType result{};
		struct Context {
			F *func;
			ReturnType *result;
		} context = { &p_func, &result };
		emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), [](void *p_context) { Context *c = (Context *)p_context; *c->result = (*c->func)(); }, &context);
		return result;
	}
}

// Re-enters the same method on the main thread (where the guard passes).
#define WEBGPU_MAIN_THREAD_GUARD(...)                                 \
	if (!emscripten_is_main_runtime_thread()) {                       \
		return _webgpu_run_on_main([&]() { return __VA_ARGS__; });    \
	}

static WGPUTextureViewDimension _texture_type_to_wgpu_view_dimension(RenderingDeviceCommons::TextureType p_type);
static bool _is_depth_stencil_format(RenderingDeviceCommons::DataFormat p_format);

// Maps a WGSL storage texture format token to its WGPUTextureFormat. Used
// when reflection lacks the image format (GLSL allows format-less writeonly
// images) -- the baked WGSL declaration is authoritative.
static WGPUTextureFormat _wgsl_storage_format_to_wgpu(const String &p_format) {
	if (p_format == "rgba8unorm") {
		return WGPUTextureFormat_RGBA8Unorm;
	} else if (p_format == "rgba8snorm") {
		return WGPUTextureFormat_RGBA8Snorm;
	} else if (p_format == "rgba8uint") {
		return WGPUTextureFormat_RGBA8Uint;
	} else if (p_format == "rgba8sint") {
		return WGPUTextureFormat_RGBA8Sint;
	} else if (p_format == "rgba16uint") {
		return WGPUTextureFormat_RGBA16Uint;
	} else if (p_format == "rgba16sint") {
		return WGPUTextureFormat_RGBA16Sint;
	} else if (p_format == "rgba16float") {
		return WGPUTextureFormat_RGBA16Float;
	} else if (p_format == "r32uint") {
		return WGPUTextureFormat_R32Uint;
	} else if (p_format == "r32sint") {
		return WGPUTextureFormat_R32Sint;
	} else if (p_format == "r32float") {
		return WGPUTextureFormat_R32Float;
	} else if (p_format == "rg32uint") {
		return WGPUTextureFormat_RG32Uint;
	} else if (p_format == "rg32sint") {
		return WGPUTextureFormat_RG32Sint;
	} else if (p_format == "rg32float") {
		return WGPUTextureFormat_RG32Float;
	} else if (p_format == "rgba32uint") {
		return WGPUTextureFormat_RGBA32Uint;
	} else if (p_format == "rgba32sint") {
		return WGPUTextureFormat_RGBA32Sint;
	} else if (p_format == "rgba32float") {
		return WGPUTextureFormat_RGBA32Float;
	} else if (p_format == "bgra8unorm") {
		return WGPUTextureFormat_BGRA8Unorm;
	}
	return WGPUTextureFormat_Undefined;
}

static WGPUComponentSwizzle _texture_swizzle_to_wgpu(RenderingDeviceCommons::TextureSwizzle p_swizzle, WGPUComponentSwizzle p_identity) {
	switch (p_swizzle) {
		case RenderingDeviceCommons::TEXTURE_SWIZZLE_IDENTITY:
			return p_identity;
		case RenderingDeviceCommons::TEXTURE_SWIZZLE_ZERO:
			return WGPUComponentSwizzle_Zero;
		case RenderingDeviceCommons::TEXTURE_SWIZZLE_ONE:
			return WGPUComponentSwizzle_One;
		case RenderingDeviceCommons::TEXTURE_SWIZZLE_R:
			return WGPUComponentSwizzle_R;
		case RenderingDeviceCommons::TEXTURE_SWIZZLE_G:
			return WGPUComponentSwizzle_G;
		case RenderingDeviceCommons::TEXTURE_SWIZZLE_B:
			return WGPUComponentSwizzle_B;
		case RenderingDeviceCommons::TEXTURE_SWIZZLE_A:
			return WGPUComponentSwizzle_A;
		default:
			return p_identity;
	}
}

static WGPUTextureFormat _data_format_to_wgpu(RenderingDeviceCommons::DataFormat p_format) {
	switch (p_format) {
		case RenderingDeviceCommons::DATA_FORMAT_R8_UNORM:
			return WGPUTextureFormat_R8Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_R8_SNORM:
			return WGPUTextureFormat_R8Snorm;
		case RenderingDeviceCommons::DATA_FORMAT_R8_UINT:
			return WGPUTextureFormat_R8Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R8_SINT:
			return WGPUTextureFormat_R8Sint;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_UNORM:
			return WGPUTextureFormat_RG8Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_SNORM:
			return WGPUTextureFormat_RG8Snorm;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_UINT:
			// Substituted globally (not just for storage usage): the clustered
			// renderer's voxel GI buffer is both a prepass color attachment and
			// a storage image (the MSAA GI resolve writes it), and rg8ui is not
			// a WGSL storage format. rg32uint keeps the two-component uint
			// fragment output and u32 sampling compatible everywhere.
			return WGPUTextureFormat_RG32Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_SINT:
			return WGPUTextureFormat_RG8Sint;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_UNORM:
			return WGPUTextureFormat_RGBA8Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_SNORM:
			return WGPUTextureFormat_RGBA8Snorm;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_UINT:
			return WGPUTextureFormat_RGBA8Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_SINT:
			return WGPUTextureFormat_RGBA8Sint;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_SRGB:
			return WGPUTextureFormat_RGBA8UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_B8G8R8A8_UNORM:
			return WGPUTextureFormat_BGRA8Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_B8G8R8A8_SRGB:
			return WGPUTextureFormat_BGRA8UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_R16_UINT:
			return WGPUTextureFormat_R16Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R16_SINT:
			return WGPUTextureFormat_R16Sint;
		case RenderingDeviceCommons::DATA_FORMAT_R16_SFLOAT:
			return WGPUTextureFormat_R16Float;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_UINT:
			return WGPUTextureFormat_RG16Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_SINT:
			return WGPUTextureFormat_RG16Sint;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_SFLOAT:
			// Substituted globally (not just for storage usage): the motion
			// vector buffers pair an rg16f MSAA attachment with a
			// storage-substituted resolve target, and resolve pairs must
			// match formats. Fragment outputs and sampling are
			// format-agnostic in WGSL, so widening is transparent.
			return WGPUTextureFormat_RGBA16Float;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_UINT:
			return WGPUTextureFormat_RGBA16Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_SINT:
			return WGPUTextureFormat_RGBA16Sint;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_SFLOAT:
			return WGPUTextureFormat_RGBA16Float;
		case RenderingDeviceCommons::DATA_FORMAT_R32_UINT:
			return WGPUTextureFormat_R32Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R32_SINT:
			return WGPUTextureFormat_R32Sint;
		case RenderingDeviceCommons::DATA_FORMAT_R32_SFLOAT:
			return WGPUTextureFormat_R32Float;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_UINT:
			return WGPUTextureFormat_RG32Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_SINT:
			return WGPUTextureFormat_RG32Sint;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_SFLOAT:
			return WGPUTextureFormat_RG32Float;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_UINT:
			return WGPUTextureFormat_RGBA32Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_SINT:
			return WGPUTextureFormat_RGBA32Sint;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_SFLOAT:
			return WGPUTextureFormat_RGBA32Float;
		case RenderingDeviceCommons::DATA_FORMAT_A2B10G10R10_UNORM_PACK32:
			return WGPUTextureFormat_RGB10A2Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_B10G11R11_UFLOAT_PACK32:
			return WGPUTextureFormat_RG11B10Ufloat;
		case RenderingDeviceCommons::DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32:
			return WGPUTextureFormat_RGB9E5Ufloat;
		case RenderingDeviceCommons::DATA_FORMAT_D16_UNORM:
			return WGPUTextureFormat_Depth16Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_X8_D24_UNORM_PACK32:
			return WGPUTextureFormat_Depth24Plus;
		case RenderingDeviceCommons::DATA_FORMAT_D32_SFLOAT:
			return WGPUTextureFormat_Depth32Float;
		case RenderingDeviceCommons::DATA_FORMAT_S8_UINT:
			return WGPUTextureFormat_Stencil8;
		case RenderingDeviceCommons::DATA_FORMAT_D24_UNORM_S8_UINT:
			return WGPUTextureFormat_Depth24PlusStencil8;
		case RenderingDeviceCommons::DATA_FORMAT_D32_SFLOAT_S8_UINT:
			return WGPUTextureFormat_Depth32FloatStencil8;
		case RenderingDeviceCommons::DATA_FORMAT_BC1_RGB_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC1_RGBA_UNORM_BLOCK:
			// WebGPU has no RGB-only BC1 enum; the RGBA decode of RGB-mode
			// blocks yields opaque alpha, so the mapping is lossless.
			return WGPUTextureFormat_BC1RGBAUnorm;
		case RenderingDeviceCommons::DATA_FORMAT_BC1_RGB_SRGB_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK:
			return WGPUTextureFormat_BC1RGBAUnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_BC2_UNORM_BLOCK:
			return WGPUTextureFormat_BC2RGBAUnorm;
		case RenderingDeviceCommons::DATA_FORMAT_BC2_SRGB_BLOCK:
			return WGPUTextureFormat_BC2RGBAUnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_BC3_UNORM_BLOCK:
			return WGPUTextureFormat_BC3RGBAUnorm;
		case RenderingDeviceCommons::DATA_FORMAT_BC3_SRGB_BLOCK:
			return WGPUTextureFormat_BC3RGBAUnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_BC4_UNORM_BLOCK:
			return WGPUTextureFormat_BC4RUnorm;
		case RenderingDeviceCommons::DATA_FORMAT_BC4_SNORM_BLOCK:
			return WGPUTextureFormat_BC4RSnorm;
		case RenderingDeviceCommons::DATA_FORMAT_BC5_UNORM_BLOCK:
			return WGPUTextureFormat_BC5RGUnorm;
		case RenderingDeviceCommons::DATA_FORMAT_BC5_SNORM_BLOCK:
			return WGPUTextureFormat_BC5RGSnorm;
		case RenderingDeviceCommons::DATA_FORMAT_BC6H_UFLOAT_BLOCK:
			return WGPUTextureFormat_BC6HRGBUfloat;
		case RenderingDeviceCommons::DATA_FORMAT_BC6H_SFLOAT_BLOCK:
			return WGPUTextureFormat_BC6HRGBFloat;
		case RenderingDeviceCommons::DATA_FORMAT_BC7_UNORM_BLOCK:
			return WGPUTextureFormat_BC7RGBAUnorm;
		case RenderingDeviceCommons::DATA_FORMAT_BC7_SRGB_BLOCK:
			return WGPUTextureFormat_BC7RGBAUnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
			return WGPUTextureFormat_ETC2RGB8Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
			return WGPUTextureFormat_ETC2RGB8UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
			return WGPUTextureFormat_ETC2RGB8A1Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
			return WGPUTextureFormat_ETC2RGB8A1UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
			return WGPUTextureFormat_ETC2RGBA8Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
			return WGPUTextureFormat_ETC2RGBA8UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_EAC_R11_UNORM_BLOCK:
			return WGPUTextureFormat_EACR11Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_EAC_R11_SNORM_BLOCK:
			return WGPUTextureFormat_EACR11Snorm;
		case RenderingDeviceCommons::DATA_FORMAT_EAC_R11G11_UNORM_BLOCK:
			return WGPUTextureFormat_EACRG11Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_EAC_R11G11_SNORM_BLOCK:
			return WGPUTextureFormat_EACRG11Snorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_4x4_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC4x4Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_4x4_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC4x4UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_5x4_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC5x4Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_5x4_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC5x4UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_5x5_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC5x5Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_5x5_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC5x5UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_6x5_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC6x5Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_6x5_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC6x5UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_6x6_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC6x6Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_6x6_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC6x6UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x5_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC8x5Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x5_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC8x5UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x6_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC8x6Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x6_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC8x6UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x8_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC8x8Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x8_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC8x8UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x5_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC10x5Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x5_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC10x5UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x6_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC10x6Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x6_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC10x6UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x8_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC10x8Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x8_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC10x8UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x10_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC10x10Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x10_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC10x10UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_12x10_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC12x10Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_12x10_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC12x10UnormSrgb;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_12x12_UNORM_BLOCK:
			return WGPUTextureFormat_ASTC12x12Unorm;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_12x12_SRGB_BLOCK:
			return WGPUTextureFormat_ASTC12x12UnormSrgb;
		default:
			return WGPUTextureFormat_Undefined;
	}
}

// Bytes per texel for the uncompressed formats mapped above; 0 if unknown or
// block-compressed.
static uint32_t _data_format_texel_size(RenderingDeviceCommons::DataFormat p_format) {
	switch (p_format) {
		case RenderingDeviceCommons::DATA_FORMAT_R8_UNORM:
		case RenderingDeviceCommons::DATA_FORMAT_R8_SNORM:
		case RenderingDeviceCommons::DATA_FORMAT_R8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R8_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_S8_UINT:
			return 1;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_UNORM:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_SNORM:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16_SFLOAT:
		case RenderingDeviceCommons::DATA_FORMAT_D16_UNORM:
			return 2;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_UNORM:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_SNORM:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_SRGB:
		case RenderingDeviceCommons::DATA_FORMAT_B8G8R8A8_UNORM:
		case RenderingDeviceCommons::DATA_FORMAT_B8G8R8A8_SRGB:
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_SFLOAT:
		case RenderingDeviceCommons::DATA_FORMAT_R32_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32_SFLOAT:
		case RenderingDeviceCommons::DATA_FORMAT_A2B10G10R10_UNORM_PACK32:
		case RenderingDeviceCommons::DATA_FORMAT_B10G11R11_UFLOAT_PACK32:
		case RenderingDeviceCommons::DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32:
		case RenderingDeviceCommons::DATA_FORMAT_X8_D24_UNORM_PACK32:
		case RenderingDeviceCommons::DATA_FORMAT_D32_SFLOAT:
		case RenderingDeviceCommons::DATA_FORMAT_D24_UNORM_S8_UINT:
			return 4;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_SFLOAT:
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_SFLOAT:
		case RenderingDeviceCommons::DATA_FORMAT_D32_SFLOAT_S8_UINT:
			return 8;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_SFLOAT:
			return 16;
		default:
			return 0;
	}
}

// Block size and bytes-per-block for the compressed formats mapped above;
// returns 0 (blocks left 1x1) for uncompressed formats. Copy commands take
// texel extents but express their buffer layout in block rows (WebGPU's rule
// for compressed formats). BC and ETC2/EAC blocks are 4x4; ASTC blocks carry
// their dimensions in the format.
static uint32_t _data_format_block_info(RenderingDeviceCommons::DataFormat p_format, uint32_t &r_block_w, uint32_t &r_block_h) {
	r_block_w = 1;
	r_block_h = 1;
	switch (p_format) {
		case RenderingDeviceCommons::DATA_FORMAT_BC1_RGB_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC1_RGB_SRGB_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC1_RGBA_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC4_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC4_SNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_EAC_R11_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_EAC_R11_SNORM_BLOCK:
			r_block_w = 4;
			r_block_h = 4;
			return 8;
		case RenderingDeviceCommons::DATA_FORMAT_BC2_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC2_SRGB_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC3_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC3_SRGB_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC5_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC5_SNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC6H_UFLOAT_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC6H_SFLOAT_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC7_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_BC7_SRGB_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_EAC_R11G11_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_EAC_R11G11_SNORM_BLOCK:
			r_block_w = 4;
			r_block_h = 4;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_4x4_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_4x4_SRGB_BLOCK:
			r_block_w = 4;
			r_block_h = 4;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_5x4_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_5x4_SRGB_BLOCK:
			r_block_w = 5;
			r_block_h = 4;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_5x5_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_5x5_SRGB_BLOCK:
			r_block_w = 5;
			r_block_h = 5;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_6x5_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_6x5_SRGB_BLOCK:
			r_block_w = 6;
			r_block_h = 5;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_6x6_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_6x6_SRGB_BLOCK:
			r_block_w = 6;
			r_block_h = 6;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x5_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x5_SRGB_BLOCK:
			r_block_w = 8;
			r_block_h = 5;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x6_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x6_SRGB_BLOCK:
			r_block_w = 8;
			r_block_h = 6;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x8_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_8x8_SRGB_BLOCK:
			r_block_w = 8;
			r_block_h = 8;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x5_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x5_SRGB_BLOCK:
			r_block_w = 10;
			r_block_h = 5;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x6_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x6_SRGB_BLOCK:
			r_block_w = 10;
			r_block_h = 6;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x8_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x8_SRGB_BLOCK:
			r_block_w = 10;
			r_block_h = 8;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x10_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_10x10_SRGB_BLOCK:
			r_block_w = 10;
			r_block_h = 10;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_12x10_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_12x10_SRGB_BLOCK:
			r_block_w = 12;
			r_block_h = 10;
			return 16;
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_12x12_UNORM_BLOCK:
		case RenderingDeviceCommons::DATA_FORMAT_ASTC_12x12_SRGB_BLOCK:
			r_block_w = 12;
			r_block_h = 12;
			return 16;
		default:
			return 0;
	}
}

RenderingDeviceDriver::QueryPoolID RenderingDeviceDriverWebGPU::timestamp_query_pool_create(uint32_t p_query_count) {
	WEBGPU_MAIN_THREAD_GUARD(timestamp_query_pool_create(p_query_count));
	TimestampPoolInfo *pool = memnew(TimestampPoolInfo);
	pool->count = p_query_count;
	pool->values.resize(p_query_count);
	for (uint32_t i = 0; i < p_query_count; i++) {
		pool->values[i] = 0;
	}
	// Without the feature the pool stays a benign token: the engine's profiling
	// scaffolding keeps working and simply reads zeros, as it did before.
	if (device_has_timestamp_query && p_query_count > 0) {
		WGPUQuerySetDescriptor qs_desc = WGPU_QUERY_SET_DESCRIPTOR_INIT;
		qs_desc.type = WGPUQueryType_Timestamp;
		// One extra slot as a scratch target. The pinned emdawnwebgpu bindings
		// marshal BOTH write indices unconditionally, so WGPU_QUERY_SET_INDEX_
		// UNDEFINED reaches JS as a literal 0xFFFFFFFF index and invalidates the
		// pass - the port cannot express "no end-of-pass timestamp". Pointing
		// the unwanted end write at a throwaway slot is the way to say it.
		qs_desc.count = p_query_count + 1;
		pool->query_set = wgpuDeviceCreateQuerySet(device, &qs_desc);

		WGPUBufferDescriptor rb = WGPU_BUFFER_DESCRIPTOR_INIT;
		rb.size = (uint64_t)p_query_count * sizeof(uint64_t);
		rb.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
		pool->resolve = wgpuDeviceCreateBuffer(device, &rb);

		WGPUBufferDescriptor mb = WGPU_BUFFER_DESCRIPTOR_INIT;
		mb.size = rb.size;
		mb.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
		pool->readback = wgpuDeviceCreateBuffer(device, &mb);
	}
	timestamp_pools.push_back(pool);
	return QueryPoolID(pool);
}

void RenderingDeviceDriverWebGPU::timestamp_query_pool_free(QueryPoolID p_pool_id) {
	WEBGPU_MAIN_THREAD_GUARD(timestamp_query_pool_free(p_pool_id));
	TimestampPoolInfo *pool = (TimestampPoolInfo *)p_pool_id.id;
	ERR_FAIL_NULL(pool);
	timestamp_pools.erase(pool);
	if (pool->query_set) {
		wgpuQuerySetRelease(pool->query_set);
	}
	if (pool->resolve) {
		wgpuBufferRelease(pool->resolve);
	}
	if (pool->readback) {
		// A map may still be in flight; releasing is safe, the callback holds
		// no reference to the pool beyond the id it was given.
		wgpuBufferRelease(pool->readback);
	}
	memdelete(pool);
}

RenderingDeviceDriver::BufferID RenderingDeviceDriverWebGPU::buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) {
	WEBGPU_MAIN_THREAD_GUARD(buffer_create(p_size, p_usage, p_allocation_type, p_frames_drawn));

	ERR_FAIL_COND_V_MSG(p_usage.has_flag(BUFFER_USAGE_DEVICE_ADDRESS_BIT), BufferID(), "Buffer device addresses are not supported by WebGPU.");

	WGPUBufferUsage usage = WGPUBufferUsage_None;
	if (p_usage.has_flag(BUFFER_USAGE_TRANSFER_FROM_BIT)) {
		usage |= WGPUBufferUsage_CopySrc;
	}
	if (p_usage.has_flag(BUFFER_USAGE_TRANSFER_TO_BIT)) {
		usage |= WGPUBufferUsage_CopyDst;
	}
	if (p_usage.has_flag(BUFFER_USAGE_UNIFORM_BIT)) {
		usage |= WGPUBufferUsage_Uniform;
	}
	if (p_usage.has_flag(BUFFER_USAGE_STORAGE_BIT)) {
		usage |= WGPUBufferUsage_Storage;
	}
	if (p_usage.has_flag(BUFFER_USAGE_TEXEL_BIT)) {
		// No WGSL equivalent; bound only as a placeholder storage buffer.
		usage |= WGPUBufferUsage_Storage;
	}
	if (p_usage.has_flag(BUFFER_USAGE_INDEX_BIT)) {
		usage |= WGPUBufferUsage_Index;
	}
	if (p_usage.has_flag(BUFFER_USAGE_VERTEX_BIT)) {
		usage |= WGPUBufferUsage_Vertex;
	}
	if (p_usage.has_flag(BUFFER_USAGE_INDIRECT_BIT)) {
		usage |= WGPUBufferUsage_Indirect;
	}
	if (p_allocation_type == MEMORY_ALLOCATION_TYPE_CPU) {
		// The shadow flush in buffer_unmap() uses wgpuQueueWriteBuffer.
		usage |= WGPUBufferUsage_CopyDst;
	}

	const bool dynamic = p_usage.has_flag(BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT);
	// Round up to 4 bytes: wgpuQueueWriteBuffer only accepts 4-byte-aligned
	// sizes, so the direct-write upload path pads odd-sized writes. The padding
	// must FIT inside the buffer, or the write falls back to an encoded copy -
	// which never lands in session-created buffers under XR. Exactly-sized odd
	// allocations (16-bit index buffers with an odd index count, e.g. glTF
	// models loaded mid-session) rendered invisible without this.
	uint64_t alloc_size = (p_size + 3ull) & ~3ull;
	uint64_t slice_stride = 0;
	if (dynamic) {
		// One slice per frame in flight; dynamic offsets must be multiples of
		// the offset alignment, so the stride is padded.
		const uint64_t alignment = MAX(device_limits.minUniformBufferOffsetAlignment, device_limits.minStorageBufferOffsetAlignment);
		slice_stride = (p_size + alignment - 1) & ~(alignment - 1);
		alloc_size = slice_stride * frame_count;
		usage |= WGPUBufferUsage_CopyDst; // The shadow flush uses wgpuQueueWriteBuffer.
	}

	WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	buffer_desc.usage = usage;
	buffer_desc.size = alloc_size;
	WGPUBuffer wgpu_buffer = wgpuDeviceCreateBuffer(device, &buffer_desc);
	ERR_FAIL_NULL_V(wgpu_buffer, BufferID());

	BufferInfo *buffer = memnew(BufferInfo);
	buffer->buffer = wgpu_buffer;
	buffer->size = alloc_size;
	buffer->creation_epoch = xr_import_epoch;
	if (dynamic) {
		buffer->dynamic = true;
		buffer->slice_size = p_size;
		buffer->slice_stride = slice_stride;
		buffer->slice_count = frame_count;
		buffer->shadow = (uint8_t *)memalloc(alloc_size);
		memset(buffer->shadow, 0, alloc_size);
		dynamic_buffers_all.push_back(buffer);
		return BufferID(buffer);
	}
	if (p_allocation_type == MEMORY_ALLOCATION_TYPE_CPU) {
		// Padded like the GPU buffer so aligned tail writes read valid bytes.
		buffer->shadow = (uint8_t *)memalloc(alloc_size);
		memset(buffer->shadow + p_size, 0, alloc_size - p_size);
	}
	return BufferID(buffer);
}

void RenderingDeviceDriverWebGPU::buffer_free(BufferID p_buffer) {
	WEBGPU_MAIN_THREAD_GUARD(buffer_free(p_buffer));
	resource_epoch++; // Invalidates cached render bundles (see BundleCacheEntry).
	{
		BufferInfo *freed = (BufferInfo *)p_buffer.id;
		if (freed->dynamic) {
			dynamic_buffers_all.erase(freed);
		}
	}
	BufferInfo *buffer = (BufferInfo *)p_buffer.id;
	wgpuBufferRelease(buffer->buffer);
	if (buffer->shadow != nullptr) {
		memfree(buffer->shadow);
	}
	if (buffer->last_upload != nullptr) {
		memfree(buffer->last_upload);
	}
	memdelete(buffer);
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_allocation_size(BufferID p_buffer) {
	return ((BufferInfo *)p_buffer.id)->size;
}

uint8_t *RenderingDeviceDriverWebGPU::buffer_persistent_map_advance(BufferID p_buffer, uint64_t p_frames_drawn) {
	WEBGPU_MAIN_THREAD_GUARD(buffer_persistent_map_advance(p_buffer, p_frames_drawn));
	BufferInfo *buffer = (BufferInfo *)p_buffer.id;
	ERR_FAIL_COND_V_MSG(!buffer->dynamic, nullptr, "Buffer must have BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT. Use buffer_map() instead.");
	buffer->frame_idx = (buffer->frame_idx + 1u) % buffer->slice_count;
	buffer->dirty = true;
	return buffer->shadow + buffer->frame_idx * buffer->slice_stride;
}

void RenderingDeviceDriverWebGPU::_flush_dynamic_slice(BufferInfo *p_buffer) {
	// The engine writes through the persistent map, so the flush API carries
	// no range: diff the slice against the bytes last uploaded and write only
	// the changed span. An unchanged slice costs one vectorized scan instead
	// of a multi-megabyte queue write; a canvas-style buffer that fills only
	// the head of its slice uploads only that head.
	const uint64_t offset = p_buffer->frame_idx * p_buffer->slice_stride;
	const uint8_t *src = p_buffer->shadow + offset;
	const uint64_t stride = p_buffer->slice_stride;
	if (!use_diff_flush) {
		_counted_write_buffer(p_buffer->buffer, offset, src, stride);
		p_buffer->dirty = false;
		return;
	}
	if (p_buffer->last_upload == nullptr) {
		p_buffer->last_upload = (uint8_t *)memalloc(p_buffer->size);
	}
	uint8_t *last = p_buffer->last_upload + offset;
	const uint32_t slice_bit = 1u << (p_buffer->frame_idx & 31u);
	if ((p_buffer->slice_inited_mask & slice_bit) == 0) {
		p_buffer->slice_inited_mask |= slice_bit;
		_counted_write_buffer(p_buffer->buffer, offset, src, stride);
		memcpy(last, src, stride);
		p_buffer->dirty = false;
		return;
	}
	// Trim from both ends at 64-byte granularity (wgpuQueueWriteBuffer needs
	// 4-byte alignment; 64 keeps the scan in memcmp's vectorized fast path).
	uint64_t lo = 0;
	while (lo < stride) {
		const uint64_t step = MIN(64ull, stride - lo);
		if (memcmp(last + lo, src + lo, step) != 0) {
			break;
		}
		lo += step;
	}
	if (lo >= stride) {
		p_buffer->dirty = false;
		return; // Identical - nothing to upload.
	}
	uint64_t hi = stride;
	while (hi > lo + 64) {
		const uint64_t step = 64;
		if (memcmp(last + hi - step, src + hi - step, step) != 0) {
			break;
		}
		hi -= step;
	}
	_counted_write_buffer(p_buffer->buffer, offset + lo, src + lo, hi - lo);
	memcpy(last + lo, src + lo, hi - lo);
	p_buffer->dirty = false;
}

void RenderingDeviceDriverWebGPU::buffer_flush(BufferID p_buffer) {
	WEBGPU_MAIN_THREAD_GUARD(buffer_flush(p_buffer));
	BufferInfo *buffer = (BufferInfo *)p_buffer.id;
	ERR_FAIL_COND(!buffer->dynamic);
	// Executes before any subsequently submitted command buffer, matching the
	// staging shadow semantics in buffer_unmap().
	_flush_dynamic_slice(buffer);
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_dynamic_offsets(Span<BufferID> p_buffers) {
	uint64_t mask = 0u;
	uint64_t shift = 0u;
	for (const BufferID &buffer_id : p_buffers) {
		const BufferInfo *buffer = (const BufferInfo *)buffer_id.id;
		if (!buffer->dynamic) {
			continue;
		}
		// Two bits per buffer: the frame count never exceeds 4.
		mask |= uint64_t(buffer->frame_idx) << shift;
		shift += 2UL;
	}
	return mask;
}

uint32_t RenderingDeviceDriverWebGPU::uniform_sets_get_dynamic_offsets(VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count) const {
	uint32_t mask = 0u;
	uint32_t shift = 0u;
	for (uint32_t i = 0; i < p_set_count; i++) {
		const UniformSetInfo *uniform_set = (const UniformSetInfo *)p_uniform_sets[i].id;
		for (const BufferInfo *dynamic_buffer : uniform_set->dynamic_buffers) {
			// Four bits per buffer, decoded in command_bind_*_uniform_sets.
			mask |= dynamic_buffer->frame_idx << shift;
			shift += 4u;
		}
	}
	return mask;
}

uint8_t *RenderingDeviceDriverWebGPU::buffer_map(BufferID p_buffer) {
	WEBGPU_MAIN_THREAD_GUARD(buffer_map(p_buffer));
	ERR_FAIL_COND_V_MSG(((BufferInfo *)p_buffer.id)->dynamic, nullptr, "Use buffer_persistent_map_advance() for dynamic buffers.");
	BufferInfo *buffer = (BufferInfo *)p_buffer.id;
	ERR_FAIL_NULL_V_MSG(buffer->shadow, nullptr, "Only CPU (upload) buffers can be mapped by the WebGPU driver; downloads are not supported yet.");
	return buffer->shadow;
}

void RenderingDeviceDriverWebGPU::buffer_unmap(BufferID p_buffer) {
	WEBGPU_MAIN_THREAD_GUARD(buffer_unmap(p_buffer));
	BufferInfo *buffer = (BufferInfo *)p_buffer.id;
	ERR_FAIL_NULL(buffer->shadow);
	_counted_write_buffer(buffer->buffer, 0, buffer->shadow, buffer->size);
}

// WebGPU only allows view reinterpretation between srgb/non-srgb siblings,
// and the sibling must be declared in viewFormats at texture creation.
static bool _wgpu_format_supports_storage(WGPUTextureFormat p_format, bool p_tier1) {
	switch (p_format) {
		case WGPUTextureFormat_RGB10A2Unorm:
			// texture-formats-tier1 grants (read/write-only) storage access;
			// the format is deliberately NOT in the substitution table, so a
			// tier1 device runs the compute octmap path the patch-68 probe
			// selects, and every other device keeps the raster path.
			return p_tier1;
		case WGPUTextureFormat_RGBA8Unorm:
		case WGPUTextureFormat_RGBA8Snorm:
		case WGPUTextureFormat_RGBA8Uint:
		case WGPUTextureFormat_RGBA8Sint:
		case WGPUTextureFormat_RGBA16Float:
		case WGPUTextureFormat_RGBA16Uint:
		case WGPUTextureFormat_RGBA16Sint:
		case WGPUTextureFormat_R32Float:
		case WGPUTextureFormat_RG32Float:
		case WGPUTextureFormat_RGBA32Float:
		case WGPUTextureFormat_R32Uint:
		case WGPUTextureFormat_R32Sint:
		case WGPUTextureFormat_RG32Uint:
		case WGPUTextureFormat_RG32Sint:
		case WGPUTextureFormat_RGBA32Uint:
		case WGPUTextureFormat_RGBA32Sint:
			return true;
		default:
			return false;
	}
}

// Storage-format substitution mirror of the bake-time SPIR-V pass: formats
// WGSL cannot use for storage images are created as value-compatible legal
// ones (the baked shaders declare the substituted format, so bindings and
// layouts agree end to end).
static WGPUTextureFormat _wgpu_storage_substitute(WGPUTextureFormat p_format) {
	switch (p_format) {
		case WGPUTextureFormat_R16Float:
			return WGPUTextureFormat_R32Float;
		case WGPUTextureFormat_RG16Float:
			return WGPUTextureFormat_RGBA16Float;
		case WGPUTextureFormat_RG11B10Ufloat:
			return WGPUTextureFormat_RGBA16Float;
		case WGPUTextureFormat_R8Unorm:
			// r32f rather than rgba8: read_write access stays legal.
			return WGPUTextureFormat_R32Float;
		case WGPUTextureFormat_RG8Unorm:
			return WGPUTextureFormat_RGBA8Unorm;
		case WGPUTextureFormat_RG16Sint:
			return WGPUTextureFormat_RG32Sint;
		case WGPUTextureFormat_R16Uint:
			return WGPUTextureFormat_R32Uint;
		default:
			return p_format;
	}
}

static WGPUTextureFormat _wgpu_srgb_sibling(WGPUTextureFormat p_format) {
	switch (p_format) {
		case WGPUTextureFormat_RGBA8Unorm:
			return WGPUTextureFormat_RGBA8UnormSrgb;
		case WGPUTextureFormat_RGBA8UnormSrgb:
			return WGPUTextureFormat_RGBA8Unorm;
		case WGPUTextureFormat_BGRA8Unorm:
			return WGPUTextureFormat_BGRA8UnormSrgb;
		case WGPUTextureFormat_BGRA8UnormSrgb:
			return WGPUTextureFormat_BGRA8Unorm;
		case WGPUTextureFormat_BC1RGBAUnorm:
			return WGPUTextureFormat_BC1RGBAUnormSrgb;
		case WGPUTextureFormat_BC1RGBAUnormSrgb:
			return WGPUTextureFormat_BC1RGBAUnorm;
		case WGPUTextureFormat_BC2RGBAUnorm:
			return WGPUTextureFormat_BC2RGBAUnormSrgb;
		case WGPUTextureFormat_BC2RGBAUnormSrgb:
			return WGPUTextureFormat_BC2RGBAUnorm;
		case WGPUTextureFormat_BC3RGBAUnorm:
			return WGPUTextureFormat_BC3RGBAUnormSrgb;
		case WGPUTextureFormat_BC3RGBAUnormSrgb:
			return WGPUTextureFormat_BC3RGBAUnorm;
		case WGPUTextureFormat_BC7RGBAUnorm:
			return WGPUTextureFormat_BC7RGBAUnormSrgb;
		case WGPUTextureFormat_BC7RGBAUnormSrgb:
			return WGPUTextureFormat_BC7RGBAUnorm;
		case WGPUTextureFormat_ETC2RGB8Unorm:
			return WGPUTextureFormat_ETC2RGB8UnormSrgb;
		case WGPUTextureFormat_ETC2RGB8UnormSrgb:
			return WGPUTextureFormat_ETC2RGB8Unorm;
		case WGPUTextureFormat_ETC2RGB8A1Unorm:
			return WGPUTextureFormat_ETC2RGB8A1UnormSrgb;
		case WGPUTextureFormat_ETC2RGB8A1UnormSrgb:
			return WGPUTextureFormat_ETC2RGB8A1Unorm;
		case WGPUTextureFormat_ETC2RGBA8Unorm:
			return WGPUTextureFormat_ETC2RGBA8UnormSrgb;
		case WGPUTextureFormat_ETC2RGBA8UnormSrgb:
			return WGPUTextureFormat_ETC2RGBA8Unorm;
		case WGPUTextureFormat_ASTC4x4Unorm:
			return WGPUTextureFormat_ASTC4x4UnormSrgb;
		case WGPUTextureFormat_ASTC4x4UnormSrgb:
			return WGPUTextureFormat_ASTC4x4Unorm;
		case WGPUTextureFormat_ASTC5x4Unorm:
			return WGPUTextureFormat_ASTC5x4UnormSrgb;
		case WGPUTextureFormat_ASTC5x4UnormSrgb:
			return WGPUTextureFormat_ASTC5x4Unorm;
		case WGPUTextureFormat_ASTC5x5Unorm:
			return WGPUTextureFormat_ASTC5x5UnormSrgb;
		case WGPUTextureFormat_ASTC5x5UnormSrgb:
			return WGPUTextureFormat_ASTC5x5Unorm;
		case WGPUTextureFormat_ASTC6x5Unorm:
			return WGPUTextureFormat_ASTC6x5UnormSrgb;
		case WGPUTextureFormat_ASTC6x5UnormSrgb:
			return WGPUTextureFormat_ASTC6x5Unorm;
		case WGPUTextureFormat_ASTC6x6Unorm:
			return WGPUTextureFormat_ASTC6x6UnormSrgb;
		case WGPUTextureFormat_ASTC6x6UnormSrgb:
			return WGPUTextureFormat_ASTC6x6Unorm;
		case WGPUTextureFormat_ASTC8x5Unorm:
			return WGPUTextureFormat_ASTC8x5UnormSrgb;
		case WGPUTextureFormat_ASTC8x5UnormSrgb:
			return WGPUTextureFormat_ASTC8x5Unorm;
		case WGPUTextureFormat_ASTC8x6Unorm:
			return WGPUTextureFormat_ASTC8x6UnormSrgb;
		case WGPUTextureFormat_ASTC8x6UnormSrgb:
			return WGPUTextureFormat_ASTC8x6Unorm;
		case WGPUTextureFormat_ASTC8x8Unorm:
			return WGPUTextureFormat_ASTC8x8UnormSrgb;
		case WGPUTextureFormat_ASTC8x8UnormSrgb:
			return WGPUTextureFormat_ASTC8x8Unorm;
		case WGPUTextureFormat_ASTC10x5Unorm:
			return WGPUTextureFormat_ASTC10x5UnormSrgb;
		case WGPUTextureFormat_ASTC10x5UnormSrgb:
			return WGPUTextureFormat_ASTC10x5Unorm;
		case WGPUTextureFormat_ASTC10x6Unorm:
			return WGPUTextureFormat_ASTC10x6UnormSrgb;
		case WGPUTextureFormat_ASTC10x6UnormSrgb:
			return WGPUTextureFormat_ASTC10x6Unorm;
		case WGPUTextureFormat_ASTC10x8Unorm:
			return WGPUTextureFormat_ASTC10x8UnormSrgb;
		case WGPUTextureFormat_ASTC10x8UnormSrgb:
			return WGPUTextureFormat_ASTC10x8Unorm;
		case WGPUTextureFormat_ASTC10x10Unorm:
			return WGPUTextureFormat_ASTC10x10UnormSrgb;
		case WGPUTextureFormat_ASTC10x10UnormSrgb:
			return WGPUTextureFormat_ASTC10x10Unorm;
		case WGPUTextureFormat_ASTC12x10Unorm:
			return WGPUTextureFormat_ASTC12x10UnormSrgb;
		case WGPUTextureFormat_ASTC12x10UnormSrgb:
			return WGPUTextureFormat_ASTC12x10Unorm;
		case WGPUTextureFormat_ASTC12x12Unorm:
			return WGPUTextureFormat_ASTC12x12UnormSrgb;
		case WGPUTextureFormat_ASTC12x12UnormSrgb:
			return WGPUTextureFormat_ASTC12x12Unorm;
		default:
			return WGPUTextureFormat_Undefined;
	}
}

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create(const TextureFormat &p_format, const TextureView &p_view) {
	WEBGPU_MAIN_THREAD_GUARD(texture_create(p_format, p_view));
	ERR_FAIL_COND_V_MSG(p_view.format != p_format.format, TextureID(), "Texture views with a different format are not supported by the WebGPU driver yet.");
	const bool identity_swizzle = p_view.swizzle_r == TEXTURE_SWIZZLE_R && p_view.swizzle_g == TEXTURE_SWIZZLE_G && p_view.swizzle_b == TEXTURE_SWIZZLE_B && p_view.swizzle_a == TEXTURE_SWIZZLE_A;
	TextureInfo::SwizzleExpand swizzle_expand = TextureInfo::SWIZZLE_EXPAND_NONE;
	// Native view swizzles (texture-component-swizzle, stable since Chrome
	// 143): the texture keeps its real format and the default view remaps
	// components, replacing the CPU texel expansion below. Swizzled views are
	// sampling-only in WebGPU, so any attachment or storage usage falls back
	// to the emulation.
	const bool native_swizzle = !identity_swizzle && device_has_texture_swizzle &&
			!(p_format.usage_bits & (TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | TEXTURE_USAGE_STORAGE_BIT | TEXTURE_USAGE_STORAGE_ATOMIC_BIT | TEXTURE_USAGE_VRS_ATTACHMENT_BIT));
	if (!identity_swizzle && !native_swizzle) {
		// WebGPU has no view swizzle: emulate the engine's patterns by
		// promoting to RGBA8 and expanding texels at upload.
		const bool rrr = p_view.swizzle_r == TEXTURE_SWIZZLE_R && p_view.swizzle_g == TEXTURE_SWIZZLE_R && p_view.swizzle_b == TEXTURE_SWIZZLE_R;
		if (p_format.format == DATA_FORMAT_R8G8_UNORM && rrr && p_view.swizzle_a == TEXTURE_SWIZZLE_G) {
			swizzle_expand = TextureInfo::SWIZZLE_EXPAND_RG_TO_RRRG;
		} else if (p_format.format == DATA_FORMAT_R8_UNORM && rrr && p_view.swizzle_a == TEXTURE_SWIZZLE_ONE) {
			swizzle_expand = TextureInfo::SWIZZLE_EXPAND_R_TO_RRR1;
		} else if (p_format.format == DATA_FORMAT_R8_UNORM && p_view.swizzle_r == TEXTURE_SWIZZLE_ZERO && p_view.swizzle_g == TEXTURE_SWIZZLE_ZERO && p_view.swizzle_b == TEXTURE_SWIZZLE_ZERO && p_view.swizzle_a == TEXTURE_SWIZZLE_R) {
			swizzle_expand = TextureInfo::SWIZZLE_EXPAND_R_TO_000R;
		} else if (p_view.swizzle_r == TEXTURE_SWIZZLE_R && p_view.swizzle_g == TEXTURE_SWIZZLE_G && p_view.swizzle_b == TEXTURE_SWIZZLE_B && p_view.swizzle_a == TEXTURE_SWIZZLE_ONE) {
			// Force-opaque alpha: pass through; the stored alpha of the
			// engine's formats using this pattern is already opaque.
			swizzle_expand = TextureInfo::SWIZZLE_EXPAND_NONE;
		} else {
			ERR_FAIL_V_MSG(TextureID(), vformat("Unsupported texture swizzle (%d,%d,%d,%d) for format %d on WebGPU.", p_view.swizzle_r, p_view.swizzle_g, p_view.swizzle_b, p_view.swizzle_a, p_format.format));
		}
	}
	WGPUTextureFormat wgpu_format = swizzle_expand != TextureInfo::SWIZZLE_EXPAND_NONE ? WGPUTextureFormat_RGBA8Unorm : _data_format_to_wgpu(p_format.format);
	if (p_format.samples != TEXTURE_SAMPLES_1 && p_format.format == DATA_FORMAT_R8G8_UINT) {
		// The rg8ui->rg32uint storage substitution only applies to
		// single-sample textures (rg32uint cannot be multisampled; the MSAA
		// voxel GI buffer is consumed by a compute resolve sampling u32).
		wgpu_format = WGPUTextureFormat_RG8Uint;
	}
	ERR_FAIL_COND_V_MSG(wgpu_format == WGPUTextureFormat_Undefined, TextureID(), vformat("Unsupported texture format %d on the WebGPU driver.", p_format.format));
	ERR_FAIL_COND_V_MSG(p_format.samples != TEXTURE_SAMPLES_1 && p_format.samples != TEXTURE_SAMPLES_4, TextureID(), "WebGPU only supports 1 or 4 samples per texture.");

	WGPUTextureUsage usage = WGPUTextureUsage_None;
	if (p_format.usage_bits & (TEXTURE_USAGE_SAMPLING_BIT | TEXTURE_USAGE_INPUT_ATTACHMENT_BIT)) {
		usage |= WGPUTextureUsage_TextureBinding;
	}
	if (p_format.usage_bits & (TEXTURE_USAGE_STORAGE_BIT | TEXTURE_USAGE_STORAGE_ATOMIC_BIT)) {
		// Degrade instead of failing: upstream creates the default VRS
		// texture as R8Uint storage unconditionally, a combination WebGPU
		// rejects at the descriptor level. The texture only breaks if a
		// shader actually reads it as a storage image, which Dawn validates
		// at bind group creation.
		if (!_wgpu_format_supports_storage(wgpu_format, device_has_texture_formats_tier1)) {
			// Mirror of the bake-time SPIR-V substitution: the shaders
			// declare the substituted format, so the texture must be it.
			wgpu_format = _wgpu_storage_substitute(wgpu_format);
		}
		if (_wgpu_format_supports_storage(wgpu_format, device_has_texture_formats_tier1)) {
			usage |= WGPUTextureUsage_StorageBinding;
		}
		// Tint converts read-only storage images into sampled textures
		// (textureLoad), and clears of non-renderable textures go through
		// writeTexture; storage textures therefore always carry the
		// TextureBinding and CopyDst usages too.
		usage |= WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	}
	if (p_format.usage_bits & (TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		usage |= WGPUTextureUsage_RenderAttachment;
	}
	if (p_format.usage_bits & (TEXTURE_USAGE_CAN_COPY_FROM_BIT | TEXTURE_USAGE_CPU_READ_BIT)) {
		usage |= WGPUTextureUsage_CopySrc;
	}
	if (p_format.usage_bits & (TEXTURE_USAGE_CAN_COPY_TO_BIT | TEXTURE_USAGE_CAN_UPDATE_BIT)) {
		usage |= WGPUTextureUsage_CopyDst;
	}

	// Tile-memory scratch (Chromium 149+): a transient attachment's contents
	// never leave the pass, so the browser can keep it in on-chip tile memory
	// without allocating VRAM - the biggest bandwidth lever on mobile GPUs.
	// The engine's TRANSIENT_BIT carries exactly that contract (the Vulkan
	// driver maps it to lazily-allocated transient images); WebGPU's rules
	// forbid every other usage on such a texture, including declared view
	// formats, so the usage collapses to the attachment bit alone. Browsers
	// without the flag simply treat the texture as a plain attachment.
	const bool transient_attachment = device_has_transient_attachments &&
			(p_format.usage_bits & TEXTURE_USAGE_TRANSIENT_BIT) &&
			(p_format.usage_bits & (TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
	if (transient_attachment) {
		usage = WGPUTextureUsage_RenderAttachment | (WGPUTextureUsage)WGPUTextureUsage_TransientAttachment;
	}
	WGPUTextureDescriptor texture_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
	texture_desc.usage = usage;
	texture_desc.format = wgpu_format;
	const WGPUTextureFormat srgb_sibling = _wgpu_srgb_sibling(wgpu_format);
	// A view may only use a format the texture declared up front, and an sRGB
	// view may not carry storage usage. WebGPU lets a view name its own usage
	// to resolve that, but the pinned emdawnwebgpu bindings do not marshal
	// WGPUTextureViewDescriptor::usage, so a view always inherits the
	// texture's - which means a texture cannot be both storage-capable and
	// readable as sRGB here, and one of the two has to give.
	//
	// The engine says which it wants: a texture that lists the sRGB sibling
	// in shareable_formats is going to be read through an sRGB view. That is
	// the decal atlas, which is filled by raster draws and never bound as a
	// storage image, so its storage usage is the safe thing to drop. Skipping
	// the declaration instead is what made decals sample without the
	// sRGB->linear conversion, roughly doubling their mid-tones until they
	// saturated into a hard-edged block.
	// Only a real sibling counts: a shareable format WebGPU has no mapping
	// for (SDFGI's R4G4B4A4 occlusion view) must not read as "sibling
	// shareable" through two Undefined values comparing equal.
	bool srgb_sibling_shareable = false;
	for (uint32_t i = 0; srgb_sibling != WGPUTextureFormat_Undefined && i < (uint32_t)p_format.shareable_formats.size(); i++) {
		if (_data_format_to_wgpu(p_format.shareable_formats[i]) == srgb_sibling) {
			srgb_sibling_shareable = true;
			break;
		}
	}
	if (srgb_sibling_shareable && (usage & WGPUTextureUsage_StorageBinding)) {
		usage &= ~(WGPUTextureUsage)WGPUTextureUsage_StorageBinding;
		texture_desc.usage = usage;
		print_verbose("WebGPU: dropped storage usage from a texture that is read as sRGB (a view cannot have both).");
	}
	// Declaring a view format can cost a driver's lossless compression, so
	// only ask when the sibling can actually be used.
	const bool declare_srgb_sibling = srgb_sibling != WGPUTextureFormat_Undefined && (usage & WGPUTextureUsage_StorageBinding) == 0 && !transient_attachment;
	if (declare_srgb_sibling) {
		texture_desc.viewFormatCount = 1;
		texture_desc.viewFormats = &srgb_sibling;
	}
	texture_desc.mipLevelCount = p_format.mipmaps;
	texture_desc.sampleCount = p_format.samples == TEXTURE_SAMPLES_4 ? 4 : 1;
	switch (p_format.texture_type) {
		case TEXTURE_TYPE_1D:
			texture_desc.dimension = WGPUTextureDimension_1D;
			texture_desc.size = { p_format.width, 1, 1 };
			break;
		case TEXTURE_TYPE_3D:
			texture_desc.dimension = WGPUTextureDimension_3D;
			texture_desc.size = { p_format.width, p_format.height, p_format.depth };
			break;
		default: // 2D, cube, and array variants: layers go in depthOrArrayLayers.
			texture_desc.dimension = WGPUTextureDimension_2D;
			texture_desc.size = { p_format.width, p_format.height, p_format.array_layers };
			break;
	}

	WGPUTexture wgpu_texture = wgpuDeviceCreateTexture(device, &texture_desc);
	ERR_FAIL_NULL_V(wgpu_texture, TextureID());
	// Explicit view: the default view of a cube texture is 2d-array, which
	// breaks cube sampling.
	WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	view_desc.format = wgpu_format; // p_view.format equals p_format.format (checked above); use the possibly promoted format.
	view_desc.dimension = _texture_type_to_wgpu_view_dimension(p_format.texture_type);
	WGPUTextureComponentSwizzleDescriptor swizzle_desc = WGPU_TEXTURE_COMPONENT_SWIZZLE_DESCRIPTOR_INIT;
	if (native_swizzle) {
		swizzle_desc.swizzle.r = _texture_swizzle_to_wgpu(p_view.swizzle_r, WGPUComponentSwizzle_R);
		swizzle_desc.swizzle.g = _texture_swizzle_to_wgpu(p_view.swizzle_g, WGPUComponentSwizzle_G);
		swizzle_desc.swizzle.b = _texture_swizzle_to_wgpu(p_view.swizzle_b, WGPUComponentSwizzle_B);
		swizzle_desc.swizzle.a = _texture_swizzle_to_wgpu(p_view.swizzle_a, WGPUComponentSwizzle_A);
		// Swizzled views are sampling-only; the usage gate above guarantees
		// TextureBinding is a subset of the texture's usage.
		view_desc.usage = WGPUTextureUsage_TextureBinding;
		view_desc.nextInChain = &swizzle_desc.chain;
	}
	WGPUTextureView wgpu_view = wgpuTextureCreateView(wgpu_texture, &view_desc);
	if (wgpu_view == nullptr) {
		wgpuTextureRelease(wgpu_texture);
		ERR_FAIL_V(TextureID());
	}

	TextureInfo *texture = memnew(TextureInfo);
	texture->texture = wgpu_texture;
	texture->view = wgpu_view;
	texture->view_dimension = view_desc.dimension;
	texture->usage = usage;
	texture->wgpu_format = wgpu_format;
	texture->srgb_sibling_declared = declare_srgb_sibling;
	texture->format = p_format.format;
	texture->swizzle_expand = swizzle_expand;
	const uint64_t texel_size = MAX(1U, _data_format_texel_size(p_format.format));
	texture->allocation_size = (uint64_t)p_format.width * p_format.height * MAX(p_format.depth, p_format.array_layers) * texel_size * (p_format.mipmaps > 1 ? 4 : 3) / 3;
	return TextureID(texture);
}

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool p_depth_stencil, uint32_t p_mipmaps) {
	WEBGPU_MAIN_THREAD_GUARD(texture_create_from_extension(p_native_texture, p_type, p_format, p_array_layers, p_depth_stencil, p_mipmaps));
	// External imports only happen for XR layer textures; entering a session
	// moves buffer creation into the epoch whose encoded copies are broken
	// on Galaxy XR's Chrome (see command_copy_buffer).
	xr_import_epoch++;
	WGPUTexture wgpu_texture = (WGPUTexture)p_native_texture;
	ERR_FAIL_NULL_V(wgpu_texture, TextureID());
	WGPUTextureView wgpu_view = wgpuTextureCreateView(wgpu_texture, nullptr);
	ERR_FAIL_NULL_V(wgpu_view, TextureID());

	TextureInfo *texture = memnew(TextureInfo);
	texture->texture = wgpu_texture;
	texture->view = wgpu_view;
	texture->wgpu_format = _data_format_to_wgpu(p_format);
	texture->format = p_format;
	texture->owned = false;
	return TextureID(texture);
}

BitField<RenderingDeviceDriver::TextureUsageBits> RenderingDeviceDriverWebGPU::texture_get_usages_supported_by_format(DataFormat p_format, bool p_cpu_readable) {
	const WGPUTextureFormat format = _data_format_to_wgpu(p_format);
	// BitField's default constructor leaves the value UNINITIALIZED.
	BitField<TextureUsageBits> supported = {};
	if (format == WGPUTextureFormat_Undefined) {
		return supported;
	}
	if (format == WGPUTextureFormat_Depth32FloatStencil8) {
		// Optional WebGPU feature the loader does not request; report it
		// unsupported so callers fall back (e.g. the scene depth buffer
		// probe picks Depth24PlusStencil8 instead).
		return supported;
	}
	const bool bc_family = p_format >= DATA_FORMAT_BC1_RGB_UNORM_BLOCK && p_format <= DATA_FORMAT_BC7_SRGB_BLOCK;
	const bool etc2_family = p_format >= DATA_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && p_format <= DATA_FORMAT_EAC_R11G11_SNORM_BLOCK;
	const bool astc_family = p_format >= DATA_FORMAT_ASTC_4x4_UNORM_BLOCK && p_format <= DATA_FORMAT_ASTC_12x12_SRGB_BLOCK;
	const bool compressed = bc_family || etc2_family || astc_family;
	if ((bc_family && !device_has_texture_compression_bc) ||
			(etc2_family && !device_has_texture_compression_etc2) ||
			(astc_family && !device_has_texture_compression_astc)) {
		// The loader requests each texture-compression-* feature
		// adapter-gated (BC on desktop GPUs, ETC2/ASTC on mobile GPUs);
		// families the device lacks report zero usage so the engine takes
		// its CPU-decompress fallback instead of a failing create.
		return supported;
	}
	supported.set_flag(TEXTURE_USAGE_SAMPLING_BIT);
	supported.set_flag(TEXTURE_USAGE_CAN_UPDATE_BIT);
	supported.set_flag(TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	supported.set_flag(TEXTURE_USAGE_CAN_COPY_TO_BIT);
	if (_is_depth_stencil_format(p_format)) {
		supported.set_flag(TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
		supported.set_flag(TEXTURE_USAGE_INPUT_ATTACHMENT_BIT);
	} else if (!compressed) {
		supported.set_flag(TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);
		supported.set_flag(TEXTURE_USAGE_INPUT_ATTACHMENT_BIT);
	}
	switch (format) {
		case WGPUTextureFormat_RGBA8Unorm:
		case WGPUTextureFormat_RGBA8Snorm:
		case WGPUTextureFormat_RGBA8Uint:
		case WGPUTextureFormat_RGBA8Sint:
		case WGPUTextureFormat_RGBA16Float:
		case WGPUTextureFormat_RGBA16Uint:
		case WGPUTextureFormat_RGBA16Sint:
		case WGPUTextureFormat_R32Float:
		case WGPUTextureFormat_RG32Float:
		case WGPUTextureFormat_RGBA32Float:
		case WGPUTextureFormat_R32Uint:
		case WGPUTextureFormat_R32Sint:
		case WGPUTextureFormat_RG32Uint:
		case WGPUTextureFormat_RG32Sint:
		case WGPUTextureFormat_RGBA32Uint:
		case WGPUTextureFormat_RGBA32Sint:
			supported.set_flag(TEXTURE_USAGE_STORAGE_BIT);
			break;
		default:
			break;
	}
	if (_wgpu_storage_substitute(format) != format) {
		// Delivered through the storage-format substitution (the texture is
		// created as the substituted format; baked shaders declare it too).
		supported.set_flag(TEXTURE_USAGE_STORAGE_BIT);
	}
	if (format == WGPUTextureFormat_R32Uint || format == WGPUTextureFormat_R32Sint) {
		supported.set_flag(TEXTURE_USAGE_STORAGE_ATOMIC_BIT);
	}
	return supported;
}

// An sRGB view may not carry storage usage, so it names its usage explicitly
// rather than inheriting the texture's. Applies to views of views too, since
// the recorded format may already be the sibling.
//
// NOTE: the pinned emdawnwebgpu bindings do not marshal this field, so it has
// no effect today - texture_create keeps storage off any texture that will be
// viewed as sRGB, which is what actually makes these views valid. Setting it
// is still correct per the API, and becomes the real fix once the port
// forwards it.
void RenderingDeviceDriverWebGPU::_apply_srgb_view_usage(WGPUTextureViewDescriptor &r_view_desc, const TextureInfo *p_original) {
	if (r_view_desc.format != WGPUTextureFormat_RGBA8UnormSrgb && r_view_desc.format != WGPUTextureFormat_BGRA8UnormSrgb) {
		return;
	}
	if (p_original->usage == WGPUTextureUsage_None) {
		// Unknown source usage: assume the common render-target set.
		r_view_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
		return;
	}
	r_view_desc.usage = p_original->usage & ~(WGPUTextureUsage)WGPUTextureUsage_StorageBinding;
}

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create_shared(TextureID p_original_texture, const TextureView &p_view) {
	WEBGPU_MAIN_THREAD_GUARD(texture_create_shared(p_original_texture, p_view));
	const TextureInfo *original = (const TextureInfo *)p_original_texture.id;
	// WebGPU views have no component swizzle; non-identity swizzles fall back
	// to the plain channels.
	WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	view_desc.format = _data_format_to_wgpu(p_view.format);
	// Only srgb/non-srgb reinterpretation is expressible; other formats
	// degrade to the texture's own (raw reinterpretation is unavailable).
	if (view_desc.format != original->wgpu_format && (view_desc.format != _wgpu_srgb_sibling(original->wgpu_format) || !original->srgb_sibling_declared)) {
		view_desc.format = original->wgpu_format;
	}
	view_desc.dimension = original->view_dimension;
	_apply_srgb_view_usage(view_desc, original);
	WGPUTextureView view = wgpuTextureCreateView(original->texture, &view_desc);
	ERR_FAIL_NULL_V(view, TextureID());

	TextureInfo *texture = memnew(TextureInfo);
	texture->texture = original->texture;
	texture->view = view;
	texture->view_dimension = view_desc.dimension;
	texture->usage = original->usage;
	texture->wgpu_format = view_desc.format;
	texture->format = p_view.format;
	texture->srgb_sibling_declared = original->srgb_sibling_declared;
	texture->owned = false;
	return TextureID(texture);
}

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create_shared_from_slice(TextureID p_original_texture, const TextureView &p_view, TextureSliceType p_slice_type, uint32_t p_layer, uint32_t p_layers, uint32_t p_mipmap, uint32_t p_mipmaps) {
	WEBGPU_MAIN_THREAD_GUARD(texture_create_shared_from_slice(p_original_texture, p_view, p_slice_type, p_layer, p_layers, p_mipmap, p_mipmaps));
	const TextureInfo *original = (const TextureInfo *)p_original_texture.id;
	WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	view_desc.format = _data_format_to_wgpu(p_view.format);
	if (view_desc.format != original->wgpu_format && (view_desc.format != _wgpu_srgb_sibling(original->wgpu_format) || !original->srgb_sibling_declared)) {
		view_desc.format = original->wgpu_format;
	}
	_apply_srgb_view_usage(view_desc, original);
	view_desc.baseMipLevel = p_mipmap;
	view_desc.mipLevelCount = p_mipmaps;
	view_desc.baseArrayLayer = p_layer;
	view_desc.arrayLayerCount = p_layers;
	switch (p_slice_type) {
		case TEXTURE_SLICE_2D:
			view_desc.dimension = WGPUTextureViewDimension_2D;
			break;
		case TEXTURE_SLICE_2D_ARRAY:
			view_desc.dimension = WGPUTextureViewDimension_2DArray;
			break;
		case TEXTURE_SLICE_CUBEMAP:
			view_desc.dimension = WGPUTextureViewDimension_Cube;
			break;
		case TEXTURE_SLICE_3D:
			view_desc.dimension = WGPUTextureViewDimension_3D;
			break;
		default:
			ERR_FAIL_V_MSG(TextureID(), vformat("Unsupported texture slice type %d on the WebGPU driver.", p_slice_type));
	}
	WGPUTextureView view = wgpuTextureCreateView(original->texture, &view_desc);
	ERR_FAIL_NULL_V(view, TextureID());

	TextureInfo *texture = memnew(TextureInfo);
	texture->texture = original->texture;
	texture->view = view;
	texture->view_dimension = view_desc.dimension;
	texture->usage = original->usage;
	texture->wgpu_format = view_desc.format;
	texture->format = p_view.format;
	texture->srgb_sibling_declared = original->srgb_sibling_declared;
	texture->owned = false;
	return TextureID(texture);
}

void RenderingDeviceDriverWebGPU::texture_free(TextureID p_texture) {
	WEBGPU_MAIN_THREAD_GUARD(texture_free(p_texture));
	resource_epoch++; // Invalidates cached render bundles (see BundleCacheEntry).
	TextureInfo *texture = (TextureInfo *)p_texture.id;
	wgpuTextureViewRelease(texture->view);
	if (texture->depth_only_view != nullptr) {
		wgpuTextureViewRelease(texture->depth_only_view);
	}
	if (texture->owned) {
		wgpuTextureRelease(texture->texture);
	}
	memdelete(texture);
}

uint64_t RenderingDeviceDriverWebGPU::texture_get_allocation_size(TextureID p_texture) {
	return ((TextureInfo *)p_texture.id)->allocation_size;
}

void RenderingDeviceDriverWebGPU::texture_get_copyable_layout(TextureID p_texture, const TextureSubresource &p_subresource, TextureCopyableLayout *r_layout) {
	TextureInfo *texture = (TextureInfo *)p_texture.id;
	const uint32_t width = MAX(1U, wgpuTextureGetWidth(texture->texture) >> p_subresource.mipmap);
	const uint32_t height = MAX(1U, wgpuTextureGetHeight(texture->texture) >> p_subresource.mipmap);
	uint32_t block_w = 1, block_h = 1;
	const uint32_t block_bytes = _data_format_block_info(texture->format, block_w, block_h);
	if (block_bytes != 0) {
		// Block-compressed: the layout is expressed in block rows.
		const uint64_t blocks_x = (width + block_w - 1) / block_w;
		const uint64_t blocks_y = (height + block_h - 1) / block_h;
		const uint64_t row_pitch = (blocks_x * block_bytes + 255) & ~255ULL;
		r_layout->row_pitch = row_pitch;
		r_layout->size = row_pitch * blocks_y;
		return;
	}
	const uint32_t texel_size = _data_format_texel_size(texture->format);
	ERR_FAIL_COND_MSG(texel_size == 0, "Copyable layout requested for an unmapped format.");
	// WebGPU requires 256-byte row alignment for buffer <-> texture copies.
	const uint64_t row_pitch = (uint64_t(width) * texel_size + 255) & ~255ULL;
	r_layout->row_pitch = row_pitch;
	r_layout->size = row_pitch * height;
}

RenderingDeviceDriver::SamplerID RenderingDeviceDriverWebGPU::sampler_create(const SamplerState &p_state) {
	WEBGPU_MAIN_THREAD_GUARD(sampler_create(p_state));
	ERR_FAIL_COND_V_MSG(p_state.unnormalized_uvw, SamplerID(), "Unnormalized sampler coordinates are not supported by WebGPU.");

	// Border colors do not exist in WebGPU; CLAMP_TO_BORDER degrades to
	// CLAMP_TO_EDGE (the renderer-level fallback patch will handle the
	// difference where it matters).
	auto repeat_to_wgpu = [](SamplerRepeatMode p_mode) {
		switch (p_mode) {
			case SAMPLER_REPEAT_MODE_REPEAT:
				return WGPUAddressMode_Repeat;
			case SAMPLER_REPEAT_MODE_MIRRORED_REPEAT:
				return WGPUAddressMode_MirrorRepeat;
			default:
				return WGPUAddressMode_ClampToEdge;
		}
	};

	WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
	sampler_desc.addressModeU = repeat_to_wgpu(p_state.repeat_u);
	sampler_desc.addressModeV = repeat_to_wgpu(p_state.repeat_v);
	sampler_desc.addressModeW = repeat_to_wgpu(p_state.repeat_w);
	sampler_desc.magFilter = p_state.mag_filter == SAMPLER_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	sampler_desc.minFilter = p_state.min_filter == SAMPLER_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	sampler_desc.mipmapFilter = p_state.mip_filter == SAMPLER_FILTER_LINEAR ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
	// WebGPU requires 0 <= lodMinClamp <= lodMaxClamp.
	sampler_desc.lodMinClamp = MAX(p_state.min_lod, 0.0f);
	sampler_desc.lodMaxClamp = CLAMP(p_state.max_lod, sampler_desc.lodMinClamp, 32.0f);
	if (p_state.enable_compare) {
		switch (p_state.compare_op) {
			case COMPARE_OP_NEVER:
				sampler_desc.compare = WGPUCompareFunction_Never;
				break;
			case COMPARE_OP_LESS:
				sampler_desc.compare = WGPUCompareFunction_Less;
				break;
			case COMPARE_OP_EQUAL:
				sampler_desc.compare = WGPUCompareFunction_Equal;
				break;
			case COMPARE_OP_LESS_OR_EQUAL:
				sampler_desc.compare = WGPUCompareFunction_LessEqual;
				break;
			case COMPARE_OP_GREATER:
				sampler_desc.compare = WGPUCompareFunction_Greater;
				break;
			case COMPARE_OP_NOT_EQUAL:
				sampler_desc.compare = WGPUCompareFunction_NotEqual;
				break;
			case COMPARE_OP_GREATER_OR_EQUAL:
				sampler_desc.compare = WGPUCompareFunction_GreaterEqual;
				break;
			default:
				sampler_desc.compare = WGPUCompareFunction_Always;
				break;
		}
	}
	// WebGPU requires all three filters to be linear when anisotropy is
	// enabled; degrade to no anisotropy otherwise (upstream enables it on
	// nearest-filter samplers).
	const bool all_linear = p_state.mag_filter == SAMPLER_FILTER_LINEAR && p_state.min_filter == SAMPLER_FILTER_LINEAR && p_state.mip_filter == SAMPLER_FILTER_LINEAR;
	sampler_desc.maxAnisotropy = (p_state.use_anisotropy && all_linear) ? (uint16_t)CLAMP((int)p_state.anisotropy_max, 1, 16) : 1;

	WGPUSampler sampler = wgpuDeviceCreateSampler(device, &sampler_desc);
	ERR_FAIL_NULL_V(sampler, SamplerID());
	if (p_state.enable_compare) {
		comparison_samplers.insert((uint64_t)sampler);
	}
	return SamplerID(sampler);
}

void RenderingDeviceDriverWebGPU::sampler_free(SamplerID p_sampler) {
	comparison_samplers.erase((uint64_t)p_sampler.id);
	WEBGPU_MAIN_THREAD_GUARD(sampler_free(p_sampler));
	resource_epoch++; // Invalidates cached render bundles (see BundleCacheEntry).
	wgpuSamplerRelease((WGPUSampler)p_sampler.id);
}

bool RenderingDeviceDriverWebGPU::sampler_is_format_supported_for_filter(DataFormat p_format, SamplerFilter p_filter) {
	if (p_filter != SAMPLER_FILTER_LINEAR) {
		return true;
	}
	// 32-bit float formats are not filterable without the float32-filterable
	// feature.
	switch (p_format) {
		case DATA_FORMAT_R32_SFLOAT:
		case DATA_FORMAT_R32G32_SFLOAT:
		case DATA_FORMAT_R32G32B32A32_SFLOAT:
			return false;
		default:
			return _data_format_to_wgpu(p_format) != WGPUTextureFormat_Undefined;
	}
}

/******************/
/**** TRANSFER ****/
/******************/

void RenderingDeviceDriverWebGPU::command_clear_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, uint64_t p_offset, uint64_t p_size) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cb_info);
	ERR_FAIL_NULL(cb_info->encoder);
	wgpuCommandEncoderClearBuffer(cb_info->encoder, ((BufferInfo *)p_buffer.id)->buffer, p_offset, p_size);
}

void RenderingDeviceDriverWebGPU::command_copy_buffer(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, BufferID p_dst_buffer, VectorView<BufferCopyRegion> p_regions) {
		// The engine keeps staging buffers persistently mapped and never calls
	// unmap (Vulkan memory is coherent), so the shadow's bytes have not
	// reached the GPU buffer. Queue writes execute before this command
	// buffer's submission, which restores the intended ordering.
	{
		// When the source is an engine staging buffer (persistently mapped,
		// bytes live in our CPU shadow), write the bytes straight into the
		// destination region instead of staging-write + encoded copy: queue
		// writes execute before this command buffer's submission, so the
		// ordering is identical - and on Galaxy XR's Chrome, encoded
		// buffer-to-buffer copies into session-created buffers never land
		// while direct queue writes do.
		BufferInfo *src_sync = (BufferInfo *)p_src_buffer.id;
		BufferInfo *dst = (BufferInfo *)p_dst_buffer.id;
		// Destinations created before any XR layer import (creation_epoch 0)
		// must NOT take the direct-write shortcut: queue writes jump ahead
		// of the frame's encoded commands, so a destination region updated
		// several times per frame with draws between (the canvas state UBO,
		// boot-created) would serve its LAST value to ALL of the frame's
		// draws. Those get the staging shadow flushed (queue write into the
		// staging ring is safe - regions are never reused within a frame)
		// and fall through to the ordered encoded copy, which works fine
		// for boot-created destinations on Galaxy XR too.
		if (src_sync->shadow != nullptr && !src_sync->dynamic && dst->creation_epoch == 0) {
			for (uint32_t i = 0; i < p_regions.size(); i++) {
				const BufferCopyRegion &region = p_regions[i];
				const uint64_t sync_offset = region.src_offset & ~3ull;
				const uint64_t sync_size = MIN((((region.src_offset + region.size + 3ull) & ~3ull) - sync_offset), src_sync->size - sync_offset) & ~3ull;
				_counted_write_buffer(src_sync->buffer, sync_offset, src_sync->shadow + sync_offset, sync_size);
			}
		} else if (src_sync->shadow != nullptr && !src_sync->dynamic) {
			bool all_direct = true;
			for (uint32_t i = 0; i < p_regions.size(); i++) {
				const BufferCopyRegion &region = p_regions[i];
				const uint64_t dst_offset = region.dst_offset & ~3ull;
				const uint64_t lead = region.dst_offset - dst_offset;
				const uint64_t src_offset = region.src_offset - lead;
				const uint64_t write_size = (region.size + lead + 3ull) & ~3ull;
				// writeBuffer requires 4-byte-aligned size AND no overflow
				// past the buffer end; fall back to the staging copy when
				// the padded write would not fit.
				if (dst_offset + write_size > dst->size) {
					all_direct = false;
					const uint64_t sync_offset = region.src_offset & ~3ull;
					const uint64_t sync_size = MIN((((region.src_offset + region.size + 3ull) & ~3ull) - sync_offset), src_sync->size - sync_offset) & ~3ull;
					_counted_write_buffer(src_sync->buffer, sync_offset, src_sync->shadow + sync_offset, sync_size);
					continue;
				}
				_counted_write_buffer(dst->buffer, dst_offset, src_sync->shadow + src_offset, write_size);
			}
			if (all_direct) {
				return;
			}
			// Mixed case: encode copies only for the regions that fell back.
			CommandBufferInfo *cb_fallback = (CommandBufferInfo *)p_cmd_buffer.id;
			_end_compute_pass(cb_fallback);
			ERR_FAIL_NULL(cb_fallback->encoder);
			for (uint32_t i = 0; i < p_regions.size(); i++) {
				const BufferCopyRegion &region = p_regions[i];
				const uint64_t dst_offset = region.dst_offset & ~3ull;
				const uint64_t write_size = (region.size + (region.dst_offset - dst_offset) + 3ull) & ~3ull;
				if (dst_offset + write_size > dst->size) {
					wgpuCommandEncoderCopyBufferToBuffer(cb_fallback->encoder, src_sync->buffer, region.src_offset, dst->buffer, region.dst_offset, region.size);
				}
			}
			return;
		}
	}
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cb_info);
	ERR_FAIL_NULL(cb_info->encoder);
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferCopyRegion &region = p_regions[i];
		wgpuCommandEncoderCopyBufferToBuffer(cb_info->encoder, ((BufferInfo *)p_src_buffer.id)->buffer, region.src_offset, ((BufferInfo *)p_dst_buffer.id)->buffer, region.dst_offset, region.size);
	}
}

static WGPUTexelCopyTextureInfo _texel_copy_texture_info(WGPUTexture p_texture, uint32_t p_mipmap, const Vector3i &p_offset, uint32_t p_layer) {
	WGPUTexelCopyTextureInfo info = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	info.texture = p_texture;
	info.mipLevel = p_mipmap;
	// WebGPU addresses array layers through origin.z (there is no separate
	// subresource field); RD supplies layers via the subresource and z = 0
	// for arrays, so the two never collide.
	info.origin = { (uint32_t)p_offset.x, (uint32_t)p_offset.y, (uint32_t)p_offset.z + p_layer };
	return info;
}

void RenderingDeviceDriverWebGPU::command_copy_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<TextureCopyRegion> p_regions) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cb_info);
	ERR_FAIL_NULL(cb_info->encoder);
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const TextureCopyRegion &region = p_regions[i];
		WGPUTexelCopyTextureInfo src = _texel_copy_texture_info(((TextureInfo *)p_src_texture.id)->texture, region.src_subresources.mipmap, region.src_offset, region.src_subresources.base_layer);
		WGPUTexelCopyTextureInfo dst = _texel_copy_texture_info(((TextureInfo *)p_dst_texture.id)->texture, region.dst_subresources.mipmap, region.dst_offset, region.dst_subresources.base_layer);
		WGPUExtent3D size = { (uint32_t)region.size.x, (uint32_t)region.size.y, (uint32_t)region.size.z };
		wgpuCommandEncoderCopyTextureToTexture(cb_info->encoder, &src, &dst, &size);
	}
}

void RenderingDeviceDriverWebGPU::command_resolve_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	const TextureInfo *src = (const TextureInfo *)p_src_texture.id;
	const TextureInfo *dst = (const TextureInfo *)p_dst_texture.id;
	ERR_FAIL_NULL(cb_info->encoder);
	_end_compute_pass(cb_info);
	// WebGPU has no standalone resolve command; run an empty render pass over
	// the multisampled source with the destination as its resolve target (the
	// resolve happens at pass end regardless of draws).
	WGPUTextureViewDescriptor src_view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	src_view_desc.format = src->wgpu_format;
	src_view_desc.dimension = WGPUTextureViewDimension_2D;
	src_view_desc.baseMipLevel = p_src_mipmap;
	src_view_desc.mipLevelCount = 1;
	src_view_desc.baseArrayLayer = p_src_layer;
	src_view_desc.arrayLayerCount = 1;
	WGPUTextureView src_view = wgpuTextureCreateView(src->texture, &src_view_desc);
	ERR_FAIL_NULL(src_view);
	WGPUTextureViewDescriptor dst_view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	dst_view_desc.format = dst->wgpu_format;
	dst_view_desc.dimension = WGPUTextureViewDimension_2D;
	dst_view_desc.baseMipLevel = p_dst_mipmap;
	dst_view_desc.mipLevelCount = 1;
	dst_view_desc.baseArrayLayer = p_dst_layer;
	dst_view_desc.arrayLayerCount = 1;
	WGPUTextureView dst_view = wgpuTextureCreateView(dst->texture, &dst_view_desc);
	if (dst_view == nullptr) {
		wgpuTextureViewRelease(src_view);
		ERR_FAIL_MSG("Failed to create the resolve target view.");
	}
	WGPURenderPassColorAttachment color_attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	color_attachment.view = src_view;
	color_attachment.resolveTarget = dst_view;
	color_attachment.loadOp = WGPULoadOp_Load;
	color_attachment.storeOp = WGPUStoreOp_Store;
	WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	pass_desc.colorAttachmentCount = 1;
	pass_desc.colorAttachments = &color_attachment;
	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(cb_info->encoder, &pass_desc);
	if (pass != nullptr) {
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
	}
	wgpuTextureViewRelease(src_view);
	wgpuTextureViewRelease(dst_view);
}

void RenderingDeviceDriverWebGPU::command_copy_buffer_to_texture(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<BufferTextureCopyRegion> p_regions) {
		{
		TextureInfo *dst_expand = (TextureInfo *)p_dst_texture.id;
		if (dst_expand->swizzle_expand != TextureInfo::SWIZZLE_EXPAND_NONE) {
			// Swizzle-emulated texture: the GPU texture is RGBA8 while the
			// engine supplies R8/R8G8 texels. Expand on the CPU and write
			// through the queue (ordered before this submission's commands).
			const BufferInfo *src_expand = (const BufferInfo *)p_src_buffer.id;
			ERR_FAIL_NULL(src_expand->shadow);
			const uint32_t src_texel = dst_expand->swizzle_expand == TextureInfo::SWIZZLE_EXPAND_RG_TO_RRRG ? 2 : 1;
			for (uint32_t i = 0; i < p_regions.size(); i++) {
				const BufferTextureCopyRegion &region = p_regions[i];
				const uint32_t w = (uint32_t)region.texture_region_size.x;
				const uint32_t rows = (uint32_t)region.texture_region_size.y * (uint32_t)region.texture_region_size.z;
				const uint32_t src_pitch = region.row_pitch != 0 ? (uint32_t)region.row_pitch : w * src_texel;
				LocalVector<uint8_t> expanded;
				expanded.resize(w * 4 * rows);
				for (uint32_t row = 0; row < rows; row++) {
					const uint8_t *src_row = src_expand->shadow + region.buffer_offset + (uint64_t)row * src_pitch;
					uint8_t *dst_row = expanded.ptr() + (uint64_t)row * w * 4;
					for (uint32_t x = 0; x < w; x++) {
						const uint8_t c0 = src_row[x * src_texel];
						switch (dst_expand->swizzle_expand) {
							case TextureInfo::SWIZZLE_EXPAND_RG_TO_RRRG: {
								dst_row[x * 4 + 0] = c0;
								dst_row[x * 4 + 1] = c0;
								dst_row[x * 4 + 2] = c0;
								dst_row[x * 4 + 3] = src_row[x * 2 + 1];
							} break;
							case TextureInfo::SWIZZLE_EXPAND_R_TO_RRR1: {
								dst_row[x * 4 + 0] = c0;
								dst_row[x * 4 + 1] = c0;
								dst_row[x * 4 + 2] = c0;
								dst_row[x * 4 + 3] = 255;
							} break;
							default: {
								dst_row[x * 4 + 0] = 0;
								dst_row[x * 4 + 1] = 0;
								dst_row[x * 4 + 2] = 0;
								dst_row[x * 4 + 3] = c0;
							} break;
						}
					}
				}
				WGPUTexelCopyTextureInfo dst_info = _texel_copy_texture_info(dst_expand->texture, region.texture_subresource.mipmap, region.texture_offset, region.texture_subresource.layer);
				WGPUTexelCopyBufferLayout layout = {};
				layout.offset = 0;
				layout.bytesPerRow = w * 4;
				layout.rowsPerImage = region.texture_region_size.y;
				WGPUExtent3D extent = { w, (uint32_t)region.texture_region_size.y, (uint32_t)region.texture_region_size.z };
				wgpuQueueWriteTexture(queue, &dst_info, expanded.ptr(), expanded.size(), &layout, &extent);
			}
			return;
		}
	}
	// See command_copy_buffer: persistently mapped staging never unmaps, so
	// push each region's source bytes from the shadow before the copy runs.
	{
		BufferInfo *src_sync = (BufferInfo *)p_src_buffer.id;
		if (src_sync->shadow != nullptr && !src_sync->dynamic) {
			TextureInfo *dst_sync = (TextureInfo *)p_dst_texture.id;
			uint32_t sync_block_w = 1, sync_block_h = 1;
			const uint32_t sync_block_bytes = _data_format_block_info(dst_sync->format, sync_block_w, sync_block_h);
			for (uint32_t i = 0; i < p_regions.size(); i++) {
				const BufferTextureCopyRegion &region = p_regions[i];
				const uint32_t sync_bytes_per_row = region.row_pitch != 0 ? (uint32_t)region.row_pitch : (sync_block_bytes != 0 ? (((uint32_t)region.texture_region_size.x + sync_block_w - 1u) / sync_block_w) * sync_block_bytes : (uint32_t)region.texture_region_size.x * _data_format_texel_size(dst_sync->format));
				const uint32_t sync_rows = sync_block_bytes != 0 ? (((uint32_t)region.texture_region_size.y + sync_block_h - 1u) / sync_block_h) : (uint32_t)region.texture_region_size.y;
				const uint64_t data_size = (uint64_t)sync_bytes_per_row * (uint64_t)sync_rows * (uint64_t)region.texture_region_size.z;
				const uint64_t write_offset = region.buffer_offset & ~3ull;
				const uint64_t write_size = MIN((((region.buffer_offset + data_size + 3ull) & ~3ull) - write_offset), src_sync->size - write_offset);
				_counted_write_buffer(src_sync->buffer, write_offset, src_sync->shadow + write_offset, write_size);
			}
		}
	}
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cb_info);
	ERR_FAIL_NULL(cb_info->encoder);
	TextureInfo *texture = (TextureInfo *)p_dst_texture.id;
	uint32_t block_w = 1, block_h = 1;
	const uint32_t block_bytes = _data_format_block_info(texture->format, block_w, block_h);
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferTextureCopyRegion &region = p_regions[i];
		WGPUTexelCopyBufferInfo src = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
		src.buffer = ((BufferInfo *)p_src_buffer.id)->buffer;
		src.layout.offset = region.buffer_offset;
		// For compressed formats the buffer layout is expressed in block
		// rows (the extent stays in texels, per WebGPU's copy rules).
		const uint32_t buffer_rows = block_bytes != 0 ? (((uint32_t)region.texture_region_size.y + block_h - 1u) / block_h) : (uint32_t)region.texture_region_size.y;
		const uint32_t bytes_per_row = region.row_pitch != 0 ? (uint32_t)region.row_pitch : (block_bytes != 0 ? (((uint32_t)region.texture_region_size.x + block_w - 1u) / block_w) * block_bytes : (uint32_t)region.texture_region_size.x * _data_format_texel_size(texture->format));
		src.layout.bytesPerRow = bytes_per_row;
		src.layout.rowsPerImage = buffer_rows;
		WGPUTexelCopyTextureInfo dst = _texel_copy_texture_info(((TextureInfo *)p_dst_texture.id)->texture, region.texture_subresource.mipmap, region.texture_offset, region.texture_subresource.layer);
		WGPUExtent3D size = { (uint32_t)region.texture_region_size.x, (uint32_t)region.texture_region_size.y, (uint32_t)region.texture_region_size.z };
		if (bytes_per_row % 256 != 0 && (buffer_rows > 1 || region.texture_region_size.z > 1)) {
			// WebGPU requires bytesPerRow to be a multiple of 256 for
			// multi-row copies, and depth subresources must be copied whole:
			// repack small uploads (e.g. 4x4 default textures) through a
			// transient 256-stride buffer.
			const uint32_t rows = buffer_rows * (uint32_t)region.texture_region_size.z;
			const uint32_t packed_stride = (bytes_per_row + 255u) & ~255u;
			WGPUBufferDescriptor staging_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
			staging_desc.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
			staging_desc.size = (uint64_t)rows * packed_stride;
			WGPUBuffer staging = wgpuDeviceCreateBuffer(device, &staging_desc);
			ERR_FAIL_NULL(staging);
			for (uint32_t row = 0; row < rows; row++) {
				wgpuCommandEncoderCopyBufferToBuffer(cb_info->encoder, src.buffer, region.buffer_offset + (uint64_t)row * bytes_per_row, staging, (uint64_t)row * packed_stride, bytes_per_row);
			}
			WGPUTexelCopyBufferInfo packed_src = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
			packed_src.buffer = staging;
			packed_src.layout.offset = 0;
			packed_src.layout.bytesPerRow = packed_stride;
			packed_src.layout.rowsPerImage = buffer_rows;
			wgpuCommandEncoderCopyBufferToTexture(cb_info->encoder, &packed_src, &dst, &size);
			wgpuBufferRelease(staging);
			continue;
		}
		wgpuCommandEncoderCopyBufferToTexture(cb_info->encoder, &src, &dst, &size);
	}
}

void RenderingDeviceDriverWebGPU::command_copy_texture_to_buffer(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, BufferID p_dst_buffer, VectorView<BufferTextureCopyRegion> p_regions) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cb_info);
	ERR_FAIL_NULL(cb_info->encoder);
	TextureInfo *texture = (TextureInfo *)p_src_texture.id;
	uint32_t block_w = 1, block_h = 1;
	const uint32_t block_bytes = _data_format_block_info(texture->format, block_w, block_h);
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferTextureCopyRegion &region = p_regions[i];
		WGPUTexelCopyTextureInfo src = _texel_copy_texture_info(((TextureInfo *)p_src_texture.id)->texture, region.texture_subresource.mipmap, region.texture_offset, region.texture_subresource.layer);
		WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
		dst.buffer = ((BufferInfo *)p_dst_buffer.id)->buffer;
		dst.layout.offset = region.buffer_offset;
		dst.layout.bytesPerRow = region.row_pitch != 0 ? (uint32_t)region.row_pitch : (block_bytes != 0 ? (((uint32_t)region.texture_region_size.x + block_w - 1u) / block_w) * block_bytes : (uint32_t)region.texture_region_size.x * _data_format_texel_size(texture->format));
		dst.layout.rowsPerImage = block_bytes != 0 ? (((uint32_t)region.texture_region_size.y + block_h - 1u) / block_h) : (uint32_t)region.texture_region_size.y;
		WGPUExtent3D size = { (uint32_t)region.texture_region_size.x, (uint32_t)region.texture_region_size.y, (uint32_t)region.texture_region_size.z };
		wgpuCommandEncoderCopyTextureToBuffer(cb_info->encoder, &src, &dst, &size);
	}
}

/****************/
/**** LIMITS ****/
/****************/

uint64_t RenderingDeviceDriverWebGPU::limit_get(Limit p_limit) {
	switch (p_limit) {
		case LIMIT_MAX_BOUND_UNIFORM_SETS:
			return device_limits.maxBindGroups;
		case LIMIT_MAX_FRAMEBUFFER_COLOR_ATTACHMENTS:
			return device_limits.maxColorAttachments;
		case LIMIT_MAX_TEXTURES_PER_UNIFORM_SET:
		case LIMIT_MAX_TEXTURES_PER_SHADER_STAGE:
			return device_limits.maxSampledTexturesPerShaderStage;
		case LIMIT_MAX_SAMPLERS_PER_UNIFORM_SET:
		case LIMIT_MAX_SAMPLERS_PER_SHADER_STAGE:
			return device_limits.maxSamplersPerShaderStage;
		case LIMIT_MAX_STORAGE_BUFFERS_PER_UNIFORM_SET:
		case LIMIT_MAX_STORAGE_BUFFERS_PER_SHADER_STAGE:
			return device_limits.maxStorageBuffersPerShaderStage;
		case LIMIT_MAX_STORAGE_IMAGES_PER_UNIFORM_SET:
		case LIMIT_MAX_STORAGE_IMAGES_PER_SHADER_STAGE:
			return device_limits.maxStorageTexturesPerShaderStage;
		case LIMIT_MAX_UNIFORM_BUFFERS_PER_UNIFORM_SET:
		case LIMIT_MAX_UNIFORM_BUFFERS_PER_SHADER_STAGE:
			return device_limits.maxUniformBuffersPerShaderStage;
		case LIMIT_MAX_FRAMEBUFFER_HEIGHT:
		case LIMIT_MAX_FRAMEBUFFER_WIDTH:
		case LIMIT_MAX_TEXTURE_SIZE_2D:
		case LIMIT_MAX_TEXTURE_SIZE_CUBE:
		case LIMIT_MAX_VIEWPORT_DIMENSIONS_X:
		case LIMIT_MAX_VIEWPORT_DIMENSIONS_Y:
			return device_limits.maxTextureDimension2D;
		case LIMIT_MAX_TEXTURE_SIZE_1D:
			return device_limits.maxTextureDimension1D;
		case LIMIT_MAX_TEXTURE_SIZE_3D:
			return device_limits.maxTextureDimension3D;
		case LIMIT_MAX_TEXTURE_ARRAY_LAYERS:
			return device_limits.maxTextureArrayLayers;
		case LIMIT_MAX_UNIFORM_BUFFER_SIZE:
			return device_limits.maxUniformBufferBindingSize;
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTES:
			return device_limits.maxVertexAttributes;
		case LIMIT_MAX_VERTEX_INPUT_BINDINGS:
			return device_limits.maxVertexBuffers;
		case LIMIT_MAX_VERTEX_INPUT_BINDING_STRIDE:
			return device_limits.maxVertexBufferArrayStride;
		case LIMIT_MIN_UNIFORM_BUFFER_OFFSET_ALIGNMENT:
			return device_limits.minUniformBufferOffsetAlignment;
		case LIMIT_MAX_COMPUTE_SHARED_MEMORY_SIZE:
			return device_limits.maxComputeWorkgroupStorageSize;
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X:
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y:
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Z:
			return device_limits.maxComputeWorkgroupsPerDimension;
		case LIMIT_MAX_COMPUTE_WORKGROUP_INVOCATIONS:
			return device_limits.maxComputeInvocationsPerWorkgroup;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_X:
			return device_limits.maxComputeWorkgroupSizeX;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Y:
			return device_limits.maxComputeWorkgroupSizeY;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Z:
			return device_limits.maxComputeWorkgroupSizeZ;
		case LIMIT_MAX_SHADER_VARYINGS:
			return device_limits.maxInterStageShaderVariables;
		case LIMIT_MAX_PUSH_CONSTANT_SIZE:
			// Push constants will be emulated with a uniform buffer.
			return 128;
		case LIMIT_MAX_DRAW_INDEXED_INDEX:
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTE_OFFSET:
			return UINT32_MAX;
		default:
			// Conservative catch-all for limits WebGPU does not express.
			return 65536;
	}
}

/*****************/
/**** SHADERS ****/
/*****************/

static WGPUShaderStage _shader_stages_to_wgpu(BitField<RenderingDeviceCommons::ShaderStage> p_stages) {
	WGPUShaderStage visibility = WGPUShaderStage_None;
	if (p_stages.has_flag(RenderingDeviceCommons::SHADER_STAGE_VERTEX_BIT)) {
		visibility |= WGPUShaderStage_Vertex;
	}
	if (p_stages.has_flag(RenderingDeviceCommons::SHADER_STAGE_FRAGMENT_BIT)) {
		visibility |= WGPUShaderStage_Fragment;
	}
	if (p_stages.has_flag(RenderingDeviceCommons::SHADER_STAGE_COMPUTE_BIT)) {
		visibility |= WGPUShaderStage_Compute;
	}
	return visibility;
}

static WGPUTextureViewDimension _texture_type_to_wgpu_view_dimension(RenderingDeviceCommons::TextureType p_type) {
	switch (p_type) {
		case RenderingDeviceCommons::TEXTURE_TYPE_1D:
			return WGPUTextureViewDimension_1D;
		case RenderingDeviceCommons::TEXTURE_TYPE_2D:
			return WGPUTextureViewDimension_2D;
		case RenderingDeviceCommons::TEXTURE_TYPE_2D_ARRAY:
			return WGPUTextureViewDimension_2DArray;
		case RenderingDeviceCommons::TEXTURE_TYPE_CUBE:
			return WGPUTextureViewDimension_Cube;
		case RenderingDeviceCommons::TEXTURE_TYPE_CUBE_ARRAY:
			return WGPUTextureViewDimension_CubeArray;
		case RenderingDeviceCommons::TEXTURE_TYPE_3D:
			return WGPUTextureViewDimension_3D;
		default:
			return WGPUTextureViewDimension_2D;
	}
}

// The sample class a texture format serves: integer formats can only be bound
// where the shader declares an integer texture, and the reverse.
static WGPUTextureSampleType _wgpu_format_sample_class(WGPUTextureFormat p_format) {
	switch (p_format) {
		case WGPUTextureFormat_R8Uint:
		case WGPUTextureFormat_RG8Uint:
		case WGPUTextureFormat_RGBA8Uint:
		case WGPUTextureFormat_R16Uint:
		case WGPUTextureFormat_RG16Uint:
		case WGPUTextureFormat_RGBA16Uint:
		case WGPUTextureFormat_R32Uint:
		case WGPUTextureFormat_RG32Uint:
		case WGPUTextureFormat_RGBA32Uint:
		case WGPUTextureFormat_Stencil8:
			return WGPUTextureSampleType_Uint;
		case WGPUTextureFormat_R8Sint:
		case WGPUTextureFormat_RG8Sint:
		case WGPUTextureFormat_RGBA8Sint:
		case WGPUTextureFormat_R16Sint:
		case WGPUTextureFormat_RG16Sint:
		case WGPUTextureFormat_RGBA16Sint:
		case WGPUTextureFormat_R32Sint:
		case WGPUTextureFormat_RG32Sint:
		case WGPUTextureFormat_RGBA32Sint:
			return WGPUTextureSampleType_Sint;
		default:
			return WGPUTextureSampleType_Float;
	}
}

static bool _wgpu_sample_class_matches(WGPUTextureSampleType p_declared, WGPUTextureFormat p_format) {
	const WGPUTextureSampleType cls = _wgpu_format_sample_class(p_format);
	switch (p_declared) {
		case WGPUTextureSampleType_Uint:
			return cls == WGPUTextureSampleType_Uint;
		case WGPUTextureSampleType_Sint:
			return cls == WGPUTextureSampleType_Sint;
		case WGPUTextureSampleType_Float:
		case WGPUTextureSampleType_UnfilterableFloat:
			return cls == WGPUTextureSampleType_Float;
		default:
			return true; // Depth slots follow the depth-specific rules at bind time.
	}
}

static WGPUTextureSampleType _data_format_to_wgpu_sample_type(RenderingDeviceCommons::DataFormat p_format) {
switch (p_format) {
		case RenderingDeviceCommons::DATA_FORMAT_D16_UNORM:
		case RenderingDeviceCommons::DATA_FORMAT_X8_D24_UNORM_PACK32:
		case RenderingDeviceCommons::DATA_FORMAT_D32_SFLOAT:
		case RenderingDeviceCommons::DATA_FORMAT_D24_UNORM_S8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_D32_SFLOAT_S8_UINT:
			return WGPUTextureSampleType_Depth;
		case RenderingDeviceCommons::DATA_FORMAT_R8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_UINT:
			return WGPUTextureSampleType_Uint;
		case RenderingDeviceCommons::DATA_FORMAT_R8_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_SINT:
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_SINT:
			return WGPUTextureSampleType_Sint;
		default:
			return WGPUTextureSampleType_Float;
	}
}

RenderingDeviceDriver::ShaderID RenderingDeviceDriverWebGPU::shader_create_from_container(const Ref<RenderingShaderContainer> &p_shader_container, const Vector<ImmutableSampler> &p_immutable_samplers) {
	WEBGPU_MAIN_THREAD_GUARD(shader_create_from_container(p_shader_container, p_immutable_samplers));
	const ShaderReflection reflection = p_shader_container->get_shader_reflection();

	ShaderInfo *shader = memnew(ShaderInfo);
	shader->name = p_shader_container->shader_name;
	shader->push_constant_size = reflection.push_constant_size;
	shader->fragment_output_mask = reflection.fragment_output_mask;
	bool set0_only_push_constant = false;
	for (const ShaderSpecializationConstant &constant : reflection.specialization_constants) {
		shader->specialization_constant_ids.push_back(constant.constant_id);
	}

	// Declared (group << 32 | binding) pairs per stage, parsed from the WGSL
	// text: the transform removes unused declarations per stage, so this is
	// the exact visibility each binding needs. Bindings no stage declares
	// (e.g. dead-eliminated arrays) get empty visibility and count toward no
	// per-stage limits.
	HashSet<uint64_t> stage_bindings[SHADER_STAGE_MAX];
	// WGSL-declared binding kinds the reflection cannot express: comparison
	// samplers and depth textures need matching layout entry types.
	HashSet<uint64_t> comparison_sampler_bindings;
	HashSet<uint64_t> depth_texture_bindings;
	HashMap<String, uint64_t> depth_texture_names;
	HashMap<String, uint64_t> plain_sampler_names;
	HashMap<String, uint64_t> texture_names;
	LocalVector<Pair<uint64_t, uint64_t>> static_texture_sampler_pairs;
	// Storage texture <format, access> parsed from the WGSL declarations;
	// reflection can lack the format (format-less writeonly GLSL images).
	HashMap<uint64_t, Pair<WGPUTextureFormat, WGPUStorageTextureAccess>> storage_texture_decls;
	// Sampled texture component types parsed from the WGSL (texture_2d<u32>
	// etc.); Tint also converts read-only storage images into sampled
	// textures, so reflection alone cannot type these bindings.
	HashMap<uint64_t, WGPUTextureSampleType> sampled_texture_decls;
	// Texture bindings whose identifier is used in any way other than a
	// direct textureLoad call; float slots never used that way become
	// UnfilterableFloat (see ShaderInfo::load_only_float_bindings).
	HashSet<uint64_t> filter_used_texture_keys;
	// Bindings the WGSL declares as multisampled textures; the layout must
	// carry the multisampled flag.
	HashSet<uint64_t> multisampled_texture_bindings;
	// Storage buffer access parsed from the WGSL: true when any stage
	// declares var<storage, read_write>. Reflection's writable flag is
	// merged across stages, which wrongly makes read-only vertex bindings
	// read-write (invalid with Vertex visibility in WebGPU).
	HashMap<uint64_t, bool> storage_buffer_read_write;

	// Shader modules from the container's WGSL.
	for (int64_t i = 0; i < p_shader_container->shaders.size(); i++) {
		const RenderingShaderContainer::Shader &stage = p_shader_container->shaders[i];
		const uint8_t *code_ptr = stage.code_compressed_bytes.ptr();
		uint32_t code_compressed_size = stage.code_compressed_bytes.size();
		uint32_t code_decompressed_size = stage.code_decompressed_size;
		uint32_t code_compression_flags = stage.code_compression_flags;
		// Devices with the subgroups feature take the stage's native wave-op
		// translation when the bake produced one (same bindings, real wave
		// coherence in the clustered light loop).
		const RenderingShaderContainerWebGPU *webgpu_container = Object::cast_to<RenderingShaderContainerWebGPU>(p_shader_container.ptr());
		if (device_has_subgroups && webgpu_container != nullptr && i < webgpu_container->native_shaders.size() && webgpu_container->native_shaders[i].code_decompressed_size > 0) {
			const RenderingShaderContainerWebGPU::NativeShader &native = webgpu_container->native_shaders[i];
			code_ptr = native.code_compressed_bytes.ptr();
			code_compressed_size = native.code_compressed_bytes.size();
			code_decompressed_size = native.code_decompressed_size;
			code_compression_flags = native.code_compression_flags;
			perf.native_subgroup_modules++;
		}
		Vector<uint8_t> wgsl;
		wgsl.resize(code_decompressed_size + 1);
		if (!p_shader_container->decompress_code(code_ptr, code_compressed_size, code_compression_flags, wgsl.ptrw(), code_decompressed_size)) {
			shader_free(ShaderID(shader));
			ERR_FAIL_V_MSG(ShaderID(), vformat("Failed to decompress WGSL for stage #%d.", i));
		}
		wgsl.ptrw()[code_decompressed_size] = 0;
		{
			// Attribute order varies (@group @binding or @binding @group);
			// pair each @binding with the nearest @group either side.
			const char *text = (const char *)wgsl.ptr();
			const char *cursor = text;
			while ((cursor = strstr(cursor, "@binding(")) != nullptr) {
				const uint32_t binding = (uint32_t)atoi(cursor + 9);
				const char *group_str = nullptr;
				for (const char *back = cursor - 1; back >= text && cursor - back < 40; back--) {
					if (strncmp(back, "@group(", 7) == 0) {
						group_str = back;
						break;
					}
				}
				if (group_str == nullptr) {
					const char *forward = strstr(cursor, "@group(");
					if (forward != nullptr && forward - cursor < 40) {
						group_str = forward;
					}
				}
				if (group_str != nullptr) {
					const uint32_t group = (uint32_t)atoi(group_str + 7);
					const uint64_t key = ((uint64_t)group << 32) | binding;
					stage_bindings[stage.shader_stage].insert(key);
					const char *decl_end = strchr(cursor, ';');
					if (decl_end != nullptr && decl_end - cursor < 200) {
						const size_t decl_len = decl_end - cursor;
						String var_name;
						const char *var_kw = strstr(cursor, "var");
						if (var_kw != nullptr && var_kw < decl_end) {
							const char *name_start = var_kw + 3;
							if (*name_start == '<') { // var<...> qualifier.
								while (name_start < decl_end && *name_start != '>') {
									name_start++;
								}
								name_start = name_start < decl_end ? name_start + 1 : decl_end;
							}
							while (name_start < decl_end && (*name_start == ' ' || *name_start == 9)) {
								name_start++;
							}
							const char *name_end = name_start;
							while (name_end < decl_end && (is_ascii_alphanumeric_char(*name_end) || *name_end == '_')) {
								name_end++;
							}
							if (name_end > name_start) {
								var_name = String::utf8(name_start, name_end - name_start);
							}
						}
						if (memmem_compat(cursor, decl_len, "_multisampled_", 14)) {
							multisampled_texture_bindings.insert(key);
						}
						const char *buffer_kw = strstr(cursor, "var<storage");
						if (buffer_kw != nullptr && buffer_kw < decl_end) {
							const bool rw = memmem_compat(buffer_kw, decl_end - buffer_kw, "read_write", 10);
							bool *existing = storage_buffer_read_write.getptr(key);
							if (existing != nullptr) {
								*existing = *existing || rw;
							} else {
								storage_buffer_read_write.insert(key, rw);
							}
						}
						const char *storage_kw = strstr(cursor, "texture_storage_");
						if (storage_kw != nullptr && storage_kw < decl_end) {
							const char *lt = strchr(storage_kw, '<');
							const char *gt = lt != nullptr ? strchr(lt, '>') : nullptr;
							if (gt != nullptr && gt < decl_end) {
								const Vector<String> params = String::utf8(lt + 1, gt - lt - 1).split(",");
								if (params.size() == 2) {
									const WGPUTextureFormat decl_format = _wgsl_storage_format_to_wgpu(params[0].strip_edges());
									const String access = params[1].strip_edges();
									const WGPUStorageTextureAccess decl_access = access == "write" ? WGPUStorageTextureAccess_WriteOnly : (access == "read_write" ? WGPUStorageTextureAccess_ReadWrite : WGPUStorageTextureAccess_ReadOnly);
									if (decl_format != WGPUTextureFormat_Undefined) {
										storage_texture_decls.insert(key, Pair<WGPUTextureFormat, WGPUStorageTextureAccess>(decl_format, decl_access));
										// Tier-gated declarations: the bake ships them for
										// every device (valid WGSL text), but creating the
										// module on a device without the format tier would
										// print a Dawn validation error. Fail the variant
										// cleanly instead - the engine treats a null variant
										// exactly like a bake hole and takes the fallback
										// path its capability probes select anyway.
										if (!_wgpu_storage_decl_supported_by_device(decl_format, decl_access)) {
											print_verbose(vformat("WebGPU: shader '%s' declares storage format/access beyond this device's texture format tiers; variant left empty.", String::utf8(shader->name.get_data())));
											shader_free(ShaderID(shader));
											return ShaderID();
										}
									}
								}
							}
						}
						if (memmem_compat(cursor, decl_len, "sampler_comparison", 18)) {
							comparison_sampler_bindings.insert(key);
						} else if (memmem_compat(cursor, decl_len, "texture_depth_", 14)) {
							depth_texture_bindings.insert(key);
							if (!var_name.is_empty()) {
								depth_texture_names.insert(var_name, key);
								texture_names.insert(var_name, key);
							}
						} else if (memmem_compat(cursor, decl_len, ": texture_", 10) && (storage_kw == nullptr || storage_kw >= decl_end)) {
							if (!var_name.is_empty()) {
								texture_names.insert(var_name, key);
							}
							// Component type: texture_2d<f32|u32|i32> etc.
							const char *lt = (const char *)memchr(cursor, '<', decl_len);
							if (lt != nullptr) {
								if (strncmp(lt + 1, "u32", 3) == 0) {
									sampled_texture_decls.insert(key, WGPUTextureSampleType_Uint);
								} else if (strncmp(lt + 1, "i32", 3) == 0) {
									sampled_texture_decls.insert(key, WGPUTextureSampleType_Sint);
								} else if (strncmp(lt + 1, "f32", 3) == 0) {
									sampled_texture_decls.insert(key, WGPUTextureSampleType_Float);
								}
							}
						} else if (memmem_compat(cursor, decl_len, ": sampler", 9)) {
							if (!var_name.is_empty()) {
								plain_sampler_names.insert(var_name, key);
							}
						}
					}
				}
				cursor += 9;
			}
		}
		{
			// Tint keeps an @id override only in stages that use it; record
			// per module so pipeline constants can be filtered per stage.
			shader->module_override_ids.resize(shader->module_override_ids.size() + 1);
			HashSet<uint32_t> &module_ids = shader->module_override_ids[shader->module_override_ids.size() - 1];
			const char *ov = (const char *)wgsl.ptr();
			while ((ov = strstr(ov, "@id(")) != nullptr) {
				module_ids.insert((uint32_t)atoi(ov + 4));
				ov += 4;
			}
		}
		{
			// WebGPU cannot pair depth textures with filtering samplers.
			// Find textureSample*/textureGather calls that statically pair a
			// depth texture with a plain sampler; those sampler slots become
			// NonFiltering and get a substitute nearest sampler at bind time.
			const char *scan_text = (const char *)wgsl.ptr();
			const char *call = strstr(scan_text, "textureSample");
			while (call != nullptr) {
				const char *p = call + 13;
				const bool is_compare = strncmp(p, "Compare", 7) == 0;
				while (*p != 0 && *p != '(') {
					p++;
				}
				if (*p != '(' || is_compare) {
					call = strstr(p, "textureSample");
					continue;
				}
				p++;
				const char *a1_start = p;
				while (*p != 0 && *p != ',' && *p != ')') {
					p++;
				}
				if (*p != ',') {
					call = strstr(p, "textureSample");
					continue;
				}
				String a1 = String::utf8(a1_start, p - a1_start).strip_edges();
				p++;
				const char *a2_start = p;
				while (*p != 0 && *p != ',' && *p != ')') {
					p++;
				}
				String a2 = String::utf8(a2_start, p - a2_start).strip_edges();
				if (texture_names.has(a1) && plain_sampler_names.has(a2)) {
					static_texture_sampler_pairs.push_back(Pair<uint64_t, uint64_t>(texture_names[a1], plain_sampler_names[a2]));
				}
				call = strstr(p, "textureSample");
			}
			// A texture slot is load-only when every occurrence of its
			// identifier is either the declaration or directly inside
			// textureLoad(...). Anything else — a sample call, or being
			// passed to a helper function that may sample it — disqualifies
			// the slot (Dawn validates texture/sampler pairings statically
			// through calls).
			for (const KeyValue<String, uint64_t> &tn : texture_names) {
				if (filter_used_texture_keys.has(tn.value)) {
					continue;
				}
				const CharString name_utf8 = tn.key.utf8();
				const int64_t name_len = name_utf8.length();
				const char *hay = scan_text;
				const char *hit;
				while ((hit = strstr(hay, name_utf8.get_data())) != nullptr) {
					const char *after = hit + name_len;
					hay = after;
					const bool left_ok = hit == scan_text || !(is_ascii_alphanumeric_char(hit[-1]) || hit[-1] == '_');
					const bool right_ok = !(is_ascii_alphanumeric_char(*after) || *after == '_');
					if (!left_ok || !right_ok) {
						continue;
					}
					const char *colon = after;
					while (*colon == ' ') {
						colon++;
					}
					if (*colon == ':') {
						continue; // Declaration site.
					}
					bool is_load = false;
					const char *lp = hit;
					while (lp > scan_text && lp[-1] == ' ') {
						lp--;
					}
					if (lp > scan_text && lp[-1] == '(') {
						lp--;
						while (lp > scan_text && lp[-1] == ' ') {
							lp--;
						}
						if (lp - scan_text >= 11 && strncmp(lp - 11, "textureLoad", 11) == 0) {
							is_load = true;
						}
					}
					if (!is_load) {
						filter_used_texture_keys.insert(tn.value);
						break;
					}
				}
			}
		}
		if (!device_has_shader_f16 && strstr((const char *)wgsl.ptr(), "enable f16;") != nullptr) {
			// Without the shader-f16 device feature, a module that enables
			// it would be an error object that poisons every frame command
			// buffer it touches. Fail cleanly instead. (Reachable only if
			// the capability report and the baked group selection disagree.)
			shader_free(ShaderID(shader));
			ERR_FAIL_V_MSG(ShaderID(), "Shader requires f16, which the WebGPU device does not have enabled.");
		}

		WGPUShaderSourceWGSL wgsl_source = WGPU_SHADER_SOURCE_WGSL_INIT;
		wgsl_source.code = { (const char *)wgsl.ptr(), code_decompressed_size };
		WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		module_desc.nextInChain = &wgsl_source.chain;
		WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &module_desc);
		if (module == nullptr) {
			shader_free(ShaderID(shader));
			ERR_FAIL_V_MSG(ShaderID(), vformat("Failed to create a WebGPU shader module for stage #%d.", i));
		}
		shader->modules.push_back(module);
		shader->module_stages.push_back((ShaderStage)stage.shader_stage);
	}

	// Float texture slots never used with a filtering call become
	// UnfilterableFloat, which additionally admits depth-aspect views (the
	// GI pass texelFetches the raw depth buffer through a plain texture2D).
	for (const KeyValue<uint64_t, WGPUTextureSampleType> &kv : sampled_texture_decls) {
		if (kv.value == WGPUTextureSampleType_Float && !filter_used_texture_keys.has(kv.key)) {
			shader->load_only_float_bindings.insert(kv.key);
		}
	}

	// Depth-format slots: reflection formats plus WGSL texture_depth_*
	// declarations. Samplers statically paired with them must not filter
	// (WebGPU forbids filtering depth textures).
	{
		HashSet<uint64_t> depth_slots;
		for (const uint64_t &e : depth_texture_bindings) {
			depth_slots.insert(e);
		}
		for (int64_t set_index = 0; set_index < reflection.uniform_sets.size(); set_index++) {
			uint32_t depth_scan_combined = 0;
			for (const ShaderUniform &uniform : reflection.uniform_sets[set_index]) {
				const uint32_t remapped = uniform.binding + depth_scan_combined;
				const bool is_depth_format = uniform.texture_format == DATA_FORMAT_D16_UNORM || uniform.texture_format == DATA_FORMAT_X8_D24_UNORM_PACK32 || uniform.texture_format == DATA_FORMAT_D32_SFLOAT;
				if (is_depth_format && (uniform.type == UNIFORM_TYPE_TEXTURE || uniform.type == UNIFORM_TYPE_SAMPLER_WITH_TEXTURE)) {
					depth_slots.insert(((uint64_t)set_index << 32) | remapped);
				}
				if (uniform.type == UNIFORM_TYPE_SAMPLER_WITH_TEXTURE) {
					depth_scan_combined++;
				}
			}
		}
		for (const Pair<uint64_t, uint64_t> &pair : static_texture_sampler_pairs) {
			if (depth_slots.has(pair.first)) {
				shader->nonfiltering_samplers.insert(pair.second);
			}
		}
		for (const uint64_t &e : depth_slots) {
			shader->depth_declared_bindings.insert(e);
		}
		for (const uint64_t &e : comparison_sampler_bindings) {
			shader->comparison_declared_bindings.insert(e);
		}
	}

	// Validate per-stage binding budgets up front: WebGPU rejects layouts
	// over the limits, and the resulting error objects would poison every
	// frame command buffer that binds them. Failing here yields a null
	// shader, which the engine handles gracefully.
	{
		enum { VIS_VERTEX,
			VIS_FRAGMENT,
			VIS_COMPUTE,
			VIS_MAX };
		uint32_t samplers[VIS_MAX] = {};
		uint32_t textures[VIS_MAX] = {};
		uint32_t storage_buffers[VIS_MAX] = {};
		uint32_t uniform_buffers[VIS_MAX] = {};
		for (int64_t set_index = 0; set_index < reflection.uniform_sets.size(); set_index++) {
			for (const ShaderUniform &uniform : reflection.uniform_sets[set_index]) {
				// length is the element count for textures/samplers but the
				// BYTE size for buffers; buffers always occupy one binding.
				const bool is_buffer_type = uniform.type == UNIFORM_TYPE_UNIFORM_BUFFER || uniform.type == UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC || uniform.type == UNIFORM_TYPE_STORAGE_BUFFER || uniform.type == UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC || uniform.type == UNIFORM_TYPE_TEXTURE_BUFFER || uniform.type == UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER || uniform.type == UNIFORM_TYPE_IMAGE_BUFFER;
				const uint32_t count = is_buffer_type ? 1u : MAX(1u, (uint32_t)uniform.length);
				const WGPUShaderStage visibility = _shader_stages_to_wgpu(uniform.stages);
				for (uint32_t stage = 0; stage < VIS_MAX; stage++) {
					const WGPUShaderStage stage_bit = stage == VIS_VERTEX ? WGPUShaderStage_Vertex : (stage == VIS_FRAGMENT ? WGPUShaderStage_Fragment : WGPUShaderStage_Compute);
					if ((visibility & stage_bit) == 0) {
						continue;
					}
					switch (uniform.type) {
						case UNIFORM_TYPE_SAMPLER:
							samplers[stage] += count;
							break;
						case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE:
							samplers[stage] += count;
							textures[stage] += count;
							break;
						case UNIFORM_TYPE_TEXTURE:
						case UNIFORM_TYPE_INPUT_ATTACHMENT:
							textures[stage] += count;
							break;
						case UNIFORM_TYPE_UNIFORM_BUFFER:
						case UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC:
							uniform_buffers[stage] += count;
							break;
						case UNIFORM_TYPE_STORAGE_BUFFER:
						case UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC:
						case UNIFORM_TYPE_TEXTURE_BUFFER:
						case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER:
						case UNIFORM_TYPE_IMAGE_BUFFER:
							storage_buffers[stage] += count;
							break;
						default:
							break;
					}
				}
			}
		}
		const WGPUShaderStage pc_visibility = shader->push_constant_size > 0 ? _shader_stages_to_wgpu(reflection.push_constant_stages) : WGPUShaderStage_None;
		for (uint32_t stage = 0; stage < VIS_MAX; stage++) {
			const WGPUShaderStage stage_bit = stage == VIS_VERTEX ? WGPUShaderStage_Vertex : (stage == VIS_FRAGMENT ? WGPUShaderStage_Fragment : WGPUShaderStage_Compute);
			// The push constant ring adds one storage buffer to every stage
			// that reads it.
			const uint32_t pc_extra = (pc_visibility & stage_bit) != 0 ? 1 : 0;
			// Sampler/texture overflows are handled by the vertex visibility
			// cap below; only buffer overflows are unfixable here.
			if (storage_buffers[stage] + pc_extra > device_limits.maxStorageBuffersPerShaderStage || uniform_buffers[stage] > device_limits.maxUniformBuffersPerShaderStage) {
				shader_free(ShaderID(shader));
				ERR_FAIL_V_MSG(ShaderID(), vformat("Shader exceeds WebGPU per-stage binding limits (stage %d: %d samplers, %d textures, %d storage buffers, %d uniform buffers).", stage, samplers[stage], textures[stage], storage_buffers[stage] + pc_extra, uniform_buffers[stage]));
			}
		}
	}

	// Visibility from the per-stage WGSL declarations collected above.
	auto binding_visibility = [&](uint32_t p_group, uint32_t p_binding) -> WGPUShaderStage {
		WGPUShaderStage visibility = WGPUShaderStage_None;
		const uint64_t key = ((uint64_t)p_group << 32) | p_binding;
		for (uint32_t stage_index = 0; stage_index < shader->module_stages.size(); stage_index++) {
			if (!stage_bindings[shader->module_stages[stage_index]].has(key)) {
				continue;
			}
			switch (shader->module_stages[stage_index]) {
				case SHADER_STAGE_VERTEX:
					visibility |= WGPUShaderStage_Vertex;
					break;
				case SHADER_STAGE_FRAGMENT:
					visibility |= WGPUShaderStage_Fragment;
					break;
				case SHADER_STAGE_COMPUTE:
					visibility |= WGPUShaderStage_Compute;
					break;
				default:
					break;
			}
		}
		return visibility;
	};

	// Per-stage sampler/texture budgets are summed across the whole pipeline
	// layout; ubershaders can exceed them in the vertex stage (dynamic
	// branching keeps every declaration alive). Cap vertex visibility at the
	// device budget: vertex-stage material sampling is exceedingly rare, and
	// an over-budget layout would poison every frame command buffer that
	// binds it.
	uint32_t vertex_samplers_used = 0;
	uint32_t vertex_textures_used = 0;
	auto cap_vertex_budget = [&](WGPUBindGroupLayoutEntry &p_entry) {
		if ((p_entry.visibility & WGPUShaderStage_Vertex) == 0) {
			return;
		}
		if (p_entry.sampler.type != WGPUSamplerBindingType_BindingNotUsed) {
			if (vertex_samplers_used >= device_limits.maxSamplersPerShaderStage) {
				p_entry.visibility &= ~WGPUShaderStage_Vertex;
				return;
			}
			vertex_samplers_used++;
		}
		if (p_entry.texture.sampleType != WGPUTextureSampleType_BindingNotUsed) {
			if (vertex_textures_used >= device_limits.maxSampledTexturesPerShaderStage) {
				p_entry.visibility &= ~WGPUShaderStage_Vertex;
				return;
			}
			vertex_textures_used++;
		}
	};

	// Bind group layouts. Binding numbers follow the fixed remaps described in
	// rendering_shader_container_webgpu.h: combined image samplers shift later
	// bindings up and add a sampler right after their texture, and push
	// constants live in a reserved uniform buffer binding.
	for (int64_t set_index = 0; set_index < reflection.uniform_sets.size(); set_index++) {
		LocalVector<WGPUBindGroupLayoutEntry> entries;
		uint32_t combined_before = 0;
		for (const ShaderUniform &uniform : reflection.uniform_sets[set_index]) {
			const uint32_t remapped_binding = uniform.binding + combined_before;
			WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
			entry.binding = remapped_binding;
			entry.visibility = binding_visibility((uint32_t)set_index, remapped_binding);
			if (uniform.length > 1 && (uniform.type == UNIFORM_TYPE_TEXTURE || uniform.type == UNIFORM_TYPE_IMAGE || uniform.type == UNIFORM_TYPE_SAMPLER)) {
				// Arrayed handles fan out to one binding per element (see
				// ARRAY_BINDING_BASE). The module may not reference them (the
				// shader transform removes unused declarations); WebGPU allows
				// layouts to declare bindings the shader does not use.
				if ((uint32_t)uniform.length > RenderingShaderContainerWebGPU::ARRAY_BINDING_STRIDE) {
					shader_free(ShaderID(shader));
					ERR_FAIL_V_MSG(ShaderID(), vformat("Arrayed uniform at set %d binding %d has %d elements; the WebGPU driver supports at most %d.", (int)set_index, uniform.binding, uniform.length, RenderingShaderContainerWebGPU::ARRAY_BINDING_STRIDE));
				}
				bool wgsl_flattened = false;
				{
					const uint64_t plain_key = ((uint64_t)set_index << 32) | remapped_binding;
					const uint64_t fanned_key = ((uint64_t)set_index << 32) | (RenderingShaderContainerWebGPU::ARRAY_BINDING_BASE + uniform.binding * RenderingShaderContainerWebGPU::ARRAY_BINDING_STRIDE);
					bool plain_declared = false;
					bool fanned_declared = false;
					for (uint32_t stage = 0; stage < SHADER_STAGE_MAX; stage++) {
						plain_declared = plain_declared || stage_bindings[stage].has(plain_key);
						fanned_declared = fanned_declared || stage_bindings[stage].has(fanned_key);
					}
					// No fanned declarations = either the bake-time truncation
					// pass collapsed the array (plain declared) or the array
					// is fully dead-eliminated; both use the single-binding
					// representation (fanning a dead array can push bindings
					// past maxBindingsPerBindGroup, e.g. the GI shader's
					// arrays at high binding indices).
					wgsl_flattened = !fanned_declared;
					(void)plain_declared;
				}
				if (wgsl_flattened) {
					// The bake-time flatten pass collapsed the array to one
					// handle at the original binding.
					shader->flattened_array_bindings.insert(((uint64_t)set_index << 32) | remapped_binding);
					WGPUBindGroupLayoutEntry single_entry = entry;
					single_entry.binding = remapped_binding;
					single_entry.visibility = binding_visibility((uint32_t)set_index, remapped_binding);
					if (uniform.type == UNIFORM_TYPE_SAMPLER) {
						single_entry.sampler.type = WGPUSamplerBindingType_Filtering;
					} else if (uniform.type == UNIFORM_TYPE_TEXTURE) {
						// The WGSL declaration types the binding (integer
						// textures such as SDFGI's packed radiance); reflection
						// only knows the format for storage images.
						const WGPUTextureSampleType *sampled_decl = sampled_texture_decls.getptr(((uint64_t)set_index << 32) | remapped_binding);
						single_entry.texture.sampleType = depth_texture_bindings.has(((uint64_t)set_index << 32) | remapped_binding) ? WGPUTextureSampleType_Depth : (sampled_decl != nullptr ? *sampled_decl : _data_format_to_wgpu_sample_type(uniform.texture_format));
						single_entry.texture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
						shader->texture_slot_types.insert(((uint64_t)set_index << 32) | remapped_binding, ShaderInfo::TextureSlotType{ single_entry.texture.sampleType, single_entry.texture.viewDimension });
					} else {
						const Pair<WGPUTextureFormat, WGPUStorageTextureAccess> *decl = storage_texture_decls.getptr(((uint64_t)set_index << 32) | remapped_binding);
						single_entry.storageTexture.access = decl != nullptr ? decl->second : (uniform.writable ? WGPUStorageTextureAccess_ReadWrite : WGPUStorageTextureAccess_ReadOnly);
						// Storage textures are created with the substituted format
						// (see texture_create); the layout has to agree, or the
						// binding is both a format mismatch and - for formats that
						// are not storage-capable at all - invalid on its own.
						const WGPUTextureFormat sf = decl != nullptr ? decl->first : _data_format_to_wgpu(uniform.texture_format);
						single_entry.storageTexture.format = _wgpu_storage_substitute(sf != WGPUTextureFormat_Undefined ? sf : WGPUTextureFormat_RGBA8Unorm);
						single_entry.storageTexture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
					}
					entries.push_back(single_entry);
					continue;
				}
				for (uint32_t element = 0; element < (uint32_t)uniform.length; element++) {
					WGPUBindGroupLayoutEntry element_entry = entry;
					element_entry.binding = RenderingShaderContainerWebGPU::ARRAY_BINDING_BASE + uniform.binding * RenderingShaderContainerWebGPU::ARRAY_BINDING_STRIDE + element;
					element_entry.visibility = binding_visibility((uint32_t)set_index, element_entry.binding);
					if (uniform.type == UNIFORM_TYPE_SAMPLER) {
						element_entry.sampler.type = comparison_sampler_bindings.has(((uint64_t)set_index << 32) | element_entry.binding) ? WGPUSamplerBindingType_Comparison : (shader->nonfiltering_samplers.has(((uint64_t)set_index << 32) | element_entry.binding) ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering);
					} else if (uniform.type == UNIFORM_TYPE_TEXTURE) {
						const WGPUTextureSampleType *sampled_decl = sampled_texture_decls.getptr(((uint64_t)set_index << 32) | element_entry.binding);
						element_entry.texture.sampleType = depth_texture_bindings.has(((uint64_t)set_index << 32) | element_entry.binding) ? WGPUTextureSampleType_Depth : (sampled_decl != nullptr ? *sampled_decl : _data_format_to_wgpu_sample_type(uniform.texture_format));
						element_entry.texture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
						shader->texture_slot_types.insert(((uint64_t)set_index << 32) | element_entry.binding, ShaderInfo::TextureSlotType{ element_entry.texture.sampleType, element_entry.texture.viewDimension });
					} else {
						const Pair<WGPUTextureFormat, WGPUStorageTextureAccess> *decl = storage_texture_decls.getptr(((uint64_t)set_index << 32) | element_entry.binding);
						element_entry.storageTexture.access = decl != nullptr ? decl->second : (uniform.writable ? WGPUStorageTextureAccess_ReadWrite : WGPUStorageTextureAccess_ReadOnly);
						const WGPUTextureFormat ef = decl != nullptr ? decl->first : _data_format_to_wgpu(uniform.texture_format);
						element_entry.storageTexture.format = _wgpu_storage_substitute(ef != WGPUTextureFormat_Undefined ? ef : WGPUTextureFormat_RGBA8Unorm);
						element_entry.storageTexture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
					}
					entries.push_back(element_entry);
				}
				continue;
			}
			switch (uniform.type) {
				case UNIFORM_TYPE_SAMPLER:
					entry.sampler.type = comparison_sampler_bindings.has(((uint64_t)set_index << 32) | entry.binding) ? WGPUSamplerBindingType_Comparison : (shader->nonfiltering_samplers.has(((uint64_t)set_index << 32) | entry.binding) ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering);
					break;
				case UNIFORM_TYPE_TEXTURE:
				case UNIFORM_TYPE_INPUT_ATTACHMENT: {
					const WGPUTextureSampleType *sampled_decl = sampled_texture_decls.getptr(((uint64_t)set_index << 32) | entry.binding);
					entry.texture.sampleType = depth_texture_bindings.has(((uint64_t)set_index << 32) | entry.binding) ? WGPUTextureSampleType_Depth : (sampled_decl != nullptr ? *sampled_decl : _data_format_to_wgpu_sample_type(uniform.texture_format));
					entry.texture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
					entry.texture.multisampled = multisampled_texture_bindings.has(((uint64_t)set_index << 32) | entry.binding);
					if (entry.texture.multisampled && entry.texture.sampleType == WGPUTextureSampleType_Float) {
						// Multisampled bindings cannot be filterable.
						entry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
					}
					if (entry.texture.sampleType == WGPUTextureSampleType_Float && shader->load_only_float_bindings.has(((uint64_t)set_index << 32) | entry.binding)) {
						// textureLoad-only slot: unfilterable also admits
						// depth-aspect views (all float formats stay valid).
						entry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
					}
					shader->texture_slot_types.insert(((uint64_t)set_index << 32) | entry.binding, ShaderInfo::TextureSlotType{ entry.texture.sampleType, entry.texture.viewDimension });
				} break;
				case UNIFORM_TYPE_IMAGE: {
					// Tint converts read-only storage images into sampled
					// textures (textureLoad); the layout must match the WGSL.
					const WGPUTextureSampleType *sampled_decl = sampled_texture_decls.getptr(((uint64_t)set_index << 32) | entry.binding);
					if (sampled_decl != nullptr && !storage_texture_decls.has(((uint64_t)set_index << 32) | entry.binding)) {
						entry.texture.sampleType = *sampled_decl;
						entry.texture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
						break;
					}
					// The WGSL declaration is authoritative for both format
					// (reflection can lack it for format-less writeonly GLSL
					// images) and access mode. A binding absent from every
					// stage's WGSL (dead-eliminated) whose reflected format
					// cannot be used for storage gets a self-consistent
					// placeholder (r32float read + a substitute texture at
					// bind time) so the layout stays valid.
					const Pair<WGPUTextureFormat, WGPUStorageTextureAccess> *decl = storage_texture_decls.getptr(((uint64_t)set_index << 32) | entry.binding);
					WGPUTextureFormat storage_format = _wgpu_storage_substitute(decl != nullptr ? decl->first : _data_format_to_wgpu(uniform.texture_format));
					if (decl == nullptr && (!_wgpu_format_supports_storage(storage_format, device_has_texture_formats_tier1) || (uniform.writable == 0 && storage_format != WGPUTextureFormat_R32Float && storage_format != WGPUTextureFormat_R32Uint && storage_format != WGPUTextureFormat_R32Sint))) {
						shader->dead_storage_bindings.insert(((uint64_t)set_index << 32) | entry.binding);
						entry.storageTexture.access = WGPUStorageTextureAccess_ReadOnly;
						entry.storageTexture.format = WGPUTextureFormat_R32Float;
						entry.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
						break;
					}
					const bool rw_ok = storage_format == WGPUTextureFormat_R32Float || storage_format == WGPUTextureFormat_R32Uint || storage_format == WGPUTextureFormat_R32Sint;
					entry.storageTexture.access = decl != nullptr ? decl->second : (uniform.writable ? (rw_ok ? WGPUStorageTextureAccess_ReadWrite : WGPUStorageTextureAccess_WriteOnly) : WGPUStorageTextureAccess_ReadOnly);
					entry.storageTexture.format = storage_format != WGPUTextureFormat_Undefined ? storage_format : WGPUTextureFormat_RGBA8Unorm;
					entry.storageTexture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
				} break;
				case UNIFORM_TYPE_UNIFORM_BUFFER:
					entry.buffer.type = WGPUBufferBindingType_Uniform;
					break;
				case UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC:
					entry.buffer.type = WGPUBufferBindingType_Uniform;
					entry.buffer.hasDynamicOffset = true;
					break;
				case UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC: {
					// No WGSL declaration in any stage = dead-eliminated
					// binding; ReadOnly keeps the layout valid everywhere.
					const bool *rw = storage_buffer_read_write.getptr(((uint64_t)set_index << 32) | entry.binding);
					entry.buffer.type = (rw != nullptr && *rw) ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;
					entry.buffer.hasDynamicOffset = true;
				} break;
				case UNIFORM_TYPE_TEXTURE_BUFFER:
				case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER:
				case UNIFORM_TYPE_IMAGE_BUFFER:
					// Texel buffers have no WGSL equivalent; the transform
					// removes their (unused) declarations from the modules,
					// so a placeholder storage binding keeps layouts and
					// bind groups consistent.
					entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
					break;
				case UNIFORM_TYPE_STORAGE_BUFFER: {
					// No WGSL declaration in any stage = dead-eliminated
					// binding; ReadOnly keeps the layout valid everywhere.
					const bool *rw = storage_buffer_read_write.getptr(((uint64_t)set_index << 32) | entry.binding);
					entry.buffer.type = (rw != nullptr && *rw) ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;
				} break;
				case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: {
					const WGPUTextureSampleType *combined_decl = sampled_texture_decls.getptr(((uint64_t)set_index << 32) | entry.binding);
					entry.texture.sampleType = depth_texture_bindings.has(((uint64_t)set_index << 32) | entry.binding) ? WGPUTextureSampleType_Depth : (combined_decl != nullptr ? *combined_decl : _data_format_to_wgpu_sample_type(uniform.texture_format));
					entry.texture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
					entry.texture.multisampled = multisampled_texture_bindings.has(((uint64_t)set_index << 32) | entry.binding);
					if (entry.texture.multisampled && entry.texture.sampleType == WGPUTextureSampleType_Float) {
						// Multisampled bindings cannot be filterable.
						entry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
					}
					if (entry.texture.sampleType == WGPUTextureSampleType_Float && shader->load_only_float_bindings.has(((uint64_t)set_index << 32) | entry.binding)) {
						// textureLoad-only slot: unfilterable also admits
						// depth-aspect views (all float formats stay valid).
						entry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
					}
					cap_vertex_budget(entry);
					entries.push_back(entry);
						WGPUBindGroupLayoutEntry sampler_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
					sampler_entry.binding = remapped_binding + 1;
					sampler_entry.visibility = binding_visibility((uint32_t)set_index, remapped_binding + 1);
					sampler_entry.sampler.type = comparison_sampler_bindings.has(((uint64_t)set_index << 32) | sampler_entry.binding) ? WGPUSamplerBindingType_Comparison : WGPUSamplerBindingType_Filtering;
						cap_vertex_budget(sampler_entry);
					entries.push_back(sampler_entry);
					combined_before++;
					continue;
				}
				default:
					shader_free(ShaderID(shader));
					ERR_FAIL_V_MSG(ShaderID(), vformat("Unsupported uniform type %d in set %d binding %d on the WebGPU driver.", uniform.type, set_index, uniform.binding));
			}
			cap_vertex_budget(entry);
			entries.push_back(entry);
		}

		if (shader->push_constant_size > 0 && (uint32_t)set_index == RenderingShaderContainerWebGPU::PUSH_CONSTANT_GROUP) {
			set0_only_push_constant = entries.is_empty();
			WGPUBindGroupLayoutEntry pc_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
			pc_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
			pc_entry.visibility = binding_visibility(RenderingShaderContainerWebGPU::PUSH_CONSTANT_GROUP, RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING);
			pc_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
			pc_entry.buffer.hasDynamicOffset = true;
			entries.push_back(pc_entry);
		}

		WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layout_desc.label = { shader->name.get_data(), WGPU_STRLEN };
		layout_desc.entryCount = entries.size();
		layout_desc.entries = entries.ptr();
		WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
		if (layout == nullptr) {
			shader_free(ShaderID(shader));
			ERR_FAIL_V_MSG(ShaderID(), vformat("Failed to create a bind group layout for set %d.", set_index));
		}
		shader->bind_group_layouts.push_back(layout);
		shader->layout_bindings.resize(shader->layout_bindings.size() + 1);
		for (const WGPUBindGroupLayoutEntry &recorded_entry : entries) {
			shader->layout_bindings[shader->layout_bindings.size() - 1].insert(recorded_entry.binding);
		}
	}

	// A push-constant-only shader with no set 0 still needs the reserved binding.
	if (shader->push_constant_size > 0 && shader->bind_group_layouts.is_empty()) {
		set0_only_push_constant = true;
		WGPUBindGroupLayoutEntry pc_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
		pc_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
		pc_entry.visibility = binding_visibility(RenderingShaderContainerWebGPU::PUSH_CONSTANT_GROUP, RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING);
		pc_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		pc_entry.buffer.hasDynamicOffset = true;
		WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layout_desc.label = { shader->name.get_data(), WGPU_STRLEN };
		layout_desc.entryCount = 1;
		layout_desc.entries = &pc_entry;
		WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
		ERR_FAIL_NULL_V(layout, ShaderID());
		shader->bind_group_layouts.push_back(layout);
		shader->layout_bindings.resize(shader->layout_bindings.size() + 1);
		shader->layout_bindings[shader->layout_bindings.size() - 1].insert(uint32_t(RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING));
	}

	// Set 0 with nothing but the reserved binding never gets a
	// uniform_set_create call; give the shader a ready-made bind group.
	if (set0_only_push_constant) {
		shader->push_constant_bind_group = _shader_build_push_constant_bind_group(shader);
		shader->push_constant_bind_group_epoch = push_constant_epoch;
		if (shader->push_constant_bind_group == nullptr) {
			shader_free(ShaderID(shader));
			ERR_FAIL_V_MSG(ShaderID(), "Failed to create the push-constant bind group.");
		}
	}

	WGPUPipelineLayoutDescriptor pipeline_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	pipeline_layout_desc.bindGroupLayoutCount = shader->bind_group_layouts.size();
	pipeline_layout_desc.bindGroupLayouts = shader->bind_group_layouts.ptr();
	shader->pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_desc);
	if (shader->pipeline_layout == nullptr) {
		shader_free(ShaderID(shader));
		ERR_FAIL_V_MSG(ShaderID(), "Failed to create the pipeline layout.");
	}
	return ShaderID(shader);
}

void RenderingDeviceDriverWebGPU::shader_free(ShaderID p_shader) {
	WEBGPU_MAIN_THREAD_GUARD(shader_free(p_shader));
	resource_epoch++; // Invalidates cached render bundles (see BundleCacheEntry).
	ShaderInfo *shader = (ShaderInfo *)p_shader.id;
	shader_destroy_modules(p_shader);
	for (WGPUBindGroupLayout layout : shader->bind_group_layouts) {
		wgpuBindGroupLayoutRelease(layout);
	}
	if (shader->pipeline_layout != nullptr) {
		wgpuPipelineLayoutRelease(shader->pipeline_layout);
	}
	if (shader->push_constant_bind_group != nullptr) {
		wgpuBindGroupRelease(shader->push_constant_bind_group);
	}
	memdelete(shader);
}

void RenderingDeviceDriverWebGPU::shader_destroy_modules(ShaderID p_shader) {
	WEBGPU_MAIN_THREAD_GUARD(shader_destroy_modules(p_shader));
	ShaderInfo *shader = (ShaderInfo *)p_shader.id;
	for (WGPUShaderModule module : shader->modules) {
		wgpuShaderModuleRelease(module);
	}
	shader->modules.clear();
	shader->module_stages.clear();
}

/*******************/
/**** PIPELINES ****/
/*******************/



static const char *SHADER_ENTRY_POINT = "main"; // Tint keeps the SPIR-V entry point name.

static bool _is_depth_stencil_format(RenderingDeviceCommons::DataFormat p_format) {
	switch (p_format) {
		case RenderingDeviceCommons::DATA_FORMAT_D16_UNORM:
		case RenderingDeviceCommons::DATA_FORMAT_X8_D24_UNORM_PACK32:
		case RenderingDeviceCommons::DATA_FORMAT_D32_SFLOAT:
		case RenderingDeviceCommons::DATA_FORMAT_S8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_D16_UNORM_S8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_D24_UNORM_S8_UINT:
		case RenderingDeviceCommons::DATA_FORMAT_D32_SFLOAT_S8_UINT:
			return true;
		default:
			return false;
	}
}

static WGPUBlendFactor _blend_factor_to_wgpu(RenderingDeviceCommons::BlendFactor p_factor) {
	switch (p_factor) {
		case RenderingDeviceCommons::BLEND_FACTOR_ZERO:
			return WGPUBlendFactor_Zero;
		case RenderingDeviceCommons::BLEND_FACTOR_ONE:
			return WGPUBlendFactor_One;
		case RenderingDeviceCommons::BLEND_FACTOR_SRC_COLOR:
			return WGPUBlendFactor_Src;
		case RenderingDeviceCommons::BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
			return WGPUBlendFactor_OneMinusSrc;
		case RenderingDeviceCommons::BLEND_FACTOR_DST_COLOR:
			return WGPUBlendFactor_Dst;
		case RenderingDeviceCommons::BLEND_FACTOR_ONE_MINUS_DST_COLOR:
			return WGPUBlendFactor_OneMinusDst;
		case RenderingDeviceCommons::BLEND_FACTOR_SRC_ALPHA:
			return WGPUBlendFactor_SrcAlpha;
		case RenderingDeviceCommons::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
			return WGPUBlendFactor_OneMinusSrcAlpha;
		case RenderingDeviceCommons::BLEND_FACTOR_DST_ALPHA:
			return WGPUBlendFactor_DstAlpha;
		case RenderingDeviceCommons::BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
			return WGPUBlendFactor_OneMinusDstAlpha;
		case RenderingDeviceCommons::BLEND_FACTOR_CONSTANT_COLOR:
		case RenderingDeviceCommons::BLEND_FACTOR_CONSTANT_ALPHA:
			return WGPUBlendFactor_Constant;
		case RenderingDeviceCommons::BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
		case RenderingDeviceCommons::BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
			return WGPUBlendFactor_OneMinusConstant;
		case RenderingDeviceCommons::BLEND_FACTOR_SRC_ALPHA_SATURATE:
			return WGPUBlendFactor_SrcAlphaSaturated;
		default:
			return WGPUBlendFactor_Undefined; // SRC1 factors need dual-source blending.
	}
}

static WGPUBlendOperation _blend_op_to_wgpu(RenderingDeviceCommons::BlendOperation p_op) {
	switch (p_op) {
		case RenderingDeviceCommons::BLEND_OP_ADD:
			return WGPUBlendOperation_Add;
		case RenderingDeviceCommons::BLEND_OP_SUBTRACT:
			return WGPUBlendOperation_Subtract;
		case RenderingDeviceCommons::BLEND_OP_REVERSE_SUBTRACT:
			return WGPUBlendOperation_ReverseSubtract;
		case RenderingDeviceCommons::BLEND_OP_MINIMUM:
			return WGPUBlendOperation_Min;
		default:
			return WGPUBlendOperation_Max;
	}
}

static WGPUStencilOperation _stencil_op_to_wgpu(RenderingDeviceCommons::StencilOperation p_op) {
	switch (p_op) {
		case RenderingDeviceCommons::STENCIL_OP_KEEP:
			return WGPUStencilOperation_Keep;
		case RenderingDeviceCommons::STENCIL_OP_ZERO:
			return WGPUStencilOperation_Zero;
		case RenderingDeviceCommons::STENCIL_OP_REPLACE:
			return WGPUStencilOperation_Replace;
		case RenderingDeviceCommons::STENCIL_OP_INCREMENT_AND_CLAMP:
			return WGPUStencilOperation_IncrementClamp;
		case RenderingDeviceCommons::STENCIL_OP_DECREMENT_AND_CLAMP:
			return WGPUStencilOperation_DecrementClamp;
		case RenderingDeviceCommons::STENCIL_OP_INVERT:
			return WGPUStencilOperation_Invert;
		case RenderingDeviceCommons::STENCIL_OP_INCREMENT_AND_WRAP:
			return WGPUStencilOperation_IncrementWrap;
		default:
			return WGPUStencilOperation_DecrementWrap;
	}
}

static WGPUCompareFunction _compare_op_to_wgpu(RenderingDeviceCommons::CompareOperator p_op) {
	switch (p_op) {
		case RenderingDeviceCommons::COMPARE_OP_NEVER:
			return WGPUCompareFunction_Never;
		case RenderingDeviceCommons::COMPARE_OP_LESS:
			return WGPUCompareFunction_Less;
		case RenderingDeviceCommons::COMPARE_OP_EQUAL:
			return WGPUCompareFunction_Equal;
		case RenderingDeviceCommons::COMPARE_OP_LESS_OR_EQUAL:
			return WGPUCompareFunction_LessEqual;
		case RenderingDeviceCommons::COMPARE_OP_GREATER:
			return WGPUCompareFunction_Greater;
		case RenderingDeviceCommons::COMPARE_OP_NOT_EQUAL:
			return WGPUCompareFunction_NotEqual;
		case RenderingDeviceCommons::COMPARE_OP_GREATER_OR_EQUAL:
			return WGPUCompareFunction_GreaterEqual;
		default:
			return WGPUCompareFunction_Always;
	}
}

static WGPUVertexFormat _vertex_format_to_wgpu(RenderingDeviceCommons::DataFormat p_format) {
	switch (p_format) {
		case RenderingDeviceCommons::DATA_FORMAT_R32_SFLOAT:
			return WGPUVertexFormat_Float32;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_SFLOAT:
			return WGPUVertexFormat_Float32x2;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32_SFLOAT:
			return WGPUVertexFormat_Float32x3;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_SFLOAT:
			return WGPUVertexFormat_Float32x4;
		case RenderingDeviceCommons::DATA_FORMAT_R32_UINT:
			return WGPUVertexFormat_Uint32;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_UINT:
			return WGPUVertexFormat_Uint32x2;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32_UINT:
			return WGPUVertexFormat_Uint32x3;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_UINT:
			return WGPUVertexFormat_Uint32x4;
		case RenderingDeviceCommons::DATA_FORMAT_R32_SINT:
			return WGPUVertexFormat_Sint32;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32_SINT:
			return WGPUVertexFormat_Sint32x2;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32_SINT:
			return WGPUVertexFormat_Sint32x3;
		case RenderingDeviceCommons::DATA_FORMAT_R32G32B32A32_SINT:
			return WGPUVertexFormat_Sint32x4;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_SFLOAT:
			return WGPUVertexFormat_Float16x2;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_SFLOAT:
			return WGPUVertexFormat_Float16x4;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_UNORM:
			return WGPUVertexFormat_Unorm8x4;
		case RenderingDeviceCommons::DATA_FORMAT_R8G8B8A8_UINT:
			return WGPUVertexFormat_Uint8x4;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16_UNORM:
			return WGPUVertexFormat_Unorm16x2;
		case RenderingDeviceCommons::DATA_FORMAT_R16G16B16A16_UNORM:
			return WGPUVertexFormat_Unorm16x4;
		default:
			return (WGPUVertexFormat)0; // No undefined member in the enum; 0 is unused.
	}
}

RenderingDeviceDriver::VertexFormatID RenderingDeviceDriverWebGPU::vertex_format_create(Span<VertexAttribute> p_vertex_attribs, const VertexAttributeBindingsMap &p_vertex_bindings) {
	WEBGPU_MAIN_THREAD_GUARD(vertex_format_create(p_vertex_attribs, p_vertex_bindings));
	VertexFormatInfo *format = memnew(VertexFormatInfo);
	// One WGPU buffer layout per binding, in binding order.
	LocalVector<uint32_t> bindings;
	for (const KeyValue<uint32_t, VertexAttributeBinding> &binding : p_vertex_bindings) {
		bindings.push_back(binding.key);
	}
	bindings.sort();

	format->attributes.resize(bindings.size());
	for (uint32_t i = 0; i < bindings.size(); i++) {
		for (const VertexAttribute &attribute : p_vertex_attribs) {
			const uint32_t attribute_binding = attribute.binding == UINT32_MAX ? attribute.location : attribute.binding;
			if (attribute_binding != bindings[i]) {
				continue;
			}
			WGPUVertexAttribute wgpu_attribute = {};
			wgpu_attribute.format = _vertex_format_to_wgpu(attribute.format);
			if (wgpu_attribute.format == (WGPUVertexFormat)0) {
				memdelete(format);
				ERR_FAIL_V_MSG(VertexFormatID(), vformat("Unsupported vertex format %d on the WebGPU driver.", attribute.format));
			}
			wgpu_attribute.offset = attribute.offset;
			wgpu_attribute.shaderLocation = attribute.location;
			format->attributes[i].push_back(wgpu_attribute);
		}
	}

	for (uint32_t i = 0; i < bindings.size(); i++) {
		const VertexAttributeBinding &binding = p_vertex_bindings[bindings[i]];
		WGPUVertexBufferLayout layout = {};
		layout.arrayStride = binding.stride;
		layout.stepMode = binding.frequency == VERTEX_FREQUENCY_INSTANCE ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
		layout.attributeCount = format->attributes[i].size();
		layout.attributes = format->attributes[i].ptr();
		format->buffer_layouts.push_back(layout);
	}
	return VertexFormatID(format);
}

void RenderingDeviceDriverWebGPU::vertex_format_free(VertexFormatID p_vertex_format) {
	WEBGPU_MAIN_THREAD_GUARD(vertex_format_free(p_vertex_format));
	memdelete((VertexFormatInfo *)p_vertex_format.id);
}

RenderingDeviceDriver::RenderPassID RenderingDeviceDriverWebGPU::render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses, VectorView<SubpassDependency> p_subpass_dependencies, uint32_t p_view_count, AttachmentReference p_fragment_density_map_attachment) {
	WEBGPU_MAIN_THREAD_GUARD(render_pass_create(p_attachments, p_subpasses, p_subpass_dependencies, p_view_count, p_fragment_density_map_attachment));
	ERR_FAIL_COND_V_MSG(p_subpasses.size() > 1, RenderPassID(), "Multiple subpasses are not supported by WebGPU.");
	ERR_FAIL_COND_V_MSG(p_view_count > 1, RenderPassID(), "Multiview is not supported by the WebGPU driver yet.");

	RenderPassInfo *pass = memnew(RenderPassInfo);
	for (uint32_t i = 0; i < p_attachments.size(); i++) {
		const Attachment &attachment = p_attachments[i];
		RenderPassAttachment pass_attachment;
		pass_attachment.format = _data_format_to_wgpu(attachment.format);
		if (attachment.samples != TEXTURE_SAMPLES_1 && attachment.format == DATA_FORMAT_R8G8_UINT) {
			// The rg8ui->rg32uint storage substitution only applies to
			// single-sample textures (rg32uint cannot be multisampled; the
			// MSAA twin is resolved by a compute shader that samples it as
			// u32, which is format-agnostic).
			pass_attachment.format = WGPUTextureFormat_RG8Uint;
		}
		pass_attachment.is_depth_stencil = _is_depth_stencil_format(attachment.format);
		pass_attachment.texture_samples = 1u << attachment.samples;
		pass_attachment.load_op = attachment.load_op == ATTACHMENT_LOAD_OP_LOAD ? WGPULoadOp_Load : WGPULoadOp_Clear;
		pass_attachment.store_op = attachment.store_op == ATTACHMENT_STORE_OP_STORE ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
		pass_attachment.stencil_load_op = attachment.stencil_load_op == ATTACHMENT_LOAD_OP_LOAD ? WGPULoadOp_Load : WGPULoadOp_Clear;
		pass_attachment.stencil_store_op = attachment.stencil_store_op == ATTACHMENT_STORE_OP_STORE ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
		pass->attachments.push_back(pass_attachment);
	}
	if (p_attachments.size() == 0) {
		// An attachment-less pass (fragment side effects only): WebGPU needs
		// at least one attachment, so the pass carries a synthetic R8 color
		// target that pipelines never write (see framebuffer_create).
		RenderPassAttachment dummy;
		dummy.format = WGPUTextureFormat_R8Unorm;
		dummy.is_depth_stencil = false;
		dummy.texture_samples = 1;
		dummy.load_op = WGPULoadOp_Clear;
		dummy.store_op = WGPUStoreOp_Discard;
		pass->attachments.push_back(dummy);
		pass->attachment_less = true;
	}
	if (p_subpasses.size() == 1) {
		const Subpass &subpass = p_subpasses[0];
		for (uint32_t c = 0; c < subpass.color_references.size(); c++) {
			if (c >= subpass.resolve_references.size()) {
				continue;
			}
			const uint32_t color_index = subpass.color_references[c].attachment;
			const uint32_t resolve_index = subpass.resolve_references[c].attachment;
			if (color_index == AttachmentReference::UNUSED || resolve_index == AttachmentReference::UNUSED) {
				continue;
			}
			pass->attachments[color_index].resolve_attachment = (int32_t)resolve_index;
			pass->attachments[resolve_index].is_resolve_target = true;
		}
		if (subpass.depth_resolve_reference.attachment != AttachmentReference::UNUSED) {
			// WebGPU has no in-pass depth resolve; skip the target so the pass
			// stays valid. The multisampled depth still works for the pass
			// itself; only consumers of the resolved copy would be affected.
			pass->attachments[subpass.depth_resolve_reference.attachment].is_resolve_target = true;
			print_verbose("WebGPU: depth resolve attachment ignored (unsupported by WebGPU render passes).");
		}
	}
	return RenderPassID(pass);
}

void RenderingDeviceDriverWebGPU::render_pass_free(RenderPassID p_render_pass) {
	WEBGPU_MAIN_THREAD_GUARD(render_pass_free(p_render_pass));
	RenderPassInfo *pass = (RenderPassInfo *)p_render_pass.id;
	ERR_FAIL_COND(pass->from_swap_chain); // Owned by the swap chain.
	memdelete(pass);
}

RenderingDeviceDriver::FramebufferID RenderingDeviceDriverWebGPU::framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) {
	WEBGPU_MAIN_THREAD_GUARD(framebuffer_create(p_render_pass, p_attachments, p_width, p_height));
	FramebufferInfo *framebuffer = memnew(FramebufferInfo);
	framebuffer->width = p_width;
	framebuffer->height = p_height;
	for (uint32_t i = 0; i < p_attachments.size(); i++) {
		framebuffer->views.push_back(((TextureInfo *)p_attachments[i].id)->view);
	}
	const RenderPassInfo *pass = (const RenderPassInfo *)p_render_pass.id;
	if (pass != nullptr && pass->attachment_less && p_attachments.size() == 0) {
		// The synthetic color target of an attachment-less pass, sized like
		// the framebuffer so the rasterizer covers the same area.
		WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
		desc.usage = WGPUTextureUsage_RenderAttachment;
		desc.dimension = WGPUTextureDimension_2D;
		desc.size = { MAX(1u, p_width), MAX(1u, p_height), 1 };
		desc.format = WGPUTextureFormat_R8Unorm;
		desc.mipLevelCount = 1;
		desc.sampleCount = 1;
		framebuffer->dummy_texture = wgpuDeviceCreateTexture(device, &desc);
		if (framebuffer->dummy_texture == nullptr) {
			memdelete(framebuffer);
			ERR_FAIL_V_MSG(FramebufferID(), "Failed to create the dummy attachment of an attachment-less framebuffer.");
		}
		framebuffer->dummy_view = wgpuTextureCreateView(framebuffer->dummy_texture, nullptr);
		framebuffer->views.push_back(framebuffer->dummy_view);
	}
	return FramebufferID(framebuffer);
}

void RenderingDeviceDriverWebGPU::framebuffer_free(FramebufferID p_framebuffer) {
	WEBGPU_MAIN_THREAD_GUARD(framebuffer_free(p_framebuffer));
	FramebufferInfo *framebuffer = (FramebufferInfo *)p_framebuffer.id;
	for (BundleCacheEntry &entry : framebuffer->bundle_cache) {
		if (entry.bundle != nullptr) {
			wgpuRenderBundleRelease(entry.bundle);
		}
		if (entry.params_buffer != nullptr) {
			wgpuBufferRelease(entry.params_buffer);
		}
	}
	// Views are owned by their textures; only the dummy target is ours.
	if (framebuffer->dummy_view != nullptr) {
		wgpuTextureViewRelease(framebuffer->dummy_view);
	}
	if (framebuffer->dummy_texture != nullptr) {
		wgpuTextureRelease(framebuffer->dummy_texture);
	}
	memdelete(framebuffer);
}

WGPUTextureView RenderingDeviceDriverWebGPU::_get_placeholder_float_view() {
	if (placeholder_float_view == nullptr) {
		WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
		desc.usage = WGPUTextureUsage_TextureBinding;
		desc.dimension = WGPUTextureDimension_2D;
		desc.size = { 4, 4, 1 };
		desc.format = WGPUTextureFormat_RGBA8Unorm;
		desc.mipLevelCount = 1;
		desc.sampleCount = 1;
		placeholder_float_texture = wgpuDeviceCreateTexture(device, &desc);
		placeholder_float_view = wgpuTextureCreateView(placeholder_float_texture, nullptr);
	}
	return placeholder_float_view;
}

WGPUTextureView RenderingDeviceDriverWebGPU::_get_placeholder_class_view(WGPUTextureSampleType p_sample_type, WGPUTextureViewDimension p_dimension) {
	const uint32_t key = ((uint32_t)p_sample_type << 8) | (uint32_t)p_dimension;
	WGPUTextureView *existing = placeholder_class_views.getptr(key);
	if (existing != nullptr) {
		return *existing;
	}
	const bool cube = p_dimension == WGPUTextureViewDimension_Cube || p_dimension == WGPUTextureViewDimension_CubeArray;
	WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
	desc.usage = WGPUTextureUsage_TextureBinding;
	desc.dimension = p_dimension == WGPUTextureViewDimension_3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
	desc.size = { 4, 4, (uint32_t)(p_dimension == WGPUTextureViewDimension_3D ? 4 : (cube ? 6 : 1)) };
	desc.format = p_sample_type == WGPUTextureSampleType_Uint ? WGPUTextureFormat_R32Uint : (p_sample_type == WGPUTextureSampleType_Sint ? WGPUTextureFormat_R32Sint : WGPUTextureFormat_RGBA8Unorm);
	desc.mipLevelCount = 1;
	desc.sampleCount = 1;
	WGPUTexture texture = wgpuDeviceCreateTexture(device, &desc);
	ERR_FAIL_NULL_V(texture, nullptr);
	WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	view_desc.dimension = p_dimension;
	WGPUTextureView view = wgpuTextureCreateView(texture, &view_desc);
	placeholder_class_textures.push_back(texture);
	placeholder_class_views.insert(key, view);
	return view;
}

WGPUTextureView RenderingDeviceDriverWebGPU::_texture_view_for_slot(const ShaderInfo *p_shader, uint32_t p_set, uint32_t p_binding, const TextureInfo *p_texture) {
	// A texture whose sample class disagrees with the layout's declaration
	// (an integer texture in a float slot or the reverse) would make the
	// whole bind group invalid; such slots are bindings the variant never
	// reads (typed from reflection) or debug paths, so a placeholder of the
	// declared class keeps the frame valid.
	const ShaderInfo::TextureSlotType *slot = p_shader->texture_slot_types.getptr(((uint64_t)p_set << 32) | p_binding);
	if (slot != nullptr && !_wgpu_sample_class_matches(slot->sample_type, p_texture->wgpu_format)) {
		return _get_placeholder_class_view(slot->sample_type, slot->dimension);
	}
	return p_texture->view;
}

WGPUTextureView RenderingDeviceDriverWebGPU::_get_placeholder_storage_view() {
	if (placeholder_storage_view == nullptr) {
		WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
		desc.usage = WGPUTextureUsage_StorageBinding;
		desc.dimension = WGPUTextureDimension_2D;
		desc.size = { 4, 4, 1 };
		desc.format = WGPUTextureFormat_R32Float;
		desc.mipLevelCount = 1;
		desc.sampleCount = 1;
		placeholder_storage_texture = wgpuDeviceCreateTexture(device, &desc);
		placeholder_storage_view = wgpuTextureCreateView(placeholder_storage_texture, nullptr);
	}
	return placeholder_storage_view;
}

WGPUSampler RenderingDeviceDriverWebGPU::_get_nonfiltering_sampler() {
	if (nonfiltering_substitute_sampler == nullptr) {
		WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
		desc.magFilter = WGPUFilterMode_Nearest;
		desc.minFilter = WGPUFilterMode_Nearest;
		desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
		desc.addressModeU = WGPUAddressMode_ClampToEdge;
		desc.addressModeV = WGPUAddressMode_ClampToEdge;
		desc.addressModeW = WGPUAddressMode_ClampToEdge;
		nonfiltering_substitute_sampler = wgpuDeviceCreateSampler(device, &desc);
	}
	return nonfiltering_substitute_sampler;
}

WGPUBindGroup RenderingDeviceDriverWebGPU::_uniform_set_build(VectorView<BoundUniform> p_uniforms, const ShaderInfo *p_shader_info, uint32_t p_set_index, UniformSetInfo *p_bookkeeping) {
	WEBGPU_MAIN_THREAD_GUARD(_uniform_set_build(p_uniforms, p_shader_info, p_set_index, p_bookkeeping));
	const ShaderInfo *shader = p_shader_info;
	ERR_FAIL_COND_V(p_set_index >= shader->bind_group_layouts.size(), nullptr);

	LocalVector<WGPUBindGroupEntry> entries;
	uint32_t combined_before = 0;
	for (uint32_t i = 0; i < p_uniforms.size(); i++) {
		const BoundUniform &uniform = p_uniforms[i];
		const uint32_t remapped_binding = uniform.binding + combined_before;
		if (p_set_index < shader->layout_bindings.size() && !shader->layout_bindings[p_set_index].has(remapped_binding) && !(uniform.ids.size() > 1 && shader->layout_bindings[p_set_index].has(RenderingShaderContainerWebGPU::ARRAY_BINDING_BASE + uniform.binding * RenderingShaderContainerWebGPU::ARRAY_BINDING_STRIDE))) {
			// The engine binds uniforms this shader variant's layout never
			// reflected (dead-stripped by variant defines); a bind group may
			// only contain the layout's entries.
			continue;
		}
		WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
		entry.binding = remapped_binding;
		switch (uniform.type) {
			case UNIFORM_TYPE_SAMPLER:
			case UNIFORM_TYPE_TEXTURE:
			case UNIFORM_TYPE_IMAGE:
			case UNIFORM_TYPE_INPUT_ATTACHMENT: {
				if (uniform.type == UNIFORM_TYPE_IMAGE && shader->dead_storage_bindings.has(((uint64_t)p_set_index << 32) | remapped_binding)) {
					// Dead-eliminated storage binding: match the layout's
					// r32float placeholder.
					entry.textureView = _get_placeholder_storage_view();
					entries.push_back(entry);
					continue;
				}
				if (uniform.ids.size() > 1 && shader->flattened_array_bindings.has(((uint64_t)p_set_index << 32) | remapped_binding)) {
					// Flattened array: the shader sees a single handle at the
					// original binding; bind the first element.
					if (uniform.type == UNIFORM_TYPE_SAMPLER) {
						entry.sampler = (WGPUSampler)uniform.ids[0].id;
					} else {
						entry.textureView = _texture_view_for_slot(shader, p_set_index, remapped_binding, (const TextureInfo *)uniform.ids[0].id);
					}
					entries.push_back(entry);
					continue;
				}
				if (uniform.ids.size() > 1) {
					// Arrayed handles: one entry per element in the reserved
					// range, mirroring the bind group layout fan-out.
					if (uniform.ids.size() > RenderingShaderContainerWebGPU::ARRAY_BINDING_STRIDE) {
						ERR_FAIL_V(nullptr);
					}
					for (uint32_t element = 0; element < uniform.ids.size(); element++) {
						WGPUBindGroupEntry element_entry = WGPU_BIND_GROUP_ENTRY_INIT;
						element_entry.binding = RenderingShaderContainerWebGPU::ARRAY_BINDING_BASE + uniform.binding * RenderingShaderContainerWebGPU::ARRAY_BINDING_STRIDE + element;
						if (uniform.type == UNIFORM_TYPE_SAMPLER) {
							element_entry.sampler = shader->nonfiltering_samplers.has(((uint64_t)p_set_index << 32) | element_entry.binding) ? _get_nonfiltering_sampler() : (WGPUSampler)uniform.ids[element].id;
						} else {
							element_entry.textureView = _texture_view_for_slot(shader, p_set_index, element_entry.binding, (const TextureInfo *)uniform.ids[element].id);
						}
						entries.push_back(element_entry);
					}
					continue;
				}
				if (uniform.type == UNIFORM_TYPE_SAMPLER) {
					const uint64_t slot_key = ((uint64_t)p_set_index << 32) | remapped_binding;
					if (shader->nonfiltering_samplers.has(slot_key)) {
						entry.sampler = _get_nonfiltering_sampler();
					} else if (comparison_samplers.has((uint64_t)uniform.ids[0].id) && !shader->comparison_declared_bindings.has(slot_key)) {
						// A comparison sampler bound where this variant's WGSL
						// has a plain sampler (the declaration was pruned):
						// substitute a plain sampler.
						entry.sampler = _get_nonfiltering_sampler();
					} else {
						entry.sampler = (WGPUSampler)uniform.ids[0].id;
					}
				} else {
					const TextureInfo *bound_texture = (const TextureInfo *)uniform.ids[0].id;
					const bool bound_depth = bound_texture->wgpu_format == WGPUTextureFormat_Depth16Unorm || bound_texture->wgpu_format == WGPUTextureFormat_Depth24Plus || bound_texture->wgpu_format == WGPUTextureFormat_Depth32Float || bound_texture->wgpu_format == WGPUTextureFormat_Depth24PlusStencil8 || bound_texture->wgpu_format == WGPUTextureFormat_Depth32FloatStencil8;
					if (bound_depth && !shader->depth_declared_bindings.has(((uint64_t)p_set_index << 32) | remapped_binding) && !shader->load_only_float_bindings.has(((uint64_t)p_set_index << 32) | remapped_binding)) {
						// A depth texture bound where the shader samples float
						// with filtering (typically the engine's default-texture
						// substitution): WebGPU rejects the sample-type
						// mismatch, so bind a placeholder instead.
						// textureLoad-only slots are UnfilterableFloat, which
						// depth-aspect views satisfy — they fall through to the
						// real view below.
						entry.textureView = _get_placeholder_float_view();
					} else if (bound_depth && (bound_texture->wgpu_format == WGPUTextureFormat_Depth24PlusStencil8 || bound_texture->wgpu_format == WGPUTextureFormat_Depth32FloatStencil8)) {
						// Combined depth/stencil: sampled bindings need a
						// DepthOnly-aspect view (the default view selects
						// both aspects, which WebGPU rejects here).
						TextureInfo *mutable_texture = (TextureInfo *)uniform.ids[0].id;
						if (mutable_texture->depth_only_view == nullptr) {
							WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
							view_desc.dimension = mutable_texture->view_dimension;
							view_desc.aspect = WGPUTextureAspect_DepthOnly;
							mutable_texture->depth_only_view = wgpuTextureCreateView(mutable_texture->texture, &view_desc);
						}
						entry.textureView = mutable_texture->depth_only_view != nullptr ? mutable_texture->depth_only_view : mutable_texture->view;
					} else {
						entry.textureView = _texture_view_for_slot(shader, p_set_index, remapped_binding, bound_texture);
					}
				}
			} break;
			case UNIFORM_TYPE_UNIFORM_BUFFER:
			case UNIFORM_TYPE_STORAGE_BUFFER: {
				entry.buffer = ((BufferInfo *)uniform.ids[0].id)->buffer;
				entry.size = WGPU_WHOLE_SIZE;
			} break;
			case UNIFORM_TYPE_TEXTURE_BUFFER:
			case UNIFORM_TYPE_IMAGE_BUFFER: {
				entry.buffer = ((BufferInfo *)uniform.ids[0].id)->buffer;
				entry.size = WGPU_WHOLE_SIZE;
			} break;
			case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER: {
				// ids are [sampler, buffer] pairs; only the buffer exists in
				// the placeholder binding.
				entry.buffer = ((BufferInfo *)uniform.ids[1].id)->buffer;
				entry.size = WGPU_WHOLE_SIZE;
			} break;
			case UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC: {
				BufferInfo *buffer = (BufferInfo *)uniform.ids[0].id;
				ERR_FAIL_COND_V_MSG(!buffer->dynamic, nullptr, "Dynamic uniforms require buffers created with BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT.");
				entry.buffer = buffer->buffer;
				entry.size = buffer->slice_size;
				if (p_bookkeeping != nullptr) {
					p_bookkeeping->dynamic_buffers.push_back(buffer);
				}
			} break;
			case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: {
				ERR_FAIL_COND_V_MSG(uniform.ids.size() != 2, nullptr, "Combined sampler arrays are not supported by WebGPU.");
				// ids are [sampler, texture] pairs; the split remap places the
				// texture at the remapped binding and the sampler right after.
				TextureInfo *combined_texture = (TextureInfo *)uniform.ids[1].id;
				if (combined_texture->wgpu_format == WGPUTextureFormat_Depth24PlusStencil8 || combined_texture->wgpu_format == WGPUTextureFormat_Depth32FloatStencil8) {
					// Combined depth/stencil needs a DepthOnly-aspect view in
					// sampled bindings (the default view has both aspects).
					if (combined_texture->depth_only_view == nullptr) {
						WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
						view_desc.dimension = combined_texture->view_dimension;
						view_desc.aspect = WGPUTextureAspect_DepthOnly;
						combined_texture->depth_only_view = wgpuTextureCreateView(combined_texture->texture, &view_desc);
					}
					entry.textureView = combined_texture->depth_only_view != nullptr ? combined_texture->depth_only_view : combined_texture->view;
				} else {
					entry.textureView = combined_texture->view;
				}
				entries.push_back(entry);
				WGPUBindGroupEntry sampler_entry = WGPU_BIND_GROUP_ENTRY_INIT;
				sampler_entry.binding = remapped_binding + 1;
				sampler_entry.sampler = (WGPUSampler)uniform.ids[0].id;
				entries.push_back(sampler_entry);
				combined_before++;
				continue;
			} break;
			default:
				ERR_FAIL_V_MSG(nullptr, vformat("Unsupported uniform type %d on the WebGPU driver.", uniform.type));
		}
		entries.push_back(entry);
	}

	if (shader->push_constant_size > 0 && p_set_index == RenderingShaderContainerWebGPU::PUSH_CONSTANT_GROUP) {
		WGPUBindGroupEntry pc_entry = WGPU_BIND_GROUP_ENTRY_INIT;
		pc_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
		pc_entry.buffer = push_constant_buffer;
		pc_entry.size = PUSH_CONSTANT_SLOT_SIZE;
		entries.push_back(pc_entry);
		if (p_bookkeeping != nullptr) {
			p_bookkeeping->has_push_constant_offset = true;
		}
	}

	WGPUBindGroupDescriptor bind_group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	bind_group_desc.layout = shader->bind_group_layouts[p_set_index];
	bind_group_desc.entryCount = entries.size();
	bind_group_desc.entries = entries.ptr();
	perf.bind_groups_created++;
	WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device, &bind_group_desc);
	ERR_FAIL_NULL_V_MSG(bind_group, nullptr, vformat("Failed to create a bind group for set %d.", p_set_index));
	return bind_group;
}

RenderingDeviceDriver::UniformSetID RenderingDeviceDriverWebGPU::uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader, uint32_t p_set_index, int p_linear_pool_index) {
	WEBGPU_MAIN_THREAD_GUARD(uniform_set_create(p_uniforms, p_shader, p_set_index, p_linear_pool_index));
	const ShaderInfo *shader = (const ShaderInfo *)p_shader.id;
	ERR_FAIL_COND_V(p_set_index >= shader->bind_group_layouts.size(), UniformSetID());

	UniformSetInfo *uniform_set = memnew(UniformSetInfo);
	uniform_set->set_index = p_set_index;
	uniform_set->uniforms.reserve(p_uniforms.size());
	for (uint32_t i = 0; i < p_uniforms.size(); i++) {
		uniform_set->uniforms.push_back(p_uniforms[i]);
	}
	WGPUBindGroup bind_group = _uniform_set_build(p_uniforms, shader, p_set_index, uniform_set);
	if (bind_group == nullptr) {
		memdelete(uniform_set);
		ERR_FAIL_V(UniformSetID());
	}
	uniform_set->layout_groups.insert((void *)shader->bind_group_layouts[p_set_index], bind_group);
	uniform_set->push_constant_epoch = push_constant_epoch;
	return UniformSetID(uniform_set);
}

void RenderingDeviceDriverWebGPU::uniform_set_free(UniformSetID p_uniform_set) {
	WEBGPU_MAIN_THREAD_GUARD(uniform_set_free(p_uniform_set));
	resource_epoch++; // Invalidates cached render bundles (see BundleCacheEntry).
	UniformSetInfo *uniform_set = (UniformSetInfo *)p_uniform_set.id;
	for (const KeyValue<void *, WGPUBindGroup> &kv : uniform_set->layout_groups) {
		wgpuBindGroupRelease(kv.value);
	}
	memdelete(uniform_set);
}

void RenderingDeviceDriverWebGPU::command_bind_push_constants(CommandBufferID p_cmd_buffer, ShaderID p_shader, uint32_t p_first_index, VectorView<uint32_t> p_data) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	const uint32_t data_size = p_data.size() * sizeof(uint32_t);
	ERR_FAIL_COND(data_size > PUSH_CONSTANT_SLOT_SIZE);
	// Wrapping mid-frame would corrupt earlier draws; drop the draw's
	// constants instead and ask begin_segment to grow the ring for the next
	// frame. Reported once per frame (a big scene can overflow by hundreds
	// of draws - per-draw spam would bury the console) and only at the cap:
	// below it the next frame simply has room.
	if (unlikely(push_constant_used + PUSH_CONSTANT_SLOT_SIZE > push_constant_capacity)) {
		push_constant_grow_pending = true;
		if (!push_constant_overflow_reported && push_constant_capacity >= PUSH_CONSTANT_RING_MAX_SIZE) {
			push_constant_overflow_reported = true;
			ERR_PRINT(vformat("Push constant ring buffer exhausted for this frame (%d slots, the maximum); draws beyond the ring are dropped.", push_constant_capacity / PUSH_CONSTANT_SLOT_SIZE));
		}
		return;
	}
	memcpy(push_constant_shadow + push_constant_used, p_data.ptr(), data_size);
	cb_info->push_constant_offset = push_constant_used;
	push_constant_used += PUSH_CONSTANT_SLOT_SIZE;
	cb_info->push_constant_dirty = true;
}

void RenderingDeviceDriverWebGPU::_flush_bind_groups(CommandBufferInfo *p_cb_info) {
	if (p_cb_info->current_shader == nullptr || (p_cb_info->bind_group_dirty_mask == 0 && !p_cb_info->push_constant_dirty)) {
		return;
	}
	const ShaderInfo *shader = p_cb_info->current_shader;
	for (uint32_t i = 0; i < shader->bind_group_layouts.size() && i < MAX_BIND_GROUPS; i++) {
		UniformSetInfo *set = p_cb_info->pending_bind_groups[i];
		// Skip clean sets: a moved push-constant offset only re-binds sets
		// whose layout actually carries the ring entry.
		const bool wants_push_constant = set != nullptr ? set->has_push_constant_offset : true;
		if ((p_cb_info->bind_group_dirty_mask & (1u << i)) == 0 && !(p_cb_info->push_constant_dirty && wants_push_constant)) {
			continue;
		}
		WGPUBindGroup bind_group = nullptr;
		if (set != nullptr) {
			if (unlikely(set->has_push_constant_offset && set->push_constant_epoch != push_constant_epoch)) {
				// Built against a ring buffer that has since been replaced.
				for (const KeyValue<void *, WGPUBindGroup> &kv : set->layout_groups) {
					wgpuBindGroupRelease(kv.value);
				}
				set->layout_groups.clear();
				set->push_constant_epoch = push_constant_epoch;
			}
			WGPUBindGroup *cached = set->layout_groups.getptr((void *)shader->bind_group_layouts[i]);
			if (cached != nullptr) {
				bind_group = *cached;
			} else {
				bind_group = _uniform_set_build(VectorView<BoundUniform>(set->uniforms.ptr(), set->uniforms.size()), shader, i, nullptr);
				if (bind_group != nullptr) {
					set->layout_groups.insert((void *)shader->bind_group_layouts[i], bind_group);
				}
			}
		} else if (i == 0) {
			if (unlikely(shader->push_constant_bind_group != nullptr && shader->push_constant_bind_group_epoch != push_constant_epoch)) {
				ShaderInfo *mutable_shader = const_cast<ShaderInfo *>(shader);
				wgpuBindGroupRelease(mutable_shader->push_constant_bind_group);
				mutable_shader->push_constant_bind_group = _shader_build_push_constant_bind_group(shader);
				mutable_shader->push_constant_bind_group_epoch = push_constant_epoch;
			}
			bind_group = shader->push_constant_bind_group;
		}
		if (bind_group == nullptr) {
			continue;
		}
		// WebGPU wants dynamic offsets ordered by binding number: dynamic
		// buffers sit at their (low) shader bindings, the push-constant ring
		// entry at the reserved high binding, so it goes last. Fixed stack
		// storage: a set carries at most 8 dynamic buffers (4-bit frame
		// indices packed into the caller's 32-bit mask) plus the ring entry.
		uint32_t offsets[9];
		uint32_t offset_count = 0;
		for (const uint32_t offset : p_cb_info->pending_dynamic_offsets[i]) {
			DEV_ASSERT(offset_count < 8);
			offsets[offset_count++] = offset;
		}
		if (wants_push_constant) {
			offsets[offset_count++] = p_cb_info->push_constant_offset;
		}
		if (p_cb_info->render_pass_encoder != nullptr) {
			if (p_cb_info->render_bundleable) {
				RenderCommandRecord rec;
				memset(&rec, 0, sizeof(rec));
				rec.type = RenderCommandRecord::TYPE_SET_BIND_GROUP;
				rec.set_bind_group.index = i;
				rec.set_bind_group.group = bind_group;
				rec.set_bind_group.offset_count = offset_count;
				for (uint32_t j = 0; j < offset_count; j++) {
					rec.set_bind_group.offsets[j] = offsets[j];
				}
				_record_render_command(p_cb_info, rec);
			} else {
				wgpuRenderPassEncoderSetBindGroup(p_cb_info->render_pass_encoder, i, bind_group, offset_count, offset_count == 0 ? nullptr : offsets);
			}
		} else if (p_cb_info->compute_pass_encoder != nullptr) {
			wgpuComputePassEncoderSetBindGroup(p_cb_info->compute_pass_encoder, i, bind_group, offset_count, offset_count == 0 ? nullptr : offsets);
		}
	}
	p_cb_info->bind_group_dirty_mask = 0;
	p_cb_info->push_constant_dirty = false;
}

void RenderingDeviceDriverWebGPU::_end_compute_pass(CommandBufferInfo *p_cb_info) {
	if (p_cb_info->compute_pass_encoder != nullptr) {
		wgpuComputePassEncoderEnd(p_cb_info->compute_pass_encoder);
		wgpuComputePassEncoderRelease(p_cb_info->compute_pass_encoder);
		p_cb_info->compute_pass_encoder = nullptr;
		p_cb_info->current_shader = nullptr;
		p_cb_info->bind_group_dirty_mask = 0;
	}
}

void RenderingDeviceDriverWebGPU::command_bind_render_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	PipelineInfo *pipeline = (PipelineInfo *)p_pipeline.id;
	if (!_ensure_render_pipeline(pipeline)) {
		return;
	}
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	if (cb_info->render_bundleable) {
		RenderCommandRecord rec;
		memset(&rec, 0, sizeof(rec));
		rec.type = RenderCommandRecord::TYPE_SET_PIPELINE;
		rec.set_pipeline.pipeline = pipeline->render_pipeline;
		_record_render_command(cb_info, rec);
	} else {
		wgpuRenderPassEncoderSetPipeline(cb_info->render_pass_encoder, pipeline->render_pipeline);
	}
	cb_info->current_shader = pipeline->shader;
	cb_info->bind_group_dirty_mask = (1u << MAX_BIND_GROUPS) - 1;
}

void RenderingDeviceDriverWebGPU::command_bind_render_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	uint32_t shift = 0;
	for (uint32_t i = 0; i < p_set_count; i++) {
		const uint32_t set_index = p_first_set_index + i;
		ERR_FAIL_COND(set_index >= MAX_BIND_GROUPS);
		UniformSetInfo *uniform_set = (UniformSetInfo *)p_uniform_sets[i].id;
		cb_info->pending_bind_groups[set_index] = uniform_set;
		cb_info->bind_group_dirty_mask |= uint8_t(1u << set_index);
		cb_info->pending_dynamic_offsets[set_index].clear();
		for (const BufferInfo *dynamic_buffer : uniform_set->dynamic_buffers) {
			const uint32_t frame_idx = (p_dynamic_offsets >> shift) & 0xFu;
			shift += 4u;
			cb_info->pending_dynamic_offsets[set_index].push_back(uint32_t(frame_idx * dynamic_buffer->slice_stride));
		}
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw(CommandBufferID p_cmd_buffer, uint32_t p_vertex_count, uint32_t p_instance_count, uint32_t p_base_vertex, uint32_t p_first_instance) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	_flush_bind_groups(cb_info);
	if (cb_info->render_bundleable) {
		RenderCommandRecord rec;
		memset(&rec, 0, sizeof(rec));
		rec.type = RenderCommandRecord::TYPE_DRAW;
		rec.draw.param_index = cb_info->render_draw_params.size();
		DrawParams params;
		params.args[0] = p_vertex_count;
		params.args[1] = p_instance_count;
		params.args[2] = p_base_vertex;
		params.args[3] = p_first_instance;
		cb_info->render_draw_params.push_back(params);
		_record_render_command(cb_info, rec);
		return;
	}
	wgpuRenderPassEncoderDraw(cb_info->render_pass_encoder, p_vertex_count, p_instance_count, p_base_vertex, p_first_instance);
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	_flush_bind_groups(cb_info);
	if (cb_info->render_bundleable) {
		RenderCommandRecord rec;
		memset(&rec, 0, sizeof(rec));
		rec.type = RenderCommandRecord::TYPE_DRAW_INDEXED;
		rec.draw_indexed.param_index = cb_info->render_draw_params.size();
		DrawParams params;
		params.args[0] = p_index_count;
		params.args[1] = p_instance_count;
		params.args[2] = p_first_index;
		params.args[3] = (uint32_t)p_vertex_offset;
		params.args[4] = p_first_instance;
		cb_info->render_draw_params.push_back(params);
		_record_render_command(cb_info, rec);
		return;
	}
	wgpuRenderPassEncoderDrawIndexed(cb_info->render_pass_encoder, p_index_count, p_instance_count, p_first_index, p_vertex_offset, p_first_instance);
}

void RenderingDeviceDriverWebGPU::command_render_draw_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	_flush_bind_groups(cb_info);
	for (uint32_t i = 0; i < p_draw_count; i++) {
		if (cb_info->render_bundleable) {
			RenderCommandRecord rec;
			memset(&rec, 0, sizeof(rec));
			rec.type = RenderCommandRecord::TYPE_DRAW_INDIRECT;
			rec.draw_indirect.buffer = ((BufferInfo *)p_indirect_buffer.id)->buffer;
			rec.draw_indirect.offset = p_offset + i * p_stride;
			_record_render_command(cb_info, rec);
		} else {
			wgpuRenderPassEncoderDrawIndirect(cb_info->render_pass_encoder, ((BufferInfo *)p_indirect_buffer.id)->buffer, p_offset + i * p_stride);
		}
	}
}

void RenderingDeviceDriverWebGPU::command_render_bind_vertex_buffers(CommandBufferID p_cmd_buffer, uint32_t p_binding_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	uint32_t shift = 0;
	for (uint32_t i = 0; i < p_binding_count; i++) {
		const BufferInfo *buffer = (const BufferInfo *)p_buffers[i].id;
		uint64_t offset = p_offsets[i];
		if (buffer->dynamic) {
			// Two bits per dynamic buffer (see buffer_get_dynamic_offsets).
			const uint32_t frame_idx = (p_dynamic_offsets >> shift) & 0x3u;
			shift += 2u;
			offset += frame_idx * buffer->slice_stride;
		}
		if (cb_info->render_bundleable) {
			RenderCommandRecord rec;
			memset(&rec, 0, sizeof(rec));
			rec.type = RenderCommandRecord::TYPE_SET_VERTEX_BUFFER;
			rec.set_vertex_buffer.slot = i;
			rec.set_vertex_buffer.buffer = buffer->buffer;
			rec.set_vertex_buffer.offset = offset;
			_record_render_command(cb_info, rec);
		} else {
			wgpuRenderPassEncoderSetVertexBuffer(cb_info->render_pass_encoder, i, buffer->buffer, offset, WGPU_WHOLE_SIZE);
		}
	}
}

void RenderingDeviceDriverWebGPU::command_render_bind_index_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, IndexBufferFormat p_format, uint64_t p_offset) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	const WGPUIndexFormat format = p_format == INDEX_BUFFER_FORMAT_UINT16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
	if (cb_info->render_bundleable) {
		RenderCommandRecord rec;
		memset(&rec, 0, sizeof(rec));
		rec.type = RenderCommandRecord::TYPE_SET_INDEX_BUFFER;
		rec.set_index_buffer.buffer = ((BufferInfo *)p_buffer.id)->buffer;
		rec.set_index_buffer.format = format;
		rec.set_index_buffer.offset = p_offset;
		_record_render_command(cb_info, rec);
		return;
	}
	wgpuRenderPassEncoderSetIndexBuffer(cb_info->render_pass_encoder, ((BufferInfo *)p_buffer.id)->buffer, format, p_offset, WGPU_WHOLE_SIZE);
}

void RenderingDeviceDriverWebGPU::command_render_set_viewport(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_viewports) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	ERR_FAIL_COND(p_viewports.size() == 0);
	if (cb_info->render_bundleable && cb_info->render_records.size() > 0) {
		// Mid-pass viewport changes cannot live inside a bundle.
		_render_pass_flush_records(cb_info);
	}
	const Rect2i &viewport = p_viewports[0];
	wgpuRenderPassEncoderSetViewport(cb_info->render_pass_encoder, viewport.position.x, viewport.position.y, viewport.size.width, viewport.size.height, 0.0f, 1.0f);
}

void RenderingDeviceDriverWebGPU::command_render_set_scissor(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_scissors) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	ERR_FAIL_COND(p_scissors.size() == 0);
	if (cb_info->render_bundleable && cb_info->render_records.size() > 0) {
		// Mid-pass scissor changes (2D canvas clips) cannot live in a bundle.
		_render_pass_flush_records(cb_info);
	}
	const Rect2i &scissor = p_scissors[0];
	wgpuRenderPassEncoderSetScissorRect(cb_info->render_pass_encoder, scissor.position.x, scissor.position.y, scissor.size.width, scissor.size.height);
}

void RenderingDeviceDriverWebGPU::command_render_set_blend_constants(CommandBufferID p_cmd_buffer, const Color &p_constants) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	if (cb_info->render_bundleable && cb_info->render_records.size() > 0) {
		// Blend constants are pass-encoder state; a bundle cannot set them.
		_render_pass_flush_records(cb_info);
	}
	WGPUColor color = { p_constants.r, p_constants.g, p_constants.b, p_constants.a };
	wgpuRenderPassEncoderSetBlendConstant(cb_info->render_pass_encoder, &color);
}

// Specialization constants become override values keyed by @id. Only ids
// present in the shader's reflection are supplied: WebGPU rejects constants
// that do not exist in the module.
static void _specialization_constants_to_wgpu(const HashSet<uint32_t> &p_module_ids, VectorView<RenderingDeviceCommons::PipelineSpecializationConstant> p_constants, LocalVector<CharString> &r_keys, LocalVector<WGPUConstantEntry> &r_entries) {
	for (uint32_t i = 0; i < p_constants.size(); i++) {
		const RenderingDeviceCommons::PipelineSpecializationConstant &constant = p_constants[i];
		if (!p_module_ids.has(constant.constant_id)) {
			continue;
		}
		r_keys.push_back(String::num_uint64(constant.constant_id).utf8());
		WGPUConstantEntry entry = WGPU_CONSTANT_ENTRY_INIT;
		entry.key = { r_keys[r_keys.size() - 1].get_data(), WGPU_STRLEN };
		switch (constant.type) {
			case RenderingDeviceCommons::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_FLOAT:
				entry.value = constant.float_value;
				break;
			case RenderingDeviceCommons::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT:
				entry.value = constant.int_value;
				break;
			default:
				entry.value = constant.bool_value ? 1.0 : 0.0;
				break;
		}
		r_entries.push_back(entry);
	}
}

// Builds the pipeline object. Split out from render_pipeline_create so the
// work can happen at first bind instead of at creation.
WGPURenderPipeline RenderingDeviceDriverWebGPU::_build_render_pipeline(const ShaderInfo *shader, VertexFormatID p_vertex_format, RenderPrimitive p_render_primitive, PipelineRasterizationState p_rasterization_state, PipelineMultisampleState p_multisample_state, PipelineDepthStencilState p_depth_stencil_state, PipelineColorBlendState p_blend_state, VectorView<int32_t> p_color_attachments, BitField<PipelineDynamicStateFlags> p_dynamic_state, RenderPassID p_render_pass, uint32_t p_render_subpass, VectorView<PipelineSpecializationConstant> p_specialization_constants, PipelineAsyncTicket *p_async_ticket) {
	const RenderPassInfo *pass = (const RenderPassInfo *)p_render_pass.id;

	WGPUPrimitiveTopology topology;
	switch (p_render_primitive) {
		case RENDER_PRIMITIVE_POINTS:
			topology = WGPUPrimitiveTopology_PointList;
			break;
		case RENDER_PRIMITIVE_LINES:
			topology = WGPUPrimitiveTopology_LineList;
			break;
		case RENDER_PRIMITIVE_LINESTRIPS:
			topology = WGPUPrimitiveTopology_LineStrip;
			break;
		case RENDER_PRIMITIVE_TRIANGLES:
			topology = WGPUPrimitiveTopology_TriangleList;
			break;
		case RENDER_PRIMITIVE_TRIANGLE_STRIPS:
		case RENDER_PRIMITIVE_TRIANGLE_STRIPS_WITH_RESTART_INDEX:
			topology = WGPUPrimitiveTopology_TriangleStrip;
			break;
		default:
			ERR_FAIL_V_MSG(nullptr, vformat("Unsupported render primitive %d on WebGPU.", p_render_primitive));
	}

	LocalVector<CharString> vertex_constant_keys;
	LocalVector<WGPUConstantEntry> vertex_constants;
	LocalVector<CharString> fragment_constant_keys;
	LocalVector<WGPUConstantEntry> fragment_constants;

	WGPUVertexState vertex_state = WGPU_VERTEX_STATE_INIT;
	WGPUFragmentState fragment_state = WGPU_FRAGMENT_STATE_INIT;
	bool has_fragment = false;
	for (uint32_t i = 0; i < shader->module_stages.size(); i++) {
		if (shader->module_stages[i] == SHADER_STAGE_VERTEX) {
			vertex_state.module = shader->modules[i];
			if (i < shader->module_override_ids.size()) {
				_specialization_constants_to_wgpu(shader->module_override_ids[i], p_specialization_constants, vertex_constant_keys, vertex_constants);
			}
		} else if (shader->module_stages[i] == SHADER_STAGE_FRAGMENT) {
			fragment_state.module = shader->modules[i];
			if (i < shader->module_override_ids.size()) {
				_specialization_constants_to_wgpu(shader->module_override_ids[i], p_specialization_constants, fragment_constant_keys, fragment_constants);
			}
			has_fragment = true;
		}
	}
	ERR_FAIL_NULL_V_MSG(vertex_state.module, nullptr, "Render pipelines require a vertex stage.");
	vertex_state.entryPoint = { SHADER_ENTRY_POINT, WGPU_STRLEN };
	vertex_state.constantCount = vertex_constants.size();
	vertex_state.constants = vertex_constants.ptr();
	const VertexFormatInfo *vertex_format = (const VertexFormatInfo *)p_vertex_format.id;
	if (vertex_format != nullptr) {
		vertex_state.bufferCount = vertex_format->buffer_layouts.size();
		vertex_state.buffers = vertex_format->buffer_layouts.ptr();
	}

	// Fragment targets come from the render pass's color attachments, blend
	// state from the pipeline description.
	LocalVector<WGPUColorTargetState> targets;
	LocalVector<WGPUBlendState> blend_states;
	blend_states.resize(p_blend_state.attachments.size());
	const WGPUDepthStencilState *depth_stencil_ptr = nullptr;
	WGPUDepthStencilState depth_stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
	for (uint32_t i = 0; i < pass->attachments.size(); i++) {
		if (pass->attachments[i].is_resolve_target) {
			continue;
		}
		if (pass->attachments[i].is_depth_stencil) {
			depth_stencil.format = pass->attachments[i].format;
			depth_stencil.depthWriteEnabled = p_depth_stencil_state.enable_depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
			depth_stencil.depthCompare = p_depth_stencil_state.enable_depth_test ? _compare_op_to_wgpu(p_depth_stencil_state.depth_compare_operator) : WGPUCompareFunction_Always;
			if (p_depth_stencil_state.enable_stencil) {
				depth_stencil.stencilFront.compare = _compare_op_to_wgpu(p_depth_stencil_state.front_op.compare);
				depth_stencil.stencilFront.failOp = _stencil_op_to_wgpu(p_depth_stencil_state.front_op.fail);
				depth_stencil.stencilFront.depthFailOp = _stencil_op_to_wgpu(p_depth_stencil_state.front_op.depth_fail);
				depth_stencil.stencilFront.passOp = _stencil_op_to_wgpu(p_depth_stencil_state.front_op.pass);
				depth_stencil.stencilBack.compare = _compare_op_to_wgpu(p_depth_stencil_state.back_op.compare);
				depth_stencil.stencilBack.failOp = _stencil_op_to_wgpu(p_depth_stencil_state.back_op.fail);
				depth_stencil.stencilBack.depthFailOp = _stencil_op_to_wgpu(p_depth_stencil_state.back_op.depth_fail);
				depth_stencil.stencilBack.passOp = _stencil_op_to_wgpu(p_depth_stencil_state.back_op.pass);
				depth_stencil.stencilReadMask = p_depth_stencil_state.front_op.compare_mask;
				depth_stencil.stencilWriteMask = p_depth_stencil_state.front_op.write_mask;
			}
			if (p_rasterization_state.depth_bias_enabled) {
				depth_stencil.depthBias = (int32_t)p_rasterization_state.depth_bias_constant_factor;
				depth_stencil.depthBiasSlopeScale = p_rasterization_state.depth_bias_slope_factor;
				depth_stencil.depthBiasClamp = p_rasterization_state.depth_bias_clamp;
			}
			depth_stencil_ptr = &depth_stencil;
			continue;
		}
		WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
		target.format = pass->attachments[i].format;
		const uint32_t blend_index = targets.size();
		if (blend_index < p_blend_state.attachments.size()) {
			const PipelineColorBlendState::Attachment &blend = p_blend_state.attachments[blend_index];
			target.writeMask = (blend.write_r ? WGPUColorWriteMask_Red : WGPUColorWriteMask_None) | (blend.write_g ? WGPUColorWriteMask_Green : WGPUColorWriteMask_None) | (blend.write_b ? WGPUColorWriteMask_Blue : WGPUColorWriteMask_None) | (blend.write_a ? WGPUColorWriteMask_Alpha : WGPUColorWriteMask_None);
			if (blend.enable_blend) {
				WGPUBlendState &blend_state = blend_states[blend_index];
				blend_state.color.srcFactor = _blend_factor_to_wgpu(blend.src_color_blend_factor);
				blend_state.color.dstFactor = _blend_factor_to_wgpu(blend.dst_color_blend_factor);
				blend_state.color.operation = _blend_op_to_wgpu(blend.color_blend_op);
				blend_state.alpha.srcFactor = _blend_factor_to_wgpu(blend.src_alpha_blend_factor);
				blend_state.alpha.dstFactor = _blend_factor_to_wgpu(blend.dst_alpha_blend_factor);
				blend_state.alpha.operation = _blend_op_to_wgpu(blend.alpha_blend_op);
				target.blend = &blend_state;
			}
		}
		if ((shader->fragment_output_mask & (1u << blend_index)) == 0 || pass->attachment_less) {
			// No fragment output feeds this target (or the target is the
			// synthetic one of an attachment-less pass); a nonzero write mask
			// is a validation error on WebGPU.
			target.writeMask = WGPUColorWriteMask_None;
		}
		targets.push_back(target);
	}

	WGPURenderPipelineDescriptor pipeline_desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	pipeline_desc.layout = shader->pipeline_layout;
	pipeline_desc.vertex = vertex_state;
	pipeline_desc.primitive.topology = topology;
	pipeline_desc.primitive.frontFace = p_rasterization_state.front_face == POLYGON_FRONT_FACE_CLOCKWISE ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
	pipeline_desc.primitive.cullMode = p_rasterization_state.cull_mode == POLYGON_CULL_FRONT ? WGPUCullMode_Front : (p_rasterization_state.cull_mode == POLYGON_CULL_BACK ? WGPUCullMode_Back : WGPUCullMode_None);
	// Depth clamping keeps geometry that crosses the near or far plane
	// rasterizing over its full screen extent instead of being clipped
	// against it. The clustered builder rasterizes light, decal and probe
	// proxy volumes and depends on it: without clamping, every proxy the
	// camera sits inside loses the part in front of the near plane, so those
	// elements go unrecorded in the clusters that plane crosses and their
	// contribution ends at a straight line on screen.
	pipeline_desc.primitive.unclippedDepth = p_rasterization_state.enable_depth_clamp && device_has_depth_clip_control;
	pipeline_desc.multisample.count = p_multisample_state.sample_count == TEXTURE_SAMPLES_4 ? 4 : 1;
	pipeline_desc.multisample.mask = p_multisample_state.sample_mask.is_empty() ? 0xFFFFFFFF : p_multisample_state.sample_mask[0];
	pipeline_desc.multisample.alphaToCoverageEnabled = p_multisample_state.enable_alpha_to_coverage;
	pipeline_desc.depthStencil = depth_stencil_ptr;
	if (has_fragment) {
		fragment_state.entryPoint = { SHADER_ENTRY_POINT, WGPU_STRLEN };
		fragment_state.constantCount = fragment_constants.size();
		fragment_state.constants = fragment_constants.ptr();
		fragment_state.targetCount = targets.size();
		fragment_state.targets = targets.ptr();
		pipeline_desc.fragment = &fragment_state;
	}

	if (p_async_ticket != nullptr) {
		// Background warm: the descriptor is fully serialized during the
		// call, so the stack-owned state above needs no extended lifetime.
		WGPUCreateRenderPipelineAsyncCallbackInfo callback_info = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
		callback_info.mode = WGPUCallbackMode_AllowSpontaneous;
		callback_info.callback = _on_render_pipeline_async;
		callback_info.userdata1 = p_async_ticket;
		p_async_ticket->kicked = true;
		wgpuDeviceCreateRenderPipelineAsync(device, &pipeline_desc, callback_info);
		return nullptr;
	}

	perf.pipelines_created++;
	WGPURenderPipeline render_pipeline = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);
	ERR_FAIL_NULL_V_MSG(render_pipeline, nullptr, "Failed to create a WebGPU render pipeline.");

	return render_pipeline;
}

RenderingDeviceDriver::PipelineID RenderingDeviceDriverWebGPU::render_pipeline_create(ShaderID p_shader, VertexFormatID p_vertex_format, RenderPrimitive p_render_primitive, PipelineRasterizationState p_rasterization_state, PipelineMultisampleState p_multisample_state, PipelineDepthStencilState p_depth_stencil_state, PipelineColorBlendState p_blend_state, VectorView<int32_t> p_color_attachments, BitField<PipelineDynamicStateFlags> p_dynamic_state, RenderPassID p_render_pass, uint32_t p_render_subpass, VectorView<PipelineSpecializationConstant> p_specialization_constants) {
	WEBGPU_MAIN_THREAD_GUARD(render_pipeline_create(p_shader, p_vertex_format, p_render_primitive, p_rasterization_state, p_multisample_state, p_depth_stencil_state, p_blend_state, p_color_attachments, p_dynamic_state, p_render_pass, p_render_subpass, p_specialization_constants));

	// Deferred to first bind, like compute pipelines: creating one is what
	// makes the browser compile the shader, and about a third of the render
	// pipelines a scene builds are never drawn with. The render pass and
	// vertex format are referenced by id, which the rendering device keeps
	// for the lifetime of the device in its format caches.
	PipelineInfo *pipeline = memnew(PipelineInfo);
	pipeline->shader = (const ShaderInfo *)p_shader.id;
	pipeline->render_pending = true;
	pipeline->render_vertex_format = p_vertex_format;
	pipeline->render_primitive = p_render_primitive;
	pipeline->render_rasterization = p_rasterization_state;
	pipeline->render_multisample = p_multisample_state;
	pipeline->render_depth_stencil = p_depth_stencil_state;
	pipeline->render_blend = p_blend_state;
	pipeline->render_dynamic_state = p_dynamic_state;
	pipeline->render_pass = p_render_pass;
	pipeline->render_subpass = p_render_subpass;
	pipeline->render_color_attachments.resize(p_color_attachments.size());
	for (uint32_t i = 0; i < p_color_attachments.size(); i++) {
		pipeline->render_color_attachments[i] = p_color_attachments[i];
	}
	pipeline->render_constants.resize(p_specialization_constants.size());
	for (uint32_t i = 0; i < p_specialization_constants.size(); i++) {
		pipeline->render_constants[i] = p_specialization_constants[i];
	}
	pipeline_warm_queue.push_back(pipeline);
	return PipelineID(pipeline);
}

void RenderingDeviceDriverWebGPU::_on_render_pipeline_async(WGPUCreatePipelineAsyncStatus p_status, WGPURenderPipeline p_pipeline, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	if (g_warm_inflight > 0) {
		g_warm_inflight--;
	}
	PipelineAsyncTicket *ticket = (PipelineAsyncTicket *)p_userdata1;
	PipelineInfo *pipeline = ticket->pipeline;
	const bool succeeded = p_status == WGPUCreatePipelineAsyncStatus_Success && p_pipeline != nullptr;
	if (pipeline == nullptr || pipeline->render_pipeline != nullptr || pipeline->render_failed) {
		// The owner was freed, or the bind-time sync path resolved it first.
		if (succeeded) {
			wgpuRenderPipelineRelease(p_pipeline);
		}
	} else if (succeeded) {
		pipeline->render_pipeline = p_pipeline;
		pipeline->render_pending = false;
	}
	// An async failure is deliberately not latched: if anything ever binds
	// this pipeline, the sync path reports the error once with full context.
	if (pipeline != nullptr) {
		pipeline->async_ticket = nullptr;
	}
	memdelete(ticket);
}

void RenderingDeviceDriverWebGPU::_on_compute_pipeline_async(WGPUCreatePipelineAsyncStatus p_status, WGPUComputePipeline p_pipeline, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	if (g_warm_inflight > 0) {
		g_warm_inflight--;
	}
	PipelineAsyncTicket *ticket = (PipelineAsyncTicket *)p_userdata1;
	PipelineInfo *pipeline = ticket->pipeline;
	const bool succeeded = p_status == WGPUCreatePipelineAsyncStatus_Success && p_pipeline != nullptr;
	if (pipeline == nullptr || pipeline->compute_pipeline != nullptr || pipeline->compute_failed) {
		if (succeeded) {
			wgpuComputePipelineRelease(p_pipeline);
		}
	} else if (succeeded) {
		pipeline->compute_pipeline = p_pipeline;
		pipeline->compute_pending = false;
	}
	if (pipeline != nullptr) {
		pipeline->async_ticket = nullptr;
	}
	memdelete(ticket);
}

void RenderingDeviceDriverWebGPU::_pump_pipeline_warm_queue() {
	// Deferred pipelines (see render/compute_pipeline_create) are handed to
	// the browser for BACKGROUND compilation a few per presented frame: the
	// bind-time sync create stays the correctness fence, and by then the
	// browser's pipeline cache has usually already done the work. Throttling
	// keeps the warm from re-creating the create-burst that saturates the
	// Dawn wire, which is what the deferral fixed in the first place.
	if (!use_pipeline_warm) {
		godot_js_webgpu_report_warm_pending(0);
		return;
	}
	// Drain hard while the app is young - the load/overlay window, where a
	// stall-free first interaction matters more than main-thread headroom
	// (async creates do not block the Dawn wire the way the old eager sync
	// creation burst did) - then drop to a trickle.
	const uint32_t warm_per_frame = present_count < 900 ? 16u : 2u;
	uint32_t kicked = 0;
	while (kicked < warm_per_frame && pipeline_warm_queue.size() > 0) {
		PipelineInfo *pipeline = pipeline_warm_queue[0];
		pipeline_warm_queue.remove_at(0);
		if (pipeline->async_ticket != nullptr || pipeline->shader == nullptr || pipeline->shader->modules.is_empty()) {
			continue;
		}
		const bool warm_render = pipeline->render_pending && pipeline->render_pipeline == nullptr && !pipeline->render_failed;
		const bool warm_compute = !warm_render && pipeline->compute_pending && pipeline->compute_pipeline == nullptr && !pipeline->compute_failed;
		if (warm_render) {
			// The engine also creates render pipelines around null shader
			// variants and never binds them; warming those would surface the
			// vertex-stage error the deferral keeps silent. The warm stays
			// quiet - the bind-time path is the designated reporter.
			bool has_vertex = false;
			for (uint32_t i = 0; i < pipeline->shader->module_stages.size(); i++) {
				if (pipeline->shader->module_stages[i] == SHADER_STAGE_VERTEX) {
					has_vertex = true;
					break;
				}
			}
			if (!has_vertex) {
				continue;
			}
		} else if (!warm_compute) {
			continue;
		}
		PipelineAsyncTicket *ticket = memnew(PipelineAsyncTicket);
		ticket->pipeline = pipeline;
		pipeline->async_ticket = ticket;
		if (warm_render) {
			_build_render_pipeline(pipeline->shader, pipeline->render_vertex_format,
					pipeline->render_primitive, pipeline->render_rasterization, pipeline->render_multisample,
					pipeline->render_depth_stencil, pipeline->render_blend,
					VectorView<int32_t>(pipeline->render_color_attachments.ptr(), pipeline->render_color_attachments.size()),
					pipeline->render_dynamic_state, pipeline->render_pass, pipeline->render_subpass,
					VectorView<PipelineSpecializationConstant>(pipeline->render_constants.ptr(), pipeline->render_constants.size()), ticket);
		} else {
			_create_compute_pipeline(pipeline->shader, VectorView<PipelineSpecializationConstant>(pipeline->compute_constants.ptr(), pipeline->compute_constants.size()), ticket);
		}
		if (!ticket->kicked) {
			// The build bailed before reaching the API; reclaim the ticket.
			pipeline->async_ticket = nullptr;
			memdelete(ticket);
			continue;
		}
		g_warm_inflight++;
		kicked++;
	}
	// Publish the backlog only when it changes (one boundary crossing).
	static int last_reported_pending = -1;
	const int pending = (int)(pipeline_warm_queue.size() + g_warm_inflight);
	if (pending != last_reported_pending) {
		last_reported_pending = pending;
		godot_js_webgpu_report_warm_pending(pending);
	}
}

bool RenderingDeviceDriverWebGPU::_ensure_render_pipeline(PipelineInfo *p_pipeline) {
	if (p_pipeline->render_pipeline != nullptr) {
		return true;
	}
	if (p_pipeline->render_failed) {
		return false; // Reported once already; do not retry every bind.
	}
	ERR_FAIL_NULL_V(p_pipeline->shader, false);

	p_pipeline->render_pipeline = _build_render_pipeline(p_pipeline->shader, p_pipeline->render_vertex_format,
			p_pipeline->render_primitive, p_pipeline->render_rasterization, p_pipeline->render_multisample,
			p_pipeline->render_depth_stencil, p_pipeline->render_blend,
			VectorView<int32_t>(p_pipeline->render_color_attachments.ptr(), p_pipeline->render_color_attachments.size()),
			p_pipeline->render_dynamic_state, p_pipeline->render_pass, p_pipeline->render_subpass,
			VectorView<PipelineSpecializationConstant>(p_pipeline->render_constants.ptr(), p_pipeline->render_constants.size()));
	if (p_pipeline->render_pipeline == nullptr) {
		p_pipeline->render_failed = true;
		return false;
	}
	p_pipeline->render_pending = false;
	return true;
}

RenderingDeviceDriver::PipelineID RenderingDeviceDriverWebGPU::compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants) {
	WEBGPU_MAIN_THREAD_GUARD(compute_pipeline_create(p_shader, p_specialization_constants));
	const ShaderInfo *shader = (const ShaderInfo *)p_shader.id;
	ERR_FAIL_COND_V(shader->modules.is_empty(), PipelineID());

	// Deferred until the pipeline is first bound: creating it is what makes
	// the browser compile the shader for the native backend, and the renderer
	// builds a pipeline for every effect it supports whether or not the scene
	// dispatches it. Nothing is given up - the pipeline is built the moment
	// something actually uses it.
	PipelineInfo *pipeline = memnew(PipelineInfo);
	pipeline->shader = shader;
	pipeline->compute_pending = true;
	pipeline->compute_constants.resize(p_specialization_constants.size());
	for (uint32_t i = 0; i < p_specialization_constants.size(); i++) {
		pipeline->compute_constants[i] = p_specialization_constants[i];
	}
	pipeline_warm_queue.push_back(pipeline);
	return PipelineID(pipeline);
}

WGPUComputePipeline RenderingDeviceDriverWebGPU::_create_compute_pipeline(const ShaderInfo *p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants, PipelineAsyncTicket *p_async_ticket) {
	LocalVector<CharString> constant_keys;
	LocalVector<WGPUConstantEntry> constants;
	if (p_shader->module_override_ids.size() > 0) {
		_specialization_constants_to_wgpu(p_shader->module_override_ids[0], p_specialization_constants, constant_keys, constants);
	}

	WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
	pipeline_desc.label = { p_shader->name.get_data(), WGPU_STRLEN };
	pipeline_desc.layout = p_shader->pipeline_layout;
	pipeline_desc.compute.module = p_shader->modules[0];
	pipeline_desc.compute.entryPoint = { SHADER_ENTRY_POINT, WGPU_STRLEN };
	pipeline_desc.compute.constantCount = constants.size();
	pipeline_desc.compute.constants = constants.ptr();

	if (p_async_ticket != nullptr) {
		// Background warm: the descriptor is serialized during the call.
		WGPUCreateComputePipelineAsyncCallbackInfo callback_info = WGPU_CREATE_COMPUTE_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
		callback_info.mode = WGPUCallbackMode_AllowSpontaneous;
		callback_info.callback = _on_compute_pipeline_async;
		callback_info.userdata1 = p_async_ticket;
		p_async_ticket->kicked = true;
		wgpuDeviceCreateComputePipelineAsync(device, &pipeline_desc, callback_info);
		return nullptr;
	}

	perf.pipelines_created++;
	return wgpuDeviceCreateComputePipeline(device, &pipeline_desc);
}

bool RenderingDeviceDriverWebGPU::_ensure_compute_pipeline(PipelineInfo *p_pipeline) {
	if (p_pipeline->compute_pipeline != nullptr) {
		return true;
	}
	if (p_pipeline->compute_failed) {
		return false; // Reported once already; do not retry every bind.
	}

	const ShaderInfo *shader = p_pipeline->shader;
	ERR_FAIL_NULL_V(shader, false);
	if (shader->modules.is_empty()) {
		p_pipeline->compute_failed = true;
		ERR_FAIL_V_MSG(false, "Compute pipeline has no shader modules.");
	}

	p_pipeline->compute_pipeline = _create_compute_pipeline(shader, VectorView<PipelineSpecializationConstant>(p_pipeline->compute_constants.ptr(), p_pipeline->compute_constants.size()));
	if (p_pipeline->compute_pipeline == nullptr) {
		p_pipeline->compute_failed = true;
		ERR_FAIL_V_MSG(false, "Failed to create a WebGPU compute pipeline.");
	}
	p_pipeline->compute_pending = false;
	return true;
}

void RenderingDeviceDriverWebGPU::pipeline_free(PipelineID p_pipeline) {
	WEBGPU_MAIN_THREAD_GUARD(pipeline_free(p_pipeline));
	resource_epoch++; // Invalidates cached render bundles (see BundleCacheEntry).
	PipelineInfo *pipeline = (PipelineInfo *)p_pipeline.id;
	if (pipeline->async_ticket != nullptr) {
		// A background compile is in flight: orphan the ticket so its
		// callback releases the result instead of touching freed memory.
		pipeline->async_ticket->pipeline = nullptr;
		pipeline->async_ticket = nullptr;
	}
	// Freed before it was warmed: the pump validates entries, but removing
	// here keeps the queue from holding a dangling pointer.
	pipeline_warm_queue.erase(pipeline);
	if (pipeline->render_pipeline != nullptr) {
		wgpuRenderPipelineRelease(pipeline->render_pipeline);
	}
	if (pipeline->compute_pipeline != nullptr) {
		wgpuComputePipelineRelease(pipeline->compute_pipeline);
	}
	memdelete(pipeline);
}

void RenderingDeviceDriverWebGPU::command_bind_compute_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	PipelineInfo *pipeline = (PipelineInfo *)p_pipeline.id;
	ERR_FAIL_NULL(cb_info->encoder);
	if (cb_info->compute_pass_encoder == nullptr) {
		// The engine has no compute pass begin/end; open one lazily and close
		// it before any encoder-level command (copies, render passes, end).
		cb_info->compute_pass_encoder = wgpuCommandEncoderBeginComputePass(cb_info->encoder, nullptr);
		ERR_FAIL_NULL(cb_info->compute_pass_encoder);
	}
	if (!_ensure_compute_pipeline(pipeline)) {
		return;
	}
	wgpuComputePassEncoderSetPipeline(cb_info->compute_pass_encoder, pipeline->compute_pipeline);
	cb_info->current_shader = pipeline->shader;
	cb_info->bind_group_dirty_mask = (1u << MAX_BIND_GROUPS) - 1;
}

void RenderingDeviceDriverWebGPU::command_bind_compute_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) {
	command_bind_render_uniform_sets(p_cmd_buffer, p_uniform_sets, p_shader, p_first_set_index, p_set_count, p_dynamic_offsets);
}

void RenderingDeviceDriverWebGPU::command_compute_dispatch(CommandBufferID p_cmd_buffer, uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->compute_pass_encoder);
	_flush_bind_groups(cb_info);
	wgpuComputePassEncoderDispatchWorkgroups(cb_info->compute_pass_encoder, p_x_groups, p_y_groups, p_z_groups);
}

void RenderingDeviceDriverWebGPU::command_compute_dispatch_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->compute_pass_encoder);
	_flush_bind_groups(cb_info);
	wgpuComputePassEncoderDispatchWorkgroupsIndirect(cb_info->compute_pass_encoder, ((BufferInfo *)p_indirect_buffer.id)->buffer, p_offset);
}

uint64_t RenderingDeviceDriverWebGPU::api_trait_get(ApiTrait p_trait) {
	switch (p_trait) {
		case API_TRAIT_HONORS_PIPELINE_BARRIERS:
			// WebGPU tracks resource hazards internally.
			return 0;
		case API_TRAIT_TEXTURE_DATA_ROW_PITCH_STEP:
			// WebGPU requires bytesPerRow % 256 for multi-row copies; asking
			// the engine to pack staging at that stride makes every upload a
			// single direct copy (no per-row repack) - the D3D12 pattern.
			return 256;
		case API_TRAIT_TEXTURE_TRANSFER_ALIGNMENT:
			// Copy offsets must be a multiple of the texel block byte size
			// (16 for the widest BC blocks); 256 keeps them row-aligned too.
			return 256;
		default:
			return RenderingDeviceDriver::api_trait_get(p_trait);
	}
}

/**********************/
/**** SYNCHRONIZATION */
/**********************/

RenderingDeviceDriver::FenceID RenderingDeviceDriverWebGPU::fence_create() {
	return FenceID(TOKEN_ID);
}

Error RenderingDeviceDriverWebGPU::fence_wait(FenceID p_fence) {
	// Blocking on the browser's main thread can never make progress; WebGPU
	// submission order provides the ordering the engine needs so far.
	return OK;
}

void RenderingDeviceDriverWebGPU::fence_free(FenceID p_fence) {
}

RenderingDeviceDriver::SemaphoreID RenderingDeviceDriverWebGPU::semaphore_create() {
	return SemaphoreID(TOKEN_ID);
}

void RenderingDeviceDriverWebGPU::semaphore_free(SemaphoreID p_semaphore) {
}

/**************/
/**** MISC ****/
/**************/

bool RenderingDeviceDriverWebGPU::_push_constant_ring_allocate(uint32_t p_capacity) {
	WGPUBufferDescriptor pc_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	pc_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
	pc_desc.size = p_capacity;
	WGPUBuffer buffer = wgpuDeviceCreateBuffer(device, &pc_desc);
	ERR_FAIL_NULL_V(buffer, false);
	if (push_constant_buffer != nullptr) {
		// Frames already submitted keep the browser-side object alive; only
		// our handle goes.
		wgpuBufferRelease(push_constant_buffer);
	}
	push_constant_buffer = buffer;
	push_constant_shadow = (uint8_t *)memrealloc(push_constant_shadow, p_capacity);
	push_constant_capacity = p_capacity;
	return true;
}

WGPUBindGroup RenderingDeviceDriverWebGPU::_shader_build_push_constant_bind_group(const ShaderInfo *p_shader) {
	WGPUBindGroupEntry pc_group_entry = WGPU_BIND_GROUP_ENTRY_INIT;
	pc_group_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
	pc_group_entry.buffer = push_constant_buffer;
	pc_group_entry.size = PUSH_CONSTANT_SLOT_SIZE;
	WGPUBindGroupDescriptor pc_group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	pc_group_desc.layout = p_shader->bind_group_layouts[0];
	pc_group_desc.entryCount = 1;
	pc_group_desc.entries = &pc_group_entry;
	return wgpuDeviceCreateBindGroup(device, &pc_group_desc);
}

void RenderingDeviceDriverWebGPU::begin_segment(uint32_t p_frame_index, uint32_t p_frames_drawn) {
	// Safe to recycle every frame: wgpuQueueWriteBuffer copies the data at
	// call time, so previously submitted frames keep the values they saw.
	push_constant_used = 0;
	push_constant_overflow_reported = false;
	if (unlikely(push_constant_grow_pending)) {
		push_constant_grow_pending = false;
		if (push_constant_capacity < PUSH_CONSTANT_RING_MAX_SIZE) {
			// Between frames nothing is bound, so the ring can be replaced
			// wholesale: a new generation retires every bind group and render
			// bundle that references the old buffer.
			const uint32_t capacity = MIN(push_constant_capacity * 2u, PUSH_CONSTANT_RING_MAX_SIZE);
			if (_push_constant_ring_allocate(capacity)) {
				push_constant_epoch++;
				resource_epoch++;
				print_verbose(vformat("WebGPU: push constant ring grown to %d slots.", capacity / PUSH_CONSTANT_SLOT_SIZE));
			}
		}
	}
}

void RenderingDeviceDriverWebGPU::end_segment() {
}

RenderingDeviceDriverWebGPU::RenderingDeviceDriverWebGPU(RenderingContextDriverWebGPU *p_context) :
		context(p_context) {
}
