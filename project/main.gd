class_name Main
extends Node

func _ready() -> void:
	var scanner := ProjectScanner.new()

	var scan_start_usec := Time.get_ticks_usec()
	var inventory: Dictionary = scanner.scan_project_inventory()
	var scan_end_usec := Time.get_ticks_usec()

	var scan_elapsed_usec := scan_end_usec - scan_start_usec
	var scan_elapsed_ms := float(scan_elapsed_usec) / 1000.0
	var scan_elapsed_seconds := float(scan_elapsed_usec) / 1_000_000.0

	var output_lines: Array[String] = []

	output_lines.append("SCAN TIMING")
	output_lines.append("===========")
	output_lines.append("Scan time microseconds: %d" % scan_elapsed_usec)
	output_lines.append("Scan time milliseconds: %.3f" % scan_elapsed_ms)
	output_lines.append("Scan time seconds: %.6f" % scan_elapsed_seconds)
	output_lines.append("Files count: %d" % inventory.get("files", []).size())
	output_lines.append("Autoloads count: %d" % inventory.get("autoloads", []).size())
	output_lines.append("Custom classes count: %d" % inventory.get("custom_classes", []).size())

	output_lines.append("")
	output_lines.append("FILES")
	output_lines.append("=====")

	for entry: Dictionary in inventory.get("files", []):
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("AUTOLOADS")
	output_lines.append("=========")

	for entry: Dictionary in inventory.get("autoloads", []):
		output_lines.append(JSON.stringify(entry))

	output_lines.append("")
	output_lines.append("CUSTOM_CLASSES")
	output_lines.append("==============")

	for entry: Dictionary in inventory.get("custom_classes", []):
		output_lines.append(JSON.stringify(entry))

	DisplayServer.clipboard_set("\n".join(output_lines))

	print("Project scan copied to clipboard.")
	print("Scan time: %.3f ms" % scan_elapsed_ms)
	print("Files: %d" % inventory.get("files", []).size())
	print("Autoloads: %d" % inventory.get("autoloads", []).size())
	print("Custom classes: %d" % inventory.get("custom_classes", []).size())
