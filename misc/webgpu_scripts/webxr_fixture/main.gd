extends Node3D
# Enters a session as soon as the browser allows one, then reports what reached
# the engine. Each GLUE line is something glue-smoke.mjs asserts on.

const WRIST_MOVE_LOG := 0.02

var webxr: XRInterface
var flat_size := Vector2()
var last_wrist := {}
var session_size_logged := false


func _ready() -> void:
	webxr = XRServer.find_interface("WebXR")
	if webxr == null:
		print("GLUE no WebXR interface")
		return
	webxr.session_supported.connect(_on_session_supported)
	webxr.session_started.connect(_on_session_started)
	webxr.session_ended.connect(_on_session_ended)
	webxr.session_failed.connect(func(message): print("GLUE session_failed ", message))
	webxr.selectstart.connect(func(id): print("GLUE selectstart ", id))
	webxr.selectend.connect(func(id): print("GLUE selectend ", id))
	webxr.select.connect(func(id): print("GLUE select ", id))
	webxr.is_session_supported("immersive-vr")


# The root's Window.size stays at the canvas size. The stuck value after a session
# was the Viewport's own, which only the visible rect reports.
func root_size() -> Vector2:
	return get_viewport().get_visible_rect().size


func _on_session_supported(mode: String, supported: bool) -> void:
	print("GLUE supported ", mode, " ", supported)
	if not supported:
		return
	flat_size = root_size()
	webxr.session_mode = mode
	webxr.required_features = "local-floor"
	webxr.optional_features = "hand-tracking"
	webxr.requested_reference_space_types = "local-floor, local"
	if not webxr.initialize():
		print("GLUE initialize failed")


func _on_session_started() -> void:
	$XROrigin3D/XRCamera3D.current = true
	get_viewport().use_xr = true
	print("GLUE session_started ", webxr.session_mode)


func _on_session_ended() -> void:
	# A camera left holding the session's two views errors in a flat viewport.
	$FlatCamera.current = true
	get_viewport().use_xr = false
	# The viewport reads back the window size one frame after the session goes.
	await get_tree().process_frame
	await get_tree().process_frame
	print("GLUE session_ended viewport_restored=", root_size() == flat_size)


func _process(_delta: float) -> void:
	if get_viewport().use_xr and not session_size_logged and root_size() != flat_size:
		session_size_logged = true
		print("GLUE session_viewport ", root_size())
	for side in ["left", "right"]:
		var tracker := XRServer.get_tracker(StringName("/user/hand_tracker/" + side)) as XRHandTracker
		if tracker == null or not tracker.has_tracking_data:
			if last_wrist.has(side):
				last_wrist.erase(side)
				print("GLUE hand_lost ", side)
			continue
		var flags := tracker.get_hand_joint_flags(XRHandTracker.HAND_JOINT_WRIST)
		if not (flags & XRHandTracker.HAND_JOINT_FLAG_POSITION_VALID):
			continue
		var wrist := tracker.get_hand_joint_transform(XRHandTracker.HAND_JOINT_WRIST).origin
		if not last_wrist.has(side) or wrist.distance_to(last_wrist[side]) > WRIST_MOVE_LOG:
			last_wrist[side] = wrist
			print("GLUE wrist ", side, " ", "%.2f" % wrist.x)
