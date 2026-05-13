class_name Main
extends Node

const PAGE_SIZE: int = 500
const POLL_INTERVAL_SECONDS: float = 0.05
const START_SCAN_TIMEOUT_SECONDS: float = 300.0
const GRAPH_SLICE_DEPTH: int = 2


func _ready() -> void:
	var scanner_context: GodotProjectContext = GodotProjectContext.new()
	var output_lines: Array[String] = []
	var failures: Array[String] = []

	if not scanner_context.initialize_database():
		_append_failure(
			failures,
			"initialize_database() failed: %s" % scanner_context.get_last_error()
		)
		_finish_with_output(scanner_context, output_lines, failures)
		return

	var scan_options: Dictionary = {
		"include_hidden": true,
		"force_rescan": true,
		"include_custom_classes": true,
		"load_existing_snapshot": true,
		"enable_parallel_traversal": true,
	}

	var scan_start_usec: int = Time.get_ticks_usec()
	var start_result: Dictionary = scanner_context.start_scan(scan_options)

	if start_result.is_empty():
		_append_failure(
			failures,
			"start_scan() returned empty result: %s" % scanner_context.get_last_error()
		)
		_finish_with_output(scanner_context, output_lines, failures)
		return

	var scan_id: int = int(start_result.get("scan_id", start_result.get("scan_run_id", 0)))

	if scan_id <= 0:
		_append_failure(
			failures,
			"start_scan() returned invalid scan id: %s" % JSON.stringify(start_result)
		)
		_finish_with_output(scanner_context, output_lines, failures)
		return

	var final_status: Dictionary = await _wait_for_scan(
		scanner_context,
		scan_id,
		START_SCAN_TIMEOUT_SECONDS
	)

	var scan_end_usec: int = Time.get_ticks_usec()
	var scan_elapsed_usec: int = scan_end_usec - scan_start_usec
	var scan_elapsed_ms: float = float(scan_elapsed_usec) / 1000.0
	var scan_elapsed_seconds: float = float(scan_elapsed_usec) / 1000000.0

	var status_text: String = String(final_status.get("status", "unknown"))
	if status_text != "completed":
		_append_failure(
			failures,
			"scan did not complete. status=%s last_error=%s"
			% [status_text, scanner_context.get_last_error()]
		)

	var metrics: Dictionary = scanner_context.get_scan_metrics(scan_id)

	var gd_scripts: Array = _load_all_files(
		scanner_context,
		{
			"is_directory": false,
			"extension": ".gd",
		}
	)

	var cs_scripts: Array = _load_all_files(
		scanner_context,
		{
			"is_directory": false,
			"extension": ".cs",
		}
	)

	var script_files: Array = []
	_append_array(script_files, gd_scripts)
	_append_array(script_files, cs_scripts)

	var custom_class_count: int = int(scanner_context.get_custom_class_count({}))
	var custom_classes: Array = _load_all_custom_classes(scanner_context, custom_class_count)

	var dependency_records_by_script: Array = []
	var total_dependency_rows: int = 0

	for script_entry: Dictionary in script_files:
		var script_file_id: int = int(script_entry.get("file_id", script_entry.get("id", 0)))
		if script_file_id <= 0:
			continue

		var dependencies: Array = scanner_context.list_dependencies_for_script(script_file_id)
		total_dependency_rows += dependencies.size()

		dependency_records_by_script.append(
			{
				"script_file_id": script_file_id,
				"path": String(script_entry.get("path", "")),
				"project_relative_path": String(script_entry.get("project_relative_path", "")),
				"dependency_count": dependencies.size(),
				"dependencies": dependencies,
			}
		)

	var unresolved_dependencies: Array = scanner_context.list_unresolved_dependencies()
	var dynamic_dependencies: Array = scanner_context.list_dynamic_dependencies()
	var dependency_cycles: Array = scanner_context.list_dependency_cycles()

	var graph_root_script_file_id: int = _find_first_script_with_dependencies(
		dependency_records_by_script
	)

	var dependency_graph_slice: Array = []
	if graph_root_script_file_id > 0:
		dependency_graph_slice = scanner_context.get_dependency_graph_slice(
			graph_root_script_file_id,
			GRAPH_SLICE_DEPTH
		)

	var debug_inventory: Dictionary = scanner_context.export_full_inventory_for_debug()

	_validate_script_scanner_results(
		failures,
		metrics,
		gd_scripts,
		custom_classes,
		dependency_records_by_script,
		total_dependency_rows,
		debug_inventory
	)

	output_lines.append("SCRIPT SCANNER TEST")
	output_lines.append("===================")
	output_lines.append("Result: %s" % ("PASS" if failures.is_empty() else "FAIL"))
	output_lines.append("Scan ID: %d" % scan_id)
	output_lines.append("Final status: %s" % status_text)
	output_lines.append("Measured wall time microseconds: %d" % scan_elapsed_usec)
	output_lines.append("Measured wall time milliseconds: %.3f" % scan_elapsed_ms)
	output_lines.append("Measured wall time seconds: %.6f" % scan_elapsed_seconds)
	output_lines.append("GDScript files: %d" % gd_scripts.size())
	output_lines.append("C# script files: %d" % cs_scripts.size())
	output_lines.append("Script files total: %d" % script_files.size())
	output_lines.append("Custom classes: %d" % custom_class_count)
	output_lines.append("Dependency rows total: %d" % total_dependency_rows)
	output_lines.append("Unresolved dependencies: %d" % unresolved_dependencies.size())
	output_lines.append("Dynamic dependencies: %d" % dynamic_dependencies.size())
	output_lines.append("Dependency cycles: %d" % dependency_cycles.size())
	output_lines.append("Graph root script_file_id: %d" % graph_root_script_file_id)
	output_lines.append("Graph slice rows: %d" % dependency_graph_slice.size())

	output_lines.append("")
	output_lines.append("FAILURES")
	output_lines.append("========")
	if failures.is_empty():
		output_lines.append("None")
	else:
		for failure: String in failures:
			output_lines.append("- %s" % failure)

	output_lines.append("")
	output_lines.append("START RESULT")
	output_lines.append("============")
	output_lines.append(JSON.stringify(start_result))

	output_lines.append("")
	output_lines.append("FINAL STATUS")
	output_lines.append("============")
	output_lines.append(JSON.stringify(final_status))

	output_lines.append("")
	output_lines.append("NATIVE METRICS")
	output_lines.append("==============")
	_append_dictionary_sorted(output_lines, metrics)

	output_lines.append("")
	output_lines.append("GDSCRIPT FILES")
	output_lines.append("==============")
	for entry: Dictionary in gd_scripts:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("CSHARP SCRIPT FILES")
	output_lines.append("===================")
	for entry: Dictionary in cs_scripts:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("CUSTOM CLASSES")
	output_lines.append("==============")
	for entry: Dictionary in custom_classes:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("DEPENDENCIES BY SCRIPT")
	output_lines.append("======================")
	for entry: Dictionary in dependency_records_by_script:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("UNRESOLVED DEPENDENCIES")
	output_lines.append("=======================")
	for entry: Dictionary in unresolved_dependencies:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("DYNAMIC DEPENDENCIES")
	output_lines.append("====================")
	for entry: Dictionary in dynamic_dependencies:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("DEPENDENCY CYCLES")
	output_lines.append("=================")
	for entry: Dictionary in dependency_cycles:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("DEPENDENCY GRAPH SLICE")
	output_lines.append("======================")
	for entry: Dictionary in dependency_graph_slice:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("DEBUG INVENTORY SUMMARY")
	output_lines.append("=======================")
	output_lines.append("file_count: %s" % str(debug_inventory.get("file_count", "")))
	output_lines.append("custom_class_count: %s" % str(debug_inventory.get("custom_class_count", "")))
	output_lines.append("scan_summary: %s" % JSON.stringify(debug_inventory.get("scan_summary", {})))

	DisplayServer.clipboard_set("\n".join(output_lines))

	print("Script scanner test copied to clipboard.")
	print("Result: %s" % ("PASS" if failures.is_empty() else "FAIL"))
	print("Scan ID: %d" % scan_id)
	print("Measured scan time: %.3f ms" % scan_elapsed_ms)
	print("GDScript files: %d" % gd_scripts.size())
	print("C# script files: %d" % cs_scripts.size())
	print("Custom classes: %d" % custom_class_count)
	print("Dependency rows: %d" % total_dependency_rows)
	print("Unresolved dependencies: %d" % unresolved_dependencies.size())
	print("Dynamic dependencies: %d" % dynamic_dependencies.size())
	print("Dependency cycles: %d" % dependency_cycles.size())

	if metrics.has("scripts_candidates"):
		print("scripts_candidates: %s" % str(metrics.get("scripts_candidates")))

	if metrics.has("scripts_parsed"):
		print("scripts_parsed: %s" % str(metrics.get("scripts_parsed")))

	if metrics.has("scripts_dependency_parsed"):
		print("scripts_dependency_parsed: %s" % str(metrics.get("scripts_dependency_parsed")))

	if metrics.has("dependency_records_created"):
		print("dependency_records_created: %s" % str(metrics.get("dependency_records_created")))

	if not failures.is_empty():
		for failure: String in failures:
			push_error("Script scanner test failure: %s" % failure)


