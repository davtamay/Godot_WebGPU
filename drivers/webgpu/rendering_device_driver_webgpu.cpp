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

// See platform/web/js/libs/library_godot_webgpu.js.
extern "C" {
// Returns 1 for bgra8unorm, 2 for rgba8unorm.
int godot_js_webgpu_preferred_format();
}

// WebGPU exposes a single, implicitly synchronized queue: queue family and
// queue handles are opaque non-zero tokens, submission order is the only
// ordering guarantee, and fences/semaphores are recorded but never waited on
// (a blocking wait on the browser's main thread can never make progress).
static const uint64_t TOKEN_ID = 1;

Error RenderingDeviceDriverWebGPU::initialize(uint32_t p_device_index, uint32_t p_frame_count) {
	ERR_FAIL_COND_V(p_device_index != 0, ERR_INVALID_PARAMETER);
	device = context->get_device();
	ERR_FAIL_NULL_V(device, ERR_UNAVAILABLE);
	queue = wgpuDeviceGetQueue(device);
	ERR_FAIL_NULL_V(queue, ERR_CANT_CREATE);

	if (wgpuDeviceGetLimits(device, &device_limits) != WGPUStatus_Success) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "Failed to query WebGPU device limits.");
	}

	WGPUBufferDescriptor pc_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	pc_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
	pc_desc.size = PUSH_CONSTANT_RING_SIZE;
	push_constant_buffer = wgpuDeviceCreateBuffer(device, &pc_desc);
	ERR_FAIL_NULL_V(push_constant_buffer, ERR_CANT_CREATE);
	push_constant_shadow = (uint8_t *)memalloc(PUSH_CONSTANT_RING_SIZE);
	push_constant_capacity = PUSH_CONSTANT_RING_SIZE;

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
			wgpuQueueWriteBuffer(queue, push_constant_buffer, 0, push_constant_shadow, push_constant_used);
		}
		wgpuQueueSubmit(queue, wgpu_buffers.size(), wgpu_buffers.ptr());
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
	cb_info->bind_groups_dirty = false;
	cb_info->push_constant_offset = 0;
	for (uint32_t i = 0; i < MAX_BIND_GROUPS; i++) {
		cb_info->pending_bind_groups[i] = nullptr;
	}
	return true;
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

	LocalVector<WGPURenderPassColorAttachment> color_attachments;
	WGPURenderPassDepthStencilAttachment depth_attachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	bool has_depth = false;
	for (uint32_t i = 0; i < pass->attachments.size(); i++) {
		const RenderPassAttachment &attachment = pass->attachments[i];
		if (attachment.is_depth_stencil) {
			depth_attachment.view = framebuffer->views[i];
			depth_attachment.depthLoadOp = attachment.load_op;
			depth_attachment.depthStoreOp = attachment.store_op;
			depth_attachment.stencilLoadOp = attachment.stencil_load_op;
			depth_attachment.stencilStoreOp = attachment.stencil_store_op;
			if (i < p_clear_values.size()) {
				depth_attachment.depthClearValue = p_clear_values[i].depth;
				depth_attachment.stencilClearValue = p_clear_values[i].stencil;
			}
			has_depth = true;
		} else {
			WGPURenderPassColorAttachment color_attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			color_attachment.view = framebuffer->views[i];
			// The swap chain pass predates render_pass_create and decides its
			// load op from the presence of clear values.
			color_attachment.loadOp = pass->from_swap_chain ? (p_clear_values.size() > 0 ? WGPULoadOp_Clear : WGPULoadOp_Load) : attachment.load_op;
			color_attachment.storeOp = attachment.store_op;
			if (i < p_clear_values.size()) {
				const Color &color = p_clear_values[i].color;
				color_attachment.clearValue = { color.r, color.g, color.b, color.a };
			}
			color_attachments.push_back(color_attachment);
		}
	}

	WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	pass_desc.colorAttachmentCount = color_attachments.size();
	pass_desc.colorAttachments = color_attachments.ptr();
	if (has_depth) {
		pass_desc.depthStencilAttachment = &depth_attachment;
	}

	cb_info->render_pass_encoder = wgpuCommandEncoderBeginRenderPass(cb_info->encoder, &pass_desc);
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	cb_info->current_shader = nullptr;
	cb_info->bind_groups_dirty = false;
	for (uint32_t i = 0; i < MAX_BIND_GROUPS; i++) {
		cb_info->pending_bind_groups[i] = nullptr;
	}
}

void RenderingDeviceDriverWebGPU::command_end_render_pass(CommandBufferID p_cmd_buffer) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	wgpuRenderPassEncoderEnd(cb_info->render_pass_encoder);
	wgpuRenderPassEncoderRelease(cb_info->render_pass_encoder);
	cb_info->render_pass_encoder = nullptr;
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
			ERR_FAIL_V_MSG(FramebufferID(), "Failed to acquire the current WebGPU surface texture.");
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
			return WGPUTextureFormat_RG8Uint;
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
			return WGPUTextureFormat_RG16Float;
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
		case RenderingDeviceCommons::DATA_FORMAT_BC1_RGBA_UNORM_BLOCK:
			return WGPUTextureFormat_BC1RGBAUnorm;
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

