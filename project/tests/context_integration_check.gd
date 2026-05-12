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

	var compatibility_summary: Dictionary = context.get_last_scan_results()
	if compatibility_summary.is_empty():
		failures.append("get_last_scan_results() should contain scan summary after scan_project().")
	elif compatibility_summary.has("files"):
		failures.append("Scan summary should not materialize full files array by default.")

	var forced_summary: Dictionary = context.start_scan({"force_rescan": true})
	if forced_summary.is_empty():
		failures.append("start_scan({force_rescan=true}) should return immediate scan lifecycle data.")

	var first_scan_id: int = int(forced_summary.get("scan_id", 0))
	if first_scan_id <= 0:
		failures.append("start_scan() should return a positive scan_id.")

	var forced_status: String = String(forced_summary.get("status", ""))
	if not (
		forced_status == "queued"
		or forced_status == "running"
		or forced_status == "already_running"
	):
		failures.append("start_scan() should return queued/running lifecycle status.")

	var first_status: Dictionary = _wait_for_scan_completion(context, first_scan_id, 60000)
	if first_status.is_empty():
		failures.append("get_scan_status(scan_id) should return lifecycle status.")
	elif String(first_status.get("status", "")) != "completed":
		failures.append("Async start_scan() should transition to completed status.")

	var first_summary: Dictionary = context.get_last_scan_results()
	if first_summary.is_empty():
		failures.append("get_last_scan_results() should contain scan summary after async completion.")
	elif not first_summary.has("scan_id"):
		failures.append("Scan summary should contain scan_id.")
	elif first_summary.has("files"):
		failures.append("Scan summary should not materialize full files array by default.")

	var first_metrics: Dictionary = context.get_scan_metrics(first_scan_id)
	if first_metrics.is_empty():
		failures.append("get_scan_metrics(scan_id) should return persisted metrics.")
	elif int(first_metrics.get("scripts_parsed", -1)) <= 0:
		failures.append("First scan should parse dirty script candidates.")

	var file_count: int = context.get_file_count({})
	if file_count <= 0:
		failures.append("get_file_count({}) should return indexed files.")

	var first_page: Array = context.get_files_page(0, 10, "path", {})
	if first_page.is_empty():
		failures.append("get_files_page() should return a visible page of rows.")
	elif first_page.size() > 10:
		failures.append("get_files_page() should respect the requested page limit.")

	var root_children: Array = context.get_directory_children(0, 0, 20, "path", {})
	if root_children.is_empty():
		failures.append("get_directory_children(0, ...) should return top-level entries.")

	var main_scene_rows: Array = context.get_files_page(0, 5, "path", {"search": "main.tscn"})
	var main_scene_entry: Dictionary = _find_file_entry(main_scene_rows, "res://main.tscn")
	if main_scene_entry.is_empty():
		failures.append("Paged file query should include res://main.tscn.")
	else:
		var main_scene_godot_type: String = String(main_scene_entry.get("godot_type", ""))
		if main_scene_godot_type != "PackedScene":
			failures.append(
				(
					"res://main.tscn cheap godot_type expected PackedScene, got %s."
					% main_scene_godot_type
				)
			)

		var main_scene_id: int = int(main_scene_entry.get("file_id", 0))
		var main_scene_details: Dictionary = context.get_file_details(main_scene_id)
		if main_scene_details.is_empty():
			failures.append("get_file_details(file_id) should return the indexed row.")

	var material_rows: Array = context.get_files_page(
		0, 5, "path", {"search": "type_probe_material.tres"}
	)
	var material_entry: Dictionary = _find_file_entry(
		material_rows, "res://tests/fixtures/type_probe_material.tres"
	)
	if material_entry.is_empty():
		failures.append("Paged file query should include the type probe resource.")
	else:
		var material_godot_type: String = String(material_entry.get("godot_type", ""))
		if material_godot_type != "Resource":
			failures.append(
				"Material cheap godot_type expected Resource, got %s." % material_godot_type
			)

	var custom_class_count: int = context.get_custom_class_count({})
	if custom_class_count <= 0:
		failures.append("get_custom_class_count({}) should include scanner_probe.gd.")

	var custom_classes: Array = context.get_custom_classes_page(0, 10, "class_name", {})
	var probe_class: Dictionary = _find_custom_class(custom_classes, "GotoolScannerProbe")
	if probe_class.is_empty():
		failures.append("get_custom_classes_page() should include GotoolScannerProbe.")
	elif String(probe_class.get("direct_base_type", "")) != "Resource":
		failures.append("GotoolScannerProbe direct_base_type should be Resource.")

	if not context.scan_current_project():
		failures.append("Second scan_current_project() should succeed after initialize_database().")

	var second_summary: Dictionary = context.get_last_scan_results()
	var second_scan_id: int = int(second_summary.get("scan_id", 0))
	var second_metrics: Dictionary = context.get_scan_metrics(second_scan_id)
	if int(second_metrics.get("scripts_parsed", -1)) != 0:
		failures.append("No-change rescan should parse zero unchanged scripts.")

	var debug_inventory: Dictionary = context.export_full_inventory_for_debug()
	if debug_inventory.is_empty():
		failures.append(
			"export_full_inventory_for_debug() should materialize explicit debug inventory."
		)
	elif not (debug_inventory.get("files", null) is Array):
		failures.append("Debug inventory should contain files array.")

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


func _find_custom_class(custom_classes: Array, expected_class_name: String) -> Dictionary:
	for entry: Dictionary in custom_classes:
		if String(entry.get("class_name", "")) == expected_class_name:
			return entry

	return {}


func _wait_for_scan_completion(
		context: GodotProjectContext,
		scan_id: int,
		timeout_ms: int = 30000
	) -> Dictionary:
	var started_ms: int = Time.get_ticks_msec()
	var status: Dictionary = {}

	while Time.get_ticks_msec() - started_ms <= timeout_ms:
		status = context.get_scan_status(scan_id)
		var value: String = String(status.get("status", ""))
		if not (value == "queued" or value == "running" or value == "already_running"):
			return status

		OS.delay_msec(25)

	return status
