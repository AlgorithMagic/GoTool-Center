#include "project_scanner/native_scan_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <unordered_set>

namespace gotool::project_scanner {

namespace {

using gotool::database::Statement;
using gotool::database::Transaction;

int64_t current_unix_time() {
    return static_cast<int64_t>(std::time(nullptr));
}

int64_t elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

bool path_matches_prefix(std::string_view path, std::string_view prefix) {
    if (prefix.empty()) {
        return true;
    }

    if (path == prefix) {
        return true;
    }

    return path.size() > prefix.size() &&
           path.substr(0, prefix.size()) == prefix &&
           path[prefix.size()] == '/';
}

std::vector<std::string> normalize_dirty_paths(const std::vector<std::string> &dirty_paths) {
    std::vector<std::string> normalized;
    normalized.reserve(dirty_paths.size());

    for (const std::string &dirty_path : dirty_paths) {
        const std::string normalized_path = normalize_project_path(dirty_path);
        if (!normalized_path.empty()) {
            normalized.push_back(normalized_path);
        }
    }

    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    return normalized;
}

bool is_path_in_dirty_set(std::string_view path, const std::vector<std::string> &normalized_dirty_paths) {
    for (const std::string &prefix : normalized_dirty_paths) {
        if (path_matches_prefix(path, prefix)) {
            return true;
        }
    }

    return false;
}

int64_t read_scalar_int64(Statement &statement) {
    if (statement.step() != Statement::StepResult::Row) {
        return 0;
    }

    return statement.column_int64(0);
}

ParseStatus parse_status_from_string(const std::string &value) {
    if (value == "parsed_class") {
        return ParseStatus::ParsedClass;
    }
    if (value == "no_class") {
        return ParseStatus::NoClass;
    }
    if (value == "io_error") {
        return ParseStatus::IoError;
    }
    if (value == "malformed") {
        return ParseStatus::Malformed;
    }
    if (value == "unsupported_language") {
        return ParseStatus::UnsupportedLanguage;
    }
    return ParseStatus::NotParsed;
}

EntryKind entry_kind_from_string(const std::string &value, int64_t is_directory) {
    if (value == "directory" || is_directory != 0) {
        return EntryKind::Directory;
    }
    return EntryKind::File;
}

std::string root_prefix_for_concat(const std::filesystem::path &root) {
    std::string prefix = root.lexically_normal().generic_string();
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    return prefix;
}

void bind_file_query(Statement &statement, int &index, const FileQuery &query) {
    if (query.parent_id.has_value()) {
        statement.bind_int64(index++, query.parent_id.value());
    }
    if (query.is_directory.has_value()) {
        statement.bind_int64(index++, query.is_directory.value() ? 1 : 0);
    }
    if (!query.extension.empty()) {
        statement.bind_text(index++, query.extension);
    }
    if (!query.file_type.empty()) {
        statement.bind_text(index++, query.file_type);
    }
    if (!query.godot_type.empty()) {
        statement.bind_text(index++, query.godot_type);
    }
    if (!query.search.empty()) {
        statement.bind_text(index++, "%" + query.search + "%");
    }
}

std::string file_query_where(const FileQuery &query) {
    std::string where = " WHERE project_id = ?1";
    if (!query.include_deleted) {
        where += " AND is_deleted = 0";
    }
    if (query.parent_id.has_value()) {
        where += " AND parent_id = ?";
    }
    if (query.is_directory.has_value()) {
        where += " AND is_directory = ?";
    }
    if (!query.extension.empty()) {
        where += " AND extension = ?";
    }
    if (!query.file_type.empty()) {
        where += " AND file_type = ?";
    }
    if (!query.godot_type.empty()) {
        where += " AND godot_type = ?";
    }
    if (!query.search.empty()) {
        where += " AND (project_relative_path LIKE ? OR file_name LIKE ?)";
    }
    return where;
}

std::string file_sort_sql(const std::string &sort) {
    if (sort == "name") {
        return "file_name COLLATE NOCASE ASC, id ASC";
    }
    if (sort == "modified_time") {
        return "modified_time_ns DESC, id ASC";
    }
    if (sort == "file_type") {
        return "file_type ASC, project_relative_path ASC";
    }
    return "project_relative_path COLLATE NOCASE ASC, id ASC";
}

std::string class_query_where(const CustomClassQuery &query) {
    std::string where = " WHERE project_id = ?1";
    if (!query.language.empty()) {
        where += " AND language = ?";
    }
    if (!query.base_type.empty()) {
        where += " AND direct_base_type = ?";
    }
    if (!query.search.empty()) {
        where += " AND (class_name LIKE ? OR script_project_relative_path LIKE ?)";
    }
    return where;
}

void bind_class_query(Statement &statement, int &index, const CustomClassQuery &query) {
    if (!query.language.empty()) {
        statement.bind_text(index++, query.language);
    }
    if (!query.base_type.empty()) {
        statement.bind_text(index++, query.base_type);
    }
    if (!query.search.empty()) {
        statement.bind_text(index++, "%" + query.search + "%");
        statement.bind_text(index++, "%" + query.search + "%");
    }
}

std::string class_sort_sql(const std::string &sort) {
    if (sort == "script_path") {
        return "script_project_relative_path COLLATE NOCASE ASC, class_name ASC";
    }
    if (sort == "base_type") {
        return "direct_base_type COLLATE NOCASE ASC, class_name ASC";
    }
    return "class_name COLLATE NOCASE ASC, id ASC";
}

FileRow read_file_row(Statement &statement) {
    FileRow row;
    row.id = statement.column_int64(0);
    row.parent_id = statement.column_int64(1);
    row.project_relative_path = statement.column_text(2);
    row.file_name = statement.column_text(3);
    row.extension = statement.column_text(4);
    row.file_type = statement.column_text(5);
    row.godot_type = statement.column_text(6);
    row.type_hint_source = statement.column_text(7);
    row.size_bytes = statement.column_int64(8);
    row.modified_time_ns = statement.column_int64(9);
    row.is_directory = statement.column_int64(10) != 0;
    row.is_hidden = statement.column_int64(11) != 0;
    row.is_deleted = statement.column_int64(12) != 0;
    row.scan_generation = statement.column_int64(13);
    row.last_seen_generation = statement.column_int64(14);
    row.dirty_state = statement.column_text(15);
    row.dirty_reason = statement.column_text(16);
    return row;
}

CustomClassRow read_custom_class_row(Statement &statement) {
    CustomClassRow row;
    row.id = statement.column_int64(0);
    row.class_name = statement.column_text(1);
    row.script_path = statement.column_text(2);
    row.script_project_relative_path = statement.column_text(3);
    row.language = statement.column_text(4);
    row.direct_base_type = statement.column_text(5);
    row.is_resource_type = statement.column_int64(6) != 0;
    row.is_node_type = statement.column_int64(7) != 0;
    row.script_file_id = statement.column_int64(8);
    row.parser_version = statement.column_int64(9);
    row.parse_status = statement.column_text(10);
    row.parse_error = statement.column_text(11);
    row.last_parsed_generation = statement.column_int64(12);
    return row;
}

ScriptDependencyRow read_script_dependency_row(Statement &statement) {
    ScriptDependencyRow row;
    row.id = statement.column_int64(0);
    row.project_id = statement.column_int64(1);
    row.source_script_file_id = statement.column_int64(2);
    if (!statement.column_is_null(3)) {
        row.source_symbol_id = statement.column_int64(3);
    }
    if (!statement.column_is_null(4)) {
        row.target_file_id = statement.column_int64(4);
    }
    row.target_project_relative_path = statement.column_text(5);
    row.target_class_name = statement.column_text(6);
    row.target_resource_uid = statement.column_text(7);
    row.dependency_kind = statement.column_text(8);
    row.reference_text = statement.column_text(9);
    row.source_line = statement.column_int64(10);
    row.source_column = statement.column_int64(11);
    row.confidence = static_cast<double>(statement.column_int64(12)) / 1000.0;
    row.is_dynamic = statement.column_int64(13) != 0;
    row.is_resolved = statement.column_int64(14) != 0;
    row.parser_version = statement.column_int64(15);
    row.scan_generation = statement.column_int64(16);
    row.created_at_unix = statement.column_int64(17);
    return row;
}

DependencyCycleRow read_dependency_cycle_row(Statement &statement) {
    DependencyCycleRow row;
    row.source_script_file_id = statement.column_int64(0);
    row.cycle_to_script_file_id = statement.column_int64(1);
    row.cycle_path = statement.column_text(2);
    row.hop_count = statement.column_int64(3);
    return row;
}

std::vector<ScriptDependencyRow> collect_script_dependency_rows(Statement &statement) {
    std::vector<ScriptDependencyRow> rows;
    while (statement.step() == Statement::StepResult::Row) {
        rows.push_back(read_script_dependency_row(statement));
    }
    return rows;
}

class ScanCancelledError : public std::runtime_error {
public:
    ScanCancelledError() :
        std::runtime_error("scan_cancelled") {}
};

bool is_cancel_requested(const std::atomic_bool *cancel_requested) {
    return cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed);
}

void throw_if_cancel_requested(const std::atomic_bool *cancel_requested, ScanMetrics &metrics) {
    if (!is_cancel_requested(cancel_requested)) {
        return;
    }

    metrics.cancellation_requested = true;
    metrics.scan_result_status = "cancelled";
    throw ScanCancelledError();
}

ScanResultSummary build_scan_summary(
    int64_t scan_run_id,
    ScanGeneration generation,
    const ScanMetrics &metrics
) {
    ScanResultSummary summary;
    summary.scan_run_id = scan_run_id;
    summary.scan_generation = generation;
    summary.status = metrics.scan_result_status;
    summary.files_seen = metrics.files_seen;
    summary.dirs_seen = metrics.dirs_seen;
    summary.entries_clean = metrics.entries_clean;
    summary.entries_dirty = metrics.entries_dirty;
    summary.entries_new = metrics.entries_new;
    summary.entries_deleted = metrics.entries_deleted;
    summary.scripts_candidates = metrics.scripts_candidates;
    summary.scripts_parsed = metrics.scripts_parsed;
    summary.scripts_skipped_clean = metrics.scripts_skipped_clean;
    summary.total_wall_ms = metrics.total_wall_ms;
    return summary;
}

} // namespace

ScanRepository::ScanRepository(gotool::database::Database &database) :
    database_(&database) {}

ScanGeneration ScanRepository::next_generation(int64_t project_id) {
    Statement statement = database_->prepare(
        "SELECT COALESCE(MAX(scan_generation), 0) + 1 FROM project_scan_runs WHERE project_id = ?1;"
    );
    statement.bind_int64(1, project_id);
    if (statement.step() != Statement::StepResult::Row) {
        return 1;
    }
    return statement.column_int64(0);
}

