extends Node2D
var case_index := 0
var pending := false
var results: Array = []
var expected: Dictionary
var readings: Dictionary
var remaining := 0
var previous_rid: RID
var last_generation := -1
var sampled: Texture2DArrayRD
var settle := -1
var awaiting_ack := false
var frame_count := 0

func _ready() -> void:
	get_tree().create_timer(90).timeout.connect(func(): print("DEPTH_INTEGRATION_TIMEOUT"))

func _process(_delta: float) -> void:
	frame_count += 1
	if settle >= 0:
		settle -= 1
		if settle == -1:
			print("DEPTH_SAMPLE_READY=" + str(case_index))
			awaiting_ack = true
		return
	if awaiting_ack:
		if int(JavaScriptBridge.eval("window.__depthSampleAck === undefined ? -1 : window.__depthSampleAck", true)) == case_index:
			awaiting_ack = false
			case_index += 1
			pending = false
		return
	if pending or not bool(JavaScriptBridge.eval("typeof window.__depthCaseSetup === 'function'", true)):
		return
	var payload := str(JavaScriptBridge.eval("JSON.stringify(window.__depthCaseSetup(%d))" % case_index, true))
	var value: Variant = JSON.parse_string(payload)
	if value == null:
		pending = true
		var passed := true
		for result in results:
			passed = passed and result.passed
		print("DEPTH_INTEGRATION_RESULT=" + JSON.stringify({"passed":passed,"engine":Engine.get_version_info().hash,"cases":results}))
		return
	if sampled:
		sampled.texture_rd_rid = RID()
	$Sample.visible = false
	expected = value
	pending = true
	readings = {}
	RenderingServer.call_on_render_thread(_request)

func _request() -> void:
	var xr := XRServer.find_interface("WebXR")
	var rd := RenderingServer.get_rendering_device()
	var info := xr.get_system_info()
	var metadata: Dictionary = info.get("webxr_depth", {})
	var valid: bool = str(metadata.get("status", "")) == expected.status
	valid = valid and int(metadata.get("valid_mask", 0)) == int(expected.mask)
	var rid: RID = info.get("webxr_depth_readback_array_rd", RID())
	var sample_rid: RID = info.get("webxr_depth_texture_array_rd", RID())
	var generation := int(metadata.get("generation", -1))
	if previous_rid.is_valid() and (generation != last_generation or expected.status != "ok"):
		valid = valid and not rd.texture_is_valid(previous_rid)
	previous_rid = rid
	last_generation = generation
	JavaScriptBridge.eval("window.__depthCaseExpire()", true) # Browser-owned textures expire now.
	if expected.status != "ok":
		valid = valid and not rid.is_valid() and not sample_rid.is_valid()
		_finish.call_deferred(valid, metadata, RID())
		return
	valid = valid and rid.is_valid() and sample_rid.is_valid()
	var again := xr.get_system_info()
	valid = valid and again.get("webxr_depth_readback_array_rd", RID()) == rid
	readings["metadata_valid"] = valid
	readings["metadata"] = metadata
	readings["sample_rid"] = sample_rid
	remaining = int(metadata.get("view_count", 0))
	for layer in range(remaining):
		var error := rd.texture_get_data_async(rid, layer, _received.bind(layer))
		if error != OK:
			_received(PackedByteArray(), layer)

func _received(data: PackedByteArray, layer: int) -> void:
	_received_main.call_deferred(data, layer)

func _received_main(data: PackedByteArray, layer: int) -> void:
	readings[str(layer)] = data.to_float32_array()
	remaining -= 1
	if remaining != 0:
		return
	var valid: bool = readings.metadata_valid
	for index in range(expected.values.size()):
		var actual: PackedFloat32Array = readings[str(index)]
		var wanted: Array = expected.values[index]
		valid = valid and actual.size() == wanted.size()
		if actual.size() == wanted.size():
			for i in range(actual.size()):
				valid = valid and absf(actual[i] - wanted[i]) <= maxf(0.00001, absf(wanted[i]) * 0.000001)
	_finish(valid, readings.metadata, readings.sample_rid)

func _finish(valid: bool, metadata: Dictionary, sample_rid: RID) -> void:
	results.append({"name":expected.name,"passed":valid,"status":metadata.get("status", ""),"mask":metadata.get("valid_mask", 0)})
	if sample_rid.is_valid():
		if sampled == null:
			sampled = Texture2DArrayRD.new()
		sampled.texture_rd_rid = sample_rid
		$Sample.material.set_shader_parameter("sensor", sampled)
		$Sample.material.set_shader_parameter("sensor_size", Vector2(metadata.width, metadata.height))
		$Sample.material.set_shader_parameter("sensor_layer", float(expected.sample_layer))
		$Sample.material.set_shader_parameter("expected_meters", float(expected.values[int(expected.sample_layer)][0]))
		$Sample.visible = true
		settle = 4
	else:
		if sampled:
			sampled.texture_rd_rid = RID()
		$Sample.visible = false
		case_index += 1
		pending = false
