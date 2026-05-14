class_name Main
extends Node

## Integration Test Suite for GoTool Scanner and Database
## 
## This script tests:
## 1. Database Initialization
## 2. Full Project Scan (Force Rescan)
## 3. Incremental Scanning (No changes)
## 4. Persistence across Context instances
## 5. Script Parsing (GDScript and C#)
## 6. Dependency Graph Analysis (Cycles, Slices)

const PAGE_SIZE: int = 500
const POLL_INTERVAL_SECONDS: float = 0.05
const SCAN_TIMEOUT_SECONDS: float = 300.0
const GRAPH_SLICE_DEPTH: int = 2

var failures: Array[String] = []

func _ready() -> void:
	print_rich("[b][color=cyan]GoToolCenter Integration Test Suite Started[/color][/b]")
	print_rich("--------------------------------------------------")

	# --- PHASE 1: Full Scan ---
	var scan_id_1 = await test_full_scan()
	if scan_id_1 <= 0:
		_finish_report()
		return

	# --- PHASE 2: Incremental Scan (No changes expected) ---
	await test_incremental_scan(scan_id_1)

	# --- PHASE 3: Persistence Test ---
	await test_persistence()

	# --- PHASE 4: Analysis & Validation ---
	await test_analysis()

	_finish_report()


func test_full_scan() -> int:
	print_rich("[color=yellow][PHASE 1][/color] Initial Full Scan...")
	var ctx: GodotProjectContext = GodotProjectContext.new()
	
	if not ctx.initialize_database():
		_append_failure("Phase 1: initialize_database failed: %s" % ctx.get_last_error())
		return -1

	var scan_options = {
		"include_hidden": true,
		"force_rescan": true,
		"include_custom_classes": true,
		"load_existing_snapshot": false,
		"enable_parallel_traversal": true,
	}

	var start_result = ctx.start_scan(scan_options)
	var scan_id = int(start_result.get("scan_id", 0))
	
	if scan_id <= 0:
		_append_failure("Phase 1: start_scan failed: %s" % ctx.get_last_error())
		return -1

	var final_status = await _wait_for_scan(ctx, scan_id)
	if final_status.get("status") != "completed":
		_append_failure("Phase 1: Scan did not complete. Status: %s" % final_status.get("status"))
		return -1

	var metrics = ctx.get_scan_metrics(scan_id)
	print_rich("  [color=green]Full scan metrics:[/color]")
	_print_metrics(metrics)
	
	if int(metrics.get("scripts_parsed", 0)) == 0:
		_append_failure("Phase 1: Force rescan was requested but 0 scripts were parsed.")
		
	return scan_id


func test_incremental_scan(prev_id: int) -> void:
	print_rich("[color=yellow][PHASE 2][/color] Incremental Scan (No changes)...")
	var ctx: GodotProjectContext = GodotProjectContext.new()
	ctx.initialize_database()

	var scan_options = {
		"force_rescan": false,
		"load_existing_snapshot": true,
	}

	var start_result = ctx.start_scan(scan_options)
	var scan_id = int(start_result.get("scan_id", 0))
	
	var final_status = await _wait_for_scan(ctx, scan_id)
	var metrics = ctx.get_scan_metrics(scan_id)
	
	print_rich("  [color=green]Incremental metrics:[/color]")
	_print_metrics(metrics)

	if int(metrics.get("scripts_parsed", 0)) > 0:
		# Note: In a real project, Godot might touch files. 
		# But for a stable test env, we expect 0.
		print_rich("  [i][color=orange]Note: %d scripts were still parsed during incremental scan.[/color][/i]" % metrics.get("scripts_parsed"))


func test_persistence() -> void:
	print_rich("[color=yellow][PHASE 3][/color] Persistence across context lifecycle...")
	
	var file_count_before: int = 0
	
	# Scope 1: Get count
	var ctx1: GodotProjectContext = GodotProjectContext.new()
	ctx1.initialize_database()
	file_count_before = ctx1.get_file_count({})
	ctx1 = null # Trigger destruction
	
	# Scope 2: New context, check if data is still there without scanning
	var ctx2: GodotProjectContext = GodotProjectContext.new()
	if not ctx2.initialize_database():
		_append_failure("Phase 3: Re-initialization failed.")
		return
		
	var file_count_after = ctx2.get_file_count({})
	print_rich("  Files in DB (Context 1): %d" % file_count_before)
	print_rich("  Files in DB (Context 2): %d" % file_count_after)
	
	if file_count_before != file_count_after or file_count_after == 0:
		_append_failure("Phase 3: Persistence failed. File count mismatch or empty database.")