int64_t ScanRepository::create_scan_run(int64_t project_id, ScanGeneration generation, int64_t started_at_unix) {
    Statement statement = database_->prepare(
        "INSERT INTO project_scan_runs (project_id, started_at_unix, status, files_found, folders_found, scan_generation) "
        "VALUES (?1, ?2, 'running', 0, 0, ?3);"
    );
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, started_at_unix);
    statement.bind_int64(3, generation);
    statement.step_done();
    return database_->last_insert_row_id();
}

void ScanRepository::complete_scan_run(
    int64_t project_id,
    int64_t scan_run_id,
    ScanGeneration generation,
    const ScanMetrics &metrics,
    int64_t finished_at_unix,
    const std::string &error_message
) {
    Statement update_run = database_->prepare(
        "UPDATE project_scan_runs "
        "SET finished_at_unix = ?1, status = ?2, files_found = ?3, folders_found = ?4, error_message = ?5 "
        "WHERE project_id = ?6 AND id = ?7;"
    );
    update_run.bind_int64(1, finished_at_unix);
    update_run.bind_text(2, metrics.scan_result_status);
    update_run.bind_int64(3, metrics.files_seen);
    update_run.bind_int64(4, metrics.dirs_seen);
    if (error_message.empty()) {
        update_run.bind_null(5);
    } else {
        update_run.bind_text(5, error_message);
    }
    update_run.bind_int64(6, project_id);
    update_run.bind_int64(7, scan_run_id);
    update_run.step_done();

    Statement insert_metrics = database_->prepare(R"sql(
        INSERT INTO scan_metrics (
            project_id, scan_run_id, scan_generation,
            total_wall_ms, traversal_ms, metadata_ms,
            existing_snapshot_load_ms, reserve_setup_ms,
            dirty_check_ms, script_candidate_ms, classification_ms, script_parse_ms,
            dependency_parse_ms, tokenizer_ms,
            sqlite_write_ms, sqlite_stage_insert_ms, sqlite_file_merge_ms, sqlite_clean_refresh_ms,
            sqlite_parent_resolve_ms, sqlite_parse_status_ms, sqlite_custom_class_ms,
            dependency_sqlite_stage_ms, dependency_resolution_ms,
            sqlite_tombstone_ms, sqlite_deleted_reconcile_ms, sqlite_metrics_write_ms,
            godot_materialization_ms,
            files_seen, dirs_seen, dirs_skipped,
            entries_clean, entries_dirty, entries_new, entries_deleted,
            rows_inserted, rows_updated, rows_clean_refreshed, rows_tombstoned,
            scripts_candidates, scripts_parsed, scripts_skipped_clean,
            scripts_dependency_parsed, scripts_dependency_skipped_clean,
            script_lines_scanned, parser_lines_scanned,
            bytes_read, parser_bytes_read, parser_tokens_generated, parser_limit_exceeded_count,
            dependency_records_created, unresolved_dependency_count, dynamic_dependency_count,
            entry_record_count, path_arena_bytes, existing_snapshot_count, parsed_script_count,
            sqlite_statement_steps, sqlite_transactions, ui_rows_materialized,
            cancellation_requested, scan_result_status, created_at_unix
        ) VALUES (
            ?1, ?2, ?3,
            ?4, ?5, ?6,
            ?7, ?8,
            ?9, ?10, ?11, ?12,
            ?13, ?14,
            ?15, ?16, ?17, ?18,
            ?19, ?20, ?21,
            ?22, ?23,
            ?24, ?25, ?26,
            ?27,
            ?28, ?29, ?30,
            ?31, ?32, ?33, ?34,
            ?35, ?36, ?37, ?38,
            ?39, ?40, ?41,
            ?42, ?43,
            ?44, ?45,
            ?46, ?47, ?48, ?49,
            ?50, ?51, ?52,
            ?53, ?54, ?55, ?56,
            ?57, ?58, ?59,
            ?60, ?61, ?62
        );
    )sql");
    insert_metrics.bind_int64(1, project_id);
    insert_metrics.bind_int64(2, scan_run_id);
    insert_metrics.bind_int64(3, generation);
    insert_metrics.bind_int64(4, metrics.total_wall_ms);
    insert_metrics.bind_int64(5, metrics.traversal_ms);
    insert_metrics.bind_int64(6, metrics.metadata_ms);
    insert_metrics.bind_int64(7, metrics.existing_snapshot_load_ms);
    insert_metrics.bind_int64(8, metrics.reserve_setup_ms);
    insert_metrics.bind_int64(9, metrics.dirty_check_ms);
    insert_metrics.bind_int64(10, metrics.script_candidate_ms);
    insert_metrics.bind_int64(11, metrics.classification_ms);
    insert_metrics.bind_int64(12, metrics.script_parse_ms);
    insert_metrics.bind_int64(13, metrics.dependency_parse_ms);
    insert_metrics.bind_int64(14, metrics.tokenizer_ms);
    insert_metrics.bind_int64(15, metrics.sqlite_write_ms);
    insert_metrics.bind_int64(16, metrics.sqlite_stage_insert_ms);
    insert_metrics.bind_int64(17, metrics.sqlite_file_merge_ms);
    insert_metrics.bind_int64(18, metrics.sqlite_clean_refresh_ms);
    insert_metrics.bind_int64(19, metrics.sqlite_parent_resolve_ms);
    insert_metrics.bind_int64(20, metrics.sqlite_parse_status_ms);
    insert_metrics.bind_int64(21, metrics.sqlite_custom_class_ms);
    insert_metrics.bind_int64(22, metrics.dependency_sqlite_stage_ms);
    insert_metrics.bind_int64(23, metrics.dependency_resolution_ms);
    insert_metrics.bind_int64(24, metrics.sqlite_tombstone_ms);
    insert_metrics.bind_int64(25, metrics.sqlite_deleted_reconcile_ms);
    insert_metrics.bind_int64(26, metrics.sqlite_metrics_write_ms);
    insert_metrics.bind_int64(27, metrics.godot_materialization_ms);
    insert_metrics.bind_int64(28, metrics.files_seen);
    insert_metrics.bind_int64(29, metrics.dirs_seen);
    insert_metrics.bind_int64(30, metrics.dirs_skipped);
    insert_metrics.bind_int64(31, metrics.entries_clean);
    insert_metrics.bind_int64(32, metrics.entries_dirty);
    insert_metrics.bind_int64(33, metrics.entries_new);
    insert_metrics.bind_int64(34, metrics.entries_deleted);
    insert_metrics.bind_int64(35, metrics.rows_inserted);
    insert_metrics.bind_int64(36, metrics.rows_updated);
    insert_metrics.bind_int64(37, metrics.rows_clean_refreshed);
    insert_metrics.bind_int64(38, metrics.rows_tombstoned);
    insert_metrics.bind_int64(39, metrics.scripts_candidates);
    insert_metrics.bind_int64(40, metrics.scripts_parsed);
    insert_metrics.bind_int64(41, metrics.scripts_skipped_clean);
    insert_metrics.bind_int64(42, metrics.scripts_dependency_parsed);
    insert_metrics.bind_int64(43, metrics.scripts_dependency_skipped_clean);
    insert_metrics.bind_int64(44, metrics.script_lines_scanned);
    insert_metrics.bind_int64(45, metrics.parser_lines_scanned);
    insert_metrics.bind_int64(46, metrics.bytes_read);
    insert_metrics.bind_int64(47, metrics.parser_bytes_read);
    insert_metrics.bind_int64(48, metrics.parser_tokens_generated);
    insert_metrics.bind_int64(49, metrics.parser_limit_exceeded_count);
    insert_metrics.bind_int64(50, metrics.dependency_records_created);
    insert_metrics.bind_int64(51, metrics.unresolved_dependency_count);
    insert_metrics.bind_int64(52, metrics.dynamic_dependency_count);
    insert_metrics.bind_int64(53, metrics.entry_record_count);
    insert_metrics.bind_int64(54, metrics.path_arena_bytes);
    insert_metrics.bind_int64(55, metrics.existing_snapshot_count);
    insert_metrics.bind_int64(56, metrics.parsed_script_count);
    insert_metrics.bind_int64(57, metrics.sqlite_statement_steps);
    insert_metrics.bind_int64(58, metrics.sqlite_transactions);
    insert_metrics.bind_int64(59, metrics.ui_rows_materialized);
    insert_metrics.bind_int64(60, metrics.cancellation_requested ? 1 : 0);
    insert_metrics.bind_text(61, metrics.scan_result_status);
    insert_metrics.bind_int64(62, finished_at_unix);
    insert_metrics.step_done();
}

ExistingEntryMap ScanRepository::load_existing_entries(int64_t project_id) {
    ExistingEntryMap existing;

    Statement count_statement = database_->prepare(
        "SELECT COUNT(*) FROM project_files WHERE project_id = ?1 AND is_deleted = 0;"
    );
    count_statement.bind_int64(1, project_id);
    const int64_t existing_count = read_scalar_int64(count_statement);
    if (existing_count > 0) {
        existing.reserve(static_cast<size_t>(existing_count + (existing_count / 8) + 16));
    }

    Statement statement = database_->prepare(R"sql(
        SELECT id, project_relative_path, entry_kind, is_directory, size_bytes, modified_time_ns,
               platform_file_id, parser_version, dependency_parser_version, classifier_version, parse_status
        FROM project_files
        WHERE project_id = ?1 AND is_deleted = 0;
    )sql");
    statement.bind_int64(1, project_id);

    while (statement.step() == Statement::StepResult::Row) {
        ExistingEntrySnapshot snapshot;
        snapshot.id = statement.column_int64(0);
        std::string path = statement.column_text(1);
        snapshot.entry_kind = entry_kind_from_string(statement.column_text(2), statement.column_int64(3));
        snapshot.size_bytes = statement.column_int64(4);
        snapshot.modified_time_ns = statement.column_int64(5);
        snapshot.platform_file_id = statement.column_text(6);
        snapshot.parser_version = statement.column_int64(7);
        snapshot.dependency_parser_version = statement.column_int64(8);
        snapshot.classifier_version = statement.column_int64(9);
        snapshot.parse_status = parse_status_from_string(statement.column_text(10));
        existing.emplace(std::move(path), std::move(snapshot));
    }

    return existing;
}

