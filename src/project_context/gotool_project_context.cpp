#include "gotool_project_context.hpp"

#include "database/gotool_project_inventory_repository.hpp"
#include "database/gotool_project_registry_repository.hpp"
#include "database/gotool_schema.hpp"
#include "project_scanner/file_watcher.hpp"
#include "project_scanner/native_scan_pipeline.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <ctime>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
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
    result["existing_snapshot_load_ms"] = metrics.existing_snapshot_load_ms;
    result["reserve_setup_ms"] = metrics.reserve_setup_ms;
    result["dirty_check_ms"] = metrics.dirty_check_ms;
    result["script_candidate_ms"] = metrics.script_candidate_ms;
    result["classification_ms"] = metrics.classification_ms;
    result["script_parse_ms"] = metrics.script_parse_ms;
    result["sqlite_write_ms"] = metrics.sqlite_write_ms;
    result["sqlite_stage_insert_ms"] = metrics.sqlite_stage_insert_ms;
    result["sqlite_file_merge_ms"] = metrics.sqlite_file_merge_ms;
    result["sqlite_clean_refresh_ms"] = metrics.sqlite_clean_refresh_ms;
    result["sqlite_parent_resolve_ms"] = metrics.sqlite_parent_resolve_ms;
    result["sqlite_parse_status_ms"] = metrics.sqlite_parse_status_ms;
    result["sqlite_custom_class_ms"] = metrics.sqlite_custom_class_ms;
    result["sqlite_tombstone_ms"] = metrics.sqlite_tombstone_ms;
    result["sqlite_deleted_reconcile_ms"] = metrics.sqlite_deleted_reconcile_ms;
    result["sqlite_metrics_write_ms"] = metrics.sqlite_metrics_write_ms;
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
    result["rows_clean_refreshed"] = metrics.rows_clean_refreshed;
    result["rows_tombstoned"] = metrics.rows_tombstoned;
    result["scripts_candidates"] = metrics.scripts_candidates;
    result["scripts_parsed"] = metrics.scripts_parsed;
    result["scripts_skipped_clean"] = metrics.scripts_skipped_clean;
    result["script_lines_scanned"] = metrics.script_lines_scanned;
    result["bytes_read"] = metrics.bytes_read;
    result["entry_record_count"] = metrics.entry_record_count;
    result["path_arena_bytes"] = metrics.path_arena_bytes;
    result["existing_snapshot_count"] = metrics.existing_snapshot_count;
    result["parsed_script_count"] = metrics.parsed_script_count;
    result["sqlite_statement_steps"] = metrics.sqlite_statement_steps;
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

