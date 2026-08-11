# Builds main.tscn for the bake fixture.
#
# The scene is generated rather than hand-written because a hand-authored
# .tscn can carry property combinations the editor would strip on save (an
# unshaded material with emission, for instance), and a stripped property
# changes the material's generated shader - which is exactly what this fixture
# is measuring. Generating it through PackedScene guarantees the committed
# file is what the engine itself would write.
#
#   godot --headless --path misc/webgpu_scripts/bake_fixture --script build_fixture.gd
#
# Run it again whenever the fixture should cover more shader families, then
# refresh expected-exclusions.txt from a local export.
extends SceneTree


func _init() -> void:
	var root := Node3D.new()
	root.name = "Fixture"

	var camera := Camera3D.new()
	camera.name = "Camera3D"
	camera.transform = Transform3D(Basis(), Vector3(0, 1.6, 4))
	root.add_child(camera)

	# Directional light with shadows: pulls in the shadow-pass variants.
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.shadow_enabled = true
	sun.transform = Transform3D(Basis.from_euler(Vector3(-0.9, 0.6, 0)), Vector3(0, 3, 0))
	root.add_child(sun)

	# An omni light with shadows covers the cubemap shadow path as well.
	var lamp := OmniLight3D.new()
	lamp.name = "Lamp"
	lamp.shadow_enabled = true
	lamp.omni_range = 8.0
	lamp.transform = Transform3D(Basis(), Vector3(1.5, 2.0, 1.5))
	root.add_child(lamp)

	# A metallic, normal-mapped surface exercises the main scene shader.
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.85, 0.45, 0.15)
	material.metallic = 0.8
	material.roughness = 0.3

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.name = "Box"
	mesh_instance.mesh = BoxMesh.new()
	mesh_instance.material_override = material
	mesh_instance.transform = Transform3D(Basis(), Vector3(0, 1, 0))
	root.add_child(mesh_instance)

	var floor_material := StandardMaterial3D.new()
	floor_material.albedo_color = Color(0.8, 0.8, 0.85)

	var floor_mesh := MeshInstance3D.new()
	floor_mesh.name = "Floor"
	var plane := PlaneMesh.new()
	plane.size = Vector2(12, 12)
	floor_mesh.mesh = plane
	floor_mesh.material_override = floor_material
	root.add_child(floor_mesh)

	# Transparency reaches the alpha pipeline variants.
	var glass := StandardMaterial3D.new()
	glass.albedo_color = Color(0.3, 0.6, 1.0, 0.45)
	glass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA

	var glass_mesh := MeshInstance3D.new()
	glass_mesh.name = "Glass"
	glass_mesh.mesh = SphereMesh.new()
	glass_mesh.material_override = glass
	glass_mesh.transform = Transform3D(Basis(), Vector3(-1.6, 1, 0))
	root.add_child(glass_mesh)

	# GPU particles bring in the particle compute shaders, which are the ones
	# that need Tint's atomic-load taint recovered.
	var process_material := ParticleProcessMaterial.new()
	process_material.direction = Vector3(0, 1, 0)
	process_material.spread = 25.0
	process_material.initial_velocity_min = 1.0
	process_material.initial_velocity_max = 2.0
	process_material.gravity = Vector3(0, -1.5, 0)

	var particles := GPUParticles3D.new()
	particles.name = "Sparks"
	particles.amount = 64
	particles.process_material = process_material
	particles.draw_pass_1 = QuadMesh.new()
	particles.transform = Transform3D(Basis(), Vector3(1.6, 1, 0))
	root.add_child(particles)

	# Sky and glow: the sky shader plus the post-processing chain.
	var sky_material := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_material

	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.glow_enabled = true
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	environment.fog_enabled = true
	environment.fog_density = 0.01

	var world_environment := WorldEnvironment.new()
	world_environment.name = "WorldEnvironment"
	world_environment.environment = environment
	root.add_child(world_environment)

	# 2D on top of 3D: the canvas shaders and the font pipeline.
	var label := Label.new()
	label.name = "Label"
	label.text = "WebGPU bake fixture"
	var layer := CanvasLayer.new()
	layer.name = "HUD"
	layer.add_child(label)
	root.add_child(layer)

	for node in [camera, sun, lamp, mesh_instance, floor_mesh, glass_mesh, particles, world_environment, layer]:
		node.owner = root
	label.owner = root

	var packed := PackedScene.new()
	var err := packed.pack(root)
	if err != OK:
		push_error("pack failed: %d" % err)
		quit(1)
		return
	err = ResourceSaver.save(packed, "res://main.tscn")
	if err != OK:
		push_error("save failed: %d" % err)
		quit(1)
		return
	print("wrote res://main.tscn")
	quit(0)
