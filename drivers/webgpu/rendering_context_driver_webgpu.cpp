/**************************************************************************/
/*  rendering_context_driver_webgpu.cpp                                   */
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

#include "rendering_context_driver_webgpu.h"

#include "rendering_device_driver_webgpu.h"

// See platform/web/js/libs/library_godot_webgpu.js.
extern "C" {
WGPUDevice godot_js_webgpu_device_import();
}

Error RenderingContextDriverWebGPU::initialize() {
	instance = wgpuCreateInstance(nullptr);
	ERR_FAIL_NULL_V_MSG(instance, ERR_CANT_CREATE, "Failed to create the WebGPU instance.");

	// Imports the GPUDevice the loader acquired before start-up; requesting
	// one here is not possible because the browser API is asynchronous.
	device = godot_js_webgpu_device_import();
	if (device == nullptr) {
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "No pre-initialized WebGPU device. Enable `experimentalWebGPU` in the web export configuration.");
	}

	device_info.name = "WebGPU";
	device_info.vendor = Vendor::VENDOR_UNKNOWN;
	device_info.type = DEVICE_TYPE_INTEGRATED_GPU;
	return OK;
}

const RenderingContextDriver::Device &RenderingContextDriverWebGPU::device_get(uint32_t p_device_index) const {
	ERR_FAIL_COND_V(p_device_index != 0, device_info);
	return device_info;
}

uint32_t RenderingContextDriverWebGPU::device_get_count() const {
	return device != nullptr ? 1 : 0;
}

bool RenderingContextDriverWebGPU::device_supports_present(uint32_t p_device_index, SurfaceID p_surface) const {
	return device != nullptr;
}

RenderingDeviceDriver *RenderingContextDriverWebGPU::driver_create() {
	return memnew(RenderingDeviceDriverWebGPU(this));
}

void RenderingContextDriverWebGPU::driver_free(RenderingDeviceDriver *p_driver) {
	memdelete(p_driver);
}

RenderingContextDriver::SurfaceID RenderingContextDriverWebGPU::surface_create(const void *p_platform_data) {
	const WindowPlatformData *wpd = (const WindowPlatformData *)p_platform_data;
	ERR_FAIL_NULL_V(wpd, SurfaceID());
	ERR_FAIL_NULL_V(wpd->canvas_selector, SurfaceID());

	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_source = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
	canvas_source.selector = { wpd->canvas_selector, WGPU_STRLEN };
	WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;
	surface_desc.nextInChain = &canvas_source.chain;

	WGPUSurface wgpu_surface = wgpuInstanceCreateSurface(instance, &surface_desc);
	ERR_FAIL_NULL_V_MSG(wgpu_surface, SurfaceID(), vformat("Failed to create a WebGPU surface for canvas '%s'.", wpd->canvas_selector));

	Surface *surface = memnew(Surface);
	surface->wgpu_surface = wgpu_surface;
	return SurfaceID(surface);
}

void RenderingContextDriverWebGPU::surface_set_size(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) {
	Surface *surface = (Surface *)p_surface;
	if (surface->width != p_width || surface->height != p_height) {
		surface->width = p_width;
		surface->height = p_height;
		surface->needs_resize = true;
	}
}

void RenderingContextDriverWebGPU::surface_set_vsync_mode(SurfaceID p_surface, DisplayServerEnums::VSyncMode p_vsync_mode) {
	// The browser always presents in sync with the compositor; store the
	// preference for reporting purposes only.
	((Surface *)p_surface)->vsync_mode = p_vsync_mode;
}

DisplayServerEnums::VSyncMode RenderingContextDriverWebGPU::surface_get_vsync_mode(SurfaceID p_surface) const {
	return ((Surface *)p_surface)->vsync_mode;
}

void RenderingContextDriverWebGPU::surface_set_hdr_output_enabled(SurfaceID p_surface, bool p_enabled) {
}

bool RenderingContextDriverWebGPU::surface_get_hdr_output_enabled(SurfaceID p_surface) const {
	return false;
}

void RenderingContextDriverWebGPU::surface_set_hdr_output_reference_luminance(SurfaceID p_surface, float p_reference_luminance) {
}

float RenderingContextDriverWebGPU::surface_get_hdr_output_reference_luminance(SurfaceID p_surface) const {
	return 0.0f;
}

void RenderingContextDriverWebGPU::surface_set_hdr_output_max_luminance(SurfaceID p_surface, float p_max_luminance) {
}

float RenderingContextDriverWebGPU::surface_get_hdr_output_max_luminance(SurfaceID p_surface) const {
	return 0.0f;
}

void RenderingContextDriverWebGPU::surface_set_hdr_output_linear_luminance_scale(SurfaceID p_surface, float p_linear_luminance_scale) {
}

float RenderingContextDriverWebGPU::surface_get_hdr_output_linear_luminance_scale(SurfaceID p_surface) const {
	return 0.0f;
}

float RenderingContextDriverWebGPU::surface_get_hdr_output_max_value(SurfaceID p_surface) const {
	return 0.0f;
}

uint32_t RenderingContextDriverWebGPU::surface_get_width(SurfaceID p_surface) const {
	return ((Surface *)p_surface)->width;
}

uint32_t RenderingContextDriverWebGPU::surface_get_height(SurfaceID p_surface) const {
	return ((Surface *)p_surface)->height;
}

void RenderingContextDriverWebGPU::surface_set_needs_resize(SurfaceID p_surface, bool p_needs_resize) {
	((Surface *)p_surface)->needs_resize = p_needs_resize;
}

bool RenderingContextDriverWebGPU::surface_get_needs_resize(SurfaceID p_surface) const {
	return ((Surface *)p_surface)->needs_resize;
}

void RenderingContextDriverWebGPU::surface_destroy(SurfaceID p_surface) {
	Surface *surface = (Surface *)p_surface;
	if (surface->wgpu_surface != nullptr) {
		wgpuSurfaceRelease(surface->wgpu_surface);
	}
	memdelete(surface);
}

bool RenderingContextDriverWebGPU::is_debug_utils_enabled() const {
	return false;
}

RenderingContextDriverWebGPU::~RenderingContextDriverWebGPU() {
	if (device != nullptr) {
		wgpuDeviceRelease(device);
	}
	if (instance != nullptr) {
		wgpuInstanceRelease(instance);
	}
}