std::string trim_ascii(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

std::string trim_inline_comment(const std::string &value) {
    const size_t comment = value.find(';');
    if (comment == std::string::npos) {
        return trim_ascii(value);
    }
    return trim_ascii(std::string_view(value).substr(0, comment));
}

std::string unquote(std::string value) {
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

struct ParsedAutoload {
    std::string autoload_name;
    std::string target_path;
    bool is_singleton = false;
};

std::vector<ParsedAutoload> parse_project_autoloads(const std::filesystem::path &project_root) {
    std::vector<ParsedAutoload> autoloads;
    const std::filesystem::path project_file = project_root / "project.godot";

    std::ifstream input(project_file, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return autoloads;
    }

    bool in_autoload_section = false;
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trim_ascii(line);
        if (trimmed.empty()) {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            in_autoload_section = (trimmed == "[autoload]");
            continue;
        }

        if (!in_autoload_section || trimmed.front() == ';' || trimmed.front() == '#') {
            continue;
        }

        const size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trim_ascii(std::string_view(trimmed).substr(0, separator));
        std::string value = trim_inline_comment(trimmed.substr(separator + 1));
        if (key.empty() || value.empty()) {
            continue;
        }

        value = unquote(value);

        ParsedAutoload entry;
        entry.autoload_name = key;
        entry.is_singleton = !value.empty() && value.front() == '*';
        if (entry.is_singleton) {
            value.erase(value.begin());
        }

        entry.target_path = trim_ascii(value);
        if (!entry.target_path.empty()) {
            autoloads.push_back(std::move(entry));
        }
    }

    return autoloads;
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
        D_METHOD("scan_project_inventory_fast", "options"),
        &GodotProjectContext::scan_project_inventory_fast
    );

    ClassDB::bind_method(
        D_METHOD("scan_current_project_fast", "options"),
        &GodotProjectContext::scan_current_project_fast
    );

    ClassDB::bind_method(
        D_METHOD("start_watcher"),
        &GodotProjectContext::start_watcher
    );

    ClassDB::bind_method(
        D_METHOD("stop_watcher"),
        &GodotProjectContext::stop_watcher
    );

    ClassDB::bind_method(
        D_METHOD("get_watcher_status"),
        &GodotProjectContext::get_watcher_status
    );

    ClassDB::bind_method(
        D_METHOD("consume_watcher_changes"),
        &GodotProjectContext::consume_watcher_changes
    );

    ClassDB::bind_method(
        D_METHOD("get_dirty_paths"),
        &GodotProjectContext::get_dirty_paths
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

GodotProjectContext::~GodotProjectContext() {
    stop_scan_worker();
    stop_watcher();
}

int64_t GodotProjectContext::current_unix_time() {
    return static_cast<int64_t>(std::time(nullptr));
}

std::filesystem::path GodotProjectContext::get_current_project_root_path() const {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (project_settings == nullptr) {
        throw std::runtime_error("ProjectSettings singleton was not available.");
    }

    return normalize_absolute_path(
        godot_string_to_path(project_settings->globalize_path("res://"))
    );
}

void GodotProjectContext::join_finished_scan_worker_if_idle() {
    std::thread worker_to_join;

    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        if (!scan_worker_.joinable()) {
            return;
        }

        const bool has_active = active_scan_state_ != nullptr;
        const bool is_running =
            has_active &&
            (active_scan_state_->status == "queued" || active_scan_state_->status == "running");

        if (!is_running) {
            worker_to_join = std::move(scan_worker_);
        }
    }

    if (worker_to_join.joinable()) {
        worker_to_join.join();
    }
}

void GodotProjectContext::stop_scan_worker() {
    std::thread worker_to_join;

    {
        std::lock_guard<std::mutex> lock(scan_mutex_);

        if (active_scan_state_ != nullptr &&
            (active_scan_state_->status == "queued" || active_scan_state_->status == "running")) {
            active_scan_state_->cancellation_requested.store(true, std::memory_order_relaxed);
        }

        if (scan_worker_.joinable()) {
            worker_to_join = std::move(scan_worker_);
        }
    }

    if (worker_to_join.joinable()) {
        worker_to_join.join();
    }

    scan_cv_.notify_all();
}

Dictionary GodotProjectContext::active_state_to_summary_dictionary(const ActiveScanState &state) const {
    const gotool::project_scanner::ScanResultSummary native_summary = {
        state.scan_id,
        state.scan_generation,
        state.status,
        state.metrics.files_seen,
        state.metrics.dirs_seen,
        state.metrics.entries_clean,
        state.metrics.entries_dirty,
        state.metrics.entries_new,
        state.metrics.entries_deleted,
        state.metrics.scripts_candidates,
        state.metrics.scripts_parsed,
        state.metrics.scripts_skipped_clean,
        state.metrics.total_wall_ms
    };

    Dictionary merged = scan_summary_to_dictionary(native_summary);
    merged["started_at_unix"] = state.started_at_unix;
    merged["finished_at_unix"] = state.finished_at_unix;
    merged["last_error"] = from_utf8(state.last_error);
    return merged;
}

void GodotProjectContext::sync_last_scan_results_from_active_state() const {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (active_scan_state_ == nullptr) {
        return;
    }

    if (active_scan_state_->status == "queued" || active_scan_state_->status == "running") {
        return;
    }

    last_scan_results_ = active_state_to_summary_dictionary(*active_scan_state_);
    last_scan_results_["metrics"] = metrics_to_dictionary(active_scan_state_->metrics);
    last_error_ = from_utf8(active_scan_state_->last_error);
}

bool GodotProjectContext::initialize_database() {
    stop_scan_worker();

    last_error_ = "";
    current_project_id_ = 0;
    current_identity_source_ = "";
    current_identity_warning_ = "";

    OS *os = OS::get_singleton();

    if (os == nullptr) {
        last_error_ = "OS singleton was not available.";
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        database_.reset();
        return false;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();

    if (project_settings == nullptr) {
        last_error_ = "ProjectSettings singleton was not available.";
        std::lock_guard<std::mutex> db_lock(database_mutex_);
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
        {
            std::lock_guard<std::mutex> db_lock(database_mutex_);
            database_ = std::make_unique<gotool::database::Database>(
                to_utf8_from_u8(database_path.generic_u8string())
            );

            // First pass creates/ensures the v2 schema and projects table. Legacy migration
            // is deferred until register_current_project() supplies a concrete project_id.
            gotool::database::create_schema(*database_, 0);
        }

        const int64_t project_id = register_current_project();

        if (project_id <= 0) {
            std::lock_guard<std::mutex> db_lock(database_mutex_);
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
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        database_.reset();
        return false;
    }
}

int64_t GodotProjectContext::register_current_project() {
    last_error_ = "";

    {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            last_error_ = "Database is not initialized. Call initialize_database() first.";
            current_project_id_ = 0;
            return 0;
        }
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

        gotool::database::RegisteredProject registered_project;
        {
            std::lock_guard<std::mutex> db_lock(database_mutex_);
            if (database_ == nullptr) {
                throw std::runtime_error("Database is not initialized. Call initialize_database() first.");
            }

            gotool::database::ProjectRegistryRepository registry(*database_);
            registered_project = registry.register_project(registration_input);
            gotool::database::create_schema(*database_, registered_project.project_id);
        }

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
    const Dictionary started = start_scan(Dictionary());
    if (started.is_empty()) {
        return false;
    }

    const int64_t scan_id = static_cast<int64_t>(started.get("scan_id", 0));
    if (scan_id <= 0) {
        return false;
    }

    std::unique_lock<std::mutex> lock(scan_mutex_);
    scan_cv_.wait(lock, [this, scan_id]() {
        if (active_scan_state_ == nullptr || active_scan_state_->scan_id != scan_id) {
            return true;
        }

        return active_scan_state_->status != "queued" &&
               active_scan_state_->status != "running";
    });

    const bool completed =
        active_scan_state_ != nullptr &&
        active_scan_state_->scan_id == scan_id &&
        active_scan_state_->status == "completed";

    lock.unlock();

    join_finished_scan_worker_if_idle();
    sync_last_scan_results_from_active_state();
    return completed;
}

bool GodotProjectContext::scan_project() {
    return scan_current_project();
}

Dictionary GodotProjectContext::start_scan(const Dictionary &options) {
    join_finished_scan_worker_if_idle();
    last_error_ = "";

    {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            last_error_ = "Database is not initialized. Call initialize_database() first.";
            last_scan_results_.clear();
            return Dictionary();
        }
    }

    const int64_t project_id = register_current_project();

    if (project_id <= 0) {
        last_scan_results_.clear();
        return Dictionary();
    }

    try {
        const bool include_hidden = static_cast<bool>(options.get("include_hidden", true));
        const bool force_rescan = static_cast<bool>(options.get("force_rescan", false));
        const bool include_custom_classes = static_cast<bool>(options.get("include_custom_classes", true));
        const bool include_deleted = static_cast<bool>(options.get("include_deleted", false));
        const std::filesystem::path project_root = get_current_project_root_path();

        {
            std::lock_guard<std::mutex> lock(scan_mutex_);
            if (active_scan_state_ != nullptr &&
                (active_scan_state_->status == "queued" || active_scan_state_->status == "running")) {
                Dictionary already_running;
                already_running["scan_id"] = active_scan_state_->scan_id;
                already_running["scan_run_id"] = active_scan_state_->scan_id;
                already_running["scan_generation"] = active_scan_state_->scan_generation;
                already_running["status"] = "already_running";
                already_running["active_status"] = from_utf8(active_scan_state_->status);
                already_running["started_at_unix"] = active_scan_state_->started_at_unix;
                return already_running;
            }
        }

        const int64_t started_at_unix = current_unix_time();
        int64_t scan_generation = 0;
        int64_t scan_id = 0;

        {
            std::lock_guard<std::mutex> db_lock(database_mutex_);
            if (database_ == nullptr) {
                throw std::runtime_error("Database is not initialized. Call initialize_database() first.");
            }

            gotool::project_scanner::ScanRepository repository(*database_);
            scan_generation = repository.next_generation(project_id);
            scan_id = repository.create_scan_run(project_id, scan_generation, started_at_unix);
        }

        auto state = std::make_shared<ActiveScanState>();
        state->scan_id = scan_id;
        state->project_id = project_id;
        state->scan_generation = scan_generation;
        state->started_at_unix = started_at_unix;
        state->status = "queued";

        {
            std::lock_guard<std::mutex> lock(scan_mutex_);
            active_scan_state_ = state;
        }

        if (scan_worker_.joinable()) {
            scan_worker_.join();
        }

        try {
            scan_worker_ = std::thread(
                [this,
                 state,
                 project_root,
                 include_hidden,
                 force_rescan,
                 include_custom_classes,
                 include_deleted]() {
                {
                    std::lock_guard<std::mutex> lock(scan_mutex_);
                    if (active_scan_state_ == state) {
                        active_scan_state_->status = "running";
                    }
                }
                scan_cv_.notify_all();

                try {
                    gotool::project_scanner::ScanOptions scan_options;
                    scan_options.project_id = state->project_id;
                    scan_options.project_root = project_root;
                    scan_options.cancel_requested = &state->cancellation_requested;
                    scan_options.include_hidden = include_hidden;
                    scan_options.force_rescan = force_rescan;
                    scan_options.persist_to_database = true;
                    scan_options.collect_custom_classes = include_custom_classes;
                    scan_options.include_deleted = include_deleted;
                    scan_options.scan_run_id = state->scan_id;
                    scan_options.scan_generation = state->scan_generation;
                    scan_options.started_at_unix = state->started_at_unix;

                    gotool::project_scanner::NativeScanResult native_result;
                    {
                        std::lock_guard<std::mutex> db_lock(database_mutex_);
                        if (database_ == nullptr) {
                            throw std::runtime_error("Database was released while scan worker was running.");
                        }
                        gotool::project_scanner::NativeScanPipeline pipeline(*database_);
                        native_result = pipeline.run_detailed(scan_options);
                    }

                    {
                        std::lock_guard<std::mutex> lock(scan_mutex_);
                        state->metrics = native_result.metrics;
                        state->status = native_result.summary.status;
                        state->finished_at_unix = current_unix_time();
                        state->last_error.clear();
                    }
                } catch (const std::exception &error) {
                    std::lock_guard<std::mutex> lock(scan_mutex_);
                    state->status = "failed";
                    state->finished_at_unix = current_unix_time();
                    state->last_error = error.what();
                    state->metrics.scan_result_status = "failed";
                }

                scan_cv_.notify_all();
            }
            );
        } catch (const std::exception &error) {
            gotool::project_scanner::ScanMetrics failed_metrics;
            failed_metrics.scan_result_status = "failed";

            {
                std::lock_guard<std::mutex> db_lock(database_mutex_);
                if (database_ != nullptr) {
                    gotool::project_scanner::ScanRepository repository(*database_);
                    repository.complete_scan_run(
                        project_id,
                        scan_id,
                        scan_generation,
                        failed_metrics,
                        current_unix_time(),
                        error.what()
                    );
                }
            }

            throw;
        }

        Dictionary started;
        started["scan_id"] = scan_id;
        started["scan_run_id"] = scan_id;
        started["scan_generation"] = scan_generation;
        started["status"] = "queued";
        started["started_at_unix"] = started_at_unix;

        last_scan_results_ = started;
        return started;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        last_scan_results_.clear();
        return Dictionary();
    }
}

bool GodotProjectContext::cancel_scan(int64_t scan_id) {
    if (scan_id <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (active_scan_state_ == nullptr || active_scan_state_->scan_id != scan_id) {
        return false;
    }

    if (active_scan_state_->status != "queued" && active_scan_state_->status != "running") {
        return false;
    }

    active_scan_state_->cancellation_requested.store(true, std::memory_order_relaxed);
    active_scan_state_->metrics.cancellation_requested = true;
    return true;
}

Dictionary GodotProjectContext::get_scan_status(int64_t scan_id) const {
    Dictionary result;

    if (scan_id <= 0) {
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        if (active_scan_state_ != nullptr && active_scan_state_->scan_id == scan_id) {
            result = active_state_to_summary_dictionary(*active_scan_state_);
            result["metrics"] = metrics_to_dictionary(active_scan_state_->metrics);
            if (active_scan_state_->status != "queued" && active_scan_state_->status != "running") {
                last_scan_results_ = result;
            }
            return result;
        }
    }

    if (database_ == nullptr || current_project_id_ <= 0) {
        return Dictionary();
    }

    try {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            return Dictionary();
        }

        gotool::project_scanner::ScanRepository repository(*database_);
        const std::string status = repository.get_scan_status(current_project_id_, scan_id);
        if (status.empty()) {
            return Dictionary();
        }
        result["scan_id"] = scan_id;
        result["scan_run_id"] = scan_id;
        result["status"] = from_utf8(status);
        result["metrics"] = metrics_to_dictionary(repository.get_scan_metrics(current_project_id_, scan_id));
    } catch (...) {
        return Dictionary();
    }

    return result;
}

Dictionary GodotProjectContext::get_scan_metrics(int64_t scan_id) const {
    if (scan_id <= 0) {
        return Dictionary();
    }

    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        if (active_scan_state_ != nullptr && active_scan_state_->scan_id == scan_id) {
            Dictionary metrics = metrics_to_dictionary(active_scan_state_->metrics);
            metrics["scan_id"] = scan_id;
            metrics["scan_run_id"] = scan_id;
            metrics["status"] = from_utf8(active_scan_state_->status);
            return metrics;
        }
    }

    if (database_ == nullptr || current_project_id_ <= 0) {
        return Dictionary();
    }

    try {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            return Dictionary();
        }

        gotool::project_scanner::ScanRepository repository(*database_);
        Dictionary metrics = metrics_to_dictionary(repository.get_scan_metrics(current_project_id_, scan_id));
        metrics["scan_id"] = scan_id;
        metrics["scan_run_id"] = scan_id;
        metrics["status"] = from_utf8(repository.get_scan_status(current_project_id_, scan_id));
        return metrics;
    } catch (...) {
        return Dictionary();
    }
}