RenderingDeviceDriver::BufferID RenderingDeviceDriverWebGPU::buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) {
	ERR_FAIL_COND_V_MSG(p_usage.has_flag(BUFFER_USAGE_TEXEL_BIT), BufferID(), "Texel buffers are not supported by WebGPU.");
	ERR_FAIL_COND_V_MSG(p_usage.has_flag(BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT), BufferID(), "Persistently mapped buffers are not supported by the WebGPU driver.");
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

	WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	buffer_desc.usage = usage;
	buffer_desc.size = p_size;
	WGPUBuffer wgpu_buffer = wgpuDeviceCreateBuffer(device, &buffer_desc);
	ERR_FAIL_NULL_V(wgpu_buffer, BufferID());

	BufferInfo *buffer = memnew(BufferInfo);
	buffer->buffer = wgpu_buffer;
	buffer->size = p_size;
	if (p_allocation_type == MEMORY_ALLOCATION_TYPE_CPU) {
		buffer->shadow = (uint8_t *)memalloc(p_size);
	}
	return BufferID(buffer);
}

void RenderingDeviceDriverWebGPU::buffer_free(BufferID p_buffer) {
	BufferInfo *buffer = (BufferInfo *)p_buffer.id;
	wgpuBufferRelease(buffer->buffer);
	if (buffer->shadow != nullptr) {
		memfree(buffer->shadow);
	}
	memdelete(buffer);
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_allocation_size(BufferID p_buffer) {
	return ((BufferInfo *)p_buffer.id)->size;
}

uint8_t *RenderingDeviceDriverWebGPU::buffer_map(BufferID p_buffer) {
	BufferInfo *buffer = (BufferInfo *)p_buffer.id;
	ERR_FAIL_NULL_V_MSG(buffer->shadow, nullptr, "Only CPU (upload) buffers can be mapped by the WebGPU driver; downloads are not supported yet.");
	return buffer->shadow;
}

void RenderingDeviceDriverWebGPU::buffer_unmap(BufferID p_buffer) {
	BufferInfo *buffer = (BufferInfo *)p_buffer.id;
	ERR_FAIL_NULL(buffer->shadow);
	wgpuQueueWriteBuffer(queue, buffer->buffer, 0, buffer->shadow, buffer->size);
}

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create(const TextureFormat &p_format, const TextureView &p_view) {
	const WGPUTextureFormat wgpu_format = _data_format_to_wgpu(p_format.format);
	ERR_FAIL_COND_V_MSG(wgpu_format == WGPUTextureFormat_Undefined, TextureID(), vformat("Unsupported texture format %d on the WebGPU driver.", p_format.format));
	ERR_FAIL_COND_V_MSG(p_view.format != p_format.format, TextureID(), "Texture views with a different format are not supported by the WebGPU driver yet.");
	const bool identity_swizzle = p_view.swizzle_r == TEXTURE_SWIZZLE_R && p_view.swizzle_g == TEXTURE_SWIZZLE_G && p_view.swizzle_b == TEXTURE_SWIZZLE_B && p_view.swizzle_a == TEXTURE_SWIZZLE_A;
	ERR_FAIL_COND_V_MSG(!identity_swizzle, TextureID(), "Texture swizzles are not supported by WebGPU.");
	ERR_FAIL_COND_V_MSG(p_format.samples != TEXTURE_SAMPLES_1 && p_format.samples != TEXTURE_SAMPLES_4, TextureID(), "WebGPU only supports 1 or 4 samples per texture.");

	WGPUTextureUsage usage = WGPUTextureUsage_None;
	if (p_format.usage_bits & (TEXTURE_USAGE_SAMPLING_BIT | TEXTURE_USAGE_INPUT_ATTACHMENT_BIT)) {
		usage |= WGPUTextureUsage_TextureBinding;
	}
	if (p_format.usage_bits & (TEXTURE_USAGE_STORAGE_BIT | TEXTURE_USAGE_STORAGE_ATOMIC_BIT)) {
		usage |= WGPUTextureUsage_StorageBinding;
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

	WGPUTextureDescriptor texture_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
	texture_desc.usage = usage;
	texture_desc.format = wgpu_format;
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
	WGPUTextureView wgpu_view = wgpuTextureCreateView(wgpu_texture, nullptr);
	if (wgpu_view == nullptr) {
		wgpuTextureRelease(wgpu_texture);
		ERR_FAIL_V(TextureID());
	}

	TextureInfo *texture = memnew(TextureInfo);
	texture->texture = wgpu_texture;
	texture->view = wgpu_view;
	texture->wgpu_format = wgpu_format;
	texture->format = p_format.format;
	const uint64_t texel_size = MAX(1U, _data_format_texel_size(p_format.format));
	texture->allocation_size = (uint64_t)p_format.width * p_format.height * MAX(p_format.depth, p_format.array_layers) * texel_size * (p_format.mipmaps > 1 ? 4 : 3) / 3;
	return TextureID(texture);
}

RenderingDeviceDriver::TextureID RenderingDeviceDriverWebGPU::texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool p_depth_stencil, uint32_t p_mipmaps) {
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

void RenderingDeviceDriverWebGPU::texture_free(TextureID p_texture) {
	TextureInfo *texture = (TextureInfo *)p_texture.id;
	wgpuTextureViewRelease(texture->view);
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
	const uint32_t texel_size = _data_format_texel_size(texture->format);
	ERR_FAIL_COND_MSG(texel_size == 0, "Copyable layouts for block-compressed formats are not supported by the WebGPU driver yet.");
	const uint32_t width = MAX(1U, wgpuTextureGetWidth(texture->texture) >> p_subresource.mipmap);
	const uint32_t height = MAX(1U, wgpuTextureGetHeight(texture->texture) >> p_subresource.mipmap);
	// WebGPU requires 256-byte row alignment for buffer <-> texture copies.
	const uint64_t row_pitch = (uint64_t(width) * texel_size + 255) & ~255ULL;
	r_layout->row_pitch = row_pitch;
	r_layout->size = row_pitch * height;
}

RenderingDeviceDriver::SamplerID RenderingDeviceDriverWebGPU::sampler_create(const SamplerState &p_state) {
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
	sampler_desc.lodMinClamp = p_state.min_lod;
	sampler_desc.lodMaxClamp = MIN(p_state.max_lod, 32.0f);
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
	sampler_desc.maxAnisotropy = p_state.use_anisotropy ? (uint16_t)CLAMP((int)p_state.anisotropy_max, 1, 16) : 1;

	WGPUSampler sampler = wgpuDeviceCreateSampler(device, &sampler_desc);
	ERR_FAIL_NULL_V(sampler, SamplerID());
	return SamplerID(sampler);
}

void RenderingDeviceDriverWebGPU::sampler_free(SamplerID p_sampler) {
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
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cb_info);
	ERR_FAIL_NULL(cb_info->encoder);
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferCopyRegion &region = p_regions[i];
		wgpuCommandEncoderCopyBufferToBuffer(cb_info->encoder, ((BufferInfo *)p_src_buffer.id)->buffer, region.src_offset, ((BufferInfo *)p_dst_buffer.id)->buffer, region.dst_offset, region.size);
	}
}

