/**************************************************************************/
/*  library_godot_webgpu.js                                               */
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

/* global WebGPU, GodotWebGPUXR */ // WebGPU: emdawnwebgpu port; GodotWebGPUXR: emitted from the $-object below at link time.

const GodotWebGPU = {
	/**
	 * WebXR bridge, exposed on Module so the renderer-agnostic WebXR glue
	 * (modules/webxr/native/library_godot_webxr.js, which must also link in
	 * builds without the emdawnwebgpu port) can reach the WebGPU bindings
	 * without a static dependency on them.
	 */
	$GodotWebGPUXR__deps: ['$WebGPU'],
	$GodotWebGPUXR__postset: 'Module["GodotWebGPUXR"] = GodotWebGPUXR;',
	$GodotWebGPUXR: {
		imported_textures: null,

		createBinding: function (session) {
			const device = Module['preinitializedWebGPUDevice'];
			if (!device || !('XRGPUBinding' in window)) {
				return null;
			}
			return new window['XRGPUBinding'](session, device);
		},

		/**
		 * Maps a GPUTexture to a stable WGPUTexture handle; XR layers cycle
		 * through a small set of opaque textures, so each is imported once.
		 */
		importTexture: function (texture) {
			if (!GodotWebGPUXR.imported_textures) {
				GodotWebGPUXR.imported_textures = new Map();
			}
			let handle = GodotWebGPUXR.imported_textures.get(texture);
			if (handle === undefined) {
				handle = WebGPU.importJsTexture(texture);
				GodotWebGPUXR.imported_textures.set(texture, handle);
			}
			return handle;
		},

		clear: function () {
			// The C++ side owns the handles' release; drop the JS map so a
			// new session starts from a clean import table.
			GodotWebGPUXR.imported_textures = null;
		},
	},

	/**
	 * Imports the GPUDevice acquired by the loader before start-up (see
	 * js/engine/engine.js) into the emdawnwebgpu bindings.
	 *
	 * @returns {number} A WGPUDevice handle, or 0 if no device was stashed.
	 */
	godot_js_webgpu_device_import__deps: ['$WebGPU', '$GodotWebGPUXR'],
	godot_js_webgpu_device_import__sig: 'p',
	godot_js_webgpu_device_import: function () {
		const device = Module['preinitializedWebGPUDevice'];
		if (!device) {
			return 0;
		}
		// While an immersive WebXR session owns the compositor, the canvas
		// returns null from getCurrentTexture() WITHOUT throwing; the
		// emdawnwebgpu glue would wrap that null as a successful acquire and
		// crash at the first view creation. Turn it into the exception its
		// error path already handles (the driver skips the frame).
		const context_class = window['GPUCanvasContext'];
		if (context_class && !context_class.prototype['__godot_null_guard']) {
			const orig = context_class.prototype['getCurrentTexture'];
			context_class.prototype['getCurrentTexture'] = function () {
				const texture = orig.apply(this, arguments);
				if (!texture) {
					throw new Error('The canvas has no current texture (suspended during an XR session).');
				}
				return texture;
			};
			context_class.prototype['__godot_null_guard'] = true;
		}
		return WebGPU.importJsDevice(device);
	},

	/**
	 * Returns the browser's preferred canvas texture format.
	 *
	 * @returns {number} 1 for bgra8unorm, 2 for rgba8unorm.
	 */
	godot_js_webgpu_preferred_format__sig: 'i',
	godot_js_webgpu_preferred_format: function () {
		return navigator['gpu']['getPreferredCanvasFormat']() === 'rgba8unorm' ? 2 : 1;
	},

	/**
	 * Creates the hidden probe canvas (see drivers/webgpu/webgpu_probe.cpp).
	 * A canvas is permanently locked to its first context type, so the probe
	 * never uses the main canvas.
	 */
	godot_js_webgpu_probe_canvas_create__sig: 'v',
	godot_js_webgpu_probe_canvas_create: function () {
		if (document.getElementById('godot-webgpu-probe')) {
			return;
		}
		const canvas = document.createElement('canvas');
		canvas.id = 'godot-webgpu-probe';
		canvas.width = 64;
		canvas.height = 64;
		canvas.style.position = 'fixed';
		canvas.style.left = '0';
		canvas.style.top = '0';
		canvas.style.pointerEvents = 'none';
		document.body.appendChild(canvas);
	},
};

mergeInto(LibraryManager.library, GodotWebGPU);