Dictionary GodotProjectContext::scan_project_inventory_fast(const Dictionary &options) {
    last_error_ = "";

    try {
        const std::filesystem::path project_root = get_current_project_root_path();
        const bool include_hidden = static_cast<bool>(options.get("include_hidden", true));
        const bool include_custom_classes = static_cast<bool>(options.get("include_custom_classes", true));
        const bool force_rescan = static_cast<bool>(options.get("force_rescan", false));
        const int64_t max_results = static_cast<int64_t>(options.get("max_results", 0));

        int64_t project_id = 0;
        if (database_ != nullptr) {
            const int64_t registered_project_id = register_current_project();
            if (registered_project_id > 0) {
                project_id = registered_project_id;
            }
        }

        gotool::project_scanner::ScanOptions scan_options;
        scan_options.project_id = project_id;
        scan_options.project_root = project_root;
        scan_options.include_hidden = include_hidden;
        scan_options.force_rescan = force_rescan;
        scan_options.persist_to_database = false;
        scan_options.collect_custom_classes = include_custom_classes;
        scan_options.result_limit = max_results;

        gotool::project_scanner::NativeScanResult native_result;
        bool ran_with_database = false;
        if (database_ != nullptr && project_id > 0) {
            std::lock_guard<std::mutex> db_lock(database_mutex_);
            if (database_ != nullptr) {
                gotool::project_scanner::NativeScanPipeline pipeline(*database_);
                native_result = pipeline.run_detailed(scan_options);
                ran_with_database = true;
            }
        }

        if (!ran_with_database) {
            gotool::project_scanner::NativeScanPipeline pipeline;
            native_result = pipeline.run_detailed(scan_options);
        }

        Array files;
        for (const gotool::project_scanner::EntryRecord &record : native_result.records) {
            const std::string project_path =
                native_result.arena.string_at(record.path_offset, record.path_length);
            const std::string file_name =
                native_result.arena.string_at(record.name_offset, record.name_length);
            const std::filesystem::path absolute_path =
                (project_root / std::filesystem::u8path(project_path)).lexically_normal();

            Dictionary row;
            row["path"] = from_utf8("res://" + project_path);
            row["name"] = from_utf8(file_name);
            row["type"] = record.entry_kind == gotool::project_scanner::EntryKind::Directory ? "folder" : "file";
            row["project_relative_path"] = from_utf8(project_path);
            row["absolute_path"] = from_utf8(to_utf8_from_u8(absolute_path.generic_u8string()));
            row["file_name"] = from_utf8(file_name);
            row["extension"] = from_utf8(gotool::project_scanner::extension_from_path(project_path));
            row["file_type"] = from_utf8(gotool::project_scanner::to_string(record.file_type_id));
            row["godot_type"] = from_utf8(gotool::project_scanner::to_string(record.godot_type_hint));
            row["size_bytes"] = record.size_bytes;
            row["modified_time_unix"] = record.modified_time_ns / 1'000'000'000LL;
            row["modified_time_ns"] = record.modified_time_ns;
            row["is_directory"] = record.entry_kind == gotool::project_scanner::EntryKind::Directory;
            row["is_hidden"] = record.is_hidden();
            row["dirty_state"] = from_utf8(gotool::project_scanner::to_string(record.dirty_state));
            row["dirty_reason"] = from_utf8(gotool::project_scanner::to_string(record.dirty_reason));
            files.append(row);
        }

        Array custom_classes;
        if (include_custom_classes) {
            for (const gotool::project_scanner::ParsedScriptRecord &parsed : native_result.parsed_scripts) {
                if (parsed.parse_result.status != gotool::project_scanner::ParseStatus::ParsedClass ||
                    parsed.parse_result.class_name.empty()) {
                    continue;
                }

                const std::string direct_base = parsed.parse_result.direct_base_type;
                Dictionary row;
                row["class_name"] = from_utf8(parsed.parse_result.class_name);
                row["script_path"] = from_utf8("res://" + parsed.project_relative_path);
                row["script_project_relative_path"] = from_utf8(parsed.project_relative_path);
                row["language"] = from_utf8(gotool::project_scanner::to_string(parsed.parse_result.language));
                row["base_type"] = from_utf8(direct_base);
                row["direct_base_type"] = from_utf8(direct_base);
                row["is_resource_type"] = gotool::project_scanner::is_builtin_resource_type_hint(direct_base);
                row["is_node_type"] = gotool::project_scanner::is_builtin_node_type_hint(direct_base);
                row["parser_version"] = gotool::project_scanner::PARSER_VERSION;
                row["parse_status"] = from_utf8(gotool::project_scanner::to_string(parsed.parse_result.status));
                row["parse_error"] = from_utf8(parsed.parse_result.parse_error);
                row["last_parsed_generation"] = native_result.summary.scan_generation;
                custom_classes.append(row);
            }
        }

        Array autoloads;
        const std::vector<ParsedAutoload> parsed_autoloads = parse_project_autoloads(project_root);
        for (const ParsedAutoload &autoload : parsed_autoloads) {
            Dictionary row;
            row["autoload_name"] = from_utf8(autoload.autoload_name);
            row["target_path"] = from_utf8(autoload.target_path);
            row["target_project_relative_path"] =
                from_utf8(gotool::project_scanner::normalize_project_path(autoload.target_path));
            row["is_singleton"] = autoload.is_singleton;
            autoloads.append(row);
        }

        Dictionary inventory;
        inventory["files"] = files;
        inventory["autoloads"] = autoloads;
        inventory["custom_classes"] = custom_classes;
        inventory["metrics"] = metrics_to_dictionary(native_result.metrics);
        inventory["scan_summary"] = scan_summary_to_dictionary(native_result.summary);
        last_error_ = "";
        return inventory;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        return Dictionary();
    }
}

