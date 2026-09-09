@tool
extends EditorPlugin
## Writes an engine build profile (for `scons build_profile=`) by driving the
## editor's own "Detect from Project", so the result is exactly the set that
## dialog produces rather than a second implementation that can drift from it.
##
## A profile is a compile-time contract with one project: the classes it leaves
## out are gone from the binary, so it has to be regenerated whenever the
## project grows. That is what makes it worth scripting.
##
## Only the class half is decided here. Which build options a target may drop
## is the target's business, not the project's, so the profile's
## "disabled_build_options" are curated afterwards by the caller.
##
## Configured through the environment, because an editor run has no argument
## contract of its own:
##   GODOT_BUILD_PROFILE_OUT   absolute path of the .build file to write
##   GODOT_BUILD_PROFILE_KEEP  classes to force back on, comma separated
## Inert without GODOT_BUILD_PROFILE_OUT, so it is safe to leave enabled.

# Detection reads the file system, so a scan still in flight would silently
# describe a subset of the project.
const SETTLE_SECONDS := 3.0

var _out := ""
var _elapsed := 0.0
var _done := false


func _enter_tree() -> void:
	_out = OS.get_environment("GODOT_BUILD_PROFILE_OUT")
	set_process(not _out.is_empty())


func _process(delta: float) -> void:
	if _done:
		return
	_elapsed += delta
	if _elapsed < SETTLE_SECONDS:
		return
	var fs := EditorInterface.get_resource_filesystem()
	if fs != null and fs.is_scanning():
		return
	_done = true
	set_process(false)
	_run()


func _run() -> void:
	# Headless runs have no editor controls to search, and there is nothing
	# useful to do without them.
	var base := EditorInterface.get_base_control()
	if base == null:
		printerr("BUILD_PROFILE: no editor interface; run this in a windowed editor")
		get_tree().quit(1)
		return
	var manager := _find_manager(base)
	if manager == null:
		printerr("BUILD_PROFILE: no EditorBuildProfileManager in the editor tree")
		get_tree().quit(1)
		return
	if not manager.has_method("detect_from_project"):
		printerr("BUILD_PROFILE: this editor does not expose detect_from_project")
		get_tree().quit(1)
		return

	manager.detect_from_project()
	var profile: Object = manager.get_current_profile()
	if profile == null:
		printerr("BUILD_PROFILE: detection produced no profile")
		get_tree().quit(1)
		return

	var kept := 0
	for entry in _env_list("GODOT_BUILD_PROFILE_KEEP"):
		kept += _keep_class(profile, entry)

	var err: int = profile.save_to_file(_out)
	if err != OK:
		printerr("BUILD_PROFILE: could not write %s (error %d)" % [_out, err])
		get_tree().quit(1)
		return

	_report(kept)
	get_tree().quit(0)


func _report(p_kept: int) -> void:
	var text := FileAccess.get_file_as_string(_out)
	var data: Dictionary = JSON.parse_string(text) if not text.is_empty() else {}
	var disabled: Array = data.get("disabled_classes", [])
	var options: Dictionary = data.get("disabled_build_options", {})
	print("BUILD_PROFILE classes_disabled=%d options=%d kept=%d saved=%s"
			% [disabled.size(), options.size(), p_kept, _out])
	if not options.is_empty():
		print("BUILD_PROFILE detected_options=%s" % JSON.stringify(options))


## Re-enables a class and every disabled ancestor: a class counts as disabled
## when any of its parents is, so putting one back means putting the chain back.
func _keep_class(p_profile: Object, p_class: String) -> int:
	if not ClassDB.class_exists(p_class):
		printerr("BUILD_PROFILE: unknown class '%s'" % p_class)
		return 0
	var count := 0
	var walk := p_class
	while not walk.is_empty():
		if p_profile.is_class_disabled(walk):
			count += 1
		p_profile.set_disable_class(walk, false)
		walk = ClassDB.get_parent_class(walk)
	return count


func _env_list(p_name: String) -> PackedStringArray:
	var out := PackedStringArray()
	for entry in OS.get_environment(p_name).split(",", false):
		var trimmed := entry.strip_edges()
		if not trimmed.is_empty():
			out.append(trimmed)
	return out


func _find_manager(p_node: Node) -> Node:
	if p_node.get_class() == "EditorBuildProfileManager":
		return p_node
	for child in p_node.get_children():
		var found := _find_manager(child)
		if found != null:
			return found
	return null
