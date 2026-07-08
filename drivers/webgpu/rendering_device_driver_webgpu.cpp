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
		wgpuQueueSubmit(queue, wgpu_buffers.size(), wgpu_buffers.ptr());
		for (uint32_t i = 0; i < p_cmd_buffers.size(); i++) {
			CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffers[i].id;
			wgpuCommandBufferRelease(cb_info->command_buffer);
			cb_info->command_buffer = nullptr;
		}
	}

	for (uint32_t i = 0; i < p_swap_chains.size(); i++) {
		SwapChainInfo *swap_chain = (SwapChainInfo *)p_swap_chains[i].id;
		if (swap_chain->framebuffer.view != nullptr) {
			wgpuTextureViewRelease(swap_chain->framebuffer.view);
			swap_chain->framebuffer.view = nullptr;
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
	return true;
}

void RenderingDeviceDriverWebGPU::command_buffer_end(CommandBufferID p_cmd_buffer) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb_info->encoder);
	cb_info->command_buffer = wgpuCommandEncoderFinish(cb_info->encoder, nullptr);
	wgpuCommandEncoderRelease(cb_info->encoder);
	cb_info->encoder = nullptr;
}

void RenderingDeviceDriverWebGPU::command_begin_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RenderPassClearValue> p_clear_values) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
	RenderPassInfo *pass = (RenderPassInfo *)p_render_pass.id;
	FramebufferInfo *framebuffer = (FramebufferInfo *)p_framebuffer.id;
	ERR_FAIL_NULL(cb_info->encoder);
	ERR_FAIL_COND_MSG(!pass->from_swap_chain, "Only swap chain render passes are supported by the WebGPU driver so far.");
	ERR_FAIL_NULL(framebuffer->view);

	WGPURenderPassColorAttachment color_attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	color_attachment.view = framebuffer->view;
	color_attachment.storeOp = WGPUStoreOp_Store;
	if (p_clear_values.size() > 0) {
		const Color &color = p_clear_values[0].color;
		color_attachment.loadOp = WGPULoadOp_Clear;
		color_attachment.clearValue = { color.r, color.g, color.b, color.a };
	} else {
		color_attachment.loadOp = WGPULoadOp_Load;
	}

	WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	pass_desc.colorAttachmentCount = 1;
	pass_desc.colorAttachments = &color_attachment;

	cb_info->render_pass_encoder = wgpuCommandEncoderBeginRenderPass(cb_info->encoder, &pass_desc);
	ERR_FAIL_NULL(cb_info->render_pass_encoder);
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
	swap_chain->render_pass.color_format = swap_chain->data_format;
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
	swap_chain->framebuffer.view = wgpuTextureCreateView(surface_texture.texture, nullptr);
	ERR_FAIL_NULL_V(swap_chain->framebuffer.view, FramebufferID());
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
	if (swap_chain->framebuffer.view != nullptr) {
		wgpuTextureViewRelease(swap_chain->framebuffer.view);
	}
	if (swap_chain->current_texture != nullptr) {
		wgpuTextureRelease(swap_chain->current_texture);
	}
	if (swap_chain->configured) {
		wgpuSurfaceUnconfigure(context->get_wgpu_surface(swap_chain->surface));
	}
	memdelete(swap_chain);
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
}

void RenderingDeviceDriverWebGPU::end_segment() {
}

RenderingDeviceDriverWebGPU::RenderingDeviceDriverWebGPU(RenderingContextDriverWebGPU *p_context) :
		context(p_context) {
}