Dictionary GodotProjectContext::scan_current_project_fast(const Dictionary &options) {
    return scan_project_inventory_fast(options);
}

bool GodotProjectContext::start_watcher() {
    last_error_ = "";

    try {
        const std::filesystem::path project_root = get_current_project_root_path();
        std::lock_guard<std::mutex> lock(watcher_mutex_);
        if (file_watcher_ == nullptr) {
            file_watcher_ = std::make_unique<gotool::project_scanner::FileWatcher>();
        }

        const bool started = file_watcher_->start(project_root);
        if (!started) {
            const gotool::project_scanner::FileWatcherStatus status = file_watcher_->get_status();
            if (!status.last_error.empty()) {
                last_error_ = from_utf8(status.last_error);
            }
        }
        return started;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        return false;
    }
}

void GodotProjectContext::stop_watcher() {
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    if (file_watcher_ != nullptr) {
        file_watcher_->stop();
    }
}

Dictionary GodotProjectContext::get_watcher_status() const {
    Dictionary status;

    std::lock_guard<std::mutex> lock(watcher_mutex_);
    if (file_watcher_ == nullptr) {
        status["running"] = false;
        status["supported"] = false;
        status["requires_full_rescan"] = true;
        status["backend"] = "none";
        status["pending_events"] = 0;
        return status;
    }

    const gotool::project_scanner::FileWatcherStatus watcher_status = file_watcher_->get_status();
    status["running"] = watcher_status.running;
    status["supported"] = watcher_status.supported;
    status["requires_full_rescan"] = watcher_status.requires_full_rescan;
    status["backend"] = from_utf8(watcher_status.backend);
    status["last_error"] = from_utf8(watcher_status.last_error);
    status["pending_events"] = watcher_status.pending_events;
    return status;
}

