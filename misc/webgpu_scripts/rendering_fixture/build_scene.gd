extends SceneTree
func _initialize() -> void:
	var scene := Node3D.new()
	scene.name = "Coverage"
	scene.set_script(load("res://main.gd"))
	var world := WorldEnvironment.new()
	world.name = "WorldEnvironment"
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	var sky_material := ProceduralSkyMaterial.new()
	sky.sky_material = sky_material
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.volumetric_fog_density = 0.06
	env.volumetric_fog_albedo = Color(0.5, 0.7, 1)
	env.sdfgi_min_cell_size = 0.2
	env.glow_enabled = true
	world.environment = env
	_add(scene,world)
	var camera := Camera3D.new()
	camera.name = "Camera3D"
	camera.position = Vector3(5,3,7)
	camera.rotation_degrees = Vector3(-19,35,0)
	_add(scene,camera)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-55,-30,0)
	light.shadow_enabled = true
	light.light_energy = 2
	_add(scene,light)
	for i in range(5):
		var mesh := MeshInstance3D.new()
		mesh.mesh = SphereMesh.new() if i % 2 == 0 else BoxMesh.new()
		mesh.position = Vector3((i-2)*1.25,0.5,0)
		mesh.gi_mode = GeometryInstance3D.GI_MODE_STATIC
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color.from_hsv(i*0.16,0.8,0.8)
		mat.metallic = 0.5
		mat.roughness = 0.2
		mesh.material_override = mat
		_add(scene,mesh)
	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(16,16)
	floor_mesh.mesh = plane
	floor_mesh.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.25,0.25,0.25)
	floor_mat.roughness = 0.15
	floor_mat.metallic = 0.8
	floor_mesh.material_override = floor_mat
	_add(scene,floor_mesh)
	var fog := FogVolume.new()
	fog.size = Vector3(4,3,4)
	var fog_mat := FogMaterial.new()
	fog_mat.density = 0.8
	fog_mat.albedo = Color(0.4,0.6,1)
	fog.material = fog_mat
	_add(scene,fog)
	var probe := ReflectionProbe.new()
	probe.name = "Probe"
	probe.size = Vector3(12,8,12)
	probe.position = Vector3(0,2,0)
	probe.visible = false
	_add(scene,probe)
	var packed := PackedScene.new()
	packed.pack(scene)
	var error := ResourceSaver.save(packed,"res://main.tscn")
	print("COVERAGE_SCENE_SAVED=" + str(error))
	scene.free()
	quit(error)
func _add(scene: Node, node: Node) -> void:
	scene.add_child(node)
	node.owner = scene
