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

// The clear color asserted by the CI harness (misc/webgpu_scripts/loader-smoke.mjs).
static const Color PROBE_CLEAR_COLOR = Color(0.2f, 0.6f, 0.9f, 1.0f);
static const uint32_t PROBE_SIZE = 64;

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

	RenderingDeviceDriver *driver = context->driver_create();
	if (driver->initialize(0, 1) != OK) {
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
		driver->command_buffer_end(cmd_buffer);
		if (driver->command_queue_execute_and_present(cmd_queue, VectorView<RenderingDeviceDriver::SemaphoreID>(), cmd_buffer, VectorView<RenderingDeviceDriver::SemaphoreID>(), RenderingDeviceDriver::FenceID(), swap_chain) != OK) {
			stage = 8;
		}
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