Array GodotProjectContext::consume_watcher_changes() {
    Array events;

    std::lock_guard<std::mutex> lock(watcher_mutex_);
    if (file_watcher_ == nullptr) {
        return events;
    }

    const std::vector<gotool::project_scanner::FileWatcherEvent> drained = file_watcher_->drain_events();
    for (const gotool::project_scanner::FileWatcherEvent &event : drained) {
        Dictionary row;
        row["project_relative_path"] = from_utf8(event.project_relative_path);
        row["path"] = from_utf8("res://" + event.project_relative_path);
        row["removed"] = event.removed;
        row["is_directory"] = event.is_directory;
        events.append(row);
    }

    return events;
}

Array GodotProjectContext::get_dirty_paths() {
    Array dirty_paths;
    const Array changes = consume_watcher_changes();
    for (int64_t i = 0; i < changes.size(); ++i) {
        const Dictionary change = changes[i];
        dirty_paths.append(change.get("path", ""));
    }
    return dirty_paths;
}

int64_t GodotProjectContext::get_file_count(const Dictionary &filter) const {
    sync_last_scan_results_from_active_state();
    if (database_ == nullptr || current_project_id_ <= 0) {
        return 0;
    }

    try {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            return 0;
        }
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

    sync_last_scan_results_from_active_state();

    if (database_ == nullptr || current_project_id_ <= 0) {
        return rows;
    }

    try {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            return Array();
        }

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
    sync_last_scan_results_from_active_state();

    if (database_ == nullptr || current_project_id_ <= 0 || file_id <= 0) {
        return Dictionary();
    }

    try {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            return Dictionary();
        }

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
    sync_last_scan_results_from_active_state();

    if (database_ == nullptr || current_project_id_ <= 0) {
        return 0;
    }

    try {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            return 0;
        }

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

    sync_last_scan_results_from_active_state();

    if (database_ == nullptr || current_project_id_ <= 0) {
        return rows;
    }

    try {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            return Array();
        }

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

    sync_last_scan_results_from_active_state();

    if (database_ == nullptr) {
        return projects;
    }

    try {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            return projects;
        }

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

    sync_last_scan_results_from_active_state();

    if (database_ == nullptr || project_id <= 0) {
        return summary;
    }

    try {
        std::lock_guard<std::mutex> db_lock(database_mutex_);
        if (database_ == nullptr) {
            return Dictionary();
        }

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
    sync_last_scan_results_from_active_state();
    return last_scan_results_;
}

String GodotProjectContext::get_last_error() const {
    sync_last_scan_results_from_active_state();
    return last_error_;
}

} // namespace godot