func _wait_for_scan(
	scanner_context: GodotProjectContext,
	scan_id: int,
	timeout_seconds: float
) -> Dictionary:
	var start_msec: int = Time.get_ticks_msec()

	while true:
		var status: Dictionary = scanner_context.get_scan_status(scan_id)
		var status_text: String = String(status.get("status", "unknown"))

		if status_text == "completed" or status_text == "failed" or status_text == "cancelled":
			return status

		var elapsed_seconds: float = float(Time.get_ticks_msec() - start_msec) / 1000.0

		if elapsed_seconds >= timeout_seconds:
			var cancelled: bool = scanner_context.cancel_scan(scan_id)
			return {
				"scan_id": scan_id,
				"status": "timeout_cancel_requested" if cancelled else "timeout_cancel_failed",
				"last_status": status,
			}

		await get_tree().create_timer(POLL_INTERVAL_SECONDS).timeout

	return {
		"scan_id": scan_id,
		"status": "unreachable_return",
	}


func _load_all_files(scanner_context: GodotProjectContext, filter: Dictionary) -> Array:
	var count: int = int(scanner_context.get_file_count(filter))
	var rows: Array = []

	for offset: int in range(0, count, PAGE_SIZE):
		var page: Array = scanner_context.get_files_page(offset, PAGE_SIZE, "path", filter)

		for entry: Dictionary in page:
			rows.append(entry)

	return rows