static WGPUTexelCopyTextureInfo _texel_copy_texture_info(WGPUTexture p_texture, uint32_t p_mipmap, const Vector3i &p_offset) {
	WGPUTexelCopyTextureInfo info = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	info.texture = p_texture;
	info.mipLevel = p_mipmap;
	info.origin = { (uint32_t)p_offset.x, (uint32_t)p_offset.y, (uint32_t)p_offset.z };
	return info;
}

void RenderingDeviceDriverWebGPU::command_copy_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<TextureCopyRegion> p_regions) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cb_info);
	ERR_FAIL_NULL(cb_info->encoder);
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const TextureCopyRegion &region = p_regions[i];
		WGPUTexelCopyTextureInfo src = _texel_copy_texture_info(((TextureInfo *)p_src_texture.id)->texture, region.src_subresources.mipmap, region.src_offset);
		WGPUTexelCopyTextureInfo dst = _texel_copy_texture_info(((TextureInfo *)p_dst_texture.id)->texture, region.dst_subresources.mipmap, region.dst_offset);
		WGPUExtent3D size = { (uint32_t)region.size.x, (uint32_t)region.size.y, (uint32_t)region.size.z };
		wgpuCommandEncoderCopyTextureToTexture(cb_info->encoder, &src, &dst, &size);
	}
}

void RenderingDeviceDriverWebGPU::command_copy_buffer_to_texture(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<BufferTextureCopyRegion> p_regions) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cb_info);
	ERR_FAIL_NULL(cb_info->encoder);
	TextureInfo *texture = (TextureInfo *)p_dst_texture.id;
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferTextureCopyRegion &region = p_regions[i];
		WGPUTexelCopyBufferInfo src = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
		src.buffer = ((BufferInfo *)p_src_buffer.id)->buffer;
		src.layout.offset = region.buffer_offset;
		src.layout.bytesPerRow = region.row_pitch != 0 ? (uint32_t)region.row_pitch : (uint32_t)region.texture_region_size.x * _data_format_texel_size(texture->format);
		src.layout.rowsPerImage = region.texture_region_size.y;
		WGPUTexelCopyTextureInfo dst = _texel_copy_texture_info(((TextureInfo *)p_dst_texture.id)->texture, region.texture_subresource.mipmap, region.texture_offset);
		WGPUExtent3D size = { (uint32_t)region.texture_region_size.x, (uint32_t)region.texture_region_size.y, (uint32_t)region.texture_region_size.z };
		wgpuCommandEncoderCopyBufferToTexture(cb_info->encoder, &src, &dst, &size);
	}
}

