#include "gotool_project_context.hpp"

#include "database/gotool_project_inventory_repository.hpp"
#include "database/gotool_schema.hpp"
#include "project_scanner/gotool_project_scanner.hpp"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/array.hpp>

#include <exception>
#include <string>

namespace godot {

void GodotProjectContext::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("initialize_database"),
        &GodotProjectContext::initialize_database
    );

    ClassDB::bind_method(
        D_METHOD("get_database_virtual_path"),
        &GodotProjectContext::get_database_virtual_path
    );

    ClassDB::bind_method(
        D_METHOD("get_database_absolute_path"),
        &GodotProjectContext::get_database_absolute_path
    );

    ClassDB::bind_method(
        D_METHOD("scan_project"),
        &GodotProjectContext::scan_project
    );

    ClassDB::bind_method(
        D_METHOD("get_last_scan_results"),
        &GodotProjectContext::get_last_scan_results
    );

    ClassDB::bind_method(
        D_METHOD("get_last_error"),
        &GodotProjectContext::get_last_error
    );
}

bool GodotProjectContext::initialize_database() {
    last_error_ = "";

    const String database_directory = "res://.godot/gotool_center";
    const String database_virtual_path = database_directory + String("/gotool_center.db");

    const Error directory_result =
        DirAccess::make_dir_recursive_absolute(database_directory);

    if (directory_result != OK && directory_result != ERR_ALREADY_EXISTS) {
        last_error_ = "Failed to create GoTool database directory: " + database_directory;
        database_.reset();
        return false;
    }

    ProjectSettings* project_settings = ProjectSettings::get_singleton();

    if (project_settings == nullptr) {
        last_error_ = "ProjectSettings singleton was not available.";
        database_.reset();
        return false;
    }

    const String database_absolute_path =
        project_settings->globalize_path(database_virtual_path);

    const CharString database_absolute_path_utf8 =
        database_absolute_path.utf8();

    try {
        database_ = std::make_unique<gotool::database::Database>(
            database_absolute_path_utf8.get_data()
        );

        gotool::database::create_schema(*database_);

        database_virtual_path_ = database_virtual_path;
        database_absolute_path_ = database_absolute_path;
        last_scan_results_.clear();

        return true;
    }
    catch (const std::exception& error) {
        last_error_ = error.what();
        database_.reset();
        return false;
    }
}

bool GodotProjectContext::scan_project() {
    last_error_ = "";

    if (database_ == nullptr) {
        last_error_ = "Database is not initialized. Call initialize_database() first.";
        last_scan_results_.clear();
        return false;
    }

    Ref<ProjectScanner> scanner;
    scanner.instantiate();

    if (scanner.is_null()) {
        last_error_ = "Failed to instantiate ProjectScanner.";
        last_scan_results_.clear();
        return false;
    }

    try {
        const Dictionary inventory = scanner->scan_project_inventory();

        if (!inventory.has("files") ||
            !inventory.has("autoloads") ||
            !inventory.has("custom_classes")) {
            last_error_ = "ProjectScanner returned an invalid inventory payload.";
            last_scan_results_.clear();
            return false;
        }

        const Variant files_variant = inventory.get("files", Variant());
        const Variant autoloads_variant = inventory.get("autoloads", Variant());
        const Variant custom_classes_variant = inventory.get("custom_classes", Variant());

        if (files_variant.get_type() != Variant::ARRAY ||
            autoloads_variant.get_type() != Variant::ARRAY ||
            custom_classes_variant.get_type() != Variant::ARRAY) {
            last_error_ = "ProjectScanner inventory payload must contain arrays.";
            last_scan_results_.clear();
            return false;
        }

        gotool::database::ProjectInventoryRepository repository(*database_);
        repository.persist_inventory(inventory);

        last_scan_results_ = inventory;
        return true;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        last_scan_results_.clear();
        return false;
    }
}

String GodotProjectContext::get_database_virtual_path() const {
    return database_virtual_path_;
}

String GodotProjectContext::get_database_absolute_path() const {
    return database_absolute_path_;
}

Dictionary GodotProjectContext::get_last_scan_results() const {
    return last_scan_results_;
}

String GodotProjectContext::get_last_error() const {
    return last_error_;
}

} // namespace godot
