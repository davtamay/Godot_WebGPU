// Device check harness: loaded in <head> before the engine starts.
//
// It hooks WebGPU at the page level (no engine change):
// - records the adapter's identity, limits and features;
// - adds COPY_SRC to the canvas configuration so frames can be read back;
// - after the engine's frame callback, when the scene asks for a capture
//   (window.__devcheckWant), copies the canvas texture into a buffer, maps it
//   asynchronously, samples it and compares with golden.json;
// - times every frame callback, for the performance case;
// - shows results in a DOM panel (which never touches the canvas pixels) and
//   posts the full report to /report.
(function () {
	'use strict';
	const TOLERANCE = 24; // per channel, 0-255
	const state = {
		adapter: null,
		limits: {},
		features: [],
		canvasFormat: null,
		device: null,
		texture: null,
		capturing: false,
		cases: [],
		frameTimes: [],
		perfCase: null,
		golden: null,
		backend: 'unknown',
		started: performance.now(),
	};
	window.__devcheck = state;

	fetch('golden.json', { cache: 'no-store' }).then((r) => r.json()).then((g) => { state.golden = g; }).catch(() => { state.golden = {}; });

	// ---- adapter identity ----------------------------------------------------
	if (navigator.gpu) {
		const origRequestAdapter = navigator.gpu.requestAdapter.bind(navigator.gpu);
		navigator.gpu.requestAdapter = async function (...args) {
			const adapter = await origRequestAdapter(...args);
			if (adapter && !state.adapter) {
				const info = adapter.info || {};
				state.adapter = { vendor: info.vendor || '', architecture: info.architecture || '', device: info.device || '', description: info.description || '' };
				for (const key in adapter.limits) {
					state.limits[key] = adapter.limits[key];
				}
				state.features = Array.from(adapter.features || []).sort();
			}
			return adapter;
		};
	}

	// ---- canvas: readable frames ---------------------------------------------
	if (window.GPUCanvasContext) {
		const proto = GPUCanvasContext.prototype;
		const origConfigure = proto.configure;
		proto.configure = function (config) {
			const c = Object.assign({}, config);
			c.usage = (c.usage || GPUTextureUsage.RENDER_ATTACHMENT) | GPUTextureUsage.COPY_SRC;
			state.canvasFormat = c.format;
			state.device = c.device;
			state.backend = 'webgpu';
			return origConfigure.call(this, c);
		};
		const origGetCurrentTexture = proto.getCurrentTexture;
		proto.getCurrentTexture = function () {
			const t = origGetCurrentTexture.call(this);
			state.texture = t;
			return t;
		};
	}

	// ---- frame hook ------------------------------------------------------------
	const origRaf = window.requestAnimationFrame.bind(window);
	window.requestAnimationFrame = function (cb) {
		return origRaf(function (ts) {
			state.texture = null;
			const t0 = performance.now();
			cb(ts);
			const dt = performance.now() - t0;
			if (state.perfCase) {
				state.frameTimes.push(dt);
			}
			afterFrame();
		});
	};

	function afterFrame() {
		const want = window.__devcheckWant;
		if (!want || state.capturing || window.__devcheckAck === want.id) {
			return;
		}
		if (want.perf) {
			if (!state.perfCase || state.perfCase.id !== want.id) {
				state.perfCase = { id: want.id, name: want.name, start: performance.now() };
				state.frameTimes = [];
				return;
			}
			if (state.frameTimes.length < 180) {
				return;
			}
			const times = state.frameTimes.slice().sort((a, b) => a - b);
			const mean = times.reduce((a, b) => a + b, 0) / times.length;
			const wall = (performance.now() - state.perfCase.start) / 1000;
			recordCase({ id: want.id, name: want.name, perf: true, pass: true, frame_ms_mean: round(mean), frame_ms_p90: round(times[Math.floor(times.length * 0.9)]), fps: round(times.length / wall) });
			state.perfCase = null;
			window.__devcheckAck = want.id;
			return;
		}
		if (state.backend !== 'webgpu' || !state.texture || !state.device) {
			recordCase({ id: want.id, name: want.name, pass: false, skipped: true, detail: 'no WebGPU canvas texture this frame (' + state.backend + ')' });
			window.__devcheckAck = want.id;
			return;
		}
		capture(want, state.texture);
	}

	function capture(want, texture) {
		state.capturing = true;
		const device = state.device;
		const w = texture.width;
		const h = texture.height;
		const bytesPerRow = Math.ceil((w * 4) / 256) * 256;
		const buffer = device.createBuffer({ size: bytesPerRow * h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
		const enc = device.createCommandEncoder();
		enc.copyTextureToBuffer({ texture }, { buffer, bytesPerRow }, { width: w, height: h });
		device.queue.submit([enc.finish()]);
		buffer.mapAsync(GPUMapMode.READ).then(() => {
			const px = new Uint8Array(buffer.getMappedRange());
			const bgra = (state.canvasFormat || '').startsWith('bgra');
			const golden = (state.golden && state.golden[want.name]) || [];
			const samples = [];
			let pass = golden.length === want.samples.length;
			want.samples.forEach((s, i) => {
				const x = Math.min(w - 1, Math.max(0, Math.floor(s[0] * w)));
				const y = Math.min(h - 1, Math.max(0, Math.floor(s[1] * h)));
				const o = y * bytesPerRow + x * 4;
				const rgb = bgra ? [px[o + 2], px[o + 1], px[o]] : [px[o], px[o + 1], px[o + 2]];
				const exp = golden[i];
				const diff = exp ? Math.max(Math.abs(rgb[0] - exp[0]), Math.abs(rgb[1] - exp[1]), Math.abs(rgb[2] - exp[2])) : 255;
				if (diff > TOLERANCE) {
					pass = false;
				}
				samples.push({ at: s, got: rgb, expected: exp || null, diff });
			});
			buffer.unmap();
			buffer.destroy();
			recordCase({ id: want.id, name: want.name, pass, size: [w, h], samples });
			state.capturing = false;
			window.__devcheckAck = want.id;
		}).catch((e) => {
			state.capturing = false;
			recordCase({ id: want.id, name: want.name, pass: false, detail: 'readback failed: ' + e });
			window.__devcheckAck = want.id;
		});
	}

	function round(v) {
		return Math.round(v * 100) / 100;
	}

	// ---- results ----------------------------------------------------------------
	const errors = [];
	['error', 'warn'].forEach((level) => {
		const orig = console[level].bind(console);
		console[level] = function (...args) {
			if (errors.length < 200) {
				errors.push('[' + level + '] ' + args.map((a) => String(a)).join(' ').slice(0, 300));
			}
			orig(...args);
		};
	});
	window.addEventListener('error', (e) => errors.push('[pageerror] ' + e.message));

	function recordCase(result) {
		state.cases.push(result);
		render();
	}

	let panel = null;
	function render(final) {
		if (!panel) {
			panel = document.createElement('div');
			panel.id = 'devcheck-panel';
			panel.style.cssText = 'position:fixed;left:8px;bottom:8px;max-width:min(560px,calc(100vw - 16px));max-height:60vh;overflow:auto;background:rgba(12,14,20,0.88);color:#e8eaf0;font:13px/1.45 system-ui,sans-serif;padding:10px 12px;border-radius:8px;z-index:1000;pointer-events:auto';
			document.body.appendChild(panel);
		}
		const plan = window.__devcheckPlan;
		const total = plan ? plan.cases.length : '?';
		const rows = state.cases.map((c) => {
			const mark = c.skipped ? '–' : c.perf ? '⏱' : c.pass ? '✓' : '✗';
			const color = c.skipped ? '#aab' : c.perf ? '#9cf' : c.pass ? '#7d7' : '#f77';
			let extra = '';
			if (c.perf) {
				extra = c.frame_ms_mean + ' ms/frame, ' + c.fps + ' fps';
			} else if (!c.pass && c.samples) {
				extra = c.samples.filter((s) => s.diff > TOLERANCE).map((s) => 'got ' + s.got.join(',') + ' want ' + (s.expected ? s.expected.join(',') : '?')).join('; ');
			} else if (c.detail) {
				extra = c.detail;
			}
			return '<div><span style="color:' + color + ';display:inline-block;width:1.2em">' + mark + '</span>' + c.name + (extra ? ' <span style="color:#aab">— ' + extra + '</span>' : '') + '</div>';
		});
		const numeric = (final && plan) ? plan.numeric.map((n) => '<div><span style="color:' + (n.pass ? '#7d7' : '#f77') + ';display:inline-block;width:1.2em">' + (n.pass ? '✓' : '✗') + '</span>' + n.name + ' <span style="color:#aab">— ' + n.detail + '</span></div>') : [];
		const a = state.adapter || {};
		panel.innerHTML = '<div style="font-weight:600;margin-bottom:4px">Device check ' + state.cases.length + '/' + total + '</div>' +
			'<div style="color:#aab;margin-bottom:6px">' + (a.vendor || '?') + ' ' + (a.architecture || '') + ' · ' + state.backend + ' · ' + (state.canvasFormat || '') + '</div>' +
			rows.join('') + numeric.join('') + (final ? '<div style="margin-top:6px;color:#aab">' + final + '</div>' : '');
	}

	function finish() {
		const plan = window.__devcheckPlan || { cases: [], numeric: [] };
		const failed = state.cases.filter((c) => !c.pass && !c.skipped).length + plan.numeric.filter((n) => !n.pass).length;
		const report = {
			when: new Date().toISOString(),
			userAgent: navigator.userAgent,
			adapter: state.adapter,
			backend: state.backend,
			canvasFormat: state.canvasFormat,
			devicePixelRatio: window.devicePixelRatio,
			screen: [window.innerWidth, window.innerHeight],
			limits: state.limits,
			features: state.features,
			cases: state.cases,
			numeric: plan.numeric,
			failed,
			errors,
			seconds: round((performance.now() - state.started) / 1000),
		};
		window.__devcheckReport = report;
		fetch('/report', { method: 'POST', body: JSON.stringify(report) })
			.then((r) => r.ok ? r.text() : Promise.reject(r.status))
			.then((id) => render((failed ? failed + ' failed' : 'All passed') + ' · report sent (' + id + ')'))
			.catch(() => render((failed ? failed + ' failed' : 'All passed') + ' · report not sent'));
	}

	const waitDone = setInterval(() => {
		if (window.__devcheckDone) {
			clearInterval(waitDone);
			finish();
		}
	}, 250);
}());