void RenderingDeviceDriverWebGPU::command_copy_texture_to_buffer(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, BufferID p_dst_buffer, VectorView<BufferTextureCopyRegion> p_regions) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	_end_compute_pass(cb_info);
	ERR_FAIL_NULL(cb_info->encoder);
	TextureInfo *texture = (TextureInfo *)p_src_texture.id;
	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferTextureCopyRegion &region = p_regions[i];
		WGPUTexelCopyTextureInfo src = _texel_copy_texture_info(((TextureInfo *)p_src_texture.id)->texture, region.texture_subresource.mipmap, region.texture_offset);
		WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
		dst.buffer = ((BufferInfo *)p_dst_buffer.id)->buffer;
		dst.layout.offset = region.buffer_offset;
		dst.layout.bytesPerRow = region.row_pitch != 0 ? (uint32_t)region.row_pitch : (uint32_t)region.texture_region_size.x * _data_format_texel_size(texture->format);
		dst.layout.rowsPerImage = region.texture_region_size.y;
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
	const ShaderReflection reflection = p_shader_container->get_shader_reflection();

	ShaderInfo *shader = memnew(ShaderInfo);
	shader->push_constant_size = reflection.push_constant_size;
	bool set0_only_push_constant = false;
	for (const ShaderSpecializationConstant &constant : reflection.specialization_constants) {
		shader->specialization_constant_ids.push_back(constant.constant_id);
	}

	// Shader modules from the container's WGSL.
	for (int64_t i = 0; i < p_shader_container->shaders.size(); i++) {
		const RenderingShaderContainer::Shader &stage = p_shader_container->shaders[i];
		Vector<uint8_t> wgsl;
		wgsl.resize(stage.code_decompressed_size + 1);
		if (!p_shader_container->decompress_code(stage.code_compressed_bytes.ptr(), stage.code_compressed_bytes.size(), stage.code_compression_flags, wgsl.ptrw(), stage.code_decompressed_size)) {
			shader_free(ShaderID(shader));
			ERR_FAIL_V_MSG(ShaderID(), vformat("Failed to decompress WGSL for stage #%d.", i));
		}
		wgsl.ptrw()[stage.code_decompressed_size] = 0;

		WGPUShaderSourceWGSL wgsl_source = WGPU_SHADER_SOURCE_WGSL_INIT;
		wgsl_source.code = { (const char *)wgsl.ptr(), stage.code_decompressed_size };
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
			entry.visibility = _shader_stages_to_wgpu(uniform.stages);
			switch (uniform.type) {
				case UNIFORM_TYPE_SAMPLER:
					entry.sampler.type = WGPUSamplerBindingType_Filtering;
					break;
				case UNIFORM_TYPE_TEXTURE:
				case UNIFORM_TYPE_INPUT_ATTACHMENT:
					entry.texture.sampleType = _data_format_to_wgpu_sample_type(uniform.texture_format);
					entry.texture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
					break;
				case UNIFORM_TYPE_IMAGE:
					entry.storageTexture.access = uniform.writable ? WGPUStorageTextureAccess_ReadWrite : WGPUStorageTextureAccess_ReadOnly;
					entry.storageTexture.format = _data_format_to_wgpu(uniform.texture_format);
					entry.storageTexture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
					break;
				case UNIFORM_TYPE_UNIFORM_BUFFER:
					entry.buffer.type = WGPUBufferBindingType_Uniform;
					break;
				case UNIFORM_TYPE_STORAGE_BUFFER:
					entry.buffer.type = uniform.writable ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;
					break;
				case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: {
					entry.texture.sampleType = _data_format_to_wgpu_sample_type(uniform.texture_format);
					entry.texture.viewDimension = _texture_type_to_wgpu_view_dimension(uniform.texture_type);
					entries.push_back(entry);
					WGPUBindGroupLayoutEntry sampler_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
					sampler_entry.binding = remapped_binding + 1;
					sampler_entry.visibility = entry.visibility;
					sampler_entry.sampler.type = WGPUSamplerBindingType_Filtering;
					entries.push_back(sampler_entry);
					combined_before++;
					continue;
				}
				default:
					shader_free(ShaderID(shader));
					ERR_FAIL_V_MSG(ShaderID(), vformat("Unsupported uniform type %d in set %d binding %d on the WebGPU driver.", uniform.type, set_index, uniform.binding));
			}
			entries.push_back(entry);
		}

		if (shader->push_constant_size > 0 && (uint32_t)set_index == RenderingShaderContainerWebGPU::PUSH_CONSTANT_GROUP) {
			set0_only_push_constant = entries.is_empty();
			WGPUBindGroupLayoutEntry pc_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
			pc_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
			pc_entry.visibility = _shader_stages_to_wgpu(reflection.push_constant_stages);
			pc_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
			pc_entry.buffer.hasDynamicOffset = true;
			entries.push_back(pc_entry);
		}

		WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layout_desc.entryCount = entries.size();
		layout_desc.entries = entries.ptr();
		WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
		if (layout == nullptr) {
			shader_free(ShaderID(shader));
			ERR_FAIL_V_MSG(ShaderID(), vformat("Failed to create a bind group layout for set %d.", set_index));
		}
		shader->bind_group_layouts.push_back(layout);
	}

	// A push-constant-only shader with no set 0 still needs the reserved binding.
	if (shader->push_constant_size > 0 && shader->bind_group_layouts.is_empty()) {
		set0_only_push_constant = true;
		WGPUBindGroupLayoutEntry pc_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
		pc_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
		pc_entry.visibility = _shader_stages_to_wgpu(reflection.push_constant_stages);
		pc_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		pc_entry.buffer.hasDynamicOffset = true;
		WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layout_desc.entryCount = 1;
		layout_desc.entries = &pc_entry;
		WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
		ERR_FAIL_NULL_V(layout, ShaderID());
		shader->bind_group_layouts.push_back(layout);
	}

	// Set 0 with nothing but the reserved binding never gets a
	// uniform_set_create call; give the shader a ready-made bind group.
	if (set0_only_push_constant) {
		WGPUBindGroupEntry pc_group_entry = WGPU_BIND_GROUP_ENTRY_INIT;
		pc_group_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
		pc_group_entry.buffer = push_constant_buffer;
		pc_group_entry.size = PUSH_CONSTANT_SLOT_SIZE;
		WGPUBindGroupDescriptor pc_group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		pc_group_desc.layout = shader->bind_group_layouts[0];
		pc_group_desc.entryCount = 1;
		pc_group_desc.entries = &pc_group_entry;
		shader->push_constant_bind_group = wgpuDeviceCreateBindGroup(device, &pc_group_desc);
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
	memdelete((VertexFormatInfo *)p_vertex_format.id);
}

