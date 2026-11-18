# Device check: renders a fixed series of cases with large flat-colour regions
# and samples each at a few normalized points.
#
# Native run (desktop Vulkan, from the editor binary): writes the reference
# colours to res://golden.json.
# Web run: for each case it asks the page (devcheck.js) to capture the canvas
# through WebGPU and compare the samples with golden.json, then hands over the
# numeric checks. The page shows the results and posts a report.
extends Node3D

const SETTLE_FRAMES := 90
const ACK_TIMEOUT_FRAMES := 900

const MAT_UNSHADED := preload("res://mat_unshaded.tres")
const MAT_SHADED := preload("res://mat_shaded.tres")
const MAT_ALPHA := preload("res://mat_alpha.tres")
const SKY_BANDS := preload("res://sky_bands.tres")
const PROC_SKY := preload("res://procsky.tres")
const SHAPES_HUD := preload("res://shapes_hud.gd")

var _web := OS.has_feature("web")
var _golden := {}
var _root: Node

func _ready() -> void:
	await get_tree().process_frame
	var cases := _cases()
	var numeric := _numeric_checks()
	if _web:
		JavaScriptBridge.eval("window.__devcheckPlan = %s" % JSON.stringify({"cases": cases.map(func(c): return c.name), "numeric": numeric}))
	for i in cases.size():
		var c: Dictionary = cases[i]
		_root = Node3D.new()
		add_child(_root)
		var vp := get_viewport()
		vp.msaa_3d = c.get("msaa", Viewport.MSAA_DISABLED)
		vp.scaling_3d_scale = c.get("scale", 1.0)
		c.build.call(_root)
		for f in SETTLE_FRAMES:
			await get_tree().process_frame
		if _web:
			var want := {"id": i, "name": c.name, "samples": c.get("samples", []), "perf": c.get("perf", false)}
			JavaScriptBridge.eval("window.__devcheckWant = %s" % JSON.stringify(want))
			for f in ACK_TIMEOUT_FRAMES:
				await get_tree().process_frame
				if int(JavaScriptBridge.eval("window.__devcheckAck === undefined ? -1 : window.__devcheckAck")) == i:
					break
		else:
			await RenderingServer.frame_post_draw
			var img := vp.get_texture().get_image()
			var cols := []
			for s in c.get("samples", []):
				var px := Vector2i(clampi(int(s[0] * img.get_width()), 0, img.get_width() - 1), clampi(int(s[1] * img.get_height()), 0, img.get_height() - 1))
				var col := img.get_pixelv(px)
				cols.append([roundi(col.r8), roundi(col.g8), roundi(col.b8)])
			_golden[c.name] = cols
			print("DEVCHECK golden %s %s" % [c.name, cols])
		_root.queue_free()
		await get_tree().process_frame
	vp_reset()
	if _web:
		JavaScriptBridge.eval("window.__devcheckDone = true")
	else:
		var f := FileAccess.open("res://golden.json", FileAccess.WRITE)
		f.store_string(JSON.stringify(_golden, "\t"))
		f.close()
		print("DEVCHECK golden written (%d cases)" % _golden.size())
		get_tree().quit()

func vp_reset() -> void:
	get_viewport().msaa_3d = Viewport.MSAA_DISABLED
	get_viewport().scaling_3d_scale = 1.0

# --- numeric checks (answers known exactly) ---------------------------------

func _numeric_checks() -> Array:
	var out := []
	# GPU readback: a SphereMesh built on the GPU, read back.
	var sphere := SphereMesh.new()
	sphere.radius = 0.5
	sphere.height = 1.0
	sphere.radial_segments = 10
	sphere.rings = 5
	var arrays := sphere.get_mesh_arrays()
	var v0: Vector3 = arrays[Mesh.ARRAY_VERTEX][0]
	var idx: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
	var max_idx := 0
	for v in idx:
		max_idx = maxi(max_idx, v)
	var ok: bool = v0.is_equal_approx(Vector3(0, 0.5, 0)) and max_idx == arrays[Mesh.ARRAY_VERTEX].size() - 1
	out.append({"name": "mesh_readback", "pass": ok, "detail": "v0=%s max_index=%d vertices=%d" % [v0, max_idx, arrays[Mesh.ARRAY_VERTEX].size()]})
	return out

