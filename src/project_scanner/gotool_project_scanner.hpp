#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class ProjectScanner : public RefCounted {
    GDCLASS(ProjectScanner, RefCounted)

protected:
    static void _bind_methods();

public:
    Array scan_res();
    Array scan_autoloads() const;
    Array scan_custom_script_classes(const Array &file_entries) const;
    Dictionary scan_project_inventory();

private:
    Dictionary make_entry(const String &full_path, const String &name, bool is_dir) const;

    String classify_file_type(
        const String &full_path,
        const String &name,
        bool is_dir
    ) const;

    String detect_godot_type(
        const String &full_path,
        const String &name,
        bool is_dir,
        const String &file_type,
        const String &extension
    ) const;

    void scan_directory(const String &path, Array &out_entries);
};

} // namespace godot