RenderingDeviceDriver::RenderPassID RenderingDeviceDriverWebGPU::render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses, VectorView<SubpassDependency> p_subpass_dependencies, uint32_t p_view_count, AttachmentReference p_fragment_density_map_attachment) {
	ERR_FAIL_COND_V_MSG(p_subpasses.size() > 1, RenderPassID(), "Multiple subpasses are not supported by WebGPU.");
	ERR_FAIL_COND_V_MSG(p_view_count > 1, RenderPassID(), "Multiview is not supported by the WebGPU driver yet.");

	RenderPassInfo *pass = memnew(RenderPassInfo);
	for (uint32_t i = 0; i < p_attachments.size(); i++) {
		const Attachment &attachment = p_attachments[i];
		RenderPassAttachment pass_attachment;
		pass_attachment.format = _data_format_to_wgpu(attachment.format);
		pass_attachment.is_depth_stencil = _is_depth_stencil_format(attachment.format);
		pass_attachment.load_op = attachment.load_op == ATTACHMENT_LOAD_OP_LOAD ? WGPULoadOp_Load : WGPULoadOp_Clear;
		pass_attachment.store_op = attachment.store_op == ATTACHMENT_STORE_OP_STORE ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
		pass_attachment.stencil_load_op = attachment.stencil_load_op == ATTACHMENT_LOAD_OP_LOAD ? WGPULoadOp_Load : WGPULoadOp_Clear;
		pass_attachment.stencil_store_op = attachment.stencil_store_op == ATTACHMENT_STORE_OP_STORE ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
		pass->attachments.push_back(pass_attachment);
	}
	return RenderPassID(pass);
}

void RenderingDeviceDriverWebGPU::render_pass_free(RenderPassID p_render_pass) {
	RenderPassInfo *pass = (RenderPassInfo *)p_render_pass.id;
	ERR_FAIL_COND(pass->from_swap_chain); // Owned by the swap chain.
	memdelete(pass);
}

RenderingDeviceDriver::FramebufferID RenderingDeviceDriverWebGPU::framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) {
	FramebufferInfo *framebuffer = memnew(FramebufferInfo);
	framebuffer->width = p_width;
	framebuffer->height = p_height;
	for (uint32_t i = 0; i < p_attachments.size(); i++) {
		framebuffer->views.push_back(((TextureInfo *)p_attachments[i].id)->view);
	}
	return FramebufferID(framebuffer);
}

void RenderingDeviceDriverWebGPU::framebuffer_free(FramebufferID p_framebuffer) {
	// Views are owned by their textures.
	memdelete((FramebufferInfo *)p_framebuffer.id);
}

RenderingDeviceDriver::UniformSetID RenderingDeviceDriverWebGPU::uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader, uint32_t p_set_index, int p_linear_pool_index) {
	const ShaderInfo *shader = (const ShaderInfo *)p_shader.id;
	ERR_FAIL_COND_V(p_set_index >= shader->bind_group_layouts.size(), UniformSetID());

	LocalVector<WGPUBindGroupEntry> entries;
	uint32_t combined_before = 0;
	for (uint32_t i = 0; i < p_uniforms.size(); i++) {
		const BoundUniform &uniform = p_uniforms[i];
		const uint32_t remapped_binding = uniform.binding + combined_before;
		WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
		entry.binding = remapped_binding;
		switch (uniform.type) {
			case UNIFORM_TYPE_SAMPLER: {
				ERR_FAIL_COND_V_MSG(uniform.ids.size() != 1, UniformSetID(), "Sampler arrays are not supported by WebGPU.");
				entry.sampler = (WGPUSampler)uniform.ids[0].id;
			} break;
			case UNIFORM_TYPE_TEXTURE:
			case UNIFORM_TYPE_IMAGE:
			case UNIFORM_TYPE_INPUT_ATTACHMENT: {
				ERR_FAIL_COND_V_MSG(uniform.ids.size() != 1, UniformSetID(), "Texture arrays are not supported by WebGPU.");
				entry.textureView = ((TextureInfo *)uniform.ids[0].id)->view;
			} break;
			case UNIFORM_TYPE_UNIFORM_BUFFER:
			case UNIFORM_TYPE_STORAGE_BUFFER: {
				entry.buffer = ((BufferInfo *)uniform.ids[0].id)->buffer;
				entry.size = WGPU_WHOLE_SIZE;
			} break;
			case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: {
				ERR_FAIL_COND_V_MSG(uniform.ids.size() != 2, UniformSetID(), "Combined sampler arrays are not supported by WebGPU.");
				// ids are [sampler, texture] pairs; the split remap places the
				// texture at the remapped binding and the sampler right after.
				entry.textureView = ((TextureInfo *)uniform.ids[1].id)->view;
				entries.push_back(entry);
				WGPUBindGroupEntry sampler_entry = WGPU_BIND_GROUP_ENTRY_INIT;
				sampler_entry.binding = remapped_binding + 1;
				sampler_entry.sampler = (WGPUSampler)uniform.ids[0].id;
				entries.push_back(sampler_entry);
				combined_before++;
				continue;
			} break;
			default:
				ERR_FAIL_V_MSG(UniformSetID(), vformat("Unsupported uniform type %d on the WebGPU driver.", uniform.type));
		}
		entries.push_back(entry);
	}

	UniformSetInfo *uniform_set = memnew(UniformSetInfo);
	if (shader->push_constant_size > 0 && p_set_index == RenderingShaderContainerWebGPU::PUSH_CONSTANT_GROUP) {
		WGPUBindGroupEntry pc_entry = WGPU_BIND_GROUP_ENTRY_INIT;
		pc_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
		pc_entry.buffer = push_constant_buffer;
		pc_entry.size = PUSH_CONSTANT_SLOT_SIZE;
		entries.push_back(pc_entry);
		uniform_set->has_push_constant_offset = true;
	}

	WGPUBindGroupDescriptor bind_group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	bind_group_desc.layout = shader->bind_group_layouts[p_set_index];
	bind_group_desc.entryCount = entries.size();
	bind_group_desc.entries = entries.ptr();
	uniform_set->bind_group = wgpuDeviceCreateBindGroup(device, &bind_group_desc);
	if (uniform_set->bind_group == nullptr) {
		memdelete(uniform_set);
		ERR_FAIL_V_MSG(UniformSetID(), vformat("Failed to create a bind group for set %d.", p_set_index));
	}
	return UniformSetID(uniform_set);
}

