#include "gotool_project_context.hpp"

#include "database/gotool_project_inventory_repository.hpp"
#include "database/gotool_project_registry_repository.hpp"
#include "database/gotool_schema.hpp"
#include "project_scanner/native_scan_pipeline.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace godot {

namespace {

std::string to_utf8(const String &value) {
    const CharString utf8_value = value.utf8();
    return utf8_value.get_data() != nullptr ? utf8_value.get_data() : "";
}

String from_utf8(const std::string &value) {
    return String::utf8(value.c_str());
}

std::string to_utf8_from_u8(const std::u8string &value) {
    return std::string(reinterpret_cast<const char *>(value.c_str()), value.size());
}

std::filesystem::path godot_string_to_path(const String &value) {
    return std::filesystem::u8path(to_utf8(value));
}

String path_to_godot_string(const std::filesystem::path &value) {
    return from_utf8(to_utf8_from_u8(value.generic_u8string()));
}

std::filesystem::path canonicalize_existing_path(
    const std::filesystem::path &path,
    const std::string &label
) {
    std::error_code absolute_error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(path, absolute_error);

    if (absolute_error) {
        throw std::runtime_error(
            "Failed to build absolute path for " + label + ": " + absolute_error.message()
        );
    }

    std::error_code canonical_error;
    const std::filesystem::path canonical_path =
        std::filesystem::weakly_canonical(absolute_path, canonical_error);

    if (canonical_error) {
        throw std::runtime_error(
            "Failed to canonicalize " + label + ": " + canonical_error.message()
        );
    }

    return canonical_path.lexically_normal();
}

std::filesystem::path normalize_absolute_path(const std::filesystem::path &path) {
    std::error_code absolute_error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(path, absolute_error);

    if (absolute_error) {
        throw std::runtime_error(
            "Failed to build absolute path: " + absolute_error.message()
        );
    }

    return absolute_path.lexically_normal();
}

std::string read_text_file_first_line(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::in | std::ios::binary);

    if (!input.is_open()) {
        return "";
    }

    std::string line;
    std::getline(input, line);

    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }

    size_t non_space_index = 0;
    while (non_space_index < line.size() && (line[non_space_index] == ' ' || line[non_space_index] == '\t')) {
        ++non_space_index;
    }

    return line.substr(non_space_index);
}

std::string make_uid_from_seed(const std::string &seed_prefix, const std::string &seed) {
    const String seed_string = String::utf8(seed.c_str());
    const String hash = seed_string.sha256_text();
    return seed_prefix + to_utf8(hash.substr(0, 40));
}

std::string make_path_derived_uid(
    const std::filesystem::path &canonical_root,
    const std::filesystem::path &project_file_path
) {
    const std::string seed =
        to_utf8_from_u8(canonical_root.generic_u8string()) + "|" +
        to_utf8_from_u8(project_file_path.generic_u8string());

    return make_uid_from_seed("path-derived-", seed);
}

std::string make_generated_uid(
    const std::filesystem::path &canonical_root,
    const std::filesystem::path &project_file_path
) {
    const std::string seed =
        to_utf8_from_u8(canonical_root.generic_u8string()) + "|" +
        to_utf8_from_u8(project_file_path.generic_u8string()) + "|" +
        std::to_string(static_cast<int64_t>(std::time(nullptr)));

    return make_uid_from_seed("gotool-", seed);
}

String build_godot_version_string() {
    Engine *engine = Engine::get_singleton();

    if (engine == nullptr) {
        return "";
    }

    const Dictionary version_info = engine->get_version_info();

    const int64_t major = static_cast<int64_t>(version_info.get("major", 0));
    const int64_t minor = static_cast<int64_t>(version_info.get("minor", 0));
    const int64_t patch = static_cast<int64_t>(version_info.get("patch", 0));
    const String status = version_info.get("status", "");

    String version_text =
        String::num_int64(major) + "." +
        String::num_int64(minor) + "." +
        String::num_int64(patch);

    if (!status.is_empty()) {
        version_text += "-" + status;
    }

    return version_text;
}

