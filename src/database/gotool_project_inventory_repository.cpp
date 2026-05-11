#include "database/gotool_project_inventory_repository.hpp"

#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <ctime>
#include <stdexcept>

namespace gotool::database {

ProjectInventoryRepository::ProjectInventoryRepository(Database &database) :
    database_(&database) {
    if (database_ == nullptr) {
        throw std::runtime_error("ProjectInventoryRepository requires a valid Database instance.");
    }
}

PersistedInventorySummary ProjectInventoryRepository::persist_inventory(const godot::Dictionary &inventory) {
    const godot::Array files = inventory.get("files", godot::Array());
    const godot::Array autoloads = inventory.get("autoloads", godot::Array());
    const godot::Array custom_classes = inventory.get("custom_classes", godot::Array());

    if (files.is_empty() &&
        !inventory.has("files") &&
        !inventory.has("autoloads") &&
        !inventory.has("custom_classes")) {
        throw std::runtime_error("Scan inventory dictionary did not contain required keys.");
    }

    PersistedInventorySummary summary;
    summary.files_found = 0;
    summary.folders_found = 0;
    summary.unknown_entries = 0;

    const int64_t started_at_unix = current_unix_time();
    const int64_t observed_at_unix = started_at_unix;

    Transaction transaction(*database_);
    summary.scan_run_id = create_scan_run(started_at_unix);

    godot::Array unknown_entries;
    unknown_entries.clear();

    for (int64_t i = 0; i < files.size(); ++i) {
        const godot::Variant value = files[i];

        if (value.get_type() != godot::Variant::DICTIONARY) {
            continue;
        }

        const godot::Dictionary file_entry = value;
        const bool is_directory = variant_to_bool(file_entry.get("is_directory", false));

        if (is_directory) {
            ++summary.folders_found;
        } else {
            ++summary.files_found;
        }

        upsert_project_file(file_entry, summary.scan_run_id, observed_at_unix);

        const godot::String file_type = variant_to_string(file_entry.get("file_type", ""));

        if (file_type == "Unknown") {
            godot::Dictionary unknown_entry;
            unknown_entry["project_relative_path"] = variant_to_string(file_entry.get("project_relative_path", ""));
            unknown_entry["file_name"] = variant_to_string(file_entry.get("file_name", ""));
            unknown_entry["extension"] = variant_to_string(file_entry.get("extension", ""));
            unknown_entry["observed_file_type"] = file_type;
            unknown_entry["observed_godot_type"] = variant_to_string(file_entry.get("godot_type", "NGT"));
            unknown_entries.append(unknown_entry);
        }
    }

    delete_missing_project_files(summary.scan_run_id);

    for (int64_t i = 0; i < autoloads.size(); ++i) {
        const godot::Variant value = autoloads[i];

        if (value.get_type() != godot::Variant::DICTIONARY) {
            continue;
        }

        upsert_autoload(value, summary.scan_run_id, observed_at_unix);
    }

    delete_missing_autoloads(summary.scan_run_id);

    for (int64_t i = 0; i < custom_classes.size(); ++i) {
        const godot::Variant value = custom_classes[i];

        if (value.get_type() != godot::Variant::DICTIONARY) {
            continue;
        }

        upsert_custom_class(value, summary.scan_run_id, observed_at_unix);
    }

    delete_missing_custom_classes(summary.scan_run_id);

    summary.unknown_entries = unknown_entries.size();

    for (int64_t i = 0; i < unknown_entries.size(); ++i) {
        const godot::Dictionary unknown_entry = unknown_entries[i];
        upsert_unknown(unknown_entry, summary.scan_run_id, observed_at_unix);
    }

    delete_missing_unknowns(summary.scan_run_id);

    complete_scan_run(
        summary.scan_run_id,
        current_unix_time(),
        summary.files_found,
        summary.folders_found
    );

    transaction.commit();
    return summary;
}

int64_t ProjectInventoryRepository::current_unix_time() {
    return static_cast<int64_t>(std::time(nullptr));
}

std::string ProjectInventoryRepository::to_utf8(const godot::String &value) {
    const godot::CharString utf8 = value.utf8();
    return utf8.get_data() != nullptr ? utf8.get_data() : "";
}

bool ProjectInventoryRepository::variant_to_bool(const godot::Variant &value) {
    switch (value.get_type()) {
        case godot::Variant::BOOL:
            return static_cast<bool>(value);
        case godot::Variant::INT:
            return static_cast<int64_t>(value) != 0;
        default:
            return false;
    }
}

int64_t ProjectInventoryRepository::variant_to_int64(const godot::Variant &value) {
    switch (value.get_type()) {
        case godot::Variant::INT:
            return static_cast<int64_t>(value);
        case godot::Variant::FLOAT:
            return static_cast<int64_t>(static_cast<double>(value));
        case godot::Variant::BOOL:
            return static_cast<bool>(value) ? 1 : 0;
        default:
            return 0;
    }
}

godot::String ProjectInventoryRepository::variant_to_string(const godot::Variant &value) {
    if (value.get_type() == godot::Variant::STRING) {
        return static_cast<godot::String>(value);
    }

    if (value.get_type() == godot::Variant::NIL) {
        return "";
    }

    return godot::String(value);
}

int64_t ProjectInventoryRepository::create_scan_run(int64_t started_at_unix) {
    Statement statement = database_->prepare(
        "INSERT INTO project_scan_runs (started_at_unix, status, files_found, folders_found) "
        "VALUES (?1, 'running', 0, 0);"
    );
    statement.bind_int64(1, started_at_unix);
    statement.step_done();
    return database_->last_insert_row_id();
}

void ProjectInventoryRepository::complete_scan_run(
    int64_t scan_run_id,
    int64_t finished_at_unix,
    int64_t files_found,
    int64_t folders_found
) {
    Statement statement = database_->prepare(
        "UPDATE project_scan_runs "
        "SET finished_at_unix = ?1, status = 'completed', files_found = ?2, folders_found = ?3, error_message = NULL "
        "WHERE id = ?4;"
    );
    statement.bind_int64(1, finished_at_unix);
    statement.bind_int64(2, files_found);
    statement.bind_int64(3, folders_found);
    statement.bind_int64(4, scan_run_id);
    statement.step_done();
}

void ProjectInventoryRepository::upsert_project_file(
    const godot::Dictionary &entry,
    int64_t scan_run_id,
    int64_t observed_at_unix
) {
    const std::string project_relative_path = to_utf8(variant_to_string(entry.get("project_relative_path", "")));
    const std::string absolute_path = to_utf8(variant_to_string(entry.get("absolute_path", "")));
    const std::string file_name = to_utf8(variant_to_string(entry.get("file_name", "")));
    const std::string extension = to_utf8(variant_to_string(entry.get("extension", "")));
    const std::string file_type = to_utf8(variant_to_string(entry.get("file_type", "")));
    const std::string godot_type = to_utf8(variant_to_string(entry.get("godot_type", "NGT")));
    const int64_t size_bytes = variant_to_int64(entry.get("size_bytes", 0));
    const int64_t modified_time_unix = variant_to_int64(entry.get("modified_time_unix", 0));
    const int64_t is_directory = variant_to_bool(entry.get("is_directory", false)) ? 1 : 0;
    const int64_t is_hidden = variant_to_bool(entry.get("is_hidden", false)) ? 1 : 0;

    Statement statement = database_->prepare(
        "INSERT INTO project_files ("
        "project_relative_path, absolute_path, file_name, extension, file_type, godot_type, "
        "size_bytes, modified_time_unix, is_directory, is_hidden, "
        "first_seen_scan_run_id, last_seen_scan_run_id, created_at_unix, updated_at_unix"
        ") VALUES ("
        "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?11, ?12, ?12"
        ") "
        "ON CONFLICT(project_relative_path) DO UPDATE SET "
        "absolute_path = excluded.absolute_path, "
        "file_name = excluded.file_name, "
        "extension = excluded.extension, "
        "file_type = excluded.file_type, "
        "godot_type = excluded.godot_type, "
        "size_bytes = excluded.size_bytes, "
        "modified_time_unix = excluded.modified_time_unix, "
        "is_directory = excluded.is_directory, "
        "is_hidden = excluded.is_hidden, "
        "last_seen_scan_run_id = excluded.last_seen_scan_run_id, "
        "updated_at_unix = excluded.updated_at_unix, "
        "first_seen_scan_run_id = COALESCE(project_files.first_seen_scan_run_id, excluded.first_seen_scan_run_id);"
    );
    statement.bind_text(1, project_relative_path);
    statement.bind_text(2, absolute_path);
    statement.bind_text(3, file_name);
    statement.bind_text(4, extension);
    statement.bind_text(5, file_type);
    statement.bind_text(6, godot_type.empty() ? std::string("NGT") : godot_type);
    statement.bind_int64(7, size_bytes);
    statement.bind_int64(8, modified_time_unix);
    statement.bind_int64(9, is_directory);
    statement.bind_int64(10, is_hidden);
    statement.bind_int64(11, scan_run_id);
    statement.bind_int64(12, observed_at_unix);
    statement.step_done();
}

void ProjectInventoryRepository::delete_missing_project_files(int64_t scan_run_id) {
    Statement statement = database_->prepare(
        "DELETE FROM project_files WHERE last_seen_scan_run_id IS NULL OR last_seen_scan_run_id <> ?1;"
    );
    statement.bind_int64(1, scan_run_id);
    statement.step_done();
}

void ProjectInventoryRepository::upsert_autoload(
    const godot::Dictionary &entry,
    int64_t scan_run_id,
    int64_t observed_at_unix
) {
    const std::string autoload_name = to_utf8(variant_to_string(entry.get("autoload_name", "")));
    const std::string target_path = to_utf8(variant_to_string(entry.get("target_path", "")));
    const std::string target_project_relative_path = to_utf8(variant_to_string(entry.get("target_project_relative_path", "")));
    const int64_t is_singleton = variant_to_bool(entry.get("is_singleton", true)) ? 1 : 0;

    Statement statement = database_->prepare(
        "INSERT INTO project_autoloads ("
        "autoload_name, target_path, target_project_relative_path, is_singleton, "
        "target_file_id, last_seen_scan_run_id, created_at_unix, updated_at_unix"
        ") VALUES ("
        "?1, ?2, ?3, ?4, "
        "(SELECT id FROM project_files WHERE project_relative_path = ?3), "
        "?5, ?6, ?6"
        ") "
        "ON CONFLICT(autoload_name) DO UPDATE SET "
        "target_path = excluded.target_path, "
        "target_project_relative_path = excluded.target_project_relative_path, "
        "is_singleton = excluded.is_singleton, "
        "target_file_id = (SELECT id FROM project_files WHERE project_relative_path = excluded.target_project_relative_path), "
        "last_seen_scan_run_id = excluded.last_seen_scan_run_id, "
        "updated_at_unix = excluded.updated_at_unix;"
    );
    statement.bind_text(1, autoload_name);
    statement.bind_text(2, target_path);
    statement.bind_text(3, target_project_relative_path);
    statement.bind_int64(4, is_singleton);
    statement.bind_int64(5, scan_run_id);
    statement.bind_int64(6, observed_at_unix);
    statement.step_done();
}

void ProjectInventoryRepository::delete_missing_autoloads(int64_t scan_run_id) {
    Statement statement = database_->prepare(
        "DELETE FROM project_autoloads WHERE last_seen_scan_run_id IS NULL OR last_seen_scan_run_id <> ?1;"
    );
    statement.bind_int64(1, scan_run_id);
    statement.step_done();
}

void ProjectInventoryRepository::upsert_custom_class(
    const godot::Dictionary &entry,
    int64_t scan_run_id,
    int64_t observed_at_unix
) {
    const std::string class_name = to_utf8(variant_to_string(entry.get("class_name", "")));
    const std::string script_path = to_utf8(variant_to_string(entry.get("script_path", "")));
    const std::string script_project_relative_path = to_utf8(variant_to_string(entry.get("script_project_relative_path", "")));
    const std::string language = to_utf8(variant_to_string(entry.get("language", "")));
    const std::string base_type = to_utf8(variant_to_string(entry.get("base_type", "")));
    const int64_t is_resource_type = variant_to_bool(entry.get("is_resource_type", false)) ? 1 : 0;
    const int64_t is_node_type = variant_to_bool(entry.get("is_node_type", false)) ? 1 : 0;

    Statement statement = database_->prepare(
        "INSERT INTO project_custom_classes ("
        "class_name, script_path, script_project_relative_path, language, base_type, "
        "is_resource_type, is_node_type, script_file_id, last_seen_scan_run_id, created_at_unix, updated_at_unix"
        ") VALUES ("
        "?1, ?2, ?3, ?4, ?5, ?6, ?7, "
        "(SELECT id FROM project_files WHERE project_relative_path = ?3), "
        "?8, ?9, ?9"
        ") "
        "ON CONFLICT(class_name) DO UPDATE SET "
        "script_path = excluded.script_path, "
        "script_project_relative_path = excluded.script_project_relative_path, "
        "language = excluded.language, "
        "base_type = excluded.base_type, "
        "is_resource_type = excluded.is_resource_type, "
        "is_node_type = excluded.is_node_type, "
        "script_file_id = (SELECT id FROM project_files WHERE project_relative_path = excluded.script_project_relative_path), "
        "last_seen_scan_run_id = excluded.last_seen_scan_run_id, "
        "updated_at_unix = excluded.updated_at_unix;"
    );
    statement.bind_text(1, class_name);
    statement.bind_text(2, script_path);
    statement.bind_text(3, script_project_relative_path);
    statement.bind_text(4, language);
    statement.bind_text(5, base_type);
    statement.bind_int64(6, is_resource_type);
    statement.bind_int64(7, is_node_type);
    statement.bind_int64(8, scan_run_id);
    statement.bind_int64(9, observed_at_unix);
    statement.step_done();
}

void ProjectInventoryRepository::delete_missing_custom_classes(int64_t scan_run_id) {
    Statement statement = database_->prepare(
        "DELETE FROM project_custom_classes WHERE last_seen_scan_run_id IS NULL OR last_seen_scan_run_id <> ?1;"
    );
    statement.bind_int64(1, scan_run_id);
    statement.step_done();
}

void ProjectInventoryRepository::upsert_unknown(
    const godot::Dictionary &entry,
    int64_t scan_run_id,
    int64_t observed_at_unix
) {
    const std::string project_relative_path = to_utf8(variant_to_string(entry.get("project_relative_path", "")));
    const std::string file_name = to_utf8(variant_to_string(entry.get("file_name", "")));
    const std::string extension = to_utf8(variant_to_string(entry.get("extension", "")));
    const std::string observed_file_type = to_utf8(variant_to_string(entry.get("observed_file_type", "Unknown")));
    const std::string observed_godot_type = to_utf8(variant_to_string(entry.get("observed_godot_type", "NGT")));

    Statement statement = database_->prepare(
        "INSERT INTO project_scan_unknowns ("
        "project_relative_path, file_name, extension, observed_file_type, observed_godot_type, "
        "last_seen_scan_run_id, created_at_unix, updated_at_unix"
        ") VALUES ("
        "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?7"
        ") "
        "ON CONFLICT(project_relative_path, extension) DO UPDATE SET "
        "file_name = excluded.file_name, "
        "observed_file_type = excluded.observed_file_type, "
        "observed_godot_type = excluded.observed_godot_type, "
        "last_seen_scan_run_id = excluded.last_seen_scan_run_id, "
        "updated_at_unix = excluded.updated_at_unix;"
    );
    statement.bind_text(1, project_relative_path);
    statement.bind_text(2, file_name);
    statement.bind_text(3, extension);
    statement.bind_text(4, observed_file_type);
    statement.bind_text(5, observed_godot_type.empty() ? std::string("NGT") : observed_godot_type);
    statement.bind_int64(6, scan_run_id);
    statement.bind_int64(7, observed_at_unix);
    statement.step_done();
}

void ProjectInventoryRepository::delete_missing_unknowns(int64_t scan_run_id) {
    Statement statement = database_->prepare(
        "DELETE FROM project_scan_unknowns WHERE last_seen_scan_run_id IS NULL OR last_seen_scan_run_id <> ?1;"
    );
    statement.bind_int64(1, scan_run_id);
    statement.step_done();
}

} // namespace gotool::database