void RenderingDeviceDriverWebGPU::uniform_set_free(UniformSetID p_uniform_set) {
	UniformSetInfo *uniform_set = (UniformSetInfo *)p_uniform_set.id;
	wgpuBindGroupRelease(uniform_set->bind_group);
	memdelete(uniform_set);
}

void RenderingDeviceDriverWebGPU::command_bind_push_constants(CommandBufferID p_cmd_buffer, ShaderID p_shader, uint32_t p_first_index, VectorView<uint32_t> p_data) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	const uint32_t data_size = p_data.size() * sizeof(uint32_t);
	ERR_FAIL_COND(data_size > PUSH_CONSTANT_SLOT_SIZE);
	// Wrapping mid-frame would corrupt earlier draws; fail loudly instead.
	ERR_FAIL_COND_MSG(push_constant_used + PUSH_CONSTANT_SLOT_SIZE > push_constant_capacity, "Push constant ring buffer exhausted for this frame.");
	memcpy(push_constant_shadow + push_constant_used, p_data.ptr(), data_size);
	cb_info->push_constant_offset = push_constant_used;
	push_constant_used += PUSH_CONSTANT_SLOT_SIZE;
	cb_info->bind_groups_dirty = true;
}

void RenderingDeviceDriverWebGPU::_flush_bind_groups(CommandBufferInfo *p_cb_info) {
	if (!p_cb_info->bind_groups_dirty || p_cb_info->current_shader == nullptr) {
		return;
	}
	const ShaderInfo *shader = p_cb_info->current_shader;
	for (uint32_t i = 0; i < shader->bind_group_layouts.size() && i < MAX_BIND_GROUPS; i++) {
		UniformSetInfo *set = p_cb_info->pending_bind_groups[i];
		WGPUBindGroup bind_group = set != nullptr ? set->bind_group : (i == 0 ? shader->push_constant_bind_group : nullptr);
		if (bind_group == nullptr) {
			continue;
		}
		const bool wants_offset = set != nullptr ? set->has_push_constant_offset : true;
		const uint32_t dynamic_offset = p_cb_info->push_constant_offset;
		if (p_cb_info->render_pass_encoder != nullptr) {
			wgpuRenderPassEncoderSetBindGroup(p_cb_info->render_pass_encoder, i, bind_group, wants_offset ? 1 : 0, wants_offset ? &dynamic_offset : nullptr);
		} else if (p_cb_info->compute_pass_encoder != nullptr) {
			wgpuComputePassEncoderSetBindGroup(p_cb_info->compute_pass_encoder, i, bind_group, wants_offset ? 1 : 0, wants_offset ? &dynamic_offset : nullptr);
		}
	}
	p_cb_info->bind_groups_dirty = false;
}

void RenderingDeviceDriverWebGPU::_end_compute_pass(CommandBufferInfo *p_cb_info) {
	if (p_cb_info->compute_pass_encoder != nullptr) {
		wgpuComputePassEncoderEnd(p_cb_info->compute_pass_encoder);
		wgpuComputePassEncoderRelease(p_cb_info->compute_pass_encoder);
		p_cb_info->compute_pass_encoder = nullptr;
		p_cb_info->current_shader = nullptr;
		p_cb_info->bind_groups_dirty = false;
	}
}

void RenderingDeviceDriverWebGPU::command_bind_render_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	PipelineInfo *pipeline = (PipelineInfo *)p_pipeline.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	wgpuRenderPassEncoderSetPipeline(cb_info->render_pass_encoder, pipeline->render_pipeline);
	cb_info->current_shader = pipeline->shader;
	cb_info->bind_groups_dirty = true;
}

