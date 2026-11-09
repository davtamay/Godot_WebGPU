# Making a game for the WebGPU web export

What a game needs to know to ship on this renderer. Most of it came from
building Dino Pals, a mobile game made on this stack to find exactly these
problems.

## Project settings

Set both lines, or a web export silently boots the OpenGL renderer:

```ini
[rendering]
renderer/rendering_method="mobile"
renderer/rendering_method.web="mobile"
rendering_device/driver.web="webgpu"
```

Export single-threaded (`variant/thread_support=false`) so the game runs on
any static host, without cross-origin isolation headers. Turn on
`shader_baker/enabled` in the Web preset; the `godot_webgpu` addon's export
toggle does both.

## Materials are baked at export

Browsers cannot compile Godot's shaders at runtime, so every shader the game
draws with is translated when it is exported. A material the export never saw
renders invisible, and the console says the shader is "missing from the baked
shader cache".

- Author materials as `.tres` files. Changing colours, textures or numbers on
  a duplicate at runtime is fine; turning features on or off (transparency,
  emission, unshaded, billboard) creates a different shader and is not.
- A material only ever made in code needs its `.tres` twin referenced from a
  `BakeAnchor` node (the `godot_webgpu` addon) in an exported scene.
- `Label3D` and `Sprite3D` build internal materials; `FontBakeAnchor` covers
  text.
- If an export prints that a shader variant was excluded, or a shader failed
  to translate, read the line: the variant is missing from the game.

## Things that now work, and what changed

- Meshes built at runtime with any index count, including odd ones
  (patch 199).
- `msaa_3d` at 2x or 8x runs as 4x; WebGPU has no other counts (200).
- HUDs drawn with `draw_circle`, `draw_arc` and `draw_colored_polygon` every
  frame no longer cost tens of milliseconds a frame (201).
- `Mesh.surface_get_arrays()` and `PrimitiveMesh.get_mesh_arrays()` return
  real data (204). Reading back a buffer a shader writes (storage) is still
  unsupported and says so on the console.
- Orthogonal and 2-split directional shadows no longer draw the ground black
  past the shadow range on Adreno phones (203).

## The GPU can be lost

Phones drop the GPU of a tab that goes to the background. The page then
reloads (after the user returns, if the tab is hidden). Save before that
happens by listening for the page's `godot-webgpu-device-lost` event; the
reload waits up to 5 seconds for promises pushed into `detail.pending`:

```gdscript
var _on_lost_cb := JavaScriptBridge.create_callback(_on_device_lost)

func _ready() -> void:
	if OS.has_feature("web"):
		JavaScriptBridge.get_interface("window").addEventListener("godot-webgpu-device-lost", _on_lost_cb)

func _on_device_lost(_args: Array) -> void:
	save_game() # Write to user:// as usual.
```

`?noreload` on the page URL keeps the page up to read the report.

## Measuring

- A headed desktop Chrome window that is not focused caps the frame rate at
  32 fps, so desktop frame-rate numbers mean little; measure frame time, or
  measure on the phone.
- `?perfcounters` prints the driver's counters every 120 frames and sets
  `window.__godotWebGPUPerf`.
- Many URL switches turn one optimization off for an A/B comparison
  (`?nobundles`, `?nolazyunmap`, `?nopcdiff` and others; see the patch table
  in `webgpu-patch-stack.md`).
- The device check (`testbed/devcheck`, outside this repository) runs a set
  of rendering cases on any phone and reports pixel mismatches against
  desktop reference colours.
