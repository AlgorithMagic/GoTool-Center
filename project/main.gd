class_name Main
extends Node

const PAGE_SIZE: int = 500
const POLL_INTERVAL_SECONDS: float = 0.05
const START_SCAN_TIMEOUT_SECONDS: float = 300.0


func _ready() -> void:
	var scanner_context := GodotProjectContext.new()

	var output_lines: Array[String] = []

	if not scanner_context.initialize_database():
		output_lines.append("INITIALIZE DATABASE FAILED")
		output_lines.append("==========================")
		output_lines.append(scanner_context.get_last_error())
		DisplayServer.clipboard_set("\n".join(output_lines))
		push_error("GoTool database initialization failed: %s" % scanner_context.get_last_error())
		return

	var scan_options: Dictionary = {
		"include_hidden": true,
		"force_rescan": false,
	}

	var scan_start_usec := Time.get_ticks_usec()
	var start_result: Dictionary = scanner_context.start_scan(scan_options)

	if start_result.is_empty():
		output_lines.append("START SCAN FAILED")
		output_lines.append("=================")
		output_lines.append(scanner_context.get_last_error())
		DisplayServer.clipboard_set("\n".join(output_lines))
		push_error("GoTool start_scan failed: %s" % scanner_context.get_last_error())
		return

	var scan_id: int = int(start_result.get("scan_id", start_result.get("scan_run_id", 0)))

	if scan_id <= 0:
		output_lines.append("START SCAN RETURNED INVALID SCAN ID")
		output_lines.append("===================================")
		output_lines.append(JSON.stringify(start_result))
		DisplayServer.clipboard_set("\n".join(output_lines))
		push_error("GoTool start_scan returned invalid scan id.")
		return

	var final_status: Dictionary = await _wait_for_scan(
		scanner_context, scan_id, START_SCAN_TIMEOUT_SECONDS
	)
	var scan_end_usec := Time.get_ticks_usec()

	var scan_elapsed_usec := scan_end_usec - scan_start_usec
	var scan_elapsed_ms := float(scan_elapsed_usec) / 1000.0
	var scan_elapsed_seconds := float(scan_elapsed_usec) / 1_000_000.0

	var status_text := String(final_status.get("status", "unknown"))

	if status_text != "completed":
		output_lines.append("SCAN DID NOT COMPLETE")
		output_lines.append("=====================")
		output_lines.append("Scan ID: %d" % scan_id)
		output_lines.append("Status: %s" % status_text)
		output_lines.append("Last error: %s" % scanner_context.get_last_error())
		output_lines.append("")
		output_lines.append("START RESULT")
		output_lines.append("============")
		output_lines.append(JSON.stringify(start_result))
		output_lines.append("")
		output_lines.append("FINAL STATUS")
		output_lines.append("============")
		output_lines.append(JSON.stringify(final_status))

		DisplayServer.clipboard_set("\n".join(output_lines))
		push_error("GoTool scan did not complete. Status: %s" % status_text)
		return

	var metrics: Dictionary = scanner_context.get_scan_metrics(scan_id)

	var file_count: int = int(scanner_context.get_file_count({}))
	var custom_class_count: int = int(scanner_context.get_custom_class_count({}))

	var files: Array = _load_all_files(scanner_context, file_count)
	var custom_classes: Array = _load_all_custom_classes(scanner_context, custom_class_count)

	var fast_inventory_for_autoloads: Dictionary = (
		scanner_context
		. scan_project_inventory_fast(
			{
				"include_hidden": true,
				"include_custom_classes": false,
				"max_results": 0,
			}
		)
	)

	var autoloads: Array = fast_inventory_for_autoloads.get("autoloads", [])

	output_lines.append("ASYNC DB-BACKED SCAN TIMING")
	output_lines.append("===========================")
	output_lines.append("Scan ID: %d" % scan_id)
	output_lines.append("Final status: %s" % status_text)
	output_lines.append("Measured wall time microseconds: %d" % scan_elapsed_usec)
	output_lines.append("Measured wall time milliseconds: %.3f" % scan_elapsed_ms)
	output_lines.append("Measured wall time seconds: %.6f" % scan_elapsed_seconds)
	output_lines.append("Files count: %d" % file_count)
	output_lines.append("Autoloads count: %d" % autoloads.size())
	output_lines.append("Custom classes count: %d" % custom_class_count)

	output_lines.append("")
	output_lines.append("NATIVE SCAN METRICS")
	output_lines.append("===================")
	_append_dictionary_sorted(output_lines, metrics)

	output_lines.append("")
	output_lines.append("START RESULT")
	output_lines.append("============")
	output_lines.append(JSON.stringify(start_result))

	output_lines.append("")
	output_lines.append("FINAL STATUS")
	output_lines.append("============")
	output_lines.append(JSON.stringify(final_status))

	output_lines.append("")
	output_lines.append("WATCHER STATUS")
	output_lines.append("==============")
	output_lines.append(JSON.stringify(scanner_context.get_watcher_status()))

	output_lines.append("")
	output_lines.append("FILES")
	output_lines.append("=====")

	for entry in files:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("AUTOLOADS")
	output_lines.append("=========")

	for entry in autoloads:
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("CUSTOM_CLASSES")
	output_lines.append("==============")

	for entry in custom_classes:
		output_lines.append(JSON.stringify(entry))

	DisplayServer.clipboard_set("\n".join(output_lines))

	print("Project scan copied to clipboard.")
	print("Scan ID: %d" % scan_id)
	print("Status: %s" % status_text)
	print("Measured scan time: %.3f ms" % scan_elapsed_ms)
	print("Files: %d" % file_count)
	print("Autoloads: %d" % autoloads.size())
	print("Custom classes: %d" % custom_class_count)

	if metrics.has("total_wall_ms"):
		print("Native total_wall_ms: %s" % str(metrics.get("total_wall_ms")))

	if metrics.has("traversal_ms"):
		print("Native traversal_ms: %s" % str(metrics.get("traversal_ms")))

	if metrics.has("sqlite_write_ms"):
		print("Native sqlite_write_ms: %s" % str(metrics.get("sqlite_write_ms")))