void RenderingDeviceDriverWebGPU::command_bind_render_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_COND_MSG(p_dynamic_offsets != 0, "Dynamic uniform buffers are not supported by the WebGPU driver yet.");
	for (uint32_t i = 0; i < p_set_count; i++) {
		const uint32_t set_index = p_first_set_index + i;
		ERR_FAIL_COND(set_index >= MAX_BIND_GROUPS);
		cb_info->pending_bind_groups[set_index] = (UniformSetInfo *)p_uniform_sets[i].id;
	}
	cb_info->bind_groups_dirty = true;
}

void RenderingDeviceDriverWebGPU::command_render_draw(CommandBufferID p_cmd_buffer, uint32_t p_vertex_count, uint32_t p_instance_count, uint32_t p_base_vertex, uint32_t p_first_instance) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	_flush_bind_groups(cb_info);
	wgpuRenderPassEncoderDraw(cb_info->render_pass_encoder, p_vertex_count, p_instance_count, p_base_vertex, p_first_instance);
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	_flush_bind_groups(cb_info);
	wgpuRenderPassEncoderDrawIndexed(cb_info->render_pass_encoder, p_index_count, p_instance_count, p_first_index, p_vertex_offset, p_first_instance);
}

void RenderingDeviceDriverWebGPU::command_render_draw_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	_flush_bind_groups(cb_info);
	for (uint32_t i = 0; i < p_draw_count; i++) {
		wgpuRenderPassEncoderDrawIndirect(cb_info->render_pass_encoder, ((BufferInfo *)p_indirect_buffer.id)->buffer, p_offset + i * p_stride);
	}
}

void RenderingDeviceDriverWebGPU::command_render_bind_vertex_buffers(CommandBufferID p_cmd_buffer, uint32_t p_binding_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	ERR_FAIL_COND_MSG(p_dynamic_offsets != 0, "Dynamic vertex buffer offsets are not supported by the WebGPU driver yet.");
	for (uint32_t i = 0; i < p_binding_count; i++) {
		wgpuRenderPassEncoderSetVertexBuffer(cb_info->render_pass_encoder, i, ((BufferInfo *)p_buffers[i].id)->buffer, p_offsets[i], WGPU_WHOLE_SIZE);
	}
}

void RenderingDeviceDriverWebGPU::command_render_bind_index_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, IndexBufferFormat p_format, uint64_t p_offset) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	wgpuRenderPassEncoderSetIndexBuffer(cb_info->render_pass_encoder, ((BufferInfo *)p_buffer.id)->buffer, p_format == INDEX_BUFFER_FORMAT_UINT16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32, p_offset, WGPU_WHOLE_SIZE);
}

void RenderingDeviceDriverWebGPU::command_render_set_viewport(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_viewports) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	ERR_FAIL_COND(p_viewports.size() == 0);
	const Rect2i &viewport = p_viewports[0];
	wgpuRenderPassEncoderSetViewport(cb_info->render_pass_encoder, viewport.position.x, viewport.position.y, viewport.size.width, viewport.size.height, 0.0f, 1.0f);
}

void RenderingDeviceDriverWebGPU::command_render_set_scissor(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_scissors) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	ERR_FAIL_COND(p_scissors.size() == 0);
	const Rect2i &scissor = p_scissors[0];
	wgpuRenderPassEncoderSetScissorRect(cb_info->render_pass_encoder, scissor.position.x, scissor.position.y, scissor.size.width, scissor.size.height);
}

void RenderingDeviceDriverWebGPU::command_render_set_blend_constants(CommandBufferID p_cmd_buffer, const Color &p_constants) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
	WGPUColor color = { p_constants.r, p_constants.g, p_constants.b, p_constants.a };
	wgpuRenderPassEncoderSetBlendConstant(cb_info->render_pass_encoder, &color);
}

