extends Node

## Optional runtime gate for the upstream sample-count negotiation interface.
## Enable as an autoload in a scratch fixture, not in a shipping project.
## The browser harness must also reject validation errors and discarded frames.
func _ready() -> void:
	var original := get_viewport().msaa_3d
	for samples in [Viewport.MSAA_DISABLED, Viewport.MSAA_2X, Viewport.MSAA_4X, Viewport.MSAA_8X]:
		get_viewport().msaa_3d = samples
		for frame in range(30):
			await get_tree().process_frame
		print("MSAA_CASE_OK:", samples)
	get_viewport().msaa_3d = original
	print("MSAA_CYCLE_COMPLETE")
