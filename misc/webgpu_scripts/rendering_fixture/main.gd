extends Node3D
var cases := ["baseline", "fsr1", "fsr2", "fsr2_motion", "fsr2_resize", "fog", "ssao_ssil", "ssr", "sdfgi", "reflection_probe"]
var index := -1
var frames := 0
var waiting := false
var complete := false
var reports: Array = []

func _ready() -> void:
	_next()

func _next() -> void:
	index += 1
	if index >= cases.size():
		complete = true
		print("RENDERING_COVERAGE_RESULT=" + JSON.stringify({"engine": Engine.get_version_info().hash, "cases":reports, "method":RenderingServer.get_current_rendering_method()}))
		if not OS.has_feature("web"):
			get_tree().quit()
		return
	frames = 0
	waiting = false
	var vp := get_viewport()
	var env: Environment = $WorldEnvironment.environment
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0
	env.volumetric_fog_enabled = false
	env.ssao_enabled = false
	env.ssil_enabled = false
	env.ssr_enabled = false
	env.sdfgi_enabled = false
	$Probe.visible = false
	$Camera3D.position = Vector3(5, 3, 7)
	$Camera3D.look_at(Vector3(0, 0.5, 0))
	var name: String = cases[index]
	if name.begins_with("fsr"):
		vp.scaling_3d_scale = 0.65
		vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR if name == "fsr1" else Viewport.SCALING_3D_MODE_FSR2
	if name == "fog": env.volumetric_fog_enabled = true
	if name == "ssao_ssil":
		env.ssao_enabled = true
		env.ssil_enabled = true
	if name == "ssr": env.ssr_enabled = true
	if name == "sdfgi": env.sdfgi_enabled = true
	if name == "reflection_probe": $Probe.visible = true
	print("RENDERING_CASE_START=" + name)

func _process(_delta: float) -> void:
	if complete: return
	if waiting:
		if OS.has_feature("web") and int(JavaScriptBridge.eval("window.__renderingAck === undefined ? -1 : window.__renderingAck", true)) == index:
			_next()
		return
	frames += 1
	if cases[index] == "fsr2_motion" and frames < 30:
		$Camera3D.position.x = 5.0 + sin(frames * 0.12) * 0.4
		$Camera3D.look_at(Vector3(0, 0.5, 0))
	if cases[index] == "fsr2_resize" and frames == 15:
		get_viewport().scaling_3d_scale = 0.8
	if frames == 65:
		reports.append({"name":cases[index],"frames":frames,"scale":get_viewport().scaling_3d_scale,"scaling_mode":get_viewport().scaling_3d_mode})
		if OS.has_feature("web"):
			waiting = true
			print("RENDERING_CASE_READY=" + str(index))
		else:
			await RenderingServer.frame_post_draw
			get_viewport().get_texture().get_image().save_png("user://coverage-" + cases[index] + ".png")
			_next()
