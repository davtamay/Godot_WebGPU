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
	$GodotWebXRDepth: {
		state: null,
		'generation': 0,
		'sequence': 0,
		lostDevices: null,
		observedDevices: null,

		reset: function () {
			const state = GodotWebXRDepth.state;
			if (state) {
				GodotWebXRDepth.disposeOutput(state);
				for (const buffer of state.uniforms) buffer.destroy();
				state.dummy.destroy();
			}
			GodotWebXRDepth.state = null;
		},

		disposeOutput: function (state) {
			for (const name of ['sample', 'readback']) {
				if (state[name]) state[name].destroy();
				if (state[name + 'Handle']) Module['GodotWebGPUXR'].releaseOwnedTexture(state[name + 'Handle']);
				state[name] = null;
				state[name + 'Handle'] = 0;
			}
		},

		pipeline: function (state, integer) {
			const key = integer ? 'uint' : 'float';
			if (state.pipelines[key]) return state.pipelines[key];
			const layout = state.device.createBindGroupLayout({ entries: [
				{ binding: 0, visibility: GPUShaderStage.COMPUTE, texture: { sampleType: integer ? 'uint' : 'unfilterable-float', viewDimension: '2d' } },
				{ binding: 1, visibility: GPUShaderStage.COMPUTE, storageTexture: { access: 'write-only', 'format': 'rgba16float', viewDimension: '2d' } },
				{ binding: 2, visibility: GPUShaderStage.COMPUTE, storageTexture: { access: 'write-only', 'format': 'r32float', viewDimension: '2d' } },
				{ binding: 3, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'uniform' } },
			] });
			const module = state.device.createShaderModule({ label: 'Godot WebXR depth normalization', code: `
struct Params { uv: mat4x4<f32>, scale: f32, valid: u32, padding: vec2<u32> };
@group(0) @binding(0) var source: texture_2d<${integer ? 'u32' : 'f32'}>;
@group(0) @binding(1) var sample_output: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var readback_output: texture_storage_2d<r32float, write>;
@group(0) @binding(3) var<uniform> params: Params;
@compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) id: vec3<u32>) {
 let size = textureDimensions(sample_output);
 if (any(id.xy >= size)) { return; }
 var meters = 0.0;
 if (params.valid != 0u) {
  let uv = (vec2<f32>(id.xy) + vec2<f32>(0.5)) / vec2<f32>(size);
  let mapped = params.uv * vec4<f32>(uv, 0.0, 1.0);
  if (abs(mapped.w) > 0.000001) {
   let q = mapped.xy / mapped.w;
   if (all(q >= vec2<f32>(0.0)) && all(q <= vec2<f32>(1.0))) {
    let extent = textureDimensions(source);
    let pixel = min(vec2<u32>(q * vec2<f32>(extent)), extent - vec2<u32>(1u));
    let raw = f32(textureLoad(source, vec2<i32>(pixel), 0).r);
    let value = raw * params.scale;
    if (value > 0.0 && value < 3.402823e38) { meters = value; }
   }
  }
 }
 textureStore(readback_output, vec2<i32>(id.xy), vec4<f32>(meters, 0.0, 0.0, 1.0));
 // A universally filterable copy for spatial materials; readback_output data remains f32.
 textureStore(sample_output, vec2<i32>(id.xy), vec4<f32>(min(meters, 65504.0), 0.0, 0.0, 1.0));
}` });
			const pipeline = state.device.createComputePipeline({ label: 'Godot WebXR depth normalization', layout: state.device.createPipelineLayout({ bindGroupLayouts: [layout] }), compute: { module, entryPoint: 'main' } });
			state.pipelines[key] = { pipeline, layout };
			return state.pipelines[key];
		},

		prepare: function () {
			const xr = GodotWebXR;
			const failure = (status) => ({ 'status': status, 'sequence': GodotWebXRDepth['sequence'], 'valid_mask': 0 });
			if (!xr.session) { GodotWebXRDepth.reset(); return failure('no_session'); }
			if (!xr.gpu_binding) return failure('webgl_session');
			if (xr.session.depthUsage !== 'gpu-optimized') return failure('cpu_depth');
			if (typeof xr.gpu_binding.getDepthInformation !== 'function') return failure('unsupported_by_browser');
			if (xr.session.depthActive === false) return failure('depth_paused');
			if (!xr.frame || !xr.pose) return failure('no_frame');
			const device = Module['preinitializedWebGPUDevice'];
			if (!device) return failure('device_unavailable');
			if (!GodotWebXRDepth.lostDevices) GodotWebXRDepth.lostDevices = new WeakSet();
			if (!GodotWebXRDepth.observedDevices) GodotWebXRDepth.observedDevices = new WeakSet();
			if (!GodotWebXRDepth.observedDevices.has(device)) {
				GodotWebXRDepth.observedDevices.add(device);
				// Do not retain per-session textures/pipelines through device.lost.
				device.lost.then(() => GodotWebXRDepth.lostDevices.add(device));
			}
			if (GodotWebXRDepth.lostDevices.has(device)) { GodotWebXRDepth.reset(); return failure('device_lost'); }
			let state = GodotWebXRDepth.state;
			if (state && (state.session !== xr.session || state.device !== device)) { GodotWebXRDepth.reset(); state = null; }
			if (!state) {
				state = { session: xr.session, device, pipelines: {}, uniforms: [], sample: null, readback: null, sampleHandle: 0, readbackHandle: 0, frame: null };
				state.dummy = device.createTexture({ size: [1, 1], 'format': 'r32float', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
				device.queue.writeTexture({ texture: state.dummy }, new Float32Array([0]), { bytesPerRow: 4 }, [1, 1]);
				GodotWebXRDepth.state = state;
			}
			if (state.frame === xr.frame) return state.report;
			state.frame = xr.frame;
			const views = xr.pose['views'];
			if (!views.length || views.length > 32) return (state.report = failure('unsupported_view_count'));
			const records = [];
			let width = 1, height = 1, validMask = 0;
			for (let index = 0; index < views.length; index++) {
				const view = views[index];
				const record = { 'valid': false, 'eye': view['eye'] || 'none', 'projection': Array.from(view.projectionMatrix || []), 'transform': Array.from(view['transform'] ? view['transform'].matrix : []) };
				try {
					const depth = xr.gpu_binding.getDepthInformation(view);
					if (!depth) { record['reason'] = 'no_depth_data'; records.push(record); continue; }
					const texture = depth.texture;
					if (!texture || !['r16uint', 'r32float'].includes(texture['format'])) throw new Error('unsupported_format');
					if (!(texture.usage & GPUTextureUsage.TEXTURE_BINDING) || texture.dimension !== '2d' || texture.mipLevelCount !== 1 || texture.sampleCount !== 1) throw new Error('invalid_texture');
					if (depth['width'] !== texture['width'] || depth['height'] !== texture['height'] || !depth['width'] || !depth['height']) throw new Error('invalid_size');
					const matrix = Array.from(depth.normDepthBufferFromNormView.matrix);
					if (matrix.length !== 16 || !matrix.every(Number.isFinite) || !Number.isFinite(depth.rawValueToMeters) || depth.rawValueToMeters <= 0) throw new Error('invalid_transform_or_scale');
					let descriptor;
					if (typeof depth.getViewDescriptor === 'function') descriptor = depth.getViewDescriptor();
					else if (texture.depthOrArrayLayers === 1) descriptor = { dimension: '2d', baseArrayLayer: 0, arrayLayerCount: 1 };
					else throw new Error('missing_view_descriptor');
					const layer = descriptor.baseArrayLayer === undefined ? 0 : descriptor.baseArrayLayer;
					if ((descriptor.aspect && descriptor.aspect !== 'all') || descriptor.dimension !== '2d' || !Number.isInteger(layer) || layer < 0 || layer >= texture.depthOrArrayLayers || (descriptor.arrayLayerCount !== undefined && descriptor.arrayLayerCount !== 1) || (descriptor.baseMipLevel || 0) !== 0 || (descriptor.mipLevelCount !== undefined && descriptor.mipLevelCount !== 1) || (descriptor['format'] && descriptor['format'] !== texture['format'])) throw new Error('invalid_view_descriptor');
					record.source = texture.createView(descriptor);
					record['valid'] = true;
					record['format'] = texture['format'];
					record['raw_to_meters'] = depth.rawValueToMeters;
					record['norm_depth_from_view'] = matrix;
					record['source_layer'] = layer;
					record['width'] = depth['width'];
					record['height'] = depth['height'];
					width = Math.max(width, depth['width']); height = Math.max(height, depth['height']);
					validMask |= (1 << index);
				} catch (error) { record['reason'] = String(error.message || error); }
				records.push(record);
			}
			// Never retain last frame's depth for a missing eye. Every output layer
			// is overwritten, with zero (unknown) where data/UV coverage is absent.
			if (!validMask && state.sample) { width = state.sample['width']; height = state.sample['height']; }
			// Texture2DArrayRD requires at least two layers, including mono sessions.
			const outputLayers = Math.max(2, views.length);
			if (!state.sample || state.sample['width'] !== width || state.sample['height'] !== height || state.sample.depthOrArrayLayers !== outputLayers) {
				GodotWebXRDepth.disposeOutput(state);
				const usage = GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC;
				state.sample = device.createTexture({ label: 'Godot normalized XR depth (filterable)', size: [width, height, outputLayers], 'format': 'rgba16float', usage });
				state.readback = device.createTexture({ label: 'Godot normalized XR depth (precise)', size: [width, height, outputLayers], 'format': 'r32float', usage });
				state.sampleHandle = Module['GodotWebGPUXR'].importOwnedTexture(state.sample);
				state.readbackHandle = Module['GodotWebGPUXR'].importOwnedTexture(state.readback);
				state['generation'] = ++GodotWebXRDepth['generation'];
			}
			const encoder = device.createCommandEncoder({ label: 'Godot WebXR depth frame' });
			for (let index = 0; index < outputLayers; index++) {
				const record = records[index] || { 'valid': false };
				const pipeline = GodotWebXRDepth.pipeline(state, record['format'] === 'r16uint');
				if (!state.uniforms[index]) state.uniforms[index] = device.createBuffer({ size: 80, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
				const params = new ArrayBuffer(80);
				if (record['valid']) { new Float32Array(params).set(record['norm_depth_from_view']); new Float32Array(params)[16] = record['raw_to_meters']; new Uint32Array(params)[17] = 1; }
				device.queue.writeBuffer(state.uniforms[index], 0, params);
				const descriptor = { dimension: '2d', baseArrayLayer: index, arrayLayerCount: 1 };
				const bindings = device.createBindGroup({ layout: pipeline.layout, entries: [
					{ binding: 0, resource: record.source || state.dummy.createView() },
					{ binding: 1, resource: state.sample.createView(descriptor) },
					{ binding: 2, resource: state.readback.createView(descriptor) },
					{ binding: 3, resource: { buffer: state.uniforms[index] } },
				] });
				const pass = encoder.beginComputePass(); pass.setPipeline(pipeline.pipeline); pass.setBindGroup(0, bindings); pass.dispatchWorkgroups(Math.ceil(width / 8), Math.ceil(height / 8)); pass.end();
				delete record.source;
			}
			device.queue.submit([encoder.finish()]);
			state.report = { 'status': validMask ? 'ok' : 'no_depth_data', 'sequence': ++GodotWebXRDepth['sequence'], 'generation': state['generation'], 'width': width, 'height': height, 'view_count': views.length, 'texture_layers': outputLayers, 'valid_mask': validMask >>> 0, 'texture_handle': state.sampleHandle, 'readback_handle': state.readbackHandle, 'units': 'meters', 'uv_space': 'normalized_view', 'views': records };
			return state.report;
		},
	},


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
	godot_webxr_get_depth_sensing_info__sig: 'i',
	godot_webxr_get_depth_sensing_info__deps: ['$GodotWebXRDepth'],
	godot_webxr_get_depth_sensing_info: function () {
		try {
			const report = GodotWebXRDepth.prepare();
			const key = [report['status'], report['generation'], report['valid_mask']].join(':');
			if (key !== GodotWebXRDepth.lastDiagnostic) {
				GodotWebXRDepth.lastDiagnostic = key;
				GodotWebXR.reportDiagnostic({ 'type': 'depth', 'status': report['status'], 'view_count': report['view_count'] || 0, 'valid_mask': report['valid_mask'] || 0, 'units': 'meters', 'normalization': 'gpu', 'source_formats': (report['views'] || []).map((view) => view['format'] || null) });
			}
			return GodotRuntime.allocString(JSON.stringify(report));
		} catch (error) {
			GodotWebXRDepth.reset();
			return GodotRuntime.allocString(JSON.stringify({ 'status': 'depth_error', 'valid_mask': 0, 'error': String(error.message || error) }));
		}
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

	godot_webxr_shares_shadows__proxy: 'sync',
	godot_webxr_shares_shadows__sig: 'i',
	godot_webxr_shares_shadows: function () {
		return new URLSearchParams(window.location.search).has('noshare') ? 0 : 1;
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

	godot_webxr_set_render_target_size_multiplier__proxy: 'sync',
	godot_webxr_set_render_target_size_multiplier__sig: 'vf',
	godot_webxr_set_render_target_size_multiplier: function (p_multiplier) {
		// Read when the session's layer is created. The browser clamps the
		// upper end to what the device supports; the floor keeps a stray
		// zero from asking for an empty layer.
		GodotWebXR.render_target_size_multiplier = Math.max(p_multiplier, 0.1);
	},

	godot_webxr_get_render_target_size_multiplier__proxy: 'sync',
	godot_webxr_get_render_target_size_multiplier__sig: 'f',
	godot_webxr_get_render_target_size_multiplier: function () {
		return GodotWebXR.render_target_size_multiplier;
	},

	// Composition layers (the WebXR Layers module): quad, cylinder and
	// equirect layers the compositor samples at display time, each fed by a
	// SubViewport through WebXRCompositionLayer nodes. The state lives here
	// and reaches library_godot_webxr.js through Module['GodotWebXRLayers']
	// at the two points that must know about it: the projection layer's
	// render-state update and the session's end. Property names of the
	// Layers API are accessed by string on purpose: the ones missing from
	// webxr.externs.js would be renamed by the closure build.
	$GodotWebXRLayers__deps: ['$GodotRuntime', '$GodotWebXR'],
	$GodotWebXRLayers__postset: 'Module["GodotWebXRLayers"] = GodotWebXRLayers;',
	$GodotWebXRLayers: {
		layers: {},
		next_id: 1,
		dirty: false,
		warned: {},

		hasFeature: () => {
			const session = GodotWebXR.session;
			if (!session) {
				return false;
			}
			if (session['enabledFeatures']) {
				return Array.from(session['enabledFeatures']).includes('layers');
			}
			// A browser that does not report its features: the binding tells.
			const binding = GodotWebXR.gpu_binding || GodotWebXR.gl_binding;
			return !!(binding && binding['createQuadLayer']);
		},

		// Layers with a negative sort order sit behind the projection layer,
		// the others in front; renderState.layers composites lowest index
		// first.
		compose: (projection) => {
			const entries = Object.keys(GodotWebXRLayers.layers).map((id) => GodotWebXRLayers.layers[id]);
			entries.sort((a, b) => a.sort_order - b.sort_order);
			const result = [];
			entries.forEach((e) => {
				if (e.sort_order < 0) {
					result.push(e.layer);
				}
			});
			if (projection) {
				result.push(projection);
			}
			entries.forEach((e) => {
				if (e.sort_order >= 0) {
					result.push(e.layer);
				}
			});
			return result;
		},

		apply: () => {
			GodotWebXRLayers.dirty = false;
			if (!GodotWebXR.session) {
				return;
			}
			GodotWebXR.session.updateRenderState({ layers: GodotWebXRLayers.compose(GodotWebXR.layer) });
		},

		readParams: (p_params) => {
			const params = [];
			for (let i = 0; i < 16; i++) {
				params.push(GodotRuntime.getHeapValue(p_params + (i * 4), 'float'));
			}
			return params;
		},

		transformOf: (params) => new XRRigidTransform(
			{ x: params[0], y: params[1], z: params[2], w: 1.0 },
			{ x: params[3], y: params[4], z: params[5], w: params[6] }
		),

		// Shape values by layer type: quad [width, height], cylinder
		// [radius, centralAngle, aspectRatio], equirect [radius,
		// centralHorizontalAngle, upperVerticalAngle, lowerVerticalAngle].
		shapeKeys: [
			['width', 'height'],
			['radius', 'centralAngle', 'aspectRatio'],
			['radius', 'centralHorizontalAngle', 'upperVerticalAngle', 'lowerVerticalAngle'],
		],

		applyParams: (entry, params) => {
			const layer = entry.layer;
			const prev = entry.params;
			let pose_changed = prev === null;
			for (let i = 0; i < 7 && !pose_changed; i++) {
				pose_changed = prev[i] !== params[i];
			}
			if (pose_changed) {
				layer['transform'] = GodotWebXRLayers.transformOf(params);
			}
			const keys = GodotWebXRLayers.shapeKeys[entry.type];
			for (let i = 0; i < keys.length; i++) {
				if (prev === null || prev[7 + i] !== params[7 + i]) {
					layer[keys[i]] = params[7 + i];
				}
			}
			if (prev === null || prev[11] !== params[11]) {
				layer['blendTextureSourceAlpha'] = params[11] > 0.5;
			}
			if ((prev === null || prev[12] !== params[12]) && 'opacity' in layer) {
				layer['opacity'] = params[12];
			}
			const sort_order = Math.round(params[13]);
			if (entry.sort_order !== sort_order) {
				entry.sort_order = sort_order;
				GodotWebXRLayers.dirty = true;
			}
			entry.params = params;
		},

		create: (type, params) => {
			const binding = GodotWebXR.gpu_binding || GodotWebXR.gl_binding;
			if (!binding || !GodotWebXR.space || !GodotWebXRLayers.hasFeature()) {
				return 0;
			}
			const method = ['createQuadLayer', 'createCylinderLayer', 'createEquirectLayer'][type];
			if (!method || typeof binding[method] !== 'function') {
				if (!GodotWebXRLayers.warned[type]) {
					GodotWebXRLayers.warned[type] = true;
					GodotRuntime.error(`WebXR: this browser's XR binding has no ${method}(); the composition layer falls back to a mesh.`);
				}
				return 0;
			}
			const init = {
				'space': GodotWebXR.space,
				'viewPixelWidth': Math.max(1, Math.round(params[14])),
				'viewPixelHeight': Math.max(1, Math.round(params[15])),
				'layout': 'mono',
				'transform': GodotWebXRLayers.transformOf(params),
			};
			if (GodotWebXR.gpu_binding) {
				init['colorFormat'] = GodotWebXR.gpu_color_format;
			}
			const keys = GodotWebXRLayers.shapeKeys[type];
			for (let i = 0; i < keys.length; i++) {
				init[keys[i]] = params[7 + i];
			}
			let layer = null;
			try {
				layer = binding[method](init);
			} catch (e) {
				if (!GodotWebXRLayers.warned[type]) {
					GodotWebXRLayers.warned[type] = true;
					GodotRuntime.error(`WebXR: ${method}() failed: ${e}; the composition layer falls back to a mesh.`);
				}
				return 0;
			}
			if (!layer) {
				return 0;
			}
			const id = GodotWebXRLayers.next_id++;
			GodotWebXRLayers.layers[id] = {
				layer,
				type,
				sort_order: Math.round(params[13]),
				params: null,
				generation: 0,
			};
			try {
				GodotWebXRLayers.applyParams(GodotWebXRLayers.layers[id], params);
				GodotWebXRLayers.apply();
			} catch (e) {
				// Creation can succeed while a property or render-state update
				// is rejected. Do not leak the layer or escape into the frame loop.
				GodotWebXRLayers.destroy(id);
				if (!GodotWebXRLayers.warned[type]) {
					GodotWebXRLayers.warned[type] = true;
					GodotRuntime.error(`WebXR: composition layer setup failed: ${e}; falling back to a mesh.`);
				}
				return 0;
			}
			return id;
		},

		destroy: (id) => {
			const entry = GodotWebXRLayers.layers[id];
			if (!entry) {
				return;
			}
			delete GodotWebXRLayers.layers[id];
			try {
				entry.layer['destroy']();
			} catch (e) {
				// Already gone with its session.
			}
			GodotWebXRLayers.dirty = true;
		},

		clear: () => {
			Object.keys(GodotWebXRLayers.layers).forEach((id) => {
				GodotWebXRLayers.destroy(id);
			});
			GodotWebXRLayers.dirty = false;
			GodotWebXRLayers.warned = {};
		},
	},

	godot_webxr_layer_create__proxy: 'sync',
	godot_webxr_layer_create__sig: 'iii',
	godot_webxr_layer_create__deps: ['$GodotWebXRLayers'],
	godot_webxr_layer_create: function (p_type, p_params) {
		return GodotWebXRLayers.create(p_type, GodotWebXRLayers.readParams(p_params));
	},

	godot_webxr_layer_frame__proxy: 'sync',
	godot_webxr_layer_frame__sig: 'iiii',
	godot_webxr_layer_frame__deps: ['$GodotWebXRLayers'],
	godot_webxr_layer_frame: function (p_id, p_params, r_info) {
		// Applies this frame's pose and shape, then hands back the layer's
		// texture for the frame: [projection layer generation, own
		// generation, color handle, texture width, height, viewport x, y,
		// width, height] in r_info (9 ints). Returns 0 when the layer has no
		// texture to draw into this frame.
		const entry = GodotWebXRLayers.layers[p_id];
		const session = GodotWebXR.session;
		const frame = GodotWebXR.frame;
		if (!entry || !session || !frame) {
			return 0;
		}
		GodotWebXRLayers.applyParams(entry, GodotWebXRLayers.readParams(p_params));
		if (GodotWebXRLayers.dirty) {
			GodotWebXRLayers.apply();
		}
		// The projection layer's sub-image comes first: on WebGPU sessions
		// fetching it can clear the texture import table, which would orphan
		// a handle imported before it.
		if (GodotWebXR.getSubImage() === null) {
			return 0;
		}
		// A layer handed to updateRenderState() joins the active render
		// state on the next frame; asking for its sub-image before then is
		// an error on strict browsers.
		const active_layers = session['renderState']['layers'];
		if (!active_layers || Array.prototype.indexOf.call(active_layers, entry.layer) < 0) {
			return 0;
		}
		const binding = GodotWebXR.gpu_binding || GodotWebXR.gl_binding;
		let subimage = null;
		try {
			subimage = binding['getSubImage'](entry.layer, frame);
		} catch (e) {
			return 0;
		}
		if (!subimage || !subimage['colorTexture']) {
			return 0;
		}
		const texture = subimage['colorTexture'];
		const handle = GodotWebXR.getTextureHandle(texture);
		if (!handle) {
			return 0;
		}
		let texture_width = 0;
		let texture_height = 0;
		if (GodotWebXR.gpu_binding) {
			texture_width = texture['width'];
			texture_height = texture['height'];
		} else {
			texture_width = subimage['textureWidth'];
			texture_height = subimage['textureHeight'];
		}
		const viewport = subimage['viewport'];
		const info = [
			GodotWebXR.layer_generation,
			entry.generation,
			handle,
			texture_width,
			texture_height,
			viewport ? viewport['x'] : 0,
			viewport ? viewport['y'] : 0,
			viewport ? viewport['width'] : texture_width,
			viewport ? viewport['height'] : texture_height,
		];
		for (let i = 0; i < info.length; i++) {
			GodotRuntime.setHeapValue(r_info + (i * 4), info[i], 'i32');
		}
		return 1;
	},

	godot_webxr_layer_free__proxy: 'sync',
	godot_webxr_layer_free__sig: 'vi',
	godot_webxr_layer_free__deps: ['$GodotWebXRLayers'],
	godot_webxr_layer_free: function (p_id) {
		GodotWebXRLayers.destroy(p_id);
		if (GodotWebXRLayers.dirty) {
			GodotWebXRLayers.apply();
		}
	},

};

autoAddDeps(GodotWebXRExt, '$GodotWebXR');
mergeInto(LibraryManager.library, GodotWebXRExt);
