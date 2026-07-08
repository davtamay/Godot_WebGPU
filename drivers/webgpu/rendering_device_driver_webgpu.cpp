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
	ERR_FAIL_NULL(cb_info->encoder);
	wgpuCommandEncoderClearBuffer(cb_info->encoder, ((BufferInfo *)p_buffer.id)->buffer, p_offset, p_size);
}

void RenderingDeviceDriverWebGPU::command_copy_buffer(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, BufferID p_dst_buffer, VectorView<BufferCopyRegion> p_regions) {
	CommandBufferInfo *cb_info = (CommandBufferInfo *)p_cmd_buffer.id;
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
			WGPUBindGroupLayoutEntry pc_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
			pc_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
			pc_entry.visibility = _shader_stages_to_wgpu(reflection.push_constant_stages);
			pc_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
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
		WGPUBindGroupLayoutEntry pc_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
		pc_entry.binding = RenderingShaderContainerWebGPU::PUSH_CONSTANT_BINDING;
		pc_entry.visibility = _shader_stages_to_wgpu(reflection.push_constant_stages);
		pc_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layout_desc.entryCount = 1;
		layout_desc.entries = &pc_entry;
		WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
		ERR_FAIL_NULL_V(layout, ShaderID());
		shader->bind_group_layouts.push_back(layout);
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
