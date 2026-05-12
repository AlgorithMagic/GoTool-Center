#pragma once

#include "database/gotool_database.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>

namespace godot {

class GodotProjectContext : public RefCounted {
    GDCLASS(GodotProjectContext, RefCounted)

protected:
    static void _bind_methods();

public:
    bool initialize_database();
    int64_t register_current_project();

    String get_database_virtual_path() const;
    String get_database_absolute_path() const;

    bool scan_current_project();
    bool scan_project();

    Array list_projects() const;
    Dictionary get_project_summary(int64_t project_id) const;

    Dictionary get_last_scan_results() const;
    String get_last_error() const;

private:
    std::unique_ptr<gotool::database::Database> database_;
    int64_t current_project_id_ = 0;
    String current_identity_source_;
    String current_identity_warning_;

    String database_virtual_path_;
    String database_absolute_path_;
    String last_error_;

    Dictionary last_scan_results_;
};

} // namespace godot
