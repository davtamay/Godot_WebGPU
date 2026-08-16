/**************************************************************************/
/*  library_godot_webxr_ext.js                                            */
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

// WebXR exports added by the WebGPU patch stack, kept in a sibling library
// so the diff against upstream's library_godot_webxr.js stays small: these
// are whole functions upstream has no counterpart for, while that file
// keeps only the changes interleaved with upstream code. State and shared
// helpers live on $GodotWebXR (declared there); everything here reaches
// them through the dependency below. When one of these features is
// upstreamed, its export moves into the upstream file as part of that PR.

const GodotWebXRExt = {

	godot_webxr_offer_session__proxy: 'sync',
	godot_webxr_offer_session__sig: 'viiiii',
	godot_webxr_offer_session: function (p_use_webgpu_binding, p_session_mode, p_required_features, p_optional_features, p_on_accepted) {
		if (!navigator.xr || !navigator.xr.offerSession) {
			// Browsers without offerSession keep the page-button flow.
			return;
		}
		const session_mode = GodotRuntime.parseString(p_session_mode);
		const required_features = GodotRuntime.parseString(p_required_features).split(',').map((s) => s.trim()).filter((s) => s !== '');
		const optional_features = GodotRuntime.parseString(p_optional_features).split(',').map((s) => s.trim()).filter((s) => s !== '');
		const use_webgpu_binding = !!(p_use_webgpu_binding && Module['preinitializedWebGPUDevice'] && Module['GodotWebGPUXR']);
		if (use_webgpu_binding && !required_features.includes('webgpu')) {
			required_features.push('webgpu');
		}
		const onaccepted = GodotRuntime.get_func(p_on_accepted);
		const session_init = GodotWebXR.buildSessionInit(required_features, optional_features, use_webgpu_binding);
		navigator.xr.offerSession(session_mode, session_init).then(function (session) {
			GodotWebXR.offered_session = session;
			GodotWebXR.offered_session_mode = session_mode;
			onaccepted();
		}).catch(function (e) {
			// A replaced offer or an unsupported mode; stay quiet - the
			// page-button flow still works.
		});
	},

	godot_webxr_get_depth_sensing_info__proxy: 'sync',
	godot_webxr_get_depth_sensing_info__sig: 'iii',
	godot_webxr_get_depth_sensing_info: function (p_view, r_params) {
		// WebGPU path only: returns an imported WGPUTexture handle for the
		// view's depth-sensing texture and writes [rawValueToMeters, width,
		// height, format(0=unorm16,1=float32)] to r_params (4 floats).
		// On failure (return 0), r_params[0] holds why: 0 = no session,
		// 1 = WebGL-binding session, 2 = the browser's XRGPUBinding has no
		// getDepthInformation(), 3 = the session has no depth data.
		const fail = function (reason) {
			GodotRuntime.setHeapValue(r_params + 0, reason, 'float');
			return 0;
		};
		// Capability checks come BEFORE the pose check: UI clicks can reach
		// this outside the XR frame callback (pose is null there), and the
		// binding/API checks don't need a pose - without this order a
		// mid-session click misreports "no session" instead of the real
		// capability verdict.
		if (!GodotWebXR.session) {
			return fail(0);
		}
		if (!GodotWebXR.gpu_binding) {
			return fail(1);
		}
		if (typeof GodotWebXR.gpu_binding.getDepthInformation !== 'function') {
			return fail(2);
		}
		if (!GodotWebXR.pose) {
			// Supported, but no pose this instant (outside the frame
			// callback). Per-frame data can't be fetched here; callers
			// polling from _process land inside the frame and succeed.
			return fail(3);
		}
		const views = GodotWebXR.pose.views;
		if (p_view >= views.length) {
			return fail(3);
		}
		let depth = null;
		try {
			depth = GodotWebXR.gpu_binding.getDepthInformation(views[p_view]);
		} catch (e) {
			return fail(3);
		}
		if (!depth || !depth.texture) {
			return fail(3);
		}
		GodotRuntime.setHeapValue(r_params + 0, depth.rawValueToMeters !== undefined ? depth.rawValueToMeters : 1.0, 'float');
		GodotRuntime.setHeapValue(r_params + 4, depth.texture.width, 'float');
		GodotRuntime.setHeapValue(r_params + 8, depth.texture.height, 'float');
		GodotRuntime.setHeapValue(r_params + 12, depth.texture.format === 'r32float' ? 1.0 : 0.0, 'float');
		return Module['GodotWebGPUXR'].importTexture(depth.texture);
	},

	godot_webxr_get_pass_info__proxy: 'sync',
	godot_webxr_get_pass_info__sig: 'ii',
	godot_webxr_get_pass_info: function (r_info) {
		// One thread crossing for everything a draw pass needs on the
		// RenderingDevice path: writes [layer_generation, color_handle,
		// depth_handle] as 3 u32s to r_info. Returns 1 on success.
		const subimage = GodotWebXR.getSubImage();
		if (subimage === null) {
			return 0;
		}
		GodotRuntime.setHeapValue(r_info + 0, GodotWebXR.layer_generation, 'i32');
		GodotRuntime.setHeapValue(r_info + 4, GodotWebXR.getTextureHandle(subimage.colorTexture), 'i32');
		GodotRuntime.setHeapValue(r_info + 8, subimage.depthStencilTexture ? GodotWebXR.getTextureHandle(subimage.depthStencilTexture) : 0, 'i32');
		return 1;
	},

	godot_webxr_uses_multiview__proxy: 'sync',
	godot_webxr_uses_multiview__sig: 'i',
	godot_webxr_uses_multiview: function () {
		return GodotWebXR.usesMultiview() ? 1 : 0;
	},

	godot_webxr_get_gl_pass_info__proxy: 'sync',
	godot_webxr_get_gl_pass_info__sig: 'iii',
	godot_webxr_get_gl_pass_info: function (p_view, r_rect) {
		// Non-multiview GL stereo: returns the layer color texture's GL id
		// and writes the view's viewport [x, y, w, h] to r_rect (4 i32s).
		// Returns 0 while the layer is not yet in the active render state.
		if (!GodotWebXR.session || !GodotWebXR.pose || !GodotWebXR.gl_binding) {
			return 0;
		}
		const views = GodotWebXR.pose.views;
		if (p_view >= views.length) {
			return 0;
		}
		const layer = GodotWebXR.getLayer();
		if (layer === null) {
			return 0;
		}
		const active_layers = GodotWebXR.session.renderState.layers;
		if (!active_layers || active_layers.indexOf(layer) < 0) {
			return 0;
		}
		let subimage = GodotWebXR.frame_view_subimages[p_view];
		if (!subimage) {
			subimage = GodotWebXR.gl_binding.getViewSubImage(layer, views[p_view]);
			GodotWebXR.frame_view_subimages[p_view] = subimage;
		}
		if (!subimage || !subimage.colorTexture) {
			return 0;
		}
		const vp = subimage.viewport;
		GodotRuntime.setHeapValue(r_rect + 0, vp.x, 'i32');
		GodotRuntime.setHeapValue(r_rect + 4, vp.y, 'i32');
		GodotRuntime.setHeapValue(r_rect + 8, vp.width, 'i32');
		GodotRuntime.setHeapValue(r_rect + 12, vp.height, 'i32');
		return GodotWebXR.getTextureId(subimage.colorTexture);
	},

	godot_webxr_get_color_format__proxy: 'sync',
	godot_webxr_get_color_format__sig: 'i',
	godot_webxr_get_color_format: function () {
		// Only meaningful on the WebGPU path; the caller must free the string.
		return GodotRuntime.allocString(GodotWebXR.gpu_color_format || '');
	},

	godot_webxr_get_frame_matrices__proxy: 'sync',
	godot_webxr_get_frame_matrices__sig: 'ii',
	godot_webxr_get_frame_matrices: function (r_data) {
		// One crossing per frame for every matrix the engine reads: the head
		// transform (16 floats), then per view its transform and projection
		// (32 floats per view, up to 2 views). Returns the view count, or 0
		// without a pose - callers fall back to the per-matrix calls.
		if (!GodotWebXR.session || !GodotWebXR.pose) {
			return 0;
		}
		const pose = GodotWebXR.pose;
		const view_count = Math.min(pose.views.length, 2);
		let offset = r_data;
		const put = function (matrix) {
			for (let i = 0; i < 16; i++) {
				GodotRuntime.setHeapValue(offset + (i * 4), matrix[i], 'float');
			}
			offset += 64;
		};
		put(pose.transform.matrix);
		for (let v = 0; v < view_count; v++) {
			put(pose.views[v].transform.matrix);
			put(pose.views[v].projectionMatrix);
		}
		return view_count;
	},

	godot_webxr_set_fixed_foveation__proxy: 'sync',
	godot_webxr_set_fixed_foveation__sig: 'vf',
	godot_webxr_set_fixed_foveation: function (p_level) {
		// Clamp and remember the request; it is applied to the current layer
		// immediately (the attribute is mutable mid-session) and re-applied
		// whenever the layer is recreated.
		GodotWebXR.fixed_foveation = Math.min(Math.max(p_level, 0.0), 1.0);
		GodotWebXR.applyFixedFoveation(GodotWebXR.layer);
	},

	godot_webxr_get_fixed_foveation__proxy: 'sync',
	godot_webxr_get_fixed_foveation__sig: 'f',
	godot_webxr_get_fixed_foveation: function () {
		const layer = GodotWebXR.layer;
		if (layer && 'fixedFoveation' in layer && typeof layer.fixedFoveation === 'number') {
			// The runtime may clamp the requested level; report its value.
			return layer.fixedFoveation;
		}
		return GodotWebXR.fixed_foveation;
	},

};

autoAddDeps(GodotWebXRExt, '$GodotWebXR');
mergeInto(LibraryManager.library, GodotWebXRExt);
