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

/* global WebGPU, GodotWebGPUXR, _wgpuTextureRelease */ // WebGPU + _wgpuTextureRelease: emdawnwebgpu port; GodotWebGPUXR: emitted from the $-object below at link time.

const GodotWebGPU = {
	/**
	 * WebXR bridge, exposed on Module so the renderer-agnostic WebXR glue
	 * (modules/webxr/native/library_godot_webxr.js, which must also link in
	 * builds without the emdawnwebgpu port) can reach the WebGPU bindings
	 * without a static dependency on them.
	 */
	$GodotWebGPUXR__deps: ['$WebGPU', 'wgpuTextureRelease'],
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
		 * Maps a GPUTexture to a stable WGPUTexture handle so each texture
		 * object is imported once. This table owns the import references:
		 * clear() releases them (the engine-side wrappers are non-owning).
		 * The WebXR glue clears it on layer recreation, per frame when the
		 * browser follows the spec's per-frame texture lifecycle, and at
		 * session end.
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

		size: function () {
			return GodotWebGPUXR.imported_textures ? GodotWebGPUXR.imported_textures.size : 0;
		},

		clear: function () {
			if (GodotWebGPUXR.imported_textures) {
				// In-flight GPU work holds its own references, so releasing
				// the import refs here never invalidates submitted frames.
				GodotWebGPUXR.imported_textures.forEach(function (handle) {
					_wgpuTextureRelease(handle);
				});
			}
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
		// These are emdawnwebgpu port internals, not standardized API, and
		// the device key is already deprecated upstream. When an Emscripten
		// upgrade renames one, the failure otherwise surfaces as "undefined
		// is not a function" from inside generated glue, several layers away
		// from the cause.
		if (typeof WebGPU.importJsDevice !== 'function' || typeof WebGPU.importJsTexture !== 'function') {
			throw new Error('The emdawnwebgpu bindings changed: importJsDevice/importJsTexture are missing. This build needs updating for the current Emscripten port.');
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
	 * Whether render-bundle caching is enabled. `?nobundles` in the page URL
	 * disables it, so the same export can be A/B benchmarked.
	 *
	 * @returns {number} 1 to use render bundles, 0 to encode directly.
	 */
	godot_js_webgpu_use_bundles__sig: 'i',
	godot_js_webgpu_use_bundles: function () {
		return window.location.search.indexOf('nobundles') >= 0 ? 0 : 1;
	},

	godot_js_webgpu_has_transient_attachments__sig: 'i',
	godot_js_webgpu_has_transient_attachments: function () {
		// Core addition in Chromium 149 (no adapter feature): the usage
		// constant exists on the interface exactly when the browser
		// understands it in createTexture. ?notransient forces the plain
		// attachment path for on-device A/B benchmarking.
		if (window.location.search.indexOf('notransient') >= 0) {
			return 0;
		}
		return (typeof GPUTextureUsage !== 'undefined' && 'TRANSIENT_ATTACHMENT' in GPUTextureUsage) ? 1 : 0;
	},

	godot_js_webgpu_use_tiers__sig: 'i',
	godot_js_webgpu_use_tiers: function () {
		// ?notiers ignores texture-formats-tier1/2 for on-device A/B of the
		// tier-gated paths (compute octmap, dynamic VoxelGI, compute bokeh).
		return window.location.search.indexOf('notiers') >= 0 ? 0 : 1;
	},

	godot_js_webgpu_use_native_swizzle__sig: 'i',
	godot_js_webgpu_use_native_swizzle: function () {
		// ?noswizzle forces the CPU texel-expansion fallback for on-device
		// A/B benchmarking of the native view-swizzle path.
		return window.location.search.indexOf('noswizzle') >= 0 ? 0 : 1;
	},

	godot_js_webgpu_use_bc__sig: 'i',
	godot_js_webgpu_use_bc: function () {
		// ?nobc reports BC texture compression unsupported for A/B of
		// compressed-texture content against its uncompressed fallback.
		return window.location.search.indexOf('nobc') >= 0 ? 0 : 1;
	},

	godot_js_webgpu_use_warm__sig: 'i',
	godot_js_webgpu_use_warm: function () {
		// ?nowarm disables background pipeline warming for A/B of the
		// first-visit warm-up curve (deferred pipelines then compile only
		// at first bind).
		return window.location.search.indexOf('nowarm') >= 0 ? 0 : 1;
	},

	godot_js_webgpu_use_fastpath__sig: 'i',
	godot_js_webgpu_use_fastpath: function () {
		// ?nofastpath disables the identity (memcmp) bundle fast path for
		// A/B against the hash lookup it replaces.
		return window.location.search.indexOf('nofastpath') >= 0 ? 0 : 1;
	},

	godot_js_webgpu_use_indirect__sig: 'i',
	godot_js_webgpu_use_indirect: function () {
		// ?noindirect makes cached bundles record literal draw parameters
		// again (a parameter change then misses the cache) for A/B of the
		// indirect-args path.
		return window.location.search.indexOf('noindirect') >= 0 ? 0 : 1;
	},

	godot_js_webgpu_use_diff_flush__sig: 'i',
	godot_js_webgpu_use_diff_flush: function () {
		// ?nodiff uploads whole dynamic-buffer slices again (no change
		// scan) for A/B of the range-diffed flush.
		return window.location.search.indexOf('nodiff') >= 0 ? 0 : 1;
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