Dictionary to_project_dictionary(const gotool::database::ProjectListItem &item) {
    Dictionary project;
    project["project_id"] = item.project_id;
    project["project_uid"] = from_utf8(item.project_uid);
    project["display_name"] = from_utf8(item.display_name);
    project["root_absolute_path"] = from_utf8(item.root_absolute_path);
    project["root_canonical_path"] = from_utf8(item.root_canonical_path);
    project["project_file_absolute_path"] = from_utf8(item.project_file_absolute_path);
    project["godot_version"] = from_utf8(item.godot_version);
    project["identity_source"] = from_utf8(item.identity_source);
    project["identity_warning"] = from_utf8(item.identity_warning);
    project["is_path_derived_identity"] = item.identity_source == "path_derived";
    project["first_seen_unix"] = item.first_seen_unix;
    project["last_seen_unix"] = item.last_seen_unix;
    project["created_at_unix"] = item.created_at_unix;
    project["updated_at_unix"] = item.updated_at_unix;
    return project;
}

gotool::project_scanner::FileQuery file_query_from_dictionary(const Dictionary &filter) {
    gotool::project_scanner::FileQuery query;
    query.include_deleted = static_cast<bool>(filter.get("include_deleted", false));

    if (filter.has("parent_id")) {
        query.parent_id = static_cast<int64_t>(filter.get("parent_id", 0));
    }

    if (filter.has("is_directory")) {
        query.is_directory = static_cast<bool>(filter.get("is_directory", false));
    }

    query.extension = to_utf8(String(filter.get("extension", "")));
    query.file_type = to_utf8(String(filter.get("file_type", "")));
    query.godot_type = to_utf8(String(filter.get("godot_type", "")));
    query.search = to_utf8(String(filter.get("search", "")));
    return query;
}

gotool::project_scanner::CustomClassQuery custom_class_query_from_dictionary(const Dictionary &filter) {
    gotool::project_scanner::CustomClassQuery query;
    query.language = to_utf8(String(filter.get("language", "")));
    query.base_type = to_utf8(String(filter.get("base_type", "")));
    query.search = to_utf8(String(filter.get("search", "")));
    return query;
}

Dictionary file_row_to_dictionary(const gotool::project_scanner::FileRow &row) {
    Dictionary entry;
    entry["id"] = row.id;
    entry["file_id"] = row.id;
    entry["parent_id"] = row.parent_id;
    entry["path"] = from_utf8("res://" + row.project_relative_path);
    entry["project_relative_path"] = from_utf8(row.project_relative_path);
    entry["file_name"] = from_utf8(row.file_name);
    entry["name"] = from_utf8(row.file_name);
    entry["extension"] = from_utf8(row.extension);
    entry["file_type"] = from_utf8(row.file_type);
    entry["type"] = row.is_directory ? "folder" : "file";
    entry["godot_type"] = from_utf8(row.godot_type);
    entry["godot_type_hint"] = from_utf8(row.godot_type);
    entry["type_hint_source"] = from_utf8(row.type_hint_source);
    entry["size_bytes"] = row.size_bytes;
    entry["modified_time_ns"] = row.modified_time_ns;
    entry["modified_time_unix"] = row.modified_time_ns / 1'000'000'000LL;
    entry["is_directory"] = row.is_directory;
    entry["is_hidden"] = row.is_hidden;
    entry["is_deleted"] = row.is_deleted;
    entry["scan_generation"] = row.scan_generation;
    entry["last_seen_generation"] = row.last_seen_generation;
    entry["dirty_state"] = from_utf8(row.dirty_state);
    entry["dirty_reason"] = from_utf8(row.dirty_reason);
    return entry;
}

Dictionary custom_class_row_to_dictionary(const gotool::project_scanner::CustomClassRow &row) {
    Dictionary entry;
    entry["id"] = row.id;
    entry["class_name"] = from_utf8(row.class_name);
    entry["script_path"] = from_utf8(row.script_path);
    entry["script_project_relative_path"] = from_utf8(row.script_project_relative_path);
    entry["language"] = from_utf8(row.language);
    entry["base_type"] = from_utf8(row.direct_base_type);
    entry["direct_base_type"] = from_utf8(row.direct_base_type);
    entry["is_resource_type"] = row.is_resource_type;
    entry["is_node_type"] = row.is_node_type;
    entry["script_file_id"] = row.script_file_id;
    entry["parser_version"] = row.parser_version;
    entry["parse_status"] = from_utf8(row.parse_status);
    entry["parse_error"] = from_utf8(row.parse_error);
    entry["last_parsed_generation"] = row.last_parsed_generation;
    return entry;
}