void ScanRepository::write_scan_results(
    int64_t project_id,
    int64_t scan_run_id,
    ScanGeneration generation,
    const std::filesystem::path &project_root,
    PathArena &arena,
    std::vector<EntryRecord> &records,
    const std::vector<ParsedScriptRecord> &parsed_scripts,
    ScanMetrics &metrics,
    const std::atomic_bool *cancel_requested
) {
    const auto write_start = std::chrono::steady_clock::now();
    const int64_t observed_at_unix = current_unix_time();
    const std::string root_prefix = root_prefix_for_concat(project_root);
    const std::string clean_state = to_string(DirtyState::Clean);
    const std::string clean_reason = to_string(DirtyReason::None);
    const std::string parsed_class_status = to_string(ParseStatus::ParsedClass);

    metrics.entry_record_count = static_cast<int64_t>(records.size());
    metrics.path_arena_bytes = static_cast<int64_t>(arena.size());
    metrics.parsed_script_count = static_cast<int64_t>(parsed_scripts.size());

    Transaction transaction(*database_);
    ++metrics.sqlite_transactions;
    throw_if_cancel_requested(cancel_requested, metrics);

    const auto stage_start = std::chrono::steady_clock::now();

    database_->exec("DROP TABLE IF EXISTS temp.scan_stage;");
    database_->exec("DROP TABLE IF EXISTS temp.parsed_script_stage;");
    database_->exec("DROP TABLE IF EXISTS temp.reparsed_script_stage;");
    database_->exec("DROP TABLE IF EXISTS temp.script_dependency_stage;");
    database_->exec(R"sql(
        CREATE TEMP TABLE temp.scan_stage (
            project_relative_path TEXT PRIMARY KEY,
            parent_project_relative_path TEXT,
            file_name TEXT NOT NULL,
            extension TEXT NOT NULL,
            file_type TEXT NOT NULL,
            godot_type TEXT NOT NULL,
            entry_kind TEXT NOT NULL,
            godot_type_hint TEXT NOT NULL,
            type_hint_source TEXT NOT NULL,
            type_hint_confidence INTEGER NOT NULL,
            size_bytes INTEGER NOT NULL,
            modified_time_unix INTEGER NOT NULL,
            modified_time_ns INTEGER NOT NULL,
            platform_file_id TEXT NOT NULL,
            is_directory INTEGER NOT NULL,
            is_hidden INTEGER NOT NULL,
            dirty_state TEXT NOT NULL,
            dirty_reason TEXT NOT NULL,
            parser_version INTEGER NOT NULL,
            dependency_parser_version INTEGER NOT NULL,
            classifier_version INTEGER NOT NULL
        ) WITHOUT ROWID;
    )sql");

    Statement insert_stage = database_->prepare(R"sql(
        INSERT INTO temp.scan_stage (
            project_relative_path,
            parent_project_relative_path,
            file_name,
            extension,
            file_type,
            godot_type,
            entry_kind,
            godot_type_hint,
            type_hint_source,
            type_hint_confidence,
            size_bytes,
            modified_time_unix,
            modified_time_ns,
            platform_file_id,
            is_directory,
            is_hidden,
            dirty_state,
            dirty_reason,
            parser_version,
            dependency_parser_version,
            classifier_version
        ) VALUES (
            ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10,
            ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21
        );
    )sql");

    for (size_t i = 0; i < records.size(); ++i) {
        if ((i % 512) == 0) {
            throw_if_cancel_requested(cancel_requested, metrics);
        }

        EntryRecord &record = records[i];
        const std::string_view project_path = arena.view(record.path_offset, record.path_length);
        const std::string_view file_name = arena.view(record.name_offset, record.name_length);
        const std::string_view extension = arena.view(record.extension_offset, record.extension_length);

        std::string_view parent_project_path;
        const size_t parent_separator = project_path.rfind('/');
        if (parent_separator != std::string_view::npos) {
            parent_project_path = project_path.substr(0, parent_separator);
        }

        insert_stage.reset();
        insert_stage.clear_bindings();
        insert_stage.bind_text(1, project_path);
        if (parent_project_path.empty()) {
            insert_stage.bind_null(2);
        } else {
            insert_stage.bind_text(2, parent_project_path);
        }
        insert_stage.bind_text(3, file_name);
        insert_stage.bind_text(4, extension);
        insert_stage.bind_text(5, to_string(record.file_type_id));
        insert_stage.bind_text(6, to_string(record.godot_type_hint));
        insert_stage.bind_text(7, to_string(record.entry_kind));
        insert_stage.bind_text(8, to_string(record.godot_type_hint));
        insert_stage.bind_text(9, to_string(record.type_hint_source));
        insert_stage.bind_int64(10, record.godot_type_hint == GodotTypeHint::NotGodotTyped ? 0 : 50);
        insert_stage.bind_int64(11, record.size_bytes);
        insert_stage.bind_int64(12, record.modified_time_ns / 1'000'000'000LL);
        insert_stage.bind_int64(13, record.modified_time_ns);
        insert_stage.bind_text(14, platform_file_id_to_string(record));
        insert_stage.bind_int64(15, record.entry_kind == EntryKind::Directory ? 1 : 0);
        insert_stage.bind_int64(16, record.is_hidden() ? 1 : 0);
        insert_stage.bind_text(17, to_string(record.dirty_state));
        insert_stage.bind_text(18, to_string(record.dirty_reason));
        insert_stage.bind_int64(19, PARSER_VERSION);
        insert_stage.bind_int64(20, DEPENDENCY_PARSER_VERSION);
        insert_stage.bind_int64(21, CLASSIFIER_VERSION);
        insert_stage.step_done();
    }
    metrics.sqlite_statement_steps += static_cast<int64_t>(records.size());
    metrics.sqlite_stage_insert_ms = elapsed_ms(stage_start, std::chrono::steady_clock::now());

    throw_if_cancel_requested(cancel_requested, metrics);

    Statement count_inserted = database_->prepare(R"sql(
        SELECT COUNT(*)
        FROM temp.scan_stage s
        LEFT JOIN project_files f
          ON f.project_id = ?1
         AND f.project_relative_path = s.project_relative_path
        WHERE f.id IS NULL;
    )sql");
    count_inserted.bind_int64(1, project_id);
    metrics.rows_inserted = read_scalar_int64(count_inserted);

    Statement count_updated = database_->prepare(R"sql(
        SELECT COUNT(*)
        FROM temp.scan_stage s
        JOIN project_files f
          ON f.project_id = ?1
         AND f.project_relative_path = s.project_relative_path
        WHERE s.dirty_state <> ?2;
    )sql");
    count_updated.bind_int64(1, project_id);
    count_updated.bind_text(2, clean_state);
    metrics.rows_updated = read_scalar_int64(count_updated);

    Statement count_clean_refresh = database_->prepare(R"sql(
        SELECT COUNT(*)
        FROM temp.scan_stage s
        JOIN project_files f
          ON f.project_id = ?1
         AND f.project_relative_path = s.project_relative_path
        WHERE s.dirty_state = ?2;
    )sql");
    count_clean_refresh.bind_int64(1, project_id);
    count_clean_refresh.bind_text(2, clean_state);
    metrics.rows_clean_refreshed = read_scalar_int64(count_clean_refresh);
    metrics.sqlite_statement_steps += 3;

    const auto merge_start = std::chrono::steady_clock::now();
    Statement merge_files = database_->prepare(R"sql(
        INSERT INTO project_files (
            project_id, project_relative_path, absolute_path, file_name, extension, file_type, godot_type,
            size_bytes, modified_time_unix, is_directory, is_hidden, first_seen_scan_run_id,
            last_seen_scan_run_id, created_at_unix, updated_at_unix, parent_id, entry_kind,
            godot_type_hint, type_hint_source, type_hint_confidence, modified_time_ns, platform_file_id,
            scan_generation, last_seen_generation, dirty_state, dirty_reason, parser_version,
            dependency_parser_version, classifier_version, parse_status, is_deleted, deleted_at_unix
        )
        SELECT
            ?1,
            s.project_relative_path,
            CASE
                WHEN ?2 = '' THEN s.project_relative_path
                ELSE ?2 || '/' || s.project_relative_path
            END,
            s.file_name,
            s.extension,
            s.file_type,
            s.godot_type,
            s.size_bytes,
            s.modified_time_unix,
            s.is_directory,
            s.is_hidden,
            ?3,
            ?3,
            ?4,
            ?4,
            0,
            s.entry_kind,
            s.godot_type_hint,
            s.type_hint_source,
            s.type_hint_confidence,
            s.modified_time_ns,
            s.platform_file_id,
            ?5,
            ?5,
            s.dirty_state,
            s.dirty_reason,
            s.parser_version,
            s.dependency_parser_version,
            s.classifier_version,
            'not_parsed',
            0,
            NULL
        FROM temp.scan_stage s
        WHERE 1
        ON CONFLICT(project_id, project_relative_path) DO UPDATE SET
            absolute_path = excluded.absolute_path,
            file_name = excluded.file_name,
            extension = excluded.extension,
            file_type = excluded.file_type,
            godot_type = excluded.godot_type,
            size_bytes = excluded.size_bytes,
            modified_time_unix = excluded.modified_time_unix,
            is_directory = excluded.is_directory,
            is_hidden = excluded.is_hidden,
            last_seen_scan_run_id = excluded.last_seen_scan_run_id,
            updated_at_unix = excluded.updated_at_unix,
            parent_id = 0,
            entry_kind = excluded.entry_kind,
            godot_type_hint = excluded.godot_type_hint,
            type_hint_source = excluded.type_hint_source,
            type_hint_confidence = excluded.type_hint_confidence,
            modified_time_ns = excluded.modified_time_ns,
            platform_file_id = excluded.platform_file_id,
            scan_generation = excluded.scan_generation,
            last_seen_generation = excluded.last_seen_generation,
            dirty_state = excluded.dirty_state,
            dirty_reason = excluded.dirty_reason,
            parser_version = excluded.parser_version,
            dependency_parser_version = excluded.dependency_parser_version,
            classifier_version = excluded.classifier_version,
            parse_status = excluded.parse_status,
            is_deleted = 0,
            deleted_at_unix = NULL,
            first_seen_scan_run_id = COALESCE(project_files.first_seen_scan_run_id, excluded.first_seen_scan_run_id)
        WHERE excluded.dirty_state <> ?6;
    )sql");
    merge_files.bind_int64(1, project_id);
    merge_files.bind_text(2, root_prefix);
    merge_files.bind_int64(3, scan_run_id);
    merge_files.bind_int64(4, observed_at_unix);
    merge_files.bind_int64(5, generation);
    merge_files.bind_text(6, clean_state);
    merge_files.step_done();
    ++metrics.sqlite_statement_steps;
    metrics.sqlite_file_merge_ms = elapsed_ms(merge_start, std::chrono::steady_clock::now());

    throw_if_cancel_requested(cancel_requested, metrics);

    const auto clean_refresh_start = std::chrono::steady_clock::now();
    Statement refresh_clean_files = database_->prepare(R"sql(
        UPDATE project_files
        SET last_seen_scan_run_id = ?1,
            scan_generation = ?2,
            last_seen_generation = ?2,
            dirty_state = ?3,
            dirty_reason = ?4,
            is_deleted = 0,
            deleted_at_unix = NULL
        WHERE project_id = ?5
          AND project_relative_path IN (
              SELECT project_relative_path
              FROM temp.scan_stage
              WHERE dirty_state = ?3
          );
    )sql");
    refresh_clean_files.bind_int64(1, scan_run_id);
    refresh_clean_files.bind_int64(2, generation);
    refresh_clean_files.bind_text(3, clean_state);
    refresh_clean_files.bind_text(4, clean_reason);
    refresh_clean_files.bind_int64(5, project_id);
    refresh_clean_files.step_done();
    ++metrics.sqlite_statement_steps;
    metrics.sqlite_clean_refresh_ms = elapsed_ms(clean_refresh_start, std::chrono::steady_clock::now());

    throw_if_cancel_requested(cancel_requested, metrics);

    const auto parent_resolve_start = std::chrono::steady_clock::now();
    Statement resolve_parents = database_->prepare(R"sql(
        UPDATE project_files
        SET parent_id = COALESCE(
            (
                SELECT parent.id
                FROM temp.scan_stage s
                JOIN project_files parent
                  ON parent.project_id = project_files.project_id
                 AND parent.project_relative_path = s.parent_project_relative_path
                 AND parent.is_deleted = 0
                WHERE s.project_relative_path = project_files.project_relative_path
                LIMIT 1
            ),
            0
        )
        WHERE project_id = ?1
          AND project_relative_path IN (
              SELECT project_relative_path
              FROM temp.scan_stage
              WHERE dirty_state <> ?2
          );
    )sql");
    resolve_parents.bind_int64(1, project_id);
    resolve_parents.bind_text(2, clean_state);
    resolve_parents.step_done();
    ++metrics.sqlite_statement_steps;
    metrics.sqlite_parent_resolve_ms = elapsed_ms(parent_resolve_start, std::chrono::steady_clock::now());

    throw_if_cancel_requested(cancel_requested, metrics);

    database_->exec(R"sql(
        CREATE TEMP TABLE temp.parsed_script_stage (
            project_relative_path TEXT PRIMARY KEY,
            parse_status TEXT NOT NULL,
            language TEXT NOT NULL,
            class_name TEXT NOT NULL,
            direct_base_type TEXT NOT NULL,
            parse_error TEXT NOT NULL,
            is_resource_type INTEGER NOT NULL,
            is_node_type INTEGER NOT NULL
        ) WITHOUT ROWID;
    )sql");

    Statement insert_parsed_stage = database_->prepare(R"sql(
        INSERT INTO temp.parsed_script_stage (
            project_relative_path,
            parse_status,
            language,
            class_name,
            direct_base_type,
            parse_error,
            is_resource_type,
            is_node_type
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);
    )sql");

    for (const ParsedScriptRecord &parsed : parsed_scripts) {
        throw_if_cancel_requested(cancel_requested, metrics);

        const std::string &direct_base = parsed.parse_result.direct_base_type;
        insert_parsed_stage.reset();
        insert_parsed_stage.clear_bindings();
        insert_parsed_stage.bind_text(1, parsed.project_relative_path);
        insert_parsed_stage.bind_text(2, to_string(parsed.parse_result.status));
        insert_parsed_stage.bind_text(3, to_string(parsed.parse_result.language));
        insert_parsed_stage.bind_text(4, parsed.parse_result.class_name);
        insert_parsed_stage.bind_text(5, direct_base);
        insert_parsed_stage.bind_text(6, parsed.parse_result.parse_error);
        insert_parsed_stage.bind_int64(7, is_builtin_resource_type_hint(direct_base) ? 1 : 0);
        insert_parsed_stage.bind_int64(8, is_builtin_node_type_hint(direct_base) ? 1 : 0);
        insert_parsed_stage.step_done();
    }
    metrics.sqlite_statement_steps += static_cast<int64_t>(parsed_scripts.size());

    database_->exec(R"sql(
        CREATE TEMP TABLE temp.reparsed_script_stage (
            file_id INTEGER PRIMARY KEY,
            project_relative_path TEXT NOT NULL UNIQUE
        ) WITHOUT ROWID;
    )sql");

    Statement populate_reparsed_stage = database_->prepare(R"sql(
        INSERT INTO temp.reparsed_script_stage (file_id, project_relative_path)
        SELECT f.id, p.project_relative_path
        FROM temp.parsed_script_stage p
        JOIN project_files f
          ON f.project_id = ?1
         AND f.project_relative_path = p.project_relative_path
        WHERE f.is_deleted = 0;
    )sql");
    populate_reparsed_stage.bind_int64(1, project_id);
    populate_reparsed_stage.step_done();
    ++metrics.sqlite_statement_steps;

    const auto parse_status_start = std::chrono::steady_clock::now();
    Statement update_parse_status = database_->prepare(R"sql(
        UPDATE project_files
        SET parse_status = (
                SELECT p.parse_status
                FROM temp.parsed_script_stage p
                WHERE p.project_relative_path = project_files.project_relative_path
            ),
            parser_version = ?1,
            updated_at_unix = ?2
        WHERE project_id = ?3
          AND project_relative_path IN (
              SELECT project_relative_path
              FROM temp.parsed_script_stage
          );
    )sql");
    update_parse_status.bind_int64(1, PARSER_VERSION);
    update_parse_status.bind_int64(2, observed_at_unix);
    update_parse_status.bind_int64(3, project_id);
    update_parse_status.step_done();
    ++metrics.sqlite_statement_steps;
    metrics.sqlite_parse_status_ms = elapsed_ms(parse_status_start, std::chrono::steady_clock::now());

    throw_if_cancel_requested(cancel_requested, metrics);

    const auto class_start = std::chrono::steady_clock::now();
    Statement delete_old_class = database_->prepare(R"sql(
        DELETE FROM project_custom_classes
        WHERE project_id = ?1
          AND script_project_relative_path IN (
              SELECT project_relative_path
              FROM temp.parsed_script_stage
              WHERE parse_status <> ?2 OR class_name = ''
          );
    )sql");
    delete_old_class.bind_int64(1, project_id);
    delete_old_class.bind_text(2, parsed_class_status);
    delete_old_class.step_done();
    ++metrics.sqlite_statement_steps;

    Statement upsert_class = database_->prepare(R"sql(
        INSERT INTO project_custom_classes (
            project_id,
            class_name,
            script_path,
            script_project_relative_path,
            language,
            base_type,
            direct_base_type,
            is_resource_type,
            is_node_type,
            parser_version,
            parse_status,
            parse_error,
            last_parsed_generation,
            script_file_id,
            last_seen_scan_run_id,
            created_at_unix,
            updated_at_unix
        )
        SELECT
            ?1,
            p.class_name,
            'res://' || p.project_relative_path,
            p.project_relative_path,
            p.language,
            p.direct_base_type,
            p.direct_base_type,
            p.is_resource_type,
            p.is_node_type,
            ?2,
            p.parse_status,
            p.parse_error,
            ?3,
            (
                SELECT f.id
                FROM project_files f
                WHERE f.project_id = ?1
                  AND f.project_relative_path = p.project_relative_path
                LIMIT 1
            ),
            ?4,
            ?5,
            ?5
        FROM temp.parsed_script_stage p
        WHERE p.parse_status = ?6
          AND p.class_name <> ''
        ON CONFLICT(project_id, class_name) DO UPDATE SET
            script_path = excluded.script_path,
            script_project_relative_path = excluded.script_project_relative_path,
            language = excluded.language,
            base_type = excluded.base_type,
            direct_base_type = excluded.direct_base_type,
            is_resource_type = excluded.is_resource_type,
            is_node_type = excluded.is_node_type,
            parser_version = excluded.parser_version,
            parse_status = excluded.parse_status,
            parse_error = excluded.parse_error,
            last_parsed_generation = excluded.last_parsed_generation,
            script_file_id = excluded.script_file_id,
            last_seen_scan_run_id = excluded.last_seen_scan_run_id,
            updated_at_unix = excluded.updated_at_unix;
    )sql");
    upsert_class.bind_int64(1, project_id);
    upsert_class.bind_int64(2, PARSER_VERSION);
    upsert_class.bind_int64(3, generation);
    upsert_class.bind_int64(4, scan_run_id);
    upsert_class.bind_int64(5, observed_at_unix);
    upsert_class.bind_text(6, parsed_class_status);
    upsert_class.step_done();
    ++metrics.sqlite_statement_steps;

    Statement refresh_clean_classes = database_->prepare(R"sql(
        UPDATE project_custom_classes
        SET last_seen_scan_run_id = ?1,
            updated_at_unix = ?2
        WHERE project_id = ?3
          AND EXISTS (
              SELECT 1 FROM project_files
              WHERE project_files.project_id = project_custom_classes.project_id
                AND project_files.project_relative_path = project_custom_classes.script_project_relative_path
                AND project_files.last_seen_generation = ?4
                AND project_files.is_deleted = 0
          );
    )sql");
    refresh_clean_classes.bind_int64(1, scan_run_id);
    refresh_clean_classes.bind_int64(2, observed_at_unix);
    refresh_clean_classes.bind_int64(3, project_id);
    refresh_clean_classes.bind_int64(4, generation);
    refresh_clean_classes.step_done();
    ++metrics.sqlite_statement_steps;

    throw_if_cancel_requested(cancel_requested, metrics);

    Statement delete_stale_classes = database_->prepare(R"sql(
        DELETE FROM project_custom_classes
        WHERE project_id = ?1
          AND NOT EXISTS (
              SELECT 1 FROM project_files
              WHERE project_files.project_id = project_custom_classes.project_id
                AND project_files.project_relative_path = project_custom_classes.script_project_relative_path
                AND project_files.last_seen_generation = ?2
                AND project_files.is_deleted = 0
          );
    )sql");
    delete_stale_classes.bind_int64(1, project_id);
    delete_stale_classes.bind_int64(2, generation);
    delete_stale_classes.step_done();
    ++metrics.sqlite_statement_steps;

        Statement delete_reparsed_symbols = database_->prepare(R"sql(
                DELETE FROM script_symbols
                WHERE project_id = ?1
                    AND script_file_id IN (
                            SELECT file_id
                            FROM temp.reparsed_script_stage
                    );
        )sql");
        delete_reparsed_symbols.bind_int64(1, project_id);
        delete_reparsed_symbols.step_done();
        ++metrics.sqlite_statement_steps;

        Statement insert_symbols = database_->prepare(R"sql(
                INSERT INTO script_symbols (
                        project_id,
                        script_file_id,
                        class_name,
                        language,
                        parser_version,
                        last_parsed_generation,
                        last_seen_scan_run_id,
                        created_at_unix,
                        updated_at_unix
                )
                SELECT
                        ?1,
                        f.id,
                        p.class_name,
                        p.language,
                        ?2,
                        ?3,
                        ?4,
                        ?5,
                        ?5
                FROM temp.parsed_script_stage p
                JOIN project_files f
                    ON f.project_id = ?1
                 AND f.project_relative_path = p.project_relative_path
                WHERE p.parse_status = ?6
                    AND p.class_name <> ''
                    AND f.is_deleted = 0
                ON CONFLICT(project_id, script_file_id) DO UPDATE SET
                        class_name = excluded.class_name,
                        language = excluded.language,
                        parser_version = excluded.parser_version,
                        last_parsed_generation = excluded.last_parsed_generation,
                        last_seen_scan_run_id = excluded.last_seen_scan_run_id,
                        updated_at_unix = excluded.updated_at_unix;
        )sql");
        insert_symbols.bind_int64(1, project_id);
        insert_symbols.bind_int64(2, PARSER_VERSION);
        insert_symbols.bind_int64(3, generation);
        insert_symbols.bind_int64(4, scan_run_id);
        insert_symbols.bind_int64(5, observed_at_unix);
        insert_symbols.bind_text(6, parsed_class_status);
        insert_symbols.step_done();
        ++metrics.sqlite_statement_steps;

        Statement delete_stale_symbols = database_->prepare(R"sql(
                DELETE FROM script_symbols
                WHERE project_id = ?1
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = script_symbols.script_file_id
                                AND f.project_id = script_symbols.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        delete_stale_symbols.bind_int64(1, project_id);
        delete_stale_symbols.step_done();
        ++metrics.sqlite_statement_steps;

    metrics.sqlite_custom_class_ms = elapsed_ms(class_start, std::chrono::steady_clock::now());

    throw_if_cancel_requested(cancel_requested, metrics);

    const auto dependency_stage_start = std::chrono::steady_clock::now();
    database_->exec(R"sql(
        CREATE TEMP TABLE temp.script_dependency_stage (
            source_project_relative_path TEXT NOT NULL,
            source_script_file_id INTEGER,
            source_symbol_id INTEGER,
            target_file_id INTEGER,
            target_project_relative_path TEXT,
            target_class_name TEXT,
            target_resource_uid TEXT,
            dependency_kind TEXT NOT NULL,
            reference_text TEXT NOT NULL,
            source_line INTEGER NOT NULL,
            source_column INTEGER NOT NULL,
            confidence INTEGER NOT NULL,
            is_dynamic INTEGER NOT NULL,
            is_resolved INTEGER NOT NULL DEFAULT 0,
            parser_version INTEGER NOT NULL,
            scan_generation INTEGER NOT NULL
        );
    )sql");

    Statement insert_dependency_stage = database_->prepare(R"sql(
        INSERT INTO temp.script_dependency_stage (
            source_project_relative_path,
            source_script_file_id,
            source_symbol_id,
            target_file_id,
            target_project_relative_path,
            target_class_name,
            target_resource_uid,
            dependency_kind,
            reference_text,
            source_line,
            source_column,
            confidence,
            is_dynamic,
            is_resolved,
            parser_version,
            scan_generation
        ) VALUES (
            ?1, NULL, NULL, NULL, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 0, ?11, ?12
        );
    )sql");

    int64_t dependency_stage_rows = 0;
    for (size_t parsed_index = 0; parsed_index < parsed_scripts.size(); ++parsed_index) {
        if ((parsed_index % 32) == 0) {
            throw_if_cancel_requested(cancel_requested, metrics);
        }

        const ParsedScriptRecord &parsed = parsed_scripts[parsed_index];
        for (const ScriptDependencyRecord &dependency : parsed.parse_result.dependencies) {
            if ((dependency_stage_rows % 256) == 0) {
                throw_if_cancel_requested(cancel_requested, metrics);
            }

            const double bounded_confidence = std::max(0.0, std::min(1.0, dependency.confidence));
            const int64_t confidence_scaled = static_cast<int64_t>(bounded_confidence * 1000.0);

            insert_dependency_stage.reset();
            insert_dependency_stage.clear_bindings();
            insert_dependency_stage.bind_text(1, parsed.project_relative_path);
            if (dependency.target_project_relative_path.has_value() &&
                !dependency.target_project_relative_path->empty()) {
                insert_dependency_stage.bind_text(2, dependency.target_project_relative_path.value());
            } else {
                insert_dependency_stage.bind_null(2);
            }
            if (dependency.target_class_name.has_value() && !dependency.target_class_name->empty()) {
                insert_dependency_stage.bind_text(3, dependency.target_class_name.value());
            } else {
                insert_dependency_stage.bind_null(3);
            }
            if (dependency.target_resource_uid.has_value() && !dependency.target_resource_uid->empty()) {
                insert_dependency_stage.bind_text(4, dependency.target_resource_uid.value());
            } else {
                insert_dependency_stage.bind_null(4);
            }
            insert_dependency_stage.bind_text(5, to_string(dependency.dependency_kind));
            insert_dependency_stage.bind_text(6, dependency.reference_text);
            insert_dependency_stage.bind_int64(7, dependency.source_line);
            insert_dependency_stage.bind_int64(8, dependency.source_column);
            insert_dependency_stage.bind_int64(9, confidence_scaled);
            insert_dependency_stage.bind_int64(10, dependency.is_dynamic ? 1 : 0);
            insert_dependency_stage.bind_int64(11, DEPENDENCY_PARSER_VERSION);
            insert_dependency_stage.bind_int64(12, generation);
            insert_dependency_stage.step_done();
            ++dependency_stage_rows;
        }
    }
    metrics.sqlite_statement_steps += dependency_stage_rows;
    metrics.dependency_records_created = dependency_stage_rows;
    metrics.dependency_sqlite_stage_ms = elapsed_ms(dependency_stage_start, std::chrono::steady_clock::now());

    throw_if_cancel_requested(cancel_requested, metrics);

    const auto dependency_resolution_start = std::chrono::steady_clock::now();

    Statement resolve_source_file_ids = database_->prepare(R"sql(
        UPDATE temp.script_dependency_stage
        SET source_script_file_id = (
            SELECT r.file_id
            FROM temp.reparsed_script_stage r
            WHERE r.project_relative_path = source_project_relative_path
            LIMIT 1
        );
    )sql");
    resolve_source_file_ids.step_done();
    ++metrics.sqlite_statement_steps;

    Statement drop_unbound_sources = database_->prepare(
        "DELETE FROM temp.script_dependency_stage WHERE source_script_file_id IS NULL;"
    );
    drop_unbound_sources.step_done();
    ++metrics.sqlite_statement_steps;

    Statement delete_old_dependencies = database_->prepare(R"sql(
        DELETE FROM script_dependencies
        WHERE project_id = ?1
          AND source_script_file_id IN (
              SELECT file_id
              FROM temp.reparsed_script_stage
          );
    )sql");
    delete_old_dependencies.bind_int64(1, project_id);
    delete_old_dependencies.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_target_paths = database_->prepare(R"sql(
        UPDATE temp.script_dependency_stage
        SET target_file_id = (
            SELECT f.id
            FROM project_files f
            WHERE f.project_id = ?1
              AND f.project_relative_path = target_project_relative_path
              AND f.is_deleted = 0
            LIMIT 1
        )
        WHERE target_project_relative_path IS NOT NULL
          AND target_project_relative_path <> '';
    )sql");
    resolve_target_paths.bind_int64(1, project_id);
    resolve_target_paths.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_target_class = database_->prepare(R"sql(
        UPDATE temp.script_dependency_stage
        SET target_file_id = (
            SELECT s.script_file_id
            FROM script_symbols s
            JOIN project_files f
              ON f.id = s.script_file_id
             AND f.project_id = s.project_id
             AND f.is_deleted = 0
            WHERE s.project_id = ?1
              AND s.class_name = target_class_name
            LIMIT 1
        )
        WHERE target_file_id IS NULL
          AND target_class_name IS NOT NULL
          AND target_class_name <> '';
    )sql");
    resolve_target_class.bind_int64(1, project_id);
    resolve_target_class.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_target_custom_class = database_->prepare(R"sql(
        UPDATE temp.script_dependency_stage
        SET target_file_id = (
            SELECT c.script_file_id
            FROM project_custom_classes c
            JOIN project_files f
              ON f.id = c.script_file_id
             AND f.project_id = c.project_id
             AND f.is_deleted = 0
            WHERE c.project_id = ?1
              AND c.class_name = target_class_name
            LIMIT 1
        )
        WHERE target_file_id IS NULL
          AND target_class_name IS NOT NULL
          AND target_class_name <> '';
    )sql");
    resolve_target_custom_class.bind_int64(1, project_id);
    resolve_target_custom_class.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_source_symbol = database_->prepare(R"sql(
        UPDATE temp.script_dependency_stage
        SET source_symbol_id = (
            SELECT s.id
            FROM script_symbols s
            WHERE s.project_id = ?1
              AND s.script_file_id = source_script_file_id
            LIMIT 1
        );
    )sql");
    resolve_source_symbol.bind_int64(1, project_id);
    resolve_source_symbol.step_done();
    ++metrics.sqlite_statement_steps;

    Statement mark_resolved = database_->prepare(
        "UPDATE temp.script_dependency_stage SET is_resolved = CASE WHEN target_file_id IS NOT NULL THEN 1 ELSE 0 END;"
    );
    mark_resolved.step_done();
    ++metrics.sqlite_statement_steps;

    Statement insert_dependencies = database_->prepare(R"sql(
        INSERT INTO script_dependencies (
            project_id,
            source_script_file_id,
            source_symbol_id,
            target_file_id,
            target_project_relative_path,
            target_class_name,
            target_resource_uid,
            dependency_kind,
            reference_text,
            source_line,
            source_column,
            confidence,
            is_dynamic,
            is_resolved,
            parser_version,
            scan_generation,
            created_at_unix
        )
        SELECT
            ?1,
            source_script_file_id,
            source_symbol_id,
            target_file_id,
            target_project_relative_path,
            target_class_name,
            target_resource_uid,
            dependency_kind,
            reference_text,
            source_line,
            source_column,
            confidence,
            is_dynamic,
            is_resolved,
            parser_version,
            scan_generation,
            ?2
        FROM temp.script_dependency_stage;
    )sql");
    insert_dependencies.bind_int64(1, project_id);
    insert_dependencies.bind_int64(2, observed_at_unix);
    insert_dependencies.step_done();
    ++metrics.sqlite_statement_steps;

    Statement unresolved_dependencies = database_->prepare(
        "SELECT COUNT(*) FROM temp.script_dependency_stage WHERE is_resolved = 0;"
    );
    metrics.unresolved_dependency_count = read_scalar_int64(unresolved_dependencies);
    ++metrics.sqlite_statement_steps;

    Statement dynamic_dependencies = database_->prepare(
        "SELECT COUNT(*) FROM temp.script_dependency_stage WHERE is_dynamic <> 0;"
    );
    metrics.dynamic_dependency_count = read_scalar_int64(dynamic_dependencies);
    ++metrics.sqlite_statement_steps;

    metrics.dependency_resolution_ms = elapsed_ms(dependency_resolution_start, std::chrono::steady_clock::now());

    throw_if_cancel_requested(cancel_requested, metrics);

    const auto tombstone_start = std::chrono::steady_clock::now();
    Statement tombstone = database_->prepare(R"sql(
        UPDATE project_files
        SET is_deleted = 1,
            dirty_state = 'deleted',
            dirty_reason = 'deleted_path',
            deleted_at_unix = ?1,
            updated_at_unix = ?1,
            scan_generation = ?2,
            last_seen_scan_run_id = ?3
        WHERE project_id = ?4
          AND is_deleted = 0
          AND (last_seen_generation IS NULL OR last_seen_generation <> ?2);
    )sql");
    tombstone.bind_int64(1, observed_at_unix);
    tombstone.bind_int64(2, generation);
    tombstone.bind_int64(3, scan_run_id);
    tombstone.bind_int64(4, project_id);
    tombstone.step_done();
    ++metrics.sqlite_statement_steps;
    metrics.rows_tombstoned = database_->changes();
    metrics.entries_deleted = metrics.rows_tombstoned;

        Statement delete_deleted_source_dependencies = database_->prepare(R"sql(
                DELETE FROM script_dependencies
                WHERE project_id = ?1
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = script_dependencies.source_script_file_id
                                AND f.project_id = script_dependencies.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        delete_deleted_source_dependencies.bind_int64(1, project_id);
        delete_deleted_source_dependencies.step_done();
        ++metrics.sqlite_statement_steps;

        Statement invalidate_deleted_targets = database_->prepare(R"sql(
                UPDATE script_dependencies
                SET target_file_id = NULL,
                        is_resolved = 0
                WHERE project_id = ?1
                    AND target_file_id IS NOT NULL
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = script_dependencies.target_file_id
                                AND f.project_id = script_dependencies.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        invalidate_deleted_targets.bind_int64(1, project_id);
        invalidate_deleted_targets.step_done();
        ++metrics.sqlite_statement_steps;

        Statement delete_deleted_source_symbols = database_->prepare(R"sql(
                DELETE FROM script_symbols
                WHERE project_id = ?1
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = script_symbols.script_file_id
                                AND f.project_id = script_symbols.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        delete_deleted_source_symbols.bind_int64(1, project_id);
        delete_deleted_source_symbols.step_done();
        ++metrics.sqlite_statement_steps;

    metrics.sqlite_tombstone_ms = elapsed_ms(tombstone_start, std::chrono::steady_clock::now());

    const auto deleted_reconcile_start = std::chrono::steady_clock::now();
    Statement deleted_entries = database_->prepare(R"sql(
        INSERT OR IGNORE INTO deleted_entries (
            project_id, file_id, project_relative_path, deleted_scan_run_id, scan_generation, deleted_at_unix
        )
        SELECT project_id, id, project_relative_path, ?1, ?2, ?3
        FROM project_files
        WHERE project_id = ?4
          AND dirty_state = 'deleted'
          AND dirty_reason = 'deleted_path'
          AND deleted_at_unix = ?3;
    )sql");
    deleted_entries.bind_int64(1, scan_run_id);
    deleted_entries.bind_int64(2, generation);
    deleted_entries.bind_int64(3, observed_at_unix);
    deleted_entries.bind_int64(4, project_id);
    deleted_entries.step_done();
    ++metrics.sqlite_statement_steps;
    metrics.sqlite_deleted_reconcile_ms = elapsed_ms(deleted_reconcile_start, std::chrono::steady_clock::now());

    throw_if_cancel_requested(cancel_requested, metrics);

    const auto metrics_write_start = std::chrono::steady_clock::now();
    complete_scan_run(project_id, scan_run_id, generation, metrics, observed_at_unix);
    metrics.sqlite_metrics_write_ms = elapsed_ms(metrics_write_start, std::chrono::steady_clock::now());
    metrics.sqlite_statement_steps += 2;

    metrics.sqlite_write_ms = elapsed_ms(write_start, std::chrono::steady_clock::now());

    transaction.commit();
}

int64_t ScanRepository::count_files(int64_t project_id, const FileQuery &query) const {
    const std::string sql = "SELECT COUNT(*) FROM project_files" + file_query_where(query) + ";";
    Statement statement = database_->prepare(sql);
    statement.bind_int64(1, project_id);
    int index = 2;
    bind_file_query(statement, index, query);
    if (!query.search.empty()) {
        statement.bind_text(index++, "%" + query.search + "%");
    }
    if (statement.step() != Statement::StepResult::Row) {
        return 0;
    }
    return statement.column_int64(0);
}

std::vector<FileRow> ScanRepository::list_files(
    int64_t project_id,
    const FileQuery &query,
    int64_t offset,
    int64_t limit,
    const std::string &sort
) const {
    limit = std::max<int64_t>(0, std::min<int64_t>(limit, 500));
    offset = std::max<int64_t>(0, offset);

    const std::string sql = R"sql(
        SELECT id, parent_id, project_relative_path, file_name, extension, file_type, godot_type,
               type_hint_source, size_bytes, modified_time_ns, is_directory, is_hidden, is_deleted,
               scan_generation, last_seen_generation, dirty_state, dirty_reason
        FROM project_files
    )sql" + file_query_where(query) + " ORDER BY " + file_sort_sql(sort) + " LIMIT ? OFFSET ?;";

    Statement statement = database_->prepare(sql);
    statement.bind_int64(1, project_id);
    int index = 2;
    bind_file_query(statement, index, query);
    if (!query.search.empty()) {
        statement.bind_text(index++, "%" + query.search + "%");
    }
    statement.bind_int64(index++, limit);
    statement.bind_int64(index++, offset);

    std::vector<FileRow> rows;
    while (statement.step() == Statement::StepResult::Row) {
        rows.push_back(read_file_row(statement));
    }
    return rows;
}

std::optional<FileRow> ScanRepository::get_file_details(int64_t project_id, int64_t file_id) const {
    Statement statement = database_->prepare(R"sql(
        SELECT id, parent_id, project_relative_path, file_name, extension, file_type, godot_type,
               type_hint_source, size_bytes, modified_time_ns, is_directory, is_hidden, is_deleted,
               scan_generation, last_seen_generation, dirty_state, dirty_reason
        FROM project_files
        WHERE project_id = ?1 AND id = ?2
        LIMIT 1;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, file_id);
    if (statement.step() != Statement::StepResult::Row) {
        return std::nullopt;
    }
    return read_file_row(statement);
}

int64_t ScanRepository::count_custom_classes(int64_t project_id, const CustomClassQuery &query) const {
    const std::string sql = "SELECT COUNT(*) FROM project_custom_classes" + class_query_where(query) + ";";
    Statement statement = database_->prepare(sql);
    statement.bind_int64(1, project_id);
    int index = 2;
    bind_class_query(statement, index, query);
    if (statement.step() != Statement::StepResult::Row) {
        return 0;
    }
    return statement.column_int64(0);
}

std::vector<CustomClassRow> ScanRepository::list_custom_classes(
    int64_t project_id,
    const CustomClassQuery &query,
    int64_t offset,
    int64_t limit,
    const std::string &sort
) const {
    limit = std::max<int64_t>(0, std::min<int64_t>(limit, 500));
    offset = std::max<int64_t>(0, offset);

    const std::string sql = R"sql(
        SELECT id, class_name, script_path, script_project_relative_path, language, direct_base_type,
               is_resource_type, is_node_type, script_file_id, parser_version, parse_status, parse_error,
               last_parsed_generation
        FROM project_custom_classes
    )sql" + class_query_where(query) + " ORDER BY " + class_sort_sql(sort) + " LIMIT ? OFFSET ?;";

    Statement statement = database_->prepare(sql);
    statement.bind_int64(1, project_id);
    int index = 2;
    bind_class_query(statement, index, query);
    statement.bind_int64(index++, limit);
    statement.bind_int64(index++, offset);

    std::vector<CustomClassRow> rows;
    while (statement.step() == Statement::StepResult::Row) {
        rows.push_back(read_custom_class_row(statement));
    }
    return rows;
}

std::vector<ScriptDependencyRow> ScanRepository::list_dependencies_for_script(
    int64_t project_id,
    int64_t script_file_id
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id, project_id, source_script_file_id, source_symbol_id, target_file_id,
            COALESCE(target_project_relative_path, ''),
            COALESCE(target_class_name, ''),
            COALESCE(target_resource_uid, ''),
            dependency_kind, reference_text,
            source_line, source_column, CAST(confidence AS INTEGER),
            is_dynamic, is_resolved, parser_version, scan_generation, created_at_unix
        FROM script_dependencies
        WHERE project_id = ?1
          AND source_script_file_id = ?2
        ORDER BY source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, script_file_id);
    return collect_script_dependency_rows(statement);
}

std::vector<ScriptDependencyRow> ScanRepository::list_dependents_of_file(
    int64_t project_id,
    int64_t target_file_id
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id, project_id, source_script_file_id, source_symbol_id, target_file_id,
            COALESCE(target_project_relative_path, ''),
            COALESCE(target_class_name, ''),
            COALESCE(target_resource_uid, ''),
            dependency_kind, reference_text,
            source_line, source_column, CAST(confidence AS INTEGER),
            is_dynamic, is_resolved, parser_version, scan_generation, created_at_unix
        FROM script_dependencies
        WHERE project_id = ?1
          AND target_file_id = ?2
        ORDER BY source_script_file_id ASC, source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, target_file_id);
    return collect_script_dependency_rows(statement);
}

std::vector<ScriptDependencyRow> ScanRepository::list_dependents_of_class(
    int64_t project_id,
    const std::string &class_name
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id, project_id, source_script_file_id, source_symbol_id, target_file_id,
            COALESCE(target_project_relative_path, ''),
            COALESCE(target_class_name, ''),
            COALESCE(target_resource_uid, ''),
            dependency_kind, reference_text,
            source_line, source_column, CAST(confidence AS INTEGER),
            is_dynamic, is_resolved, parser_version, scan_generation, created_at_unix
        FROM script_dependencies
        WHERE project_id = ?1
          AND target_class_name = ?2
        ORDER BY source_script_file_id ASC, source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_text(2, class_name);
    return collect_script_dependency_rows(statement);
}

std::vector<ScriptDependencyRow> ScanRepository::list_unresolved_dependencies(int64_t project_id) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id, project_id, source_script_file_id, source_symbol_id, target_file_id,
            COALESCE(target_project_relative_path, ''),
            COALESCE(target_class_name, ''),
            COALESCE(target_resource_uid, ''),
            dependency_kind, reference_text,
            source_line, source_column, CAST(confidence AS INTEGER),
            is_dynamic, is_resolved, parser_version, scan_generation, created_at_unix
        FROM script_dependencies
        WHERE project_id = ?1
          AND is_resolved = 0
        ORDER BY source_script_file_id ASC, source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    return collect_script_dependency_rows(statement);
}

