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

	if not context.scan_project():
		failures.append("Second scan_project() call should succeed.")

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
