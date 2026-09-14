# WebXR bootstrap for the headset smoke fixture.
#
# Entering an immersive session needs a user gesture, so the session starts
# from the Enter VR button. Everything the checklist asks for is printed to
# the console and mirrored on the HUD label so it can be read on the device.
extends Node3D

@onready var status: Label = $HUD/Panel/VBox/Status
@onready var enter_button: Button = $HUD/Panel/VBox/EnterVR
@onready var counter: Label3D = $Counter

var webxr: WebXRInterface
var frames := 0


func _ready() -> void:
	webxr = XRServer.find_interface("WebXR")
	enter_button.disabled = true
	if webxr == null:
		status.text = "No WebXR interface: this is not a web export."
		return

	webxr.session_supported.connect(_on_session_supported)
	webxr.session_started.connect(_on_session_started)
	webxr.session_ended.connect(_on_session_ended)
	webxr.session_failed.connect(_on_session_failed)
	enter_button.pressed.connect(_on_enter_pressed)

	status.text = "Checking immersive-vr support..."
	webxr.is_session_supported("immersive-vr")


func _on_session_supported(session_mode: String, supported: bool) -> void:
	if session_mode != "immersive-vr":
		return
	enter_button.disabled = not supported
	if supported:
		status.text = "immersive-vr is supported: press Enter VR."
	else:
		status.text = "immersive-vr is not supported by this browser."


func _on_enter_pressed() -> void:
	webxr.session_mode = "immersive-vr"
	webxr.requested_reference_space_types = "bounded-floor, local-floor, local"
	webxr.required_features = "local-floor"
	webxr.optional_features = "bounded-floor, hand-tracking"
	if not webxr.initialize():
		status.text = "WebXRInterface.initialize() returned false."


func _on_session_started() -> void:
	get_viewport().use_xr = true
	# get_view_count() is what the renderer draws per pass: 1 on the per-view
	# path (WebGPU, or WebGL without multiview), 2 with WebGL multiview.
	var line := "session started: space=%s renderer_views=%d target=%s features=%s" % [
		webxr.reference_space_type,
		webxr.get_view_count(),
		webxr.get_render_target_size(),
		webxr.enabled_features,
	]
	print("XR smoke: " + line)
	status.text = line


func _on_session_ended() -> void:
	get_viewport().use_xr = false
	status.text = "session ended: press Enter VR to start another (resume check)."
	print("XR smoke: session ended")


func _on_session_failed(message: String) -> void:
	status.text = "session failed: " + message
	print("XR smoke: session failed: " + message)


func _process(_delta: float) -> void:
	# Both eyes must show the same, live number: a stale eye means a pass
	# stopped rendering.
	frames += 1
	counter.text = str(frames)