std::vector<ScriptDependencyRow> ScanRepository::list_dynamic_dependencies(int64_t project_id) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id, project_id, source_script_file_id, source_symbol_id, target_file_id,
            COALESCE(target_project_relative_path, ''),
            COALESCE(target_class_name, ''),
            COALESCE(target_resource_uid, ''),
            dependency_kind, reference_text,
            source_line, source_column, CAST(confidence AS INTEGER),
            is_dynamic, is_resolved, parser_version, scan_generation, created_at_unix
        FROM script_dependencies
        WHERE project_id = ?1
          AND is_dynamic <> 0
        ORDER BY source_script_file_id ASC, source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    return collect_script_dependency_rows(statement);
}

std::vector<DependencyCycleRow> ScanRepository::list_dependency_cycles(int64_t project_id) const {
    Statement statement = database_->prepare(R"sql(
        WITH RECURSIVE
        edges AS (
            SELECT source_script_file_id AS src, target_file_id AS dst
            FROM script_dependencies
            WHERE project_id = ?1
              AND target_file_id IS NOT NULL
        ),
        walk(origin, current, path, depth) AS (
            SELECT src, dst, printf('%d,%d', src, dst), 1
            FROM edges
            UNION ALL
            SELECT walk.origin,
                   edges.dst,
                   walk.path || ',' || edges.dst,
                   walk.depth + 1
            FROM walk
            JOIN edges ON edges.src = walk.current
            WHERE walk.depth < 12
              AND instr(',' || walk.path || ',', ',' || edges.dst || ',') = 0
        ),
        cycles AS (
            SELECT walk.origin AS source_script_file_id,
                   edges.dst AS cycle_to_script_file_id,
                   walk.path || ',' || edges.dst AS cycle_path,
                   walk.depth + 1 AS hop_count
            FROM walk
            JOIN edges ON edges.src = walk.current
            WHERE edges.dst = walk.origin
        )
        SELECT
            source_script_file_id,
            cycle_to_script_file_id,
            cycle_path,
            MIN(hop_count) AS hop_count
        FROM cycles
        GROUP BY source_script_file_id, cycle_to_script_file_id, cycle_path
        ORDER BY hop_count ASC, source_script_file_id ASC
        LIMIT 256;
    )sql");
    statement.bind_int64(1, project_id);

    std::vector<DependencyCycleRow> rows;
    while (statement.step() == Statement::StepResult::Row) {
        rows.push_back(read_dependency_cycle_row(statement));
    }
    return rows;
}

