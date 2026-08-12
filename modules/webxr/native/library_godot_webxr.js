/**************************************************************************/
/*  library_godot_webxr.js                                                */
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

const GodotWebXR = {
	$GodotWebXR__deps: ['$MainLoop', '$GL', '$GodotRuntime', '$runtimeKeepalivePush', '$runtimeKeepalivePop'],
	$GodotWebXR: {
		gl: null,

		session: null,
		gl_binding: null,
		// WebGPU path (via Module['GodotWebGPUXR'], present only in builds
		// that link the WebGPU driver): an XRGPUBinding instead of the
		// XRWebGLBinding, chosen at session start by the active renderer.
		gpu_binding: null,
		gpu_color_format: null,
		layer: null,
		// Recreating the layer (resize, view-count change) invalidates every
		// texture wrapped from it; the engine watches this counter to evict.
		layer_generation: 0,
		// getViewSubImage() is only valid once per frame per view; several
		// engine calls need it, so it is computed once per animation frame.
		frame_subimage: null,
		frame_view_subimages: [null, null],
		// Whether the WebGL context can do single-pass stereo (OVR_multiview2
		// or OCULUS_multiview). Android XR's browser has neither: stereo then
		// renders one pass per view into a shared side-by-side layer texture.
		gl_multiview: null,
		// Requested fixed foveation level (0.0 = off, 1.0 = maximum). The
		// compositor applies it when rendering the projection layer, so it
		// costs the engine nothing; runtimes without support ignore it.
		fixed_foveation: 0.0,
		space: null,
		frame: null,
		pose: null,
		view_count: 1,
		input_sources: new Array(16),
		touches: new Array(5),
		onsimpleevent: null,

		// Monkey-patch the requestAnimationFrame() used by Emscripten for the main
		// loop, so that we can swap it out for XRSession.requestAnimationFrame()
		// when an XR session is started.
		orig_requestAnimationFrame: null,
		requestAnimationFrame: (callback) => {
			if (GodotWebXR.session && GodotWebXR.space) {
				const onFrame = function (time, frame) {
					GodotWebXR.frame = frame;
					GodotWebXR.pose = frame.getViewerPose(GodotWebXR.space);
					GodotWebXR.frame_subimage = null;
					GodotWebXR.frame_view_subimages = [null, null];
					callback(time);
					GodotWebXR.frame = null;
					GodotWebXR.pose = null;
					GodotWebXR.frame_subimage = null;
					GodotWebXR.frame_view_subimages = [null, null];
				};
				GodotWebXR.session.requestAnimationFrame(onFrame);
			} else {
				GodotWebXR.orig_requestAnimationFrame(callback);
			}
		},
		monkeyPatchRequestAnimationFrame: (enable) => {
			if (GodotWebXR.orig_requestAnimationFrame === null) {
				GodotWebXR.orig_requestAnimationFrame = MainLoop.requestAnimationFrame;
			}
			MainLoop.requestAnimationFrame = enable
				? GodotWebXR.requestAnimationFrame
				: GodotWebXR.orig_requestAnimationFrame;
		},
		pauseResumeMainLoop: () => {
			// Once both GodotWebXR.session and GodotWebXR.space are set or
			// unset, our monkey-patched requestAnimationFrame() should be
			// enabled or disabled. When using the WebXR API Emulator, this
			// gets picked up automatically, however, in the Oculus Browser
			// on the Quest, we need to pause and resume the main loop.
			MainLoop.pause();
			runtimeKeepalivePush();
			window.setTimeout(function () {
				runtimeKeepalivePop();
				MainLoop.resume();
			}, 0);
		},

		usesMultiview: () => {
			if (GodotWebXR.gl_multiview === null) {
				const gl = GodotWebXR.gl;
				GodotWebXR.gl_multiview = !!(gl && (gl.getExtension('OVR_multiview2') || gl.getExtension('OCULUS_multiview')));
			}
			return GodotWebXR.gl_multiview;
		},

		applyFixedFoveation: (layer) => {
			// fixedFoveation is an optional attribute of XRCompositionLayer;
			// only assign when the runtime exposes it and a level was
			// requested, so unsupporting browsers see no behavior change.
			if (layer && GodotWebXR.fixed_foveation > 0.0 && 'fixedFoveation' in layer) {
				layer.fixedFoveation = GodotWebXR.fixed_foveation;
			}
		},

		getLayer: () => {
			// XPERIMENT (strip or productize): immersive sessions are stereo;
			// defaulting the pre-pose count to 2 makes the eagerly created
			// layer the final one (no recreation + updateRenderState churn).
			const new_view_count = (GodotWebXR.pose) ? GodotWebXR.pose.views.length : 2;
			let layer = GodotWebXR.layer;

			// If the view count hasn't changed since creating this layer, then
			// we can simply return it.
			if (layer && GodotWebXR.view_count === new_view_count) {
				return layer;
			}

			if (GodotWebXR.gpu_binding) {
				// The XRGPUBinding default layer shape is a texture array
				// (one layer per view); passing an explicit textureType has
				// produced broken sub-images on experimental implementations.
				layer = GodotWebXR.gpu_binding.createProjectionLayer({
					colorFormat: GodotWebXR.gpu_color_format,
				});
			} else if (GodotWebXR.session && GodotWebXR.gl_binding && GodotWebXR.gl_binding.createProjectionLayer) {
				const gl = GodotWebXR.gl;

				const layer_multiview = new_view_count > 1 && GodotWebXR.usesMultiview();
				const layer_init = {
					textureType: layer_multiview ? 'texture-array' : 'texture',
					colorFormat: gl.RGBA8,
				};
				if (layer_multiview || new_view_count <= 1) {
					// Only paths that render directly into the layer write its
					// depth. The per-view blit path copies color only: an
					// unwritten (zero-filled) depth buffer wastes memory and
					// hands the compositor spec-undefined reprojection input.
					layer_init.depthFormat = gl.DEPTH_COMPONENT24;
				}
				layer = GodotWebXR.gl_binding.createProjectionLayer(layer_init);
			} else {
				return null;
			}
			GodotWebXR.applyFixedFoveation(layer);
			GodotWebXR.session.updateRenderState({ layers: [layer] });


			GodotWebXR.layer = layer;
			GodotWebXR.layer_generation++;
			GodotWebXR.frame_subimage = null;
			GodotWebXR.frame_view_subimages = [null, null];
			GodotWebXR.view_count = new_view_count;
			return layer;
		},

		getSubImage: () => {
			if (GodotWebXR.frame_subimage) {
				return GodotWebXR.frame_subimage;
			}
			if (!GodotWebXR.pose) {
				return null;
			}
			const layer = GodotWebXR.getLayer();
			if (layer === null) {
				return null;
			}

			// A layer handed to updateRenderState() only joins the ACTIVE
			// render state on the next animation frame; querying sub-images
			// before that throws on strict implementations (Android XR),
			// while others tolerate it. Present nothing until it is active.
			const active_layers = GodotWebXR.session.renderState.layers;
			if (!active_layers || active_layers.indexOf(layer) < 0) {
				return null;
			}

			// Because we always use "texture-array" for multiview and "texture"
			// when there is only 1 view, it should be safe to only grab the
			// subimage for the first view.
			const binding = GodotWebXR.gpu_binding || GodotWebXR.gl_binding;
			GodotWebXR.frame_subimage = binding.getViewSubImage(layer, GodotWebXR.pose.views[0]);
			return GodotWebXR.frame_subimage;
		},

		getTextureHandle: (texture) => {
			if (!texture) {
				return 0;
			}
			if (GodotWebXR.gpu_binding) {
				// A stable WGPUTexture handle the WebGPU driver can wrap.
				return Module['GodotWebGPUXR'].importTexture(texture);
			}
			return GodotWebXR.getTextureId(texture);
		},

		getTextureId: (texture) => {
			if (texture.name !== undefined) {
				return texture.name;
			}

			const id = GL.getNewId(GL.textures);
			texture.name = id;
			GL.textures[id] = texture;

			return id;
		},

		addInputSource: (input_source) => {
			let name = -1;
			if (input_source.targetRayMode === 'tracked-pointer' && input_source.handedness === 'left') {
				name = 0;
			} else if (input_source.targetRayMode === 'tracked-pointer' && input_source.handedness === 'right') {
				name = 1;
			} else {
				for (let i = 2; i < 16; i++) {
					if (!GodotWebXR.input_sources[i]) {
						name = i;
						break;
					}
				}
			}
			if (name >= 0) {
				GodotWebXR.input_sources[name] = input_source;
				input_source.name = name;

				// Find a free touch index for screen sources.
				if (input_source.targetRayMode === 'screen') {
					let touch_index = -1;
					for (let i = 0; i < 5; i++) {
						if (!GodotWebXR.touches[i]) {
							touch_index = i;
							break;
						}
					}
					if (touch_index >= 0) {
						GodotWebXR.touches[touch_index] = input_source;
						input_source.touch_index = touch_index;
					}
				}
			}
			return name;
		},

		removeInputSource: (input_source) => {
			if (input_source.name !== undefined) {
				const name = input_source.name;
				// Only clear the slot if it still holds THIS source: a
				// replacement may already occupy it.
				if (name >= 0 && name < 16 && GodotWebXR.input_sources[name] === input_source) {
					GodotWebXR.input_sources[name] = null;
				}

				if (input_source.touch_index !== undefined) {
					const touch_index = input_source.touch_index;
					if (touch_index >= 0 && touch_index < 5) {
						GodotWebXR.touches[touch_index] = null;
					}
				}
				return name;
			}
			return -1;
		},

		getInputSourceId: (input_source) => {
			if (input_source !== undefined) {
				return input_source.name;
			}
			return -1;
		},

		getTouchIndex: (input_source) => {
			if (input_source.touch_index !== undefined) {
				return input_source.touch_index;
			}
			return -1;
		},
	},

	godot_webxr_is_supported__proxy: 'sync',
	godot_webxr_is_supported__sig: 'i',
	godot_webxr_is_supported: function () {
		return !!navigator.xr;
	},

	godot_webxr_is_session_supported__proxy: 'sync',
	godot_webxr_is_session_supported__sig: 'vii',
	godot_webxr_is_session_supported: function (p_session_mode, p_callback) {
		const session_mode = GodotRuntime.parseString(p_session_mode);
		const cb = GodotRuntime.get_func(p_callback);
		if (navigator.xr) {
			navigator.xr.isSessionSupported(session_mode).then(function (supported) {
				const c_str = GodotRuntime.allocString(session_mode);
				cb(c_str, supported ? 1 : 0);
				GodotRuntime.free(c_str);
			});
		} else {
			const c_str = GodotRuntime.allocString(session_mode);
			cb(c_str, 0);
			GodotRuntime.free(c_str);
		}
	},

	godot_webxr_initialize__deps: ['emscripten_webgl_get_current_context'],
	godot_webxr_initialize__proxy: 'sync',
	godot_webxr_initialize__sig: 'viiiiiiiiii',
	godot_webxr_initialize: function (p_use_webgpu_binding, p_session_mode, p_required_features, p_optional_features, p_requested_reference_spaces, p_on_session_started, p_on_session_ended, p_on_session_failed, p_on_input_event, p_on_simple_event) {
		GodotWebXR.monkeyPatchRequestAnimationFrame(true);

		const session_mode = GodotRuntime.parseString(p_session_mode);
		const required_features = GodotRuntime.parseString(p_required_features).split(',').map((s) => s.trim()).filter((s) => s !== '');
		const optional_features = GodotRuntime.parseString(p_optional_features).split(',').map((s) => s.trim()).filter((s) => s !== '');

		// Without the 'webgpu' session feature the browser creates a
		// WebGL-based session that XRGPUBinding refuses to attach to.
		// The engine passes which renderer actually booted: a pre-initialized
		// device on Module is NOT sufficient (the loader can prepare WebGPU
		// while a gl_compatibility project still boots GLES3).
		const use_webgpu_binding = !!(p_use_webgpu_binding && Module['preinitializedWebGPUDevice'] && Module['GodotWebGPUXR']);
		if (use_webgpu_binding && !required_features.includes('webgpu')) {
			required_features.push('webgpu');
		}
		const requested_reference_space_types = GodotRuntime.parseString(p_requested_reference_spaces).split(',').map((s) => s.trim());
		const onstarted = GodotRuntime.get_func(p_on_session_started);
		const onended = GodotRuntime.get_func(p_on_session_ended);
		const onfailed = GodotRuntime.get_func(p_on_session_failed);
		const oninputevent = GodotRuntime.get_func(p_on_input_event);
		const onsimpleevent = GodotRuntime.get_func(p_on_simple_event);

		const session_init = {};
		if (required_features.length > 0) {
			session_init['requiredFeatures'] = required_features;
		}
		if (optional_features.length > 0) {
			session_init['optionalFeatures'] = optional_features;
		}
		if (required_features.includes('depth-sensing') || optional_features.includes('depth-sensing')) {
			// The depth-sensing feature is only granted when this init dict
			// accompanies it; without it browsers silently drop the feature
			// (Android XR grants depth to WebGL sessions too).
			session_init['depthSensing'] = {
				// Browsers honor the preference order. WebGL sessions prefer
				// CPU depth: the GL render path does not consume the GPU
				// texture, while cpu-optimized enables
				// XRFrame.getDepthInformation() for script-side consumers.
				// WebGPU sessions keep gpu-optimized first for the upcoming
				// XRGPUBinding sensor-occlusion path.
				usagePreference: use_webgpu_binding ? ['gpu-optimized', 'cpu-optimized'] : ['cpu-optimized', 'gpu-optimized'],
				// luminance-alpha FIRST: it is the spec's guaranteed format
				// (16-bit value packed L+A, storing millimeters) with a
				// DOCUMENTED decode - raw = L + A*256, meters = raw *
				// rawValueToMeters. unsigned-short's GPU encoding is
				// undocumented and resisted every linearization we tried.
				dataFormatPreference: ['luminance-alpha', 'float32', 'unsigned-short'],
				// Prefer RAW depth: consumers use depth live (occlusion,
				// per-frame view), where temporal 'smooth' fusion blends
				// moving objects (a hand) into the background and erases
				// exactly what dynamic occlusion needs. Smooth only helped a
				// persistent accumulated scan, which is no longer the default.
				// UAs predating depthTypeRequest ignore it (backward-safe).
				depthTypeRequest: ['raw', 'smooth'],
			};
		}

		// XPERIMENT (strip or productize): move the session request out of the
		// engine's requestAnimationFrame task into a timer task, matching how
		// DOM-button-driven apps (Unity, the samples) request sessions.
		// Transient user activation survives a zero-delay timer.
		setTimeout(function () {
		navigator.xr.requestSession(session_mode, session_init).then(function (session) {
			GodotWebXR.session = session;

			session.addEventListener('end', function (evt) {
				onended();
			});

			session.addEventListener('inputsourceschange', function (evt) {
				// Removals first: when the browser replaces a source in a
				// single event (e.g. re-adding a hand in a different mode),
				// processing the add first lets the stale removal wipe the
				// slot the replacement just claimed.
				evt.removed.forEach(GodotWebXR.removeInputSource);
				evt.added.forEach(GodotWebXR.addInputSource);
			});

			['selectstart', 'selectend', 'squeezestart', 'squeezeend'].forEach((input_event, index) => {
				session.addEventListener(input_event, function (evt) {
					// Since this happens in-between normal frames, we need to
					// grab the frame from the event in order to get poses for
					// the input sources.
					GodotWebXR.frame = evt.frame;
					oninputevent(index, GodotWebXR.getInputSourceId(evt.inputSource));
					GodotWebXR.frame = null;
				});
			});

			session.addEventListener('visibilitychange', function (evt) {
				const c_str = GodotRuntime.allocString('visibility_state_changed');
				onsimpleevent(c_str);
				GodotRuntime.free(c_str);
			});

			// Store onsimpleevent so we can use it later.
			GodotWebXR.onsimpleevent = onsimpleevent;

			function onReferenceSpaceSuccess(reference_space, reference_space_type) {
				GodotWebXR.space = reference_space;

				// Using reference_space.addEventListener() crashes when
				// using the polyfill with the WebXR Emulator extension,
				// so we set the event property instead.
				reference_space.onreset = function (evt) {
					const c_str = GodotRuntime.allocString('reference_space_reset');
					onsimpleevent(c_str);
					GodotRuntime.free(c_str);
				};

				// Now that both GodotWebXR.session and GodotWebXR.space are
				// set, we need to pause and resume the main loop for the XR
				// main loop to kick in.
				GodotWebXR.pauseResumeMainLoop();

				// Call in setTimeout() so that errors in the onstarted()
				// callback don't bubble up here and cause Godot to try the
				// next reference space.
				window.setTimeout(function () {
					const reference_space_c_str = GodotRuntime.allocString(reference_space_type);
					const enabled_features = 'enabledFeatures' in session ? Array.from(session.enabledFeatures) : [];
					const enabled_features_c_str = GodotRuntime.allocString(enabled_features.join(','));
					const environment_blend_mode = 'environmentBlendMode' in session ? session.environmentBlendMode : '';
					const environment_blend_mode_c_str = GodotRuntime.allocString(environment_blend_mode);
					onstarted(reference_space_c_str, enabled_features_c_str, environment_blend_mode_c_str);
					GodotRuntime.free(reference_space_c_str);
					GodotRuntime.free(enabled_features_c_str);
					GodotRuntime.free(environment_blend_mode_c_str);
				}, 0);
			}

			function requestReferenceSpace() {
				const reference_space_type = requested_reference_space_types.shift();
				session.requestReferenceSpace(reference_space_type)
					.then((refSpace) => {
						onReferenceSpaceSuccess(refSpace, reference_space_type);
					})
					.catch(() => {
						if (requested_reference_space_types.length === 0) {
							const c_str = GodotRuntime.allocString('Unable to get any of the requested reference space types');
							onfailed(c_str);
							GodotRuntime.free(c_str);
						} else {
							requestReferenceSpace();
						}
					});
			}

			function setupWebGPU() {
				try {
					GodotWebXR.gpu_binding = Module['GodotWebGPUXR'].createBinding(session);
					if (!GodotWebXR.gpu_binding) {
						throw new Error('This browser cannot bind WebXR sessions to WebGPU (XRGPUBinding is unavailable).');
					}
					GodotWebXR.gpu_color_format = GodotWebXR.gpu_binding.getPreferredColorFormat
						? GodotWebXR.gpu_binding.getPreferredColorFormat()
						: 'rgba8unorm';

					// This will trigger the layer to get created.
					const layer = GodotWebXR.getLayer();
					if (!layer) {
						throw new Error('Unable to create WebXR Layer.');
					}

					requestReferenceSpace();
				} catch (error) {
					const c_str = GodotRuntime.allocString(`Unable to bind WebXR to WebGPU: ${error}`);
					onfailed(c_str);
					GodotRuntime.free(c_str);
				}
			}

			function setupWebGL() {
				const gl_context_handle = _emscripten_webgl_get_current_context();
				const gl = GL.getContext(gl_context_handle).GLctx;
				GodotWebXR.gl = gl;

				gl.makeXRCompatible().then(function () {
					const throwNoWebXRLayersError = () => {
						throw new Error('This browser doesn\'t support WebXR Layers (which Godot requires) nor is the polyfill in use. If you are the developer of this application, please consider including the polyfill.');
					};

					try {
						GodotWebXR.gl_binding = new XRWebGLBinding(session, gl);
					} catch (error) {
						// We'll end up here for browsers that don't have XRWebGLBinding at all, or if the browser does support WebXR Layers,
						// but is using the WebXR polyfill, so calling native XRWebGLBinding with the polyfilled XRSession won't work.
						throwNoWebXRLayersError();
					}

					if (!GodotWebXR.gl_binding.createProjectionLayer) {
						// On other browsers, XRWebGLBinding exists and works, but it doesn't support creating projection layers (which is
						// contrary to the spec, which says this MUST be supported) and so the polyfill is required.
						throwNoWebXRLayersError();
					}

					// This will trigger the layer to get created.
					const layer = GodotWebXR.getLayer();
					if (!layer) {
						throw new Error('Unable to create WebXR Layer.');
					}

					requestReferenceSpace();
				}).catch(function (error) {
					const c_str = GodotRuntime.allocString(`Unable to make WebGL context compatible with WebXR: ${error}`);
					onfailed(c_str);
					GodotRuntime.free(c_str);
				});
			}

			// The renderer that booted decides the binding type: only a
			// WebGPU-driver boot stashes the pre-initialized device and
			// the WebGPU XR bridge on Module.
			if (use_webgpu_binding) {
				setupWebGPU();
			} else {
				setupWebGL();
			}
		}).catch(function (error) {
			const c_str = GodotRuntime.allocString(`Unable to start session: ${error}`);
			onfailed(c_str);
			GodotRuntime.free(c_str);
		});
		}, 0);
	},

	godot_webxr_uninitialize__proxy: 'sync',
	godot_webxr_uninitialize__sig: 'v',
	godot_webxr_uninitialize: function () {
		if (GodotWebXR.session) {
			GodotWebXR.session.end()
				// Prevent exception when session has already ended.
				.catch((e) => { });
		}

		if (GodotWebXR.gpu_binding && Module['GodotWebGPUXR']) {
			Module['GodotWebGPUXR'].clear();
		}

		GodotWebXR.session = null;
		GodotWebXR.gl_binding = null;
		GodotWebXR.gpu_binding = null;
		GodotWebXR.gpu_color_format = null;
		GodotWebXR.layer = null;
		GodotWebXR.space = null;
		GodotWebXR.frame = null;
		GodotWebXR.pose = null;
		GodotWebXR.view_count = 1;
		GodotWebXR.input_sources = new Array(16);
		GodotWebXR.touches = new Array(5);
		GodotWebXR.onsimpleevent = null;

		// Disable the monkey-patched window.requestAnimationFrame() and
		// pause/restart the main loop to activate it on all platforms.
		GodotWebXR.monkeyPatchRequestAnimationFrame(false);
		GodotWebXR.pauseResumeMainLoop();
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

	godot_webxr_get_view_count__proxy: 'sync',
	godot_webxr_get_view_count__sig: 'i',
	godot_webxr_get_view_count: function () {
		if (!GodotWebXR.session || !GodotWebXR.pose) {
			return 1;
		}
		const view_count = GodotWebXR.pose.views.length;
		return view_count > 0 ? view_count : 1;
	},

	godot_webxr_get_render_target_size__proxy: 'sync',
	godot_webxr_get_render_target_size__sig: 'ii',
	godot_webxr_get_render_target_size: function (r_size) {
		const subimage = GodotWebXR.getSubImage();
		if (subimage === null) {
			return false;
		}

		GodotRuntime.setHeapValue(r_size + 0, subimage.viewport.width, 'i32');
		GodotRuntime.setHeapValue(r_size + 4, subimage.viewport.height, 'i32');

		return true;
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

	godot_webxr_get_transform_for_view__proxy: 'sync',
	godot_webxr_get_transform_for_view__sig: 'iii',
	godot_webxr_get_transform_for_view: function (p_view, r_transform) {
		if (!GodotWebXR.session || !GodotWebXR.pose) {
			return false;
		}

		const views = GodotWebXR.pose.views;
		let matrix;
		if (p_view >= 0) {
			matrix = views[p_view].transform.matrix;
		} else {
			// For -1 (or any other negative value) return the HMD transform.
			matrix = GodotWebXR.pose.transform.matrix;
		}

		for (let i = 0; i < 16; i++) {
			GodotRuntime.setHeapValue(r_transform + (i * 4), matrix[i], 'float');
		}

		return true;
	},

	godot_webxr_get_projection_for_view__proxy: 'sync',
	godot_webxr_get_projection_for_view__sig: 'iii',
	godot_webxr_get_projection_for_view: function (p_view, r_transform) {
		if (!GodotWebXR.session || !GodotWebXR.pose) {
			return false;
		}

		const matrix = GodotWebXR.pose.views[p_view].projectionMatrix;
		for (let i = 0; i < 16; i++) {
			GodotRuntime.setHeapValue(r_transform + (i * 4), matrix[i], 'float');
		}

		return true;
	},

	godot_webxr_get_color_texture__proxy: 'sync',
	godot_webxr_get_color_texture__sig: 'i',
	godot_webxr_get_color_texture: function () {
		const subimage = GodotWebXR.getSubImage();
		if (subimage === null) {
			return 0;
		}
		return GodotWebXR.getTextureHandle(subimage.colorTexture);
	},

	godot_webxr_get_depth_texture__proxy: 'sync',
	godot_webxr_get_depth_texture__sig: 'i',
	godot_webxr_get_depth_texture: function () {
		const subimage = GodotWebXR.getSubImage();
		if (subimage === null) {
			return 0;
		}
		if (!subimage.depthStencilTexture) {
			return 0;
		}
		return GodotWebXR.getTextureHandle(subimage.depthStencilTexture);
	},

	godot_webxr_get_velocity_texture__proxy: 'sync',
	godot_webxr_get_velocity_texture__sig: 'i',
	godot_webxr_get_velocity_texture: function () {
		const subimage = GodotWebXR.getSubImage();
		if (subimage === null) {
			return 0;
		}
		if (!subimage.motionVectorTexture) {
			return 0;
		}
		return GodotWebXR.getTextureHandle(subimage.motionVectorTexture);
	},

	godot_webxr_update_input_source__proxy: 'sync',
	godot_webxr_update_input_source__sig: 'iiiiiiiiiiiiiii',
	godot_webxr_update_input_source: function (p_input_source_id, r_target_pose, r_target_ray_mode, r_touch_index, r_has_grip_pose, r_grip_pose, r_has_standard_mapping, r_button_count, r_buttons, r_axes_count, r_axes, r_has_hand_data, r_hand_joints, r_hand_radii) {
		if (!GodotWebXR.session || !GodotWebXR.frame) {
			return 0;
		}

		if (p_input_source_id < 0 || p_input_source_id >= GodotWebXR.input_sources.length || !GodotWebXR.input_sources[p_input_source_id]) {
			return false;
		}

		const input_source = GodotWebXR.input_sources[p_input_source_id];
		const frame = GodotWebXR.frame;
		const space = GodotWebXR.space;

		// Target pose.
		const target_pose = frame.getPose(input_source.targetRaySpace, space);
		if (!target_pose) {
			// This can mean that the controller lost tracking.
			return false;
		}
		const target_pose_matrix = target_pose.transform.matrix;
		for (let i = 0; i < 16; i++) {
			GodotRuntime.setHeapValue(r_target_pose + (i * 4), target_pose_matrix[i], 'float');
		}

		// Target ray mode.
		let target_ray_mode = 0;
		switch (input_source.targetRayMode) {
		case 'gaze':
			target_ray_mode = 1;
			break;

		case 'tracked-pointer':
			target_ray_mode = 2;
			break;

		case 'screen':
			target_ray_mode = 3;
			break;

		default:
		}
		GodotRuntime.setHeapValue(r_target_ray_mode, target_ray_mode, 'i32');

		// Touch index.
		GodotRuntime.setHeapValue(r_touch_index, GodotWebXR.getTouchIndex(input_source), 'i32');

		// Grip pose.
		let has_grip_pose = false;
		if (input_source.gripSpace) {
			const grip_pose = frame.getPose(input_source.gripSpace, space);
			if (grip_pose) {
				const grip_pose_matrix = grip_pose.transform.matrix;
				for (let i = 0; i < 16; i++) {
					GodotRuntime.setHeapValue(r_grip_pose + (i * 4), grip_pose_matrix[i], 'float');
				}
				has_grip_pose = true;
			}
		}
		GodotRuntime.setHeapValue(r_has_grip_pose, has_grip_pose ? 1 : 0, 'i32');

		// Gamepad data (mapping, buttons and axes).
		let has_standard_mapping = false;
		let button_count = 0;
		let axes_count = 0;
		if (input_source.gamepad) {
			if (input_source.gamepad.mapping === 'xr-standard') {
				has_standard_mapping = true;
			}

			button_count = Math.min(input_source.gamepad.buttons.length, 10);
			for (let i = 0; i < button_count; i++) {
				GodotRuntime.setHeapValue(r_buttons + (i * 4), input_source.gamepad.buttons[i].value, 'float');
			}

			axes_count = Math.min(input_source.gamepad.axes.length, 10);
			for (let i = 0; i < axes_count; i++) {
				GodotRuntime.setHeapValue(r_axes + (i * 4), input_source.gamepad.axes[i], 'float');
			}
		}
		GodotRuntime.setHeapValue(r_has_standard_mapping, has_standard_mapping ? 1 : 0, 'i32');
		GodotRuntime.setHeapValue(r_button_count, button_count, 'i32');
		GodotRuntime.setHeapValue(r_axes_count, axes_count, 'i32');

		// Hand tracking data.
		let has_hand_data = false;
		if (input_source.hand && r_hand_joints !== 0 && r_hand_radii !== 0) {
			const hand_joint_array = new Float32Array(25 * 16);
			const hand_radii_array = new Float32Array(25);
			if (frame.fillPoses(input_source.hand.values(), space, hand_joint_array) && frame.fillJointRadii(input_source.hand.values(), hand_radii_array)) {
				GodotRuntime.heapCopy(HEAPF32, hand_joint_array, r_hand_joints);
				GodotRuntime.heapCopy(HEAPF32, hand_radii_array, r_hand_radii);
				has_hand_data = true;
			}
		}
		GodotRuntime.setHeapValue(r_has_hand_data, has_hand_data ? 1 : 0, 'i32');

		return true;
	},

	godot_webxr_get_visibility_state__proxy: 'sync',
	godot_webxr_get_visibility_state__sig: 'i',
	godot_webxr_get_visibility_state: function () {
		if (!GodotWebXR.session || !GodotWebXR.session.visibilityState) {
			return 0;
		}

		return GodotRuntime.allocString(GodotWebXR.session.visibilityState);
	},

	godot_webxr_get_bounds_geometry__proxy: 'sync',
	godot_webxr_get_bounds_geometry__sig: 'ii',
	godot_webxr_get_bounds_geometry: function (r_points) {
		if (!GodotWebXR.space || !GodotWebXR.space.boundsGeometry) {
			return 0;
		}

		const point_count = GodotWebXR.space.boundsGeometry.length;
		if (point_count === 0) {
			return 0;
		}

		const buf = GodotRuntime.malloc(point_count * 3 * 4);
		for (let i = 0; i < point_count; i++) {
			const point = GodotWebXR.space.boundsGeometry[i];
			GodotRuntime.setHeapValue(buf + ((i * 3) + 0) * 4, point.x, 'float');
			GodotRuntime.setHeapValue(buf + ((i * 3) + 1) * 4, point.y, 'float');
			GodotRuntime.setHeapValue(buf + ((i * 3) + 2) * 4, point.z, 'float');
		}
		GodotRuntime.setHeapValue(r_points, buf, 'i32');

		return point_count;
	},

	godot_webxr_get_frame_rate__proxy: 'sync',
	godot_webxr_get_frame_rate__sig: 'i',
	godot_webxr_get_frame_rate: function () {
		if (!GodotWebXR.session || GodotWebXR.session.frameRate === undefined) {
			return 0;
		}
		return GodotWebXR.session.frameRate;
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

	godot_webxr_update_target_frame_rate__proxy: 'sync',
	godot_webxr_update_target_frame_rate__sig: 'vi',
	godot_webxr_update_target_frame_rate: function (p_frame_rate) {
		if (!GodotWebXR.session || GodotWebXR.session.updateTargetFrameRate === undefined) {
			return;
		}

		GodotWebXR.session.updateTargetFrameRate(p_frame_rate).then(() => {
			const c_str = GodotRuntime.allocString('display_refresh_rate_changed');
			GodotWebXR.onsimpleevent(c_str);
			GodotRuntime.free(c_str);
		});
	},

	godot_webxr_get_supported_frame_rates__proxy: 'sync',
	godot_webxr_get_supported_frame_rates__sig: 'ii',
	godot_webxr_get_supported_frame_rates: function (r_frame_rates) {
		if (!GodotWebXR.session || GodotWebXR.session.supportedFrameRates === undefined) {
			return 0;
		}

		const frame_rate_count = GodotWebXR.session.supportedFrameRates.length;
		if (frame_rate_count === 0) {
			return 0;
		}

		const buf = GodotRuntime.malloc(frame_rate_count * 4);
		for (let i = 0; i < frame_rate_count; i++) {
			GodotRuntime.setHeapValue(buf + (i * 4), GodotWebXR.session.supportedFrameRates[i], 'float');
		}
		GodotRuntime.setHeapValue(r_frame_rates, buf, 'i32');

		return frame_rate_count;
	},

};

autoAddDeps(GodotWebXR, '$GodotWebXR');
mergeInto(LibraryManager.library, GodotWebXR);