Dictionary metrics_to_dictionary(const gotool::project_scanner::ScanMetrics &metrics) {
    Dictionary result;
    result["total_wall_ms"] = metrics.total_wall_ms;
    result["traversal_ms"] = metrics.traversal_ms;
    result["metadata_ms"] = metrics.metadata_ms;
    result["dirty_check_ms"] = metrics.dirty_check_ms;
    result["classification_ms"] = metrics.classification_ms;
    result["script_parse_ms"] = metrics.script_parse_ms;
    result["sqlite_write_ms"] = metrics.sqlite_write_ms;
    result["godot_materialization_ms"] = metrics.godot_materialization_ms;
    result["files_seen"] = metrics.files_seen;
    result["dirs_seen"] = metrics.dirs_seen;
    result["dirs_skipped"] = metrics.dirs_skipped;
    result["entries_clean"] = metrics.entries_clean;
    result["entries_dirty"] = metrics.entries_dirty;
    result["entries_new"] = metrics.entries_new;
    result["entries_deleted"] = metrics.entries_deleted;
    result["rows_inserted"] = metrics.rows_inserted;
    result["rows_updated"] = metrics.rows_updated;
    result["rows_tombstoned"] = metrics.rows_tombstoned;
    result["scripts_candidates"] = metrics.scripts_candidates;
    result["scripts_parsed"] = metrics.scripts_parsed;
    result["scripts_skipped_clean"] = metrics.scripts_skipped_clean;
    result["bytes_read"] = metrics.bytes_read;
    result["sqlite_transactions"] = metrics.sqlite_transactions;
    result["ui_rows_materialized"] = metrics.ui_rows_materialized;
    result["cancellation_requested"] = metrics.cancellation_requested;
    result["scan_result_status"] = from_utf8(metrics.scan_result_status);
    return result;
}

Dictionary scan_summary_to_dictionary(const gotool::project_scanner::ScanResultSummary &summary) {
    Dictionary result;
    result["scan_id"] = summary.scan_run_id;
    result["scan_run_id"] = summary.scan_run_id;
    result["scan_generation"] = summary.scan_generation;
    result["status"] = from_utf8(summary.status);
    result["files_seen"] = summary.files_seen;
    result["dirs_seen"] = summary.dirs_seen;
    result["entries_clean"] = summary.entries_clean;
    result["entries_dirty"] = summary.entries_dirty;
    result["entries_new"] = summary.entries_new;
    result["entries_deleted"] = summary.entries_deleted;
    result["scripts_candidates"] = summary.scripts_candidates;
    result["scripts_parsed"] = summary.scripts_parsed;
    result["scripts_skipped_clean"] = summary.scripts_skipped_clean;
    result["total_wall_ms"] = summary.total_wall_ms;
    return result;
}

} // namespace