std::vector<ScriptDependencyRow> ScanRepository::get_dependency_graph_slice(
    int64_t project_id,
    int64_t root_script_file_id,
    int64_t depth
) const {
    const int64_t bounded_depth = std::max<int64_t>(0, std::min<int64_t>(depth, 16));

    Statement statement = database_->prepare(R"sql(
        WITH RECURSIVE nodes(depth, file_id) AS (
            SELECT 0, ?2
            UNION
            SELECT nodes.depth + 1, d.target_file_id
            FROM nodes
            JOIN script_dependencies d
              ON d.project_id = ?1
             AND d.source_script_file_id = nodes.file_id
             AND d.target_file_id IS NOT NULL
            WHERE nodes.depth < ?3
        ),
        selected_edges AS (
            SELECT DISTINCT d.id
            FROM script_dependencies d
            JOIN nodes n ON n.file_id = d.source_script_file_id
            WHERE d.project_id = ?1
        )
        SELECT
            d.id,
            d.project_id,
            d.source_script_file_id,
            d.source_symbol_id,
            d.target_file_id,
            COALESCE(d.target_project_relative_path, ''),
            COALESCE(d.target_class_name, ''),
            COALESCE(d.target_resource_uid, ''),
            d.dependency_kind,
            d.reference_text,
            d.source_line,
            d.source_column,
            CAST(d.confidence AS INTEGER),
            d.is_dynamic,
            d.is_resolved,
            d.parser_version,
            d.scan_generation,
            d.created_at_unix
        FROM script_dependencies d
        WHERE d.id IN (SELECT id FROM selected_edges)
        ORDER BY d.source_script_file_id ASC, d.source_line ASC, d.source_column ASC, d.id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, root_script_file_id);
    statement.bind_int64(3, bounded_depth);
    return collect_script_dependency_rows(statement);
}