func _wait_for_scan(
	scanner_context: GodotProjectContext, scan_id: int, timeout_seconds: float
) -> Dictionary:
	var start_msec := Time.get_ticks_msec()

	while true:
		var status: Dictionary = scanner_context.get_scan_status(scan_id)
		var status_text := String(status.get("status", "unknown"))

		if status_text == "completed" or status_text == "failed" or status_text == "cancelled":
			return status

		var elapsed_seconds := float(Time.get_ticks_msec() - start_msec) / 1000.0

		if elapsed_seconds >= timeout_seconds:
			var cancelled := scanner_context.cancel_scan(scan_id)
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


func _load_all_files(scanner_context: GodotProjectContext, file_count: int) -> Array:
	var rows: Array = []

	for offset in range(0, file_count, PAGE_SIZE):
		var page: Array = scanner_context.get_files_page(offset, PAGE_SIZE, "path", {})

		for entry in page:
			rows.append(entry)

	return rows


func _load_all_custom_classes(
	scanner_context: GodotProjectContext, custom_class_count: int
) -> Array:
	var rows: Array = []

	for offset in range(0, custom_class_count, PAGE_SIZE):
		var page: Array = scanner_context.get_custom_classes_page(
			offset, PAGE_SIZE, "class_name", {}
		)

		for entry in page:
			rows.append(entry)

	return rows


func _append_dictionary_sorted(output_lines: Array[String], dictionary: Dictionary) -> void:
	var keys: Array = dictionary.keys()
	keys.sort()

	for key in keys:
		output_lines.append("%s: %s" % [str(key), str(dictionary[key])])