func _load_all_custom_classes(
	scanner_context: GodotProjectContext,
	custom_class_count: int
) -> Array:
	var rows: Array = []

	for offset: int in range(0, custom_class_count, PAGE_SIZE):
		var page: Array = scanner_context.get_custom_classes_page(
			offset,
			PAGE_SIZE,
			"class_name",
			{}
		)

		for entry: Dictionary in page:
			rows.append(entry)

	return rows


func _validate_script_scanner_results(
	failures: Array[String],
	metrics: Dictionary,
	gd_scripts: Array,
	custom_classes: Array,
	dependency_records_by_script: Array,
	total_dependency_rows: int,
	debug_inventory: Dictionary
) -> void:
	if gd_scripts.is_empty():
		_append_failure(failures, "No .gd files were returned by get_files_page().")

	if int(metrics.get("scripts_candidates", 0)) <= 0:
		_append_failure(failures, "metrics.scripts_candidates should be greater than zero.")

	if int(metrics.get("scripts_parsed", 0)) <= 0:
		_append_failure(failures, "metrics.scripts_parsed should be greater than zero on force_rescan.")

	if int(metrics.get("scripts_dependency_parsed", 0)) <= 0:
		_append_failure(
			failures,
			"metrics.scripts_dependency_parsed should be greater than zero on force_rescan."
		)

	if total_dependency_rows <= 0:
		_append_failure(failures, "No script dependency rows were returned.")

	var probe_class: Dictionary = _find_custom_class(custom_classes, "GotoolScannerProbe")
	if probe_class.is_empty():
		_append_failure(
			failures,
			"Expected fixture custom class GotoolScannerProbe was not indexed."
		)
		return

	var probe_base_type: String = String(probe_class.get("direct_base_type", ""))
	if probe_base_type != "Resource":
		_append_failure(
			failures,
			"GotoolScannerProbe direct_base_type expected Resource, got %s."
			% probe_base_type
		)

	var probe_script_file_id: int = int(probe_class.get("script_file_id", 0))
	if probe_script_file_id <= 0:
		_append_failure(failures, "GotoolScannerProbe script_file_id should be positive.")
		return

	var probe_dependency_block: Dictionary = _find_dependency_block_for_script(
		dependency_records_by_script,
		probe_script_file_id
	)

	if probe_dependency_block.is_empty():
		_append_failure(failures, "No dependency block found for GotoolScannerProbe script.")
		return

	var probe_dependencies: Array = probe_dependency_block.get("dependencies", [])

	if not _has_dependency(probe_dependencies, "class_name_declaration", "GotoolScannerProbe", ""):
		_append_failure(
			failures,
			"GotoolScannerProbe should emit a ClassNameDeclaration dependency."
		)

	if not _has_dependency(probe_dependencies, "extends_class", "Resource", ""):
		_append_failure(
			failures,
			"GotoolScannerProbe should emit an ExtendsClass dependency targeting Resource."
		)

	if debug_inventory.is_empty():
		_append_failure(failures, "export_full_inventory_for_debug() returned empty dictionary.")
	elif not (debug_inventory.get("custom_classes", null) is Array):
		_append_failure(failures, "Debug inventory should include custom_classes array.")


