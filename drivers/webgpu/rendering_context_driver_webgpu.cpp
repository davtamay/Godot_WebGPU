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

#ifdef WEBGPU_ENABLED
#include <webgpu/webgpu.h>
#endif

static constexpr const char *UNIMPLEMENTED = "The WebGPU rendering context driver is not implemented yet.";

Error RenderingContextDriverWebGPU::initialize() {
#ifdef WEBGPU_ENABLED
	// Probe the API so the emdawnwebgpu port is linked and exercised; surface
	// and presentation support land in later patches, so still report the
	// context as unavailable.
	WGPUInstance instance = wgpuCreateInstance(nullptr);
	if (instance != nullptr) {
		wgpuInstanceRelease(instance);
	}
#endif
	ERR_FAIL_V_MSG(ERR_UNAVAILABLE, UNIMPLEMENTED);
}

const RenderingContextDriver::Device &RenderingContextDriverWebGPU::device_get(uint32_t p_device_index) const {
	ERR_FAIL_V_MSG(device, UNIMPLEMENTED);
}

uint32_t RenderingContextDriverWebGPU::device_get_count() const {
	return 0;
}

bool RenderingContextDriverWebGPU::device_supports_present(uint32_t p_device_index, SurfaceID p_surface) const {
	return false;
}

RenderingDeviceDriver *RenderingContextDriverWebGPU::driver_create() {
	return memnew(RenderingDeviceDriverWebGPU);
}

void RenderingContextDriverWebGPU::driver_free(RenderingDeviceDriver *p_driver) {
	memdelete(p_driver);
}

RenderingContextDriver::SurfaceID RenderingContextDriverWebGPU::surface_create(const void *p_platform_data) {
	ERR_FAIL_V_MSG(SurfaceID(), UNIMPLEMENTED);
}

void RenderingContextDriverWebGPU::surface_set_size(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) {
	ERR_FAIL_MSG(UNIMPLEMENTED);
}

void RenderingContextDriverWebGPU::surface_set_vsync_mode(SurfaceID p_surface, DisplayServerEnums::VSyncMode p_vsync_mode) {
	ERR_FAIL_MSG(UNIMPLEMENTED);
}

DisplayServerEnums::VSyncMode RenderingContextDriverWebGPU::surface_get_vsync_mode(SurfaceID p_surface) const {
	ERR_FAIL_V_MSG(DisplayServerEnums::VSYNC_ENABLED, UNIMPLEMENTED);
}

void RenderingContextDriverWebGPU::surface_set_hdr_output_enabled(SurfaceID p_surface, bool p_enabled) {
	ERR_FAIL_MSG(UNIMPLEMENTED);
}

bool RenderingContextDriverWebGPU::surface_get_hdr_output_enabled(SurfaceID p_surface) const {
	return false;
}

void RenderingContextDriverWebGPU::surface_set_hdr_output_reference_luminance(SurfaceID p_surface, float p_reference_luminance) {
	ERR_FAIL_MSG(UNIMPLEMENTED);
}

float RenderingContextDriverWebGPU::surface_get_hdr_output_reference_luminance(SurfaceID p_surface) const {
	return 0.0f;
}

void RenderingContextDriverWebGPU::surface_set_hdr_output_max_luminance(SurfaceID p_surface, float p_max_luminance) {
	ERR_FAIL_MSG(UNIMPLEMENTED);
}

float RenderingContextDriverWebGPU::surface_get_hdr_output_max_luminance(SurfaceID p_surface) const {
	return 0.0f;
}

void RenderingContextDriverWebGPU::surface_set_hdr_output_linear_luminance_scale(SurfaceID p_surface, float p_linear_luminance_scale) {
	ERR_FAIL_MSG(UNIMPLEMENTED);
}

float RenderingContextDriverWebGPU::surface_get_hdr_output_linear_luminance_scale(SurfaceID p_surface) const {
	return 0.0f;
}

float RenderingContextDriverWebGPU::surface_get_hdr_output_max_value(SurfaceID p_surface) const {
	return 0.0f;
}

uint32_t RenderingContextDriverWebGPU::surface_get_width(SurfaceID p_surface) const {
	ERR_FAIL_V_MSG(0, UNIMPLEMENTED);
}

uint32_t RenderingContextDriverWebGPU::surface_get_height(SurfaceID p_surface) const {
	ERR_FAIL_V_MSG(0, UNIMPLEMENTED);
}

void RenderingContextDriverWebGPU::surface_set_needs_resize(SurfaceID p_surface, bool p_needs_resize) {
	ERR_FAIL_MSG(UNIMPLEMENTED);
}

bool RenderingContextDriverWebGPU::surface_get_needs_resize(SurfaceID p_surface) const {
	return false;
}

void RenderingContextDriverWebGPU::surface_destroy(SurfaceID p_surface) {
	ERR_FAIL_MSG(UNIMPLEMENTED);
}

bool RenderingContextDriverWebGPU::is_debug_utils_enabled() const {
	return false;
}