// Specialization constants become override values keyed by @id. Only ids
// present in the shader's reflection are supplied: WebGPU rejects constants
// that do not exist in the module.
static void _specialization_constants_to_wgpu(const LocalVector<uint32_t> &p_shader_ids, VectorView<RenderingDeviceCommons::PipelineSpecializationConstant> p_constants, LocalVector<CharString> &r_keys, LocalVector<WGPUConstantEntry> &r_entries) {
	for (uint32_t i = 0; i < p_constants.size(); i++) {
		const RenderingDeviceCommons::PipelineSpecializationConstant &constant = p_constants[i];
		if (!p_shader_ids.has(constant.constant_id)) {
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

RenderingDeviceDriver::PipelineID RenderingDeviceDriverWebGPU::render_pipeline_create(ShaderID p_shader, VertexFormatID p_vertex_format, RenderPrimitive p_render_primitive, PipelineRasterizationState p_rasterization_state, PipelineMultisampleState p_multisample_state, PipelineDepthStencilState p_depth_stencil_state, PipelineColorBlendState p_blend_state, VectorView<int32_t> p_color_attachments, BitField<PipelineDynamicStateFlags> p_dynamic_state, RenderPassID p_render_pass, uint32_t p_render_subpass, VectorView<PipelineSpecializationConstant> p_specialization_constants) {
	const ShaderInfo *shader = (const ShaderInfo *)p_shader.id;
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
			ERR_FAIL_V_MSG(PipelineID(), vformat("Unsupported render primitive %d on WebGPU.", p_render_primitive));
	}

	LocalVector<CharString> constant_keys;
	LocalVector<WGPUConstantEntry> constants;
	_specialization_constants_to_wgpu(shader->specialization_constant_ids, p_specialization_constants, constant_keys, constants);

	WGPUVertexState vertex_state = WGPU_VERTEX_STATE_INIT;
	WGPUFragmentState fragment_state = WGPU_FRAGMENT_STATE_INIT;
	bool has_fragment = false;
	for (uint32_t i = 0; i < shader->module_stages.size(); i++) {
		if (shader->module_stages[i] == SHADER_STAGE_VERTEX) {
			vertex_state.module = shader->modules[i];
		} else if (shader->module_stages[i] == SHADER_STAGE_FRAGMENT) {
			fragment_state.module = shader->modules[i];
			has_fragment = true;
		}
	}
	ERR_FAIL_NULL_V_MSG(vertex_state.module, PipelineID(), "Render pipelines require a vertex stage.");
	vertex_state.entryPoint = { SHADER_ENTRY_POINT, WGPU_STRLEN };
	vertex_state.constantCount = constants.size();
	vertex_state.constants = constants.ptr();
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
		targets.push_back(target);
	}

	WGPURenderPipelineDescriptor pipeline_desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	pipeline_desc.layout = shader->pipeline_layout;
	pipeline_desc.vertex = vertex_state;
	pipeline_desc.primitive.topology = topology;
	pipeline_desc.primitive.frontFace = p_rasterization_state.front_face == POLYGON_FRONT_FACE_CLOCKWISE ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
	pipeline_desc.primitive.cullMode = p_rasterization_state.cull_mode == POLYGON_CULL_FRONT ? WGPUCullMode_Front : (p_rasterization_state.cull_mode == POLYGON_CULL_BACK ? WGPUCullMode_Back : WGPUCullMode_None);
	pipeline_desc.multisample.count = p_multisample_state.sample_count == TEXTURE_SAMPLES_4 ? 4 : 1;
	pipeline_desc.multisample.mask = p_multisample_state.sample_mask.is_empty() ? 0xFFFFFFFF : p_multisample_state.sample_mask[0];
	pipeline_desc.multisample.alphaToCoverageEnabled = p_multisample_state.enable_alpha_to_coverage;
	pipeline_desc.depthStencil = depth_stencil_ptr;
	if (has_fragment) {
		fragment_state.entryPoint = { SHADER_ENTRY_POINT, WGPU_STRLEN };
		fragment_state.constantCount = constants.size();
		fragment_state.constants = constants.ptr();
		fragment_state.targetCount = targets.size();
		fragment_state.targets = targets.ptr();
		pipeline_desc.fragment = &fragment_state;
	}

	WGPURenderPipeline render_pipeline = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);
	ERR_FAIL_NULL_V_MSG(render_pipeline, PipelineID(), "Failed to create a WebGPU render pipeline.");

	PipelineInfo *pipeline = memnew(PipelineInfo);
	pipeline->render_pipeline = render_pipeline;
	pipeline->shader = shader;
	return PipelineID(pipeline);
}

RenderingDeviceDriver::PipelineID RenderingDeviceDriverWebGPU::compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants) {
	const ShaderInfo *shader = (const ShaderInfo *)p_shader.id;
	ERR_FAIL_COND_V(shader->modules.is_empty(), PipelineID());

	LocalVector<CharString> constant_keys;
	LocalVector<WGPUConstantEntry> constants;
	_specialization_constants_to_wgpu(shader->specialization_constant_ids, p_specialization_constants, constant_keys, constants);

	WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
	pipeline_desc.layout = shader->pipeline_layout;
	pipeline_desc.compute.module = shader->modules[0];
	pipeline_desc.compute.entryPoint = { SHADER_ENTRY_POINT, WGPU_STRLEN };
	pipeline_desc.compute.constantCount = constants.size();
	pipeline_desc.compute.constants = constants.ptr();

	WGPUComputePipeline compute_pipeline = wgpuDeviceCreateComputePipeline(device, &pipeline_desc);
	ERR_FAIL_NULL_V_MSG(compute_pipeline, PipelineID(), "Failed to create a WebGPU compute pipeline.");

	PipelineInfo *pipeline = memnew(PipelineInfo);
	pipeline->compute_pipeline = compute_pipeline;
	pipeline->shader = shader;
	return PipelineID(pipeline);
}

void RenderingDeviceDriverWebGPU::pipeline_free(PipelineID p_pipeline) {
	PipelineInfo *pipeline = (PipelineInfo *)p_pipeline.id;
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
	wgpuComputePassEncoderSetPipeline(cb_info->compute_pass_encoder, pipeline->compute_pipeline);
	cb_info->current_shader = pipeline->shader;
	cb_info->bind_groups_dirty = true;
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

void RenderingDeviceDriverWebGPU::begin_segment(uint32_t p_frame_index, uint32_t p_frames_drawn) {
	// Safe to recycle every frame: wgpuQueueWriteBuffer copies the data at
	// call time, so previously submitted frames keep the values they saw.
	push_constant_used = 0;
}

void RenderingDeviceDriverWebGPU::end_segment() {
}

RenderingDeviceDriverWebGPU::RenderingDeviceDriverWebGPU(RenderingContextDriverWebGPU *p_context) :
		context(p_context) {
}