func _find_custom_class(custom_classes: Array, expected_class_name: String) -> Dictionary:
	for entry: Dictionary in custom_classes:
		if String(entry.get("class_name", "")) == expected_class_name:
			return entry

	return {}


func _find_dependency_block_for_script(
	dependency_records_by_script: Array,
	script_file_id: int
) -> Dictionary:
	for entry: Dictionary in dependency_records_by_script:
		if int(entry.get("script_file_id", 0)) == script_file_id:
			return entry

	return {}


func _find_first_script_with_dependencies(dependency_records_by_script: Array) -> int:
	for entry: Dictionary in dependency_records_by_script:
		if int(entry.get("dependency_count", 0)) > 0:
			return int(entry.get("script_file_id", 0))

	return 0


func _has_dependency(
	dependencies: Array,
	expected_kind: String,
	expected_target_class_name: String,
	expected_target_project_relative_path: String
) -> bool:
	for dependency: Dictionary in dependencies:
		var dependency_kind: String = String(dependency.get("dependency_kind", ""))
		if dependency_kind != expected_kind:
			continue

		if not expected_target_class_name.is_empty():
			var target_class_name: String = String(dependency.get("target_class_name", ""))
			if target_class_name != expected_target_class_name:
				continue

		if not expected_target_project_relative_path.is_empty():
			var target_project_relative_path: String = String(
				dependency.get("target_project_relative_path", "")
			)
			if target_project_relative_path != expected_target_project_relative_path:
				continue

		return true

	return false


func _append_array(target: Array, source: Array) -> void:
	for entry in source:
		target.append(entry)


func _append_failure(failures: Array[String], message: String) -> void:
	failures.append(message)


func _append_dictionary_sorted(output_lines: Array[String], dictionary: Dictionary) -> void:
	var keys: Array = dictionary.keys()
	keys.sort()

	for key in keys:
		output_lines.append("%s: %s" % [str(key), str(dictionary[key])])


func _finish_with_output(
	scanner_context: GodotProjectContext,
	output_lines: Array[String],
	failures: Array[String]
) -> void:
	output_lines.append("SCRIPT SCANNER TEST")
	output_lines.append("===================")
	output_lines.append("Result: FAIL")
	output_lines.append("Last error: %s" % scanner_context.get_last_error())
	output_lines.append("")
	output_lines.append("FAILURES")
	output_lines.append("========")

	for failure: String in failures:
		output_lines.append("- %s" % failure)
		push_error("Script scanner test failure: %s" % failure)

	DisplayServer.clipboard_set("\n".join(output_lines))
