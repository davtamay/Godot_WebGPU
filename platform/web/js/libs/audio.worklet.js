/**************************************************************************/
/*  audio.worklet.js                                                      */
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

class RingBuffer {
	constructor(p_buffer, p_state, p_threads) {
		this.buffer = p_buffer;
		this.avail = p_state;
		this.threads = p_threads;
		this.rpos = 0;
		this.wpos = 0;
	}

	data_left() {
		return this.threads ? Atomics.load(this.avail, 0) : this.avail;
	}

	space_left() {
		return this.buffer.length - this.data_left();
	}

	read(output) {
		const size = this.buffer.length;
		const channels = output.length;
		const frames = output[0].length;
		const samples = channels * frames;
		// Deinterleave directly from the ring, without a scratch copy or views.
		// Split each channel at the wrap so the inner loops stay contiguous
		// in frame order, including rings not aligned to a channel boundary.
		for (let ch = 0; ch < channels; ch++) {
			const dest = output[ch];
			let pos = this.rpos + ch;
			const high = Math.min(frames, Math.max(0, Math.ceil((size - pos) / channels)));
			for (let frame = 0; frame < high; frame++, pos += channels) {
				dest[frame] = this.buffer[pos];
			}
			pos -= size;
			for (let frame = high; frame < frames; frame++, pos += channels) {
				dest[frame] = this.buffer[pos];
			}
		}
		this.rpos += samples;
		if (this.rpos >= size) {
			this.rpos -= size;
		}
		if (this.threads) {
			Atomics.add(this.avail, 0, -samples);
			// The mixer waits on the process notifier, not this sample count.
		} else {
			this.avail -= samples;
		}
	}

	write(p_buffer) {
		const to_write = p_buffer.length;
		const mw = this.buffer.length - this.wpos;
		if (mw >= to_write) {
			this.buffer.set(p_buffer, this.wpos);
			this.wpos += to_write;
			if (mw === to_write) {
				this.wpos = 0;
			}
		} else {
			const high = p_buffer.subarray(0, mw);
			const low = p_buffer.subarray(mw);
			this.buffer.set(high, this.wpos);
			this.buffer.set(low);
			this.wpos = low.length;
		}
		if (this.threads) {
			Atomics.add(this.avail, 0, to_write);
			// The mixer waits on the process notifier, not this sample count.
		} else {
			this.avail += to_write;
		}
	}
}

class GodotProcessor extends AudioWorkletProcessor {
	constructor() {
		super();
		this.threads = false;
		this.running = true;
		this.lock = null;
		this.notifier = null;
		this.output = null;
		this.pending_read = 0;
		this.input = null;
		this.input_buffer = new Float32Array();
		this.port.onmessage = (event) => {
			const cmd = event.data['cmd'];
			const data = event.data['data'];
			this.parse_message(cmd, data);
		};
	}

	process_notify() {
		if (this.notifier) {
			Atomics.add(this.notifier, 0, 1);
			Atomics.notify(this.notifier, 0);
		}
	}

	parse_message(p_cmd, p_data) {
		if (p_cmd === 'start' && p_data) {
			const state = p_data[0];
			let idx = 0;
			this.threads = true;
			this.lock = state.subarray(idx, ++idx);
			this.notifier = state.subarray(idx, ++idx);
			const avail_in = state.subarray(idx, ++idx);
			const avail_out = state.subarray(idx, ++idx);
			this.input = new RingBuffer(p_data[1], avail_in, true);
			this.output = new RingBuffer(p_data[2], avail_out, true);
		} else if (p_cmd === 'stop') {
			this.pending_read = 0;
			this.running = false;
			this.output = null;
			this.input = null;
			this.lock = null;
			this.notifier = null;
		} else if (p_cmd === 'start_nothreads') {
			this.pending_read = 0;
			this.output = new RingBuffer(p_data[0], p_data[0].length, false);
		} else if (p_cmd === 'chunk') {
			this.output.write(p_data);
		}
	}

	static array_has_data(arr) {
		return arr.length && arr[0].length && arr[0][0].length;
	}

	process(inputs, outputs, parameters) {
		if (!this.running) {
			return false; // Stop processing.
		}
		if (this.output === null) {
			return true; // Not ready yet, keep processing.
		}
		const process_input = GodotProcessor.array_has_data(inputs);
		// Keep capture latency unchanged; output-only mixing can be batched.
		let notify = process_input;
		if (process_input) {
			const input = inputs[0];
			const chunk = input[0].length * input.length;
			if (this.input_buffer.length !== chunk) {
				this.input_buffer = new Float32Array(chunk);
			}
			if (!this.threads) {
				GodotProcessor.write_input(this.input_buffer, input);
				this.port.postMessage({ 'cmd': 'input', 'data': this.input_buffer });
			} else if (this.input.space_left() >= chunk) {
				GodotProcessor.write_input(this.input_buffer, input);
				this.input.write(this.input_buffer);
			} else {
				// this.port.postMessage('Input buffer is full! Skipping input frame.'); // Uncomment this line to debug input buffer.
			}
		}
		const process_output = GodotProcessor.array_has_data(outputs);
		if (process_output) {
			const output = outputs[0];
			const chunk = output[0].length * output.length;
			if (this.output.data_left() >= chunk) {
				this.output.read(output);
				if (!this.threads) {
					this.pending_read += chunk;
					// Amortize the main-thread round trip without changing the ring size.
					// Keep at least three quarters of the buffer as scheduling headroom,
					// and never batch more than four render quanta. Small buffers and
					// recovery from an underrun still request a refill immediately.
					const batch = Math.min(Math.floor(this.output.buffer.length / (4 * chunk)) * chunk, 4 * chunk);
					if (this.pending_read >= batch || this.output.data_left() < chunk) {
						this.port.postMessage({ 'cmd': 'read', 'data': this.pending_read });
						this.pending_read = 0;
					}
				}
			} else {
				// this.port.postMessage('Output buffer has not enough frames! Skipping output frame.'); // Uncomment this line to debug output buffer.
			}
			if (this.threads) {
				// Use the same bounded refill batch as the non-threaded path. The
				// mixer waits for this notification instead of polling the ring.
				const batch = Math.max(chunk, Math.min(Math.floor(this.output.buffer.length / (4 * chunk)) * chunk, 4 * chunk));
				notify = notify || this.output.space_left() >= batch;
			}
		}
		if (notify) {
			this.process_notify();
		}
		return true;
	}

	static write_input(dest, source) {
		const channels = source.length;
		for (let ch = 0; ch < channels; ch++) {
			for (let sample = 0; sample < source[ch].length; sample++) {
				dest[sample * channels + ch] = source[ch][sample];
			}
		}
	}
}

registerProcessor('godot-processor', GodotProcessor);
