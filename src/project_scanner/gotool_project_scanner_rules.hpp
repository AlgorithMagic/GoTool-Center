#pragma once

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace gotool::project_scanner {

static constexpr const char *NOT_GODOT_TYPED = "NGT";

struct FileFacts {
    godot::String full_path;
    godot::String name;
    godot::String lower_path;
    godot::String lower_name;
    godot::String extension;
    bool is_directory = false;
};

FileFacts build_file_facts(
    const godot::String &full_path,
    const godot::String &name,
    bool is_directory
);

bool should_skip_scan_path(const godot::String &full_path);
godot::String to_project_relative_path(const godot::String &full_path);
bool is_script_extension(const godot::String &extension);
godot::String language_from_script_extension(const godot::String &extension);

godot::Dictionary parse_custom_script_class(
    const godot::String &path,
    const godot::String &extension
);

bool is_class_or_parent_class(const godot::String &class_name, const char *expected_parent);

godot::String classify_file_type_from_facts(const FileFacts &facts);
godot::String detect_godot_type_from_facts(const FileFacts &facts, const godot::String &file_type);

} // namespace gotool::project_scanner