# --- cases --------------------------------------------------------------------

func _camera(root: Node, pos := Vector3(0, 0, 5), target := Vector3.ZERO, far := 4000.0) -> Camera3D:
	var cam := Camera3D.new()
	cam.far = far
	root.add_child(cam)
	cam.look_at_from_position(pos, target, Vector3.UP if absf(pos.normalized().y) < 0.99 else Vector3.FORWARD)
	cam.current = true
	return cam

func _env(root: Node, sky_material: Material = null, bg := Color(0.3, 0.3, 0.3)) -> void:
	var env := Environment.new()
	if sky_material != null:
		env.background_mode = Environment.BG_SKY
		var sky := Sky.new()
		sky.sky_material = sky_material
		env.sky = sky
	else:
		env.background_mode = Environment.BG_COLOR
		env.background_color = bg
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var we := WorldEnvironment.new()
	we.environment = env
	root.add_child(we)

func _quad(root: Node, mat: Material, color: Color, center: Vector3, size: Vector2) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var q := QuadMesh.new()
	q.size = size
	mi.mesh = q
	var m = mat.duplicate()
	m.albedo_color = color
	mi.material_override = m
	mi.position = center
	root.add_child(mi)
	return mi

func _halves(root: Node) -> void:
	_camera(root)
	_env(root)
	_quad(root, MAT_UNSHADED, Color(1, 0, 0), Vector3(-10, 0, 0), Vector2(20, 40))
	_quad(root, MAT_UNSHADED, Color(0, 0, 1), Vector3(10, 0, 0), Vector2(20, 40))

func _cases() -> Array:
	var halves := [[0.3, 0.5], [0.7, 0.5]]
	var bands := [[0.5, 0.1], [0.5, 0.9]]
	return [
		{"name": "unshaded", "build": _halves, "samples": halves},
		{"name": "msaa2", "build": _halves, "samples": halves, "msaa": Viewport.MSAA_2X},
		{"name": "msaa4", "build": _halves, "samples": halves, "msaa": Viewport.MSAA_4X},
		{"name": "alpha_blend", "build": _alpha_blend, "samples": halves},
		{"name": "sky_bands", "build": _sky_bands, "samples": bands},
		{"name": "sky_bands_scaled", "build": _sky_bands, "samples": bands, "scale": 0.8},
		{"name": "procedural_sky", "build": _proc_sky, "samples": [[0.5, 0.05], [0.5, 0.45], [0.5, 0.95]]},
		{"name": "procedural_sky_msaa4_scaled", "build": _proc_sky, "samples": [[0.5, 0.05], [0.5, 0.45], [0.5, 0.95]], "msaa": Viewport.MSAA_4X, "scale": 0.8},
		{"name": "far_plane", "build": _far_plane, "samples": [[0.3, 0.5], [0.7, 0.1], [0.7, 0.9]]},
		{"name": "lit_shadow", "build": _lit_shadow, "samples": [[0.5, 0.56], [0.5, 0.87]]},
		{"name": "ortho_shadow_far", "build": _ortho_shadow_far, "samples": [[0.5, 0.53], [0.5, 0.9]]},
		{"name": "odd_index_mesh", "build": _odd_mesh, "samples": halves},
		{"name": "canvas_shapes", "build": _canvas_shapes, "samples": [[0.25, 0.5], [0.75, 0.5]]},
		{"name": "stress_2000", "build": _stress, "samples": [], "perf": true},
	]

func _alpha_blend(r: Node) -> void:
	_camera(r)
	_env(r)
	_quad(r, MAT_UNSHADED, Color(1, 0, 0), Vector3(0, 0, -0.5), Vector2(40, 40))
	_quad(r, MAT_ALPHA, Color(0, 1, 0, 0.5), Vector3(-10, 0, 0), Vector2(20, 40))

