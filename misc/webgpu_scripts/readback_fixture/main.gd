extends Node2D

# Real GPU writes. Sources are released immediately after recording requests;
# callbacks must own their results and survive frame/staging reuse.
var expected: Dictionary = {}
var results: Dictionary = {}
var callback_counts: Dictionary = {}
var submitted := false
var finished := false
var frames_seen := 0
var rd: RenderingDevice


func _ready() -> void:
	RenderingServer.call_on_render_thread(_request)
	get_tree().create_timer(20.0).timeout.connect(_timeout)


func _process(_delta: float) -> void:
	frames_seen += 1


func _request() -> void:
	rd = RenderingServer.get_rendering_device()
	if rd == null:
		print('READBACK_RESULT=' + JSON.stringify({"passed": false, "results": {"device": {"state": "no-rendering-device"}}}))
		return
	var destination := rd.storage_buffer_create(4096)
	for iteration in range(24):
		var data := PackedByteArray()
		data.resize(4096)
		for i in range(data.size()):
			data[i] = (i * 17 + iteration * 31 + 1) % 256
		var source := rd.storage_buffer_create(data.size(), data)
		rd.buffer_copy(source, destination, 0, 0, data.size())
		var test := "buffer_%02d" % iteration
		var offset := iteration % 4
		var size := 4096 - offset - (iteration % 3)
		expected[test] = data.slice(offset, offset + size)
		var error := rd.buffer_get_data_async(destination, _received.bind(test), offset, size)
		if error != OK:
			_record.call_deferred(test, {"state": "request-error", "error": error})
		rd.free_rid(source)
	rd.free_rid(destination)
	_texture("rgba_odd", 5, 3, 1, 1, 1, false)
	_texture("r8_odd", 7, 3, 1, 1, 1, true)
	_texture("rgba_tiled", 257, 129, 1, 1, 1, false)
	_texture("rgba_mips", 8, 8, 1, 1, 4, false)
	_texture("rgba_volume", 5, 3, 2, 1, 1, false)
	_texture("rgba_array", 5, 3, 1, 3, 1, false)
	submitted = true


func _texture(test: String, width: int, height: int, depth: int, layers: int, mips: int, single_channel: bool) -> void:
	var format := RDTextureFormat.new()
	format.width = width
	format.height = height
	format.depth = depth
	format.array_layers = layers
	format.mipmaps = mips
	format.texture_type = RenderingDevice.TEXTURE_TYPE_3D if depth > 1 else (RenderingDevice.TEXTURE_TYPE_2D_ARRAY if layers > 1 else RenderingDevice.TEXTURE_TYPE_2D)
	format.format = RenderingDevice.DATA_FORMAT_R8_UNORM if single_channel else RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT
	var data := PackedByteArray()
	for mip in range(mips):
		var pixels: int = maxi(1, width >> mip) * maxi(1, height >> mip) * maxi(1, depth >> mip)
		for i in range(pixels):
			if depth > 1 or width > 256:
				# Different rows and slices expose incorrect pitch/z assembly.
				data.append_array(PackedByteArray([i % 251, int(i / width) % 251, 17 + mip * 31, 255]))
			elif mips > 1:
				data.append_array(PackedByteArray([255 if mip % 3 == 0 else 0, 255 if mip % 3 == 1 else 0, 255 if mip % 3 == 2 else 0, 255]))
			else:
				data.append(255)
				if not single_channel:
					data.append_array(PackedByteArray([0, 0, 255]))
	if depth == 1:
		format.usage_bits |= RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT
	var texture := rd.texture_create(format, RDTextureView.new())
	if depth > 1 or width > 256:
		# The current driver supports only zero clears on volumes. Use an
		# actual GPU texture copy to test nonzero volume data independently.
		var source := rd.texture_create(format, RDTextureView.new(), [data])
		rd.texture_copy(source, texture, Vector3.ZERO, Vector3.ZERO, Vector3(width, height, depth), 0, 0, 0, 0)
		rd.free_rid(source)
	elif mips > 1:
		for mip in range(mips):
			var color := Color(1 if mip % 3 == 0 else 0, 1 if mip % 3 == 1 else 0, 1 if mip % 3 == 2 else 0, 1)
			rd.texture_clear(texture, color, mip, 1, 0, layers)
	else:
		if layers > 1:
			rd.texture_clear(texture, Color(0, 0, 1, 1), 0, mips, 0, layers)
		rd.texture_clear(texture, Color(1, 0, 0, 1), 0, mips, layers - 1, 1)
	expected[test] = data
	var error := rd.texture_get_data_async(texture, layers - 1, _received.bind(test))
	if error != OK:
		_record.call_deferred(test, {"state": "request-error", "error": error})
	rd.free_rid(texture)


func _received(data: PackedByteArray, test: String) -> void:
	var state := "pass" if data == expected[test] else ("empty" if data.is_empty() else "mismatch")
	_record.call_deferred(test, {"state": state, "callback_received": true, "expected_size": expected[test].size(), "actual_size": data.size(), "expected_hash": expected[test].hex_encode().sha256_text(), "actual_hash": data.hex_encode().sha256_text()})


func _record(test: String, result: Dictionary) -> void:
	callback_counts[test] = callback_counts.get(test, 0) + 1
	results[test] = result
	if not finished and submitted and results.size() == expected.size():
		finished = true
		# Observe several more frames to catch duplicate callbacks and retain a
		# live render loop while asynchronous mapping is outstanding.
		get_tree().create_timer(0.5).timeout.connect(_report)


func _report() -> void:
	var passed := true
	for test in results:
		passed = passed and results[test].state == "pass" and callback_counts[test] == 1
	print("READBACK_RESULT=" + JSON.stringify({"passed": passed, "engine": Engine.get_version_info().hash, "frames_seen": frames_seen, "callback_counts": callback_counts, "results": results}))
	$Status.text = "Async GPU readback: PASS" if passed else "Async GPU readback: FAIL (see JSON report)"


func _timeout() -> void:
	if finished:
		return
	for test in expected:
		if not results.has(test):
			_record(test, {"state": "timeout"})
