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

    Dictionary start_scan(const Dictionary &options);
    bool cancel_scan(int64_t scan_id);
    Dictionary get_scan_status(int64_t scan_id) const;
    Dictionary get_scan_metrics(int64_t scan_id) const;
    int64_t get_file_count(const Dictionary &filter) const;
    Array get_files_page(int64_t offset, int64_t limit, const String &sort, const Dictionary &filter) const;
    Dictionary get_file_details(int64_t file_id) const;
    Array get_directory_children(
        int64_t directory_id,
        int64_t offset,
        int64_t limit,
        const String &sort,
        const Dictionary &filter
    ) const;
    int64_t get_custom_class_count(const Dictionary &filter) const;
    Array get_custom_classes_page(
        int64_t offset,
        int64_t limit,
        const String &sort,
        const Dictionary &filter
    ) const;
    Dictionary export_full_inventory_for_debug() const;

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
