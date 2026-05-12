extends SceneTree


func _initialize() -> void:
	var failures: PackedStringArray = PackedStringArray()
	var context: GodotProjectContext = GodotProjectContext.new()

	if context.scan_project():
		failures.append("scan_project() should fail before initialize_database().")

	if context.get_last_error().is_empty():
		failures.append("scan_project() before initialization must populate last_error.")

	if not context.initialize_database():
		failures.append("initialize_database() should succeed.")

	var project_id: int = context.register_current_project()
	if project_id <= 0:
		failures.append(
			"register_current_project() should return a positive project id after initialization."
		)

	var database_absolute_path: String = context.get_database_absolute_path()
	var database_virtual_path: String = context.get_database_virtual_path()

	if database_virtual_path.is_empty():
		failures.append("get_database_virtual_path() should not be empty after initialization.")

	if database_absolute_path.is_empty():
		failures.append("get_database_absolute_path() should not be empty after initialization.")

	if not FileAccess.file_exists(database_absolute_path):
		failures.append("Database file should exist after initialization.")

	if not context.scan_project():
		failures.append("scan_project() should succeed after initialize_database().")

	if not context.scan_current_project():
		failures.append("scan_current_project() should succeed after initialize_database().")

	var inventory: Dictionary = context.get_last_scan_results()

	if not inventory.has("files"):
		failures.append("Inventory should contain files key.")
	if not inventory.has("autoloads"):
		failures.append("Inventory should contain autoloads key.")
	if not inventory.has("custom_classes"):
		failures.append("Inventory should contain custom_classes key.")

	if inventory.get("files", null) == null or not (inventory["files"] is Array):
		failures.append("Inventory files should be an Array.")
	if inventory.get("autoloads", null) == null or not (inventory["autoloads"] is Array):
		failures.append("Inventory autoloads should be an Array.")
	if inventory.get("custom_classes", null) == null or not (inventory["custom_classes"] is Array):
		failures.append("Inventory custom_classes should be an Array.")

	var files: Array = inventory.get("files", [])
	var main_scene_entry: Dictionary = _find_file_entry(files, "res://main.tscn")
	if main_scene_entry.is_empty():
		failures.append("Inventory should include res://main.tscn.")
	else:
		var main_scene_godot_type: String = String(main_scene_entry.get("godot_type", ""))
		if main_scene_godot_type != "Node":
			failures.append(
				"res://main.tscn godot_type expected Node, got %s." % main_scene_godot_type
			)

	var material_entry: Dictionary = _find_file_entry(
		files, "res://tests/fixtures/type_probe_material.tres"
	)
	if material_entry.is_empty():
		failures.append("Inventory should include the StandardMaterial3D type probe resource.")
	else:
		var material_godot_type: String = String(material_entry.get("godot_type", ""))
		if material_godot_type != "StandardMaterial3D":
			failures.append(
				"Material godot_type expected StandardMaterial3D, got %s." % material_godot_type
			)

	if not context.scan_project():
		failures.append("Second scan_project() call should succeed.")

	var projects: Array = context.list_projects()
	if projects.is_empty():
		failures.append("list_projects() should include at least the current project.")

	var summary: Dictionary = context.get_project_summary(project_id)
	if summary.is_empty():
		failures.append("get_project_summary(project_id) should return project summary data.")
	elif not summary.has("project"):
		failures.append("Project summary should contain a project dictionary.")

	if not context.get_last_error().is_empty():
		failures.append("last_error should be empty after successful scan_project().")

	if failures.is_empty():
		print("PASS: context integration checks")
		quit(0)
		return

	push_error("FAIL: context integration checks")
	for failure: String in failures:
		push_error("- %s" % failure)

	quit(1)


func _find_file_entry(files: Array, path: String) -> Dictionary:
	for entry: Dictionary in files:
		if String(entry.get("path", "")) == path:
			return entry

	return {}