void GodotProjectContext::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("initialize_database"),
        &GodotProjectContext::initialize_database
    );

    ClassDB::bind_method(
        D_METHOD("register_current_project"),
        &GodotProjectContext::register_current_project
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
        D_METHOD("scan_current_project"),
        &GodotProjectContext::scan_current_project
    );

    ClassDB::bind_method(
        D_METHOD("scan_project"),
        &GodotProjectContext::scan_project
    );

    ClassDB::bind_method(
        D_METHOD("start_scan", "options"),
        &GodotProjectContext::start_scan
    );

    ClassDB::bind_method(
        D_METHOD("cancel_scan", "scan_id"),
        &GodotProjectContext::cancel_scan
    );

    ClassDB::bind_method(
        D_METHOD("get_scan_status", "scan_id"),
        &GodotProjectContext::get_scan_status
    );

    ClassDB::bind_method(
        D_METHOD("get_scan_metrics", "scan_id"),
        &GodotProjectContext::get_scan_metrics
    );

    ClassDB::bind_method(
        D_METHOD("get_file_count", "filter"),
        &GodotProjectContext::get_file_count
    );

    ClassDB::bind_method(
        D_METHOD("get_files_page", "offset", "limit", "sort", "filter"),
        &GodotProjectContext::get_files_page
    );

    ClassDB::bind_method(
        D_METHOD("get_file_details", "file_id"),
        &GodotProjectContext::get_file_details
    );

    ClassDB::bind_method(
        D_METHOD("get_directory_children", "directory_id", "offset", "limit", "sort", "filter"),
        &GodotProjectContext::get_directory_children
    );

    ClassDB::bind_method(
        D_METHOD("get_custom_class_count", "filter"),
        &GodotProjectContext::get_custom_class_count
    );

    ClassDB::bind_method(
        D_METHOD("get_custom_classes_page", "offset", "limit", "sort", "filter"),
        &GodotProjectContext::get_custom_classes_page
    );

    ClassDB::bind_method(
        D_METHOD("export_full_inventory_for_debug"),
        &GodotProjectContext::export_full_inventory_for_debug
    );

    ClassDB::bind_method(
        D_METHOD("list_projects"),
        &GodotProjectContext::list_projects
    );

    ClassDB::bind_method(
        D_METHOD("get_project_summary", "project_id"),
        &GodotProjectContext::get_project_summary
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
    current_project_id_ = 0;
    current_identity_source_ = "";
    current_identity_warning_ = "";

    OS *os = OS::get_singleton();

    if (os == nullptr) {
        last_error_ = "OS singleton was not available.";
        database_.reset();
        return false;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();

    if (project_settings == nullptr) {
        last_error_ = "ProjectSettings singleton was not available.";
        database_.reset();
        return false;
    }

    try {
        std::filesystem::path os_data_root = godot_string_to_path(os->get_data_dir());

        if (os_data_root.empty()) {
            os_data_root = godot_string_to_path(os->get_user_data_dir());
        }

        if (os_data_root.empty()) {
            throw std::runtime_error("OS data directory was empty.");
        }

        const std::filesystem::path storage_root =
            normalize_absolute_path(os_data_root / "GoToolCenter");

        std::error_code mkdir_error;
        std::filesystem::create_directories(storage_root, mkdir_error);

        if (mkdir_error) {
            throw std::runtime_error(
                "Failed to create GoTool storage directory '" +
                to_utf8_from_u8(storage_root.generic_u8string()) +
                "': " + mkdir_error.message()
            );
        }

        const std::filesystem::path database_path = storage_root / "gotool_center.sqlite3";
        database_ = std::make_unique<gotool::database::Database>(
            to_utf8_from_u8(database_path.generic_u8string())
        );

        // First pass creates/ensures the v2 schema and projects table. Legacy migration
        // is deferred until register_current_project() supplies a concrete project_id.
        gotool::database::create_schema(*database_, 0);

        const int64_t project_id = register_current_project();

        if (project_id <= 0) {
            database_.reset();
            return false;
        }

        database_absolute_path_ = path_to_godot_string(database_path.lexically_normal());

        const String localized_database_path =
            project_settings->localize_path(database_absolute_path_);

        if (localized_database_path.begins_with("res://") ||
            localized_database_path.begins_with("user://")) {
            database_virtual_path_ = localized_database_path;
        } else {
            database_virtual_path_ = database_absolute_path_;
        }

        last_scan_results_.clear();
        last_error_ = "";
        return true;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        database_.reset();
        return false;
    }
}

int64_t GodotProjectContext::register_current_project() {
    last_error_ = "";

    if (database_ == nullptr) {
        last_error_ = "Database is not initialized. Call initialize_database() first.";
        current_project_id_ = 0;
        return 0;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();

    if (project_settings == nullptr) {
        last_error_ = "ProjectSettings singleton was not available.";
        current_project_id_ = 0;
        return 0;
    }

    try {
        const std::filesystem::path project_root_absolute =
            normalize_absolute_path(godot_string_to_path(project_settings->globalize_path("res://")));

        const std::filesystem::path project_file_absolute =
            normalize_absolute_path(godot_string_to_path(project_settings->globalize_path("res://project.godot")));

        if (!std::filesystem::exists(project_file_absolute)) {
            throw std::runtime_error(
                "Project file was not found at '" +
                to_utf8_from_u8(project_file_absolute.generic_u8string()) +
                "'."
            );
        }

        const std::filesystem::path project_root_canonical =
            canonicalize_existing_path(project_root_absolute, "project root");

        const std::filesystem::path project_file_canonical =
            canonicalize_existing_path(project_file_absolute, "project.godot file");

        const std::filesystem::path uid_file_path =
            project_root_canonical / ".godot" / "gotool_center" / "project.uid";

        std::string project_uid = read_text_file_first_line(uid_file_path);
        std::string identity_source = "uid_file";
        std::string identity_warning;

        if (project_uid.empty()) {
            std::error_code mkdir_error;
            std::filesystem::create_directories(uid_file_path.parent_path(), mkdir_error);

            if (!mkdir_error) {
                const std::string generated_uid =
                    make_generated_uid(project_root_canonical, project_file_canonical);

                std::ofstream uid_output(uid_file_path, std::ios::out | std::ios::trunc);

                if (uid_output.is_open()) {
                    uid_output << generated_uid << '\n';
                    uid_output.flush();

                    if (uid_output.good()) {
                        project_uid = generated_uid;
                    } else {
                        identity_source = "path_derived";
                        identity_warning =
                            "Could not persist project UID to '" +
                            to_utf8_from_u8(uid_file_path.generic_u8string()) +
                            "'. Falling back to path-derived identity.";
                    }
                } else {
                    identity_source = "path_derived";
                    identity_warning =
                        "Could not open project UID file for writing at '" +
                        to_utf8_from_u8(uid_file_path.generic_u8string()) +
                        "'. Falling back to path-derived identity.";
                }
            } else {
                identity_source = "path_derived";
                identity_warning =
                    "Could not create UID directory '" +
                    to_utf8_from_u8(uid_file_path.parent_path().generic_u8string()) +
                    "': " + mkdir_error.message() +
                    ". Falling back to path-derived identity.";
            }
        }

        if (project_uid.empty()) {
            identity_source = "path_derived";
            if (identity_warning.empty()) {
                identity_warning = "Falling back to path-derived identity because project UID was unavailable.";
            }
            project_uid = make_path_derived_uid(project_root_canonical, project_file_canonical);
        }

        String display_name = project_settings->get_setting("application/config/name", "");

        if (display_name.is_empty()) {
            display_name = path_to_godot_string(project_root_canonical.filename());
        }

        gotool::database::ProjectRegistrationInput registration_input;
        registration_input.project_uid = project_uid;
        registration_input.display_name = to_utf8(display_name);
        registration_input.root_absolute_path = project_root_absolute;
        registration_input.root_canonical_path = project_root_canonical;
        registration_input.project_file_absolute_path = project_file_canonical;
        registration_input.godot_version = to_utf8(build_godot_version_string());
        registration_input.identity_source = identity_source;
        registration_input.identity_warning = identity_warning;
        registration_input.observed_at_unix = static_cast<int64_t>(std::time(nullptr));

        gotool::database::ProjectRegistryRepository registry(*database_);
        const gotool::database::RegisteredProject registered_project =
            registry.register_project(registration_input);

        gotool::database::create_schema(*database_, registered_project.project_id);

        current_project_id_ = registered_project.project_id;
        current_identity_source_ = from_utf8(registered_project.identity_source);
        current_identity_warning_ = from_utf8(registered_project.identity_warning);
        return current_project_id_;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        current_project_id_ = 0;
        current_identity_source_ = "";
        current_identity_warning_ = "";
        return 0;
    }
}

bool GodotProjectContext::scan_current_project() {
    const Dictionary result = start_scan(Dictionary());
    return !result.is_empty() && String(result.get("status", "")) == "completed";
}

bool GodotProjectContext::scan_project() {
    return scan_current_project();
}

Dictionary GodotProjectContext::start_scan(const Dictionary &options) {
    last_error_ = "";

    if (database_ == nullptr) {
        last_error_ = "Database is not initialized. Call initialize_database() first.";
        last_scan_results_.clear();
        return Dictionary();
    }

    const int64_t project_id = register_current_project();

    if (project_id <= 0) {
        last_scan_results_.clear();
        return Dictionary();
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();

    if (project_settings == nullptr) {
        last_error_ = "ProjectSettings singleton was not available.";
        last_scan_results_.clear();
        return Dictionary();
    }

    try {
        gotool::project_scanner::ScanOptions scan_options;
        scan_options.project_id = project_id;
        scan_options.project_root = normalize_absolute_path(
            godot_string_to_path(project_settings->globalize_path("res://"))
        );
        scan_options.include_hidden = static_cast<bool>(options.get("include_hidden", true));
        scan_options.force_rescan = static_cast<bool>(options.get("force_rescan", false));

        gotool::project_scanner::NativeScanPipeline pipeline(*database_);
        const gotool::project_scanner::ScanResultSummary summary = pipeline.run(scan_options);

        last_scan_results_ = scan_summary_to_dictionary(summary);
        last_scan_results_["metrics"] = metrics_to_dictionary(
            gotool::project_scanner::ScanRepository(*database_).get_scan_metrics(project_id, summary.scan_run_id)
        );
        last_error_ = "";
        return last_scan_results_;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        last_scan_results_.clear();
        return Dictionary();
    }
}

bool GodotProjectContext::cancel_scan(int64_t scan_id) {
    (void)scan_id;
    return false;
}

Dictionary GodotProjectContext::get_scan_status(int64_t scan_id) const {
    Dictionary result;

    if (database_ == nullptr || current_project_id_ <= 0 || scan_id <= 0) {
        return result;
    }

    try {
        gotool::project_scanner::ScanRepository repository(*database_);
        result["scan_id"] = scan_id;
        result["status"] = from_utf8(repository.get_scan_status(current_project_id_, scan_id));
    } catch (...) {
        return Dictionary();
    }

    return result;
}

Dictionary GodotProjectContext::get_scan_metrics(int64_t scan_id) const {
    if (database_ == nullptr || current_project_id_ <= 0 || scan_id <= 0) {
        return Dictionary();
    }

    try {
        gotool::project_scanner::ScanRepository repository(*database_);
        return metrics_to_dictionary(repository.get_scan_metrics(current_project_id_, scan_id));
    } catch (...) {
        return Dictionary();
    }
}

int64_t GodotProjectContext::get_file_count(const Dictionary &filter) const {
    if (database_ == nullptr || current_project_id_ <= 0) {
        return 0;
    }

    try {
        gotool::project_scanner::ScanRepository repository(*database_);
        return repository.count_files(current_project_id_, file_query_from_dictionary(filter));
    } catch (...) {
        return 0;
    }
}

Array GodotProjectContext::get_files_page(
    int64_t offset,
    int64_t limit,
    const String &sort,
    const Dictionary &filter
) const {
    Array rows;

    if (database_ == nullptr || current_project_id_ <= 0) {
        return rows;
    }

    try {
        gotool::project_scanner::ScanRepository repository(*database_);
        const std::vector<gotool::project_scanner::FileRow> page = repository.list_files(
            current_project_id_,
            file_query_from_dictionary(filter),
            offset,
            limit,
            to_utf8(sort)
        );

        for (const gotool::project_scanner::FileRow &row : page) {
            rows.append(file_row_to_dictionary(row));
        }
    } catch (...) {
        return Array();
    }

    return rows;
}

Dictionary GodotProjectContext::get_file_details(int64_t file_id) const {
    if (database_ == nullptr || current_project_id_ <= 0 || file_id <= 0) {
        return Dictionary();
    }

    try {
        gotool::project_scanner::ScanRepository repository(*database_);
        const std::optional<gotool::project_scanner::FileRow> row =
            repository.get_file_details(current_project_id_, file_id);
        if (!row.has_value()) {
            return Dictionary();
        }
        return file_row_to_dictionary(row.value());
    } catch (...) {
        return Dictionary();
    }
}

Array GodotProjectContext::get_directory_children(
    int64_t directory_id,
    int64_t offset,
    int64_t limit,
    const String &sort,
    const Dictionary &filter
) const {
    Dictionary child_filter = filter.duplicate();
    child_filter["parent_id"] = directory_id < 0 ? 0 : directory_id;
    return get_files_page(offset, limit, sort, child_filter);
}

int64_t GodotProjectContext::get_custom_class_count(const Dictionary &filter) const {
    if (database_ == nullptr || current_project_id_ <= 0) {
        return 0;
    }

    try {
        gotool::project_scanner::ScanRepository repository(*database_);
        return repository.count_custom_classes(current_project_id_, custom_class_query_from_dictionary(filter));
    } catch (...) {
        return 0;
    }
}

Array GodotProjectContext::get_custom_classes_page(
    int64_t offset,
    int64_t limit,
    const String &sort,
    const Dictionary &filter
) const {
    Array rows;

    if (database_ == nullptr || current_project_id_ <= 0) {
        return rows;
    }

    try {
        gotool::project_scanner::ScanRepository repository(*database_);
        const std::vector<gotool::project_scanner::CustomClassRow> page = repository.list_custom_classes(
            current_project_id_,
            custom_class_query_from_dictionary(filter),
            offset,
            limit,
            to_utf8(sort)
        );

        for (const gotool::project_scanner::CustomClassRow &row : page) {
            rows.append(custom_class_row_to_dictionary(row));
        }
    } catch (...) {
        return Array();
    }

    return rows;
}

Dictionary GodotProjectContext::export_full_inventory_for_debug() const {
    Dictionary inventory;

    if (database_ == nullptr || current_project_id_ <= 0) {
        return inventory;
    }

    const int64_t file_count = get_file_count(Dictionary());
    const int64_t class_count = get_custom_class_count(Dictionary());

    Array files;
    for (int64_t offset = 0; offset < file_count; offset += 500) {
        const Array page = get_files_page(offset, 500, "path", Dictionary());
        for (int64_t i = 0; i < page.size(); ++i) {
            files.append(page[i]);
        }
    }

    Array custom_classes;
    for (int64_t offset = 0; offset < class_count; offset += 500) {
        const Array page = get_custom_classes_page(offset, 500, "class_name", Dictionary());
        for (int64_t i = 0; i < page.size(); ++i) {
            custom_classes.append(page[i]);
        }
    }

    inventory["files"] = files;
    inventory["custom_classes"] = custom_classes;
    inventory["file_count"] = file_count;
    inventory["custom_class_count"] = class_count;
    inventory["scan_summary"] = last_scan_results_;
    return inventory;
}

Array GodotProjectContext::list_projects() const {
    Array projects;

    if (database_ == nullptr) {
        return projects;
    }

    try {
        gotool::database::ProjectRegistryRepository registry(*database_);
        const std::vector<gotool::database::ProjectListItem> items = registry.list_projects();

        for (const gotool::database::ProjectListItem &item : items) {
            projects.append(to_project_dictionary(item));
        }
    } catch (...) {
        return projects;
    }

    return projects;
}

Dictionary GodotProjectContext::get_project_summary(int64_t project_id) const {
    Dictionary summary;

    if (database_ == nullptr || project_id <= 0) {
        return summary;
    }

    try {
        gotool::database::ProjectRegistryRepository registry(*database_);
        const std::optional<gotool::database::ProjectSummary> project_summary =
            registry.get_project_summary(project_id);

        if (!project_summary.has_value()) {
            return summary;
        }

        summary["project"] = to_project_dictionary(project_summary->project);
        summary["latest_scan_run_id"] = project_summary->latest_scan_run_id;
        summary["latest_scan_status"] = from_utf8(project_summary->latest_scan_status);
        summary["files_count"] = project_summary->files_count;
        summary["autoloads_count"] = project_summary->autoloads_count;
        summary["custom_classes_count"] = project_summary->custom_classes_count;
        summary["unknowns_count"] = project_summary->unknowns_count;
    } catch (...) {
        return Dictionary();
    }

    return summary;
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