ScanMetrics ScanRepository::get_scan_metrics(int64_t project_id, int64_t scan_run_id) const {
    ScanMetrics metrics;
    Statement statement = database_->prepare(R"sql(
        SELECT
            total_wall_ms, traversal_ms, metadata_ms,
            existing_snapshot_load_ms, reserve_setup_ms,
            dirty_check_ms, script_candidate_ms, classification_ms, script_parse_ms,
            dependency_parse_ms, tokenizer_ms,
            sqlite_write_ms, sqlite_stage_insert_ms, sqlite_file_merge_ms, sqlite_clean_refresh_ms,
            sqlite_parent_resolve_ms, sqlite_parse_status_ms, sqlite_custom_class_ms,
            dependency_sqlite_stage_ms, dependency_resolution_ms,
            sqlite_tombstone_ms, sqlite_deleted_reconcile_ms, sqlite_metrics_write_ms,
            godot_materialization_ms,
            files_seen, dirs_seen, dirs_skipped,
            entries_clean, entries_dirty, entries_new, entries_deleted,
            rows_inserted, rows_updated, rows_clean_refreshed, rows_tombstoned,
            scripts_candidates, scripts_parsed, scripts_skipped_clean,
            scripts_dependency_parsed, scripts_dependency_skipped_clean,
            script_lines_scanned, parser_lines_scanned,
            bytes_read, parser_bytes_read, parser_tokens_generated, parser_limit_exceeded_count,
            dependency_records_created, unresolved_dependency_count, dynamic_dependency_count,
            entry_record_count, path_arena_bytes, existing_snapshot_count, parsed_script_count,
            sqlite_statement_steps, sqlite_transactions, ui_rows_materialized,
            cancellation_requested, scan_result_status
        FROM scan_metrics
        WHERE project_id = ?1 AND scan_run_id = ?2
        ORDER BY id DESC
        LIMIT 1;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, scan_run_id);
    if (statement.step() != Statement::StepResult::Row) {
        return metrics;
    }

    metrics.total_wall_ms = statement.column_int64(0);
    metrics.traversal_ms = statement.column_int64(1);
    metrics.metadata_ms = statement.column_int64(2);
    metrics.existing_snapshot_load_ms = statement.column_int64(3);
    metrics.reserve_setup_ms = statement.column_int64(4);
    metrics.dirty_check_ms = statement.column_int64(5);
    metrics.script_candidate_ms = statement.column_int64(6);
    metrics.classification_ms = statement.column_int64(7);
    metrics.script_parse_ms = statement.column_int64(8);
    metrics.dependency_parse_ms = statement.column_int64(9);
    metrics.tokenizer_ms = statement.column_int64(10);
    metrics.sqlite_write_ms = statement.column_int64(11);
    metrics.sqlite_stage_insert_ms = statement.column_int64(12);
    metrics.sqlite_file_merge_ms = statement.column_int64(13);
    metrics.sqlite_clean_refresh_ms = statement.column_int64(14);
    metrics.sqlite_parent_resolve_ms = statement.column_int64(15);
    metrics.sqlite_parse_status_ms = statement.column_int64(16);
    metrics.sqlite_custom_class_ms = statement.column_int64(17);
    metrics.dependency_sqlite_stage_ms = statement.column_int64(18);
    metrics.dependency_resolution_ms = statement.column_int64(19);
    metrics.sqlite_tombstone_ms = statement.column_int64(20);
    metrics.sqlite_deleted_reconcile_ms = statement.column_int64(21);
    metrics.sqlite_metrics_write_ms = statement.column_int64(22);
    metrics.godot_materialization_ms = statement.column_int64(23);
    metrics.files_seen = statement.column_int64(24);
    metrics.dirs_seen = statement.column_int64(25);
    metrics.dirs_skipped = statement.column_int64(26);
    metrics.entries_clean = statement.column_int64(27);
    metrics.entries_dirty = statement.column_int64(28);
    metrics.entries_new = statement.column_int64(29);
    metrics.entries_deleted = statement.column_int64(30);
    metrics.rows_inserted = statement.column_int64(31);
    metrics.rows_updated = statement.column_int64(32);
    metrics.rows_clean_refreshed = statement.column_int64(33);
    metrics.rows_tombstoned = statement.column_int64(34);
    metrics.scripts_candidates = statement.column_int64(35);
    metrics.scripts_parsed = statement.column_int64(36);
    metrics.scripts_skipped_clean = statement.column_int64(37);
    metrics.scripts_dependency_parsed = statement.column_int64(38);
    metrics.scripts_dependency_skipped_clean = statement.column_int64(39);
    metrics.script_lines_scanned = statement.column_int64(40);
    metrics.parser_lines_scanned = statement.column_int64(41);
    metrics.bytes_read = statement.column_int64(42);
    metrics.parser_bytes_read = statement.column_int64(43);
    metrics.parser_tokens_generated = statement.column_int64(44);
    metrics.parser_limit_exceeded_count = statement.column_int64(45);
    metrics.dependency_records_created = statement.column_int64(46);
    metrics.unresolved_dependency_count = statement.column_int64(47);
    metrics.dynamic_dependency_count = statement.column_int64(48);
    metrics.entry_record_count = statement.column_int64(49);
    metrics.path_arena_bytes = statement.column_int64(50);
    metrics.existing_snapshot_count = statement.column_int64(51);
    metrics.parsed_script_count = statement.column_int64(52);
    metrics.sqlite_statement_steps = statement.column_int64(53);
    metrics.sqlite_transactions = statement.column_int64(54);
    metrics.ui_rows_materialized = statement.column_int64(55);
    metrics.cancellation_requested = statement.column_int64(56) != 0;
    metrics.scan_result_status = statement.column_text(57);
    return metrics;
}