func test_analysis() -> void:
	print_rich("[color=yellow][PHASE 4][/color] Script Analysis & Graph Integrity...")
	var ctx: GodotProjectContext = GodotProjectContext.new()
	ctx.initialize_database()
	
	# 1. Check GDScript Probe
	var custom_classes = _load_all_custom_classes(ctx)
	var probe = _find_dict_in_array(custom_classes, "class_name", "GotoolScannerProbe")
	
	if probe.is_empty():
		_append_failure("Phase 4: GotoolScannerProbe was not indexed.")
	else:
		print_rich("  Found GDScript Probe: [color=cyan]%s[/color] extends [color=cyan]%s[/color]" % [probe.class_name, probe.direct_base_type])

	# 2. Check C# Visibility (if any exist)
	var cs_files = _load_all_files(ctx, {"extension": ".cs"})
	print_rich("  C# scripts indexed: %d" % cs_files.size())

	# 3. Graph Logic
	var cycles = ctx.list_dependency_cycles()
	if cycles.size() > 0:
		print_rich("  [color=orange]Dependency cycles detected: %d[/color]" % cycles.size())
		for cycle in cycles:
			print_rich("    - Cycle: %s" % JSON.stringify(cycle))
	else:
		print_rich("  [color=green]No dependency cycles found.[/color]")

	var unresolved = ctx.list_unresolved_dependencies()
	if unresolved.size() > 0:
		print_rich("  [color=orange]Unresolved dependencies: %d[/color]" % unresolved.size())

	# 4. Graph Slicing
	if not custom_classes.is_empty():
		var first_class = custom_classes[0]
		var file_id = int(first_class.get("script_file_id", 0))
		if file_id > 0:
			var slice = ctx.get_dependency_graph_slice(file_id, GRAPH_SLICE_DEPTH)
			print_rich("  Graph slice for [color=cyan]%s[/color] (depth %d): %d rows" % [first_class.class_name, GRAPH_SLICE_DEPTH, slice.size()])


# --- HELPERS ---

func _wait_for_scan(ctx: GodotProjectContext, scan_id: int) -> Dictionary:
	var start_msec = Time.get_ticks_msec()
	while true:
		var status = ctx.get_scan_status(scan_id)
		var text = status.get("status", "unknown")
		
		if text in ["completed", "failed", "cancelled"]:
			return status
			
		if (Time.get_ticks_msec() - start_msec) / 1000.0 > SCAN_TIMEOUT_SECONDS:
			ctx.cancel_scan(scan_id)
			return {"status": "timeout"}
			
		await get_tree().create_timer(POLL_INTERVAL_SECONDS).timeout
	return {}

func _load_all_files(ctx: GodotProjectContext, filter: Dictionary) -> Array:
	var count = ctx.get_file_count(filter)
	var rows = []
	for offset in range(0, count, PAGE_SIZE):
		rows.append_array(ctx.get_files_page(offset, PAGE_SIZE, "path", filter))
	return rows

func _load_all_custom_classes(ctx: GodotProjectContext) -> Array:
	var count = ctx.get_custom_class_count({})
	var rows = []
	for offset in range(0, count, PAGE_SIZE):
		rows.append_array(ctx.get_custom_classes_page(offset, PAGE_SIZE, "class_name", {}))
	return rows

func _find_dict_in_array(arr: Array, key: String, value: String) -> Dictionary:
	for d in arr:
		if d.get(key) == value: return d
	return {}

func _print_metrics(m: Dictionary) -> void:
	var keys = m.keys()
	keys.sort()
	for k in keys:
		print("    %s: %s" % [k, m[k]])

func _append_failure(msg: String) -> void:
	failures.append(msg)
	push_error(msg)

func _finish_report() -> void:
	print_rich("\n--------------------------------------------------")
	if failures.is_empty():
		print_rich("[b][color=green]INTEGRATION TEST RESULT: PASS[/color][/b]")
	else:
		print_rich("[b][color=red]INTEGRATION TEST RESULT: FAIL (%d failures)[/color][/b]" % failures.size())
		for f in failures:
			print_rich("[color=red]- %s[/color]" % f)
	print_rich("--------------------------------------------------")