func _sky_bands(r: Node) -> void:
	_camera(r)
	_env(r, SKY_BANDS)

func _proc_sky(r: Node) -> void:
	_camera(r)
	_env(r, PROC_SKY)

func _far_plane(r: Node) -> void:
	# Geometry near the far plane with the sky behind it.
	_camera(r, Vector3(0, 0, 5), Vector3.ZERO, 100.0)
	_env(r, SKY_BANDS)
	_quad(r, MAT_UNSHADED, Color(1, 0, 1), Vector3(-200, 0, -90), Vector2(400, 1))

func _canvas_shapes(r: Node) -> void:
	_env(r)
	_camera(r)
	var layer := CanvasLayer.new()
	var hud := Control.new()
	hud.set_script(SHAPES_HUD)
	hud.set_anchors_preset(Control.PRESET_FULL_RECT)
	layer.add_child(hud)
	r.add_child(layer)

func _lit_shadow(r: Node) -> void:
	_camera(r, Vector3(0, 6, 0), Vector3(0, -1, 0))
	_env(r)
	var floor_mi := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	floor_mi.mesh = plane
	floor_mi.material_override = MAT_SHADED
	floor_mi.position = Vector3(0, -1, 0)
	r.add_child(floor_mi)
	var box := MeshInstance3D.new()
	var bm := BoxMesh.new()
	box.mesh = bm
	box.material_override = MAT_SHADED
	box.position = Vector3(0, 0.5, -1)
	r.add_child(box)
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	r.add_child(sun)
	sun.look_at_from_position(Vector3(0, 10, -10), Vector3.ZERO)

func _ortho_shadow_far(r: Node) -> void:
	# Orthogonal directional shadows computing only split 0: ground past the
	# shadow range is shaded with the matrices of splits never computed
	# (Dino Pals finding 8: black on Adreno, lit elsewhere).
	_camera(r, Vector3(0, 2, 0), Vector3(0, 0, -60))
	_env(r)
	var floor_mi := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(600, 600)
	floor_mi.mesh = plane
	floor_mi.material_override = MAT_SHADED
	r.add_child(floor_mi)
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
	sun.directional_shadow_max_distance = 20.0
	r.add_child(sun)
	sun.look_at_from_position(Vector3(10, 20, 10), Vector3.ZERO)

func _odd_mesh(r: Node) -> void:
	_camera(r)
	_env(r)
	# Three triangles = 9 sixteen-bit indices = 18 bytes, not a multiple of 4.
	var verts := PackedVector3Array([Vector3(-20, -20, 0), Vector3(0, -20, 0), Vector3(0, 20, 0), Vector3(-20, 20, 0)])
	var indices := PackedInt32Array([0, 2, 1, 0, 3, 2, 0, 3, 1])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_INDEX] = indices
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var mi := MeshInstance3D.new()
	mi.mesh = am
	var m = MAT_UNSHADED.duplicate()
	m.albedo_color = Color(1, 0.5, 0)
	mi.material_override = m
	r.add_child(mi)
	_quad(r, MAT_UNSHADED, Color(0, 0, 1), Vector3(10, 0, 0), Vector2(20, 40))

func _stress(r: Node) -> void:
	_camera(r, Vector3(0, 3, 8), Vector3(0, 0, -20))
	_env(r, PROC_SKY)
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	r.add_child(sun)
	sun.look_at_from_position(Vector3(5, 10, 5), Vector3.ZERO)
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	var bm := BoxMesh.new()
	for i in 2000:
		var mi := MeshInstance3D.new()
		mi.mesh = bm
		mi.material_override = MAT_SHADED
		mi.position = Vector3(rng.randf_range(-30, 30), rng.randf_range(0, 6), rng.randf_range(-60, 0))
		r.add_child(mi)