std::string ScanRepository::get_scan_status(int64_t project_id, int64_t scan_run_id) const {
    Statement statement = database_->prepare(
        "SELECT status FROM project_scan_runs WHERE project_id = ?1 AND id = ?2 LIMIT 1;"
    );
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, scan_run_id);
    if (statement.step() != Statement::StepResult::Row) {
        return "";
    }
    return statement.column_text(0);
}

NativeScanPipeline::NativeScanPipeline(gotool::database::Database &database) :
    database_(&database) {}

NativeScanResult NativeScanPipeline::run_detailed(const ScanOptions &options) {
    if (options.project_root.empty()) {
        throw std::runtime_error("NativeScanPipeline requires a project root.");
    }

    if (options.persist_to_database) {
        if (database_ == nullptr) {
            throw std::runtime_error("NativeScanPipeline requires a database when persistence is enabled.");
        }
        if (options.project_id <= 0) {
            throw std::runtime_error("NativeScanPipeline requires a valid project_id when persistence is enabled.");
        }
    }

    const auto scan_start = std::chrono::steady_clock::now();
    NativeScanResult result;
    ScanMetrics &metrics = result.metrics;
    result.records.reserve(4096);

    int64_t scan_run_id = options.scan_run_id;
    ScanGeneration generation = options.scan_generation;

    std::optional<ScanRepository> repository;
    if (database_ != nullptr && options.project_id > 0) {
        repository.emplace(*database_);
    }

    if (options.persist_to_database) {
        if (!repository.has_value()) {
            throw std::runtime_error("NativeScanPipeline could not initialize repository for persistent scan.");
        }

        if (scan_run_id > 0 && generation <= 0) {
            throw std::runtime_error("scan_generation must be provided when scan_run_id is preallocated.");
        }

        if (generation <= 0) {
            generation = repository->next_generation(options.project_id);
        }

        if (scan_run_id <= 0) {
            const int64_t started_at_unix =
                options.started_at_unix > 0 ? options.started_at_unix : current_unix_time();
            scan_run_id = repository->create_scan_run(options.project_id, generation, started_at_unix);
        }
    }

    ExistingEntryMap existing;

    try {
        throw_if_cancel_requested(options.cancel_requested, metrics);

        const auto metadata_start = std::chrono::steady_clock::now();
        if (repository.has_value() && options.load_existing_snapshot) {
            existing = repository->load_existing_entries(options.project_id);
        }
        metrics.existing_snapshot_count = static_cast<int64_t>(existing.size());
        metrics.existing_snapshot_load_ms = elapsed_ms(metadata_start, std::chrono::steady_clock::now());
        metrics.metadata_ms = metrics.existing_snapshot_load_ms;

        throw_if_cancel_requested(options.cancel_requested, metrics);

        const auto reserve_start = std::chrono::steady_clock::now();
        if (!existing.empty()) {
            const size_t estimated_records = existing.size() + (existing.size() / 4) + 1024;
            if (estimated_records > result.records.capacity()) {
                result.records.reserve(estimated_records);
            }

            // Roughly account for path + file name + extension slices in one contiguous arena.
            const size_t estimated_arena_bytes = existing.size() * 96;
            result.arena.reserve(estimated_arena_bytes);
        }
        metrics.reserve_setup_ms = elapsed_ms(reserve_start, std::chrono::steady_clock::now());

        throw_if_cancel_requested(options.cancel_requested, metrics);

        SkipPolicy skip_policy;
        NativeDirectoryEnumerator enumerator;
        EnumerationOptions enumeration_options;
        enumeration_options.root = options.project_root;
        enumeration_options.include_hidden = options.include_hidden;
        enumeration_options.enable_parallel_traversal = options.enable_parallel_traversal;
        enumeration_options.deterministic_record_order = options.deterministic_record_order;
        enumeration_options.max_parallel_workers = options.max_parallel_workers;
        enumeration_options.skip_policy = &skip_policy;
        enumeration_options.cancel_requested = options.cancel_requested;

        const auto traversal_start = std::chrono::steady_clock::now();
        const EnumerationResult enumeration_result =
            enumerator.enumerate(enumeration_options, result.arena, result.records);
        metrics.traversal_ms = elapsed_ms(traversal_start, std::chrono::steady_clock::now());
        metrics.classification_ms = enumeration_result.classification_ms;
        metrics.files_seen = enumeration_result.files_seen;
        metrics.dirs_seen = enumeration_result.dirs_seen;
        metrics.dirs_skipped = enumeration_result.dirs_skipped;
        metrics.entry_record_count = static_cast<int64_t>(result.records.size());
        metrics.path_arena_bytes = static_cast<int64_t>(result.arena.size());

        if (!enumeration_result.completed) {
            metrics.cancellation_requested = true;
            metrics.scan_result_status = "cancelled";
            throw ScanCancelledError();
        }

        throw_if_cancel_requested(options.cancel_requested, metrics);

        if (!options.persist_to_database &&
            options.result_limit > 0 &&
            static_cast<int64_t>(result.records.size()) > options.result_limit) {
            result.records.resize(static_cast<size_t>(options.result_limit));
            metrics.entry_record_count = static_cast<int64_t>(result.records.size());
        }

        const bool use_dirty_path_filter =
            options.use_dirty_path_filter &&
            !options.force_rescan &&
            !options.dirty_paths.empty();
        const std::vector<std::string> normalized_dirty_paths =
            use_dirty_path_filter
                ? normalize_dirty_paths(options.dirty_paths)
                : std::vector<std::string>();

        const auto dirty_start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < result.records.size(); ++i) {
            if ((i % 512) == 0) {
                throw_if_cancel_requested(options.cancel_requested, metrics);
            }

            EntryRecord &record = result.records[i];
            const std::string_view project_path = result.arena.view(record.path_offset, record.path_length);

            std::string platform_id;
            if (record.has_platform_file_id()) {
                platform_id = platform_file_id_to_string(record);
            }

            std::optional<ExistingEntrySnapshot> snapshot;
            const auto found = existing.find(project_path);
            if (found != existing.end()) {
                snapshot = found->second;
            }

            if (!normalized_dirty_paths.empty()) {
                const bool in_dirty_path = is_path_in_dirty_set(project_path, normalized_dirty_paths);

                if (!in_dirty_path && snapshot.has_value()) {
                    record.dirty_state = DirtyState::Clean;
                    record.dirty_reason = DirtyReason::None;
                    ++metrics.entries_clean;
                    continue;
                }

                if (in_dirty_path && snapshot.has_value()) {
                    record.dirty_state = DirtyState::Dirty;
                    record.dirty_reason = DirtyReason::WatcherInvalidated;
                    ++metrics.entries_dirty;
                    continue;
                }
            }

            const DirtyCheckResult dirty = detect_dirty_state(
                record.entry_kind,
                record.size_bytes,
                record.modified_time_ns,
                platform_id,
                snapshot,
                options.force_rescan
            );
            record.dirty_state = dirty.state;
            record.dirty_reason = dirty.reason;

            if (dirty.state == DirtyState::Clean) {
                ++metrics.entries_clean;
            } else {
                ++metrics.entries_dirty;
                if (dirty.reason == DirtyReason::NewPath) {
                    ++metrics.entries_new;
                }
            }
        }
        metrics.dirty_check_ms = elapsed_ms(dirty_start, std::chrono::steady_clock::now());

        throw_if_cancel_requested(options.cancel_requested, metrics);

        const auto candidate_start = std::chrono::steady_clock::now();
        std::vector<size_t> script_candidates;
        script_candidates.reserve(256);
        const bool parse_script_metadata = options.collect_custom_classes || options.collect_script_dependencies;

        if (parse_script_metadata) {
            for (size_t i = 0; i < result.records.size(); ++i) {
                if ((i % 1024) == 0) {
                    throw_if_cancel_requested(options.cancel_requested, metrics);
                }

                const EntryRecord &record = result.records[i];
                if (record.entry_kind != EntryKind::File) {
                    continue;
                }

                const std::string_view extension = result.arena.view(record.extension_offset, record.extension_length);
                if (!is_script_extension(extension)) {
                    continue;
                }

                ++metrics.scripts_candidates;
                if (record.dirty_state == DirtyState::Clean) {
                    ++metrics.scripts_skipped_clean;
                    if (options.collect_script_dependencies) {
                        ++metrics.scripts_dependency_skipped_clean;
                    }
                    continue;
                }

                script_candidates.push_back(i);
            }
        }
        metrics.script_candidate_ms = elapsed_ms(candidate_start, std::chrono::steady_clock::now());

        throw_if_cancel_requested(options.cancel_requested, metrics);

        if (parse_script_metadata) {
            int64_t script_parse_ns_total = 0;
            for (size_t record_index : script_candidates) {
                throw_if_cancel_requested(options.cancel_requested, metrics);

                const EntryRecord &record = result.records[record_index];
                const std::string_view project_path_view =
                    result.arena.view(record.path_offset, record.path_length);
                const std::string_view extension_view =
                    result.arena.view(record.extension_offset, record.extension_length);

                const std::string project_path(project_path_view);

                const auto parse_start = std::chrono::steady_clock::now();
                ParsedScriptRecord parsed;
                parsed.record_index = record_index;
                parsed.project_relative_path = project_path;
                parsed.parse_result = parse_script_header(
                    options.project_root / std::filesystem::u8path(project_path),
                    extension_view
                );
                const auto parse_end = std::chrono::steady_clock::now();
                script_parse_ns_total +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(parse_end - parse_start).count();
                metrics.bytes_read += parsed.parse_result.bytes_read;
                metrics.parser_bytes_read += parsed.parse_result.bytes_read;
                metrics.script_lines_scanned += parsed.parse_result.lines_scanned;
                metrics.parser_lines_scanned += parsed.parse_result.lines_scanned;
                metrics.parser_tokens_generated += parsed.parse_result.tokens_generated;
                metrics.tokenizer_ms += parsed.parse_result.tokenizer_ms;
                metrics.dependency_parse_ms += parsed.parse_result.dependency_parse_ms;
                if (parsed.parse_result.limit_exceeded) {
                    ++metrics.parser_limit_exceeded_count;
                }
                if (options.collect_script_dependencies) {
                    ++metrics.scripts_dependency_parsed;
                }
                metrics.dependency_records_created += static_cast<int64_t>(parsed.parse_result.dependencies.size());
                for (const ScriptDependencyRecord &dependency : parsed.parse_result.dependencies) {
                    if (dependency.is_dynamic) {
                        ++metrics.dynamic_dependency_count;
                    }
                }
                ++metrics.scripts_parsed;
                result.parsed_scripts.push_back(std::move(parsed));
            }

            metrics.script_parse_ms = script_parse_ns_total / 1'000'000LL;
        }
        metrics.parsed_script_count = static_cast<int64_t>(result.parsed_scripts.size());

        throw_if_cancel_requested(options.cancel_requested, metrics);

        if (options.persist_to_database) {
            repository->write_scan_results(
                options.project_id,
                scan_run_id,
                generation,
                options.project_root,
                result.arena,
                result.records,
                result.parsed_scripts,
                metrics,
                options.cancel_requested
            );
        }

        metrics.total_wall_ms = elapsed_ms(scan_start, std::chrono::steady_clock::now());

        if (options.persist_to_database) {
            // Persist final wall time after the scan_metrics row has been written.
            Statement update_metrics = database_->prepare(
                "UPDATE scan_metrics SET total_wall_ms = ?1 WHERE project_id = ?2 AND scan_run_id = ?3;"
            );
            update_metrics.bind_int64(1, metrics.total_wall_ms);
            update_metrics.bind_int64(2, options.project_id);
            update_metrics.bind_int64(3, scan_run_id);
            update_metrics.step_done();
        }
    } catch (const ScanCancelledError &) {
        metrics.cancellation_requested = true;
        metrics.scan_result_status = "cancelled";
        metrics.total_wall_ms = elapsed_ms(scan_start, std::chrono::steady_clock::now());

        if (options.persist_to_database && repository.has_value() && scan_run_id > 0) {
            repository->complete_scan_run(
                options.project_id,
                scan_run_id,
                generation,
                metrics,
                current_unix_time()
            );
        }
    } catch (const std::exception &error) {
        metrics.scan_result_status = "failed";
        metrics.total_wall_ms = elapsed_ms(scan_start, std::chrono::steady_clock::now());

        if (options.persist_to_database && repository.has_value() && scan_run_id > 0) {
            repository->complete_scan_run(
                options.project_id,
                scan_run_id,
                generation,
                metrics,
                current_unix_time(),
                error.what()
            );
        }

        throw;
    }

    result.summary = build_scan_summary(scan_run_id, generation, metrics);
    return result;
}

ScanResultSummary NativeScanPipeline::run(const ScanOptions &options) {
    return run_detailed(options).summary;
}

} // namespace gotool::project_scanner
