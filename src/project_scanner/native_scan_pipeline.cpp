#include "project_scanner/native_scan_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <unordered_map>
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

ScriptSymbolRow read_script_symbol_row(Statement &statement) {
    ScriptSymbolRow row;
    row.id = statement.column_int64(0);
    row.project_id = statement.column_int64(1);
    row.script_file_id = statement.column_int64(2);
    row.symbol_slot = statement.column_int64(3);
    if (!statement.column_is_null(4)) {
        row.parent_symbol_slot = statement.column_int64(4);
    }
    if (!statement.column_is_null(5)) {
        row.parent_symbol_id = statement.column_int64(5);
    }
    row.symbol_kind = statement.column_text(6);
    row.name = statement.column_text(7);
    row.qualified_name = statement.column_text(8);
    row.declared_type = statement.column_text(9);
    row.return_type = statement.column_text(10);
    row.default_value_excerpt = statement.column_text(11);
    row.visibility = statement.column_text(12);
    row.flags = statement.column_int64(13);
    row.doc_comment_state = statement.column_text(14);
    row.symbol_name = statement.column_text(15);
    row.class_name = statement.column_text(16);
    row.language = statement.column_text(17);
    row.signature_text = statement.column_text(18);
    row.symbol_flags = statement.column_int64(19);
    row.line_start = statement.column_int64(20);
    row.column_start = statement.column_int64(21);
    row.line_end = statement.column_int64(22);
    row.column_end = statement.column_int64(23);
    row.parser_version = statement.column_int64(24);
    row.last_parsed_generation = statement.column_int64(25);
    row.last_seen_scan_run_id = statement.column_int64(26);
    row.created_at_unix = statement.column_int64(27);
    row.updated_at_unix = statement.column_int64(28);
    if (row.name.empty()) {
        row.name = row.symbol_name;
    }
    if (row.flags == 0) {
        row.flags = row.symbol_flags;
    }
    return row;
}

ScriptReferenceRow read_script_reference_row(Statement &statement) {
    ScriptReferenceRow row;
    row.id = statement.column_int64(0);
    row.project_id = statement.column_int64(1);
    row.script_file_id = statement.column_int64(2);
    row.source_script_file_id = statement.column_int64(3);
    if (!statement.column_is_null(4)) {
        row.source_symbol_id = statement.column_int64(4);
    }
    if (!statement.column_is_null(5)) {
        row.target_file_id = statement.column_int64(5);
    }
    if (!statement.column_is_null(6)) {
        row.target_symbol_id = statement.column_int64(6);
    }
    row.target_project_relative_path = statement.column_text(7);
    row.target_class_name = statement.column_text(8);
    row.target_symbol_name = statement.column_text(9);
    row.target_resource_uid = statement.column_text(10);
    row.reference_kind = statement.column_text(11);
    row.reference_text = statement.column_text(12);
    row.source_line = statement.column_int64(13);
    row.source_column = statement.column_int64(14);
    row.source_line_end = statement.column_int64(15);
    row.source_column_end = statement.column_int64(16);
    row.confidence = static_cast<double>(statement.column_int64(17)) / 1000.0;
    row.is_dynamic = statement.column_int64(18) != 0;
    row.is_resolved = statement.column_int64(19) != 0;
    row.is_unresolved = statement.column_int64(20) != 0;
    row.parser_version = statement.column_int64(21);
    row.scan_generation = statement.column_int64(22);
    row.created_at_unix = statement.column_int64(23);
    if (row.script_file_id <= 0) {
        row.script_file_id = row.source_script_file_id;
    }
    return row;
}

SceneScriptAttachmentRow read_scene_attachment_row(Statement &statement) {
    SceneScriptAttachmentRow row;
    row.id = statement.column_int64(0);
    row.project_id = statement.column_int64(1);
    row.scene_file_id = statement.column_int64(2);
    row.node_path = statement.column_text(3);
    row.node_name = statement.column_text(4);
    row.node_type = statement.column_text(5);
    row.attachment_kind = statement.column_text(6);
    row.ext_resource_id = statement.column_text(7);
    row.ext_resource_slot = statement.column_text(8);
    row.script_resource_path = statement.column_text(9);
    row.script_uid = statement.column_text(10);
    row.script_project_relative_path = statement.column_text(11);
    row.script_resource_uid = statement.column_text(12);
    if (!statement.column_is_null(13)) {
        row.script_file_id = statement.column_int64(13);
    }
    if (!statement.column_is_null(14)) {
        row.script_symbol_id = statement.column_int64(14);
    }
    row.is_dynamic = statement.column_int64(15) != 0;
    row.is_resolved = statement.column_int64(16) != 0;
    row.source_line = statement.column_int64(17);
    row.source_column = statement.column_int64(18);
    row.parser_version = statement.column_int64(19);
    row.scan_generation = statement.column_int64(20);
    row.created_at_unix = statement.column_int64(21);
    if (row.ext_resource_slot.empty()) {
        row.ext_resource_slot = row.ext_resource_id;
    }
    if (row.script_resource_path.empty()) {
        row.script_resource_path = row.script_project_relative_path;
    }
    if (row.script_uid.empty()) {
        row.script_uid = row.script_resource_uid;
    }
    return row;
}

std::vector<ScriptSymbolRow> collect_script_symbol_rows(Statement &statement) {
    std::vector<ScriptSymbolRow> rows;
    while (statement.step() == Statement::StepResult::Row) {
        rows.push_back(read_script_symbol_row(statement));
    }
    return rows;
}

std::vector<ScriptReferenceRow> collect_script_reference_rows(Statement &statement) {
    std::vector<ScriptReferenceRow> rows;
    while (statement.step() == Statement::StepResult::Row) {
        rows.push_back(read_script_reference_row(statement));
    }
    return rows;
}

std::vector<SceneScriptAttachmentRow> collect_scene_attachment_rows(Statement &statement) {
    std::vector<SceneScriptAttachmentRow> rows;
    while (statement.step() == Statement::StepResult::Row) {
        rows.push_back(read_scene_attachment_row(statement));
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
            dependency_parse_ms, full_symbol_parse_ms, doc_comment_parse_ms, scene_attachment_parse_ms, tokenizer_ms,
            sqlite_write_ms, sqlite_stage_insert_ms, sqlite_file_merge_ms, sqlite_clean_refresh_ms,
            sqlite_parent_resolve_ms, sqlite_parse_status_ms, sqlite_custom_class_ms,
            dependency_sqlite_stage_ms, dependency_resolution_ms,
            sqlite_tombstone_ms, sqlite_deleted_reconcile_ms, sqlite_metrics_write_ms,
            godot_materialization_ms,
            files_seen, dirs_seen, dirs_skipped,
            entries_clean, entries_dirty, entries_new, entries_deleted,
            rows_inserted, rows_updated, rows_clean_refreshed, rows_tombstoned,
            scripts_candidates, scripts_parsed, scripts_skipped_clean, symbols_skipped_clean, scenes_skipped_clean,
            scripts_dependency_parsed, scripts_dependency_skipped_clean,
            script_lines_scanned, parser_lines_scanned,
            bytes_read, parser_bytes_read, parser_tokens_generated, parser_limit_exceeded_count,
            symbol_rows_created, reference_rows_created, doc_comment_rows_created, scene_attachment_rows_created,
            dependency_records_created, unresolved_dependency_count, dynamic_dependency_count,
            entry_record_count, path_arena_bytes, existing_snapshot_count, parsed_script_count,
            sqlite_statement_steps, sqlite_transactions, ui_rows_materialized,
            cancellation_requested, scan_result_status, created_at_unix
        ) VALUES (
            ?1, ?2, ?3,
            ?4, ?5, ?6,
            ?7, ?8,
            ?9, ?10, ?11, ?12,
            ?13, ?14, ?15, ?16, ?17,
            ?18, ?19, ?20, ?21,
            ?22, ?23, ?24,
            ?25, ?26,
            ?27, ?28, ?29,
            ?30,
            ?31, ?32, ?33,
            ?34, ?35, ?36, ?37,
            ?38, ?39, ?40, ?41,
            ?42, ?43, ?44, ?45, ?46,
            ?47, ?48,
            ?49, ?50,
            ?51, ?52, ?53, ?54,
            ?55, ?56, ?57, ?58,
            ?59, ?60, ?61,
            ?62, ?63, ?64, ?65,
            ?66, ?67, ?68,
            ?69, ?70, ?71
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
    insert_metrics.bind_int64(14, metrics.full_symbol_parse_ms);
    insert_metrics.bind_int64(15, metrics.doc_comment_parse_ms);
    insert_metrics.bind_int64(16, metrics.scene_attachment_parse_ms);
    insert_metrics.bind_int64(17, metrics.tokenizer_ms);
    insert_metrics.bind_int64(18, metrics.sqlite_write_ms);
    insert_metrics.bind_int64(19, metrics.sqlite_stage_insert_ms);
    insert_metrics.bind_int64(20, metrics.sqlite_file_merge_ms);
    insert_metrics.bind_int64(21, metrics.sqlite_clean_refresh_ms);
    insert_metrics.bind_int64(22, metrics.sqlite_parent_resolve_ms);
    insert_metrics.bind_int64(23, metrics.sqlite_parse_status_ms);
    insert_metrics.bind_int64(24, metrics.sqlite_custom_class_ms);
    insert_metrics.bind_int64(25, metrics.dependency_sqlite_stage_ms);
    insert_metrics.bind_int64(26, metrics.dependency_resolution_ms);
    insert_metrics.bind_int64(27, metrics.sqlite_tombstone_ms);
    insert_metrics.bind_int64(28, metrics.sqlite_deleted_reconcile_ms);
    insert_metrics.bind_int64(29, metrics.sqlite_metrics_write_ms);
    insert_metrics.bind_int64(30, metrics.godot_materialization_ms);
    insert_metrics.bind_int64(31, metrics.files_seen);
    insert_metrics.bind_int64(32, metrics.dirs_seen);
    insert_metrics.bind_int64(33, metrics.dirs_skipped);
    insert_metrics.bind_int64(34, metrics.entries_clean);
    insert_metrics.bind_int64(35, metrics.entries_dirty);
    insert_metrics.bind_int64(36, metrics.entries_new);
    insert_metrics.bind_int64(37, metrics.entries_deleted);
    insert_metrics.bind_int64(38, metrics.rows_inserted);
    insert_metrics.bind_int64(39, metrics.rows_updated);
    insert_metrics.bind_int64(40, metrics.rows_clean_refreshed);
    insert_metrics.bind_int64(41, metrics.rows_tombstoned);
    insert_metrics.bind_int64(42, metrics.scripts_candidates);
    insert_metrics.bind_int64(43, metrics.scripts_parsed);
    insert_metrics.bind_int64(44, metrics.scripts_skipped_clean);
    insert_metrics.bind_int64(45, metrics.symbols_skipped_clean);
    insert_metrics.bind_int64(46, metrics.scenes_skipped_clean);
    insert_metrics.bind_int64(47, metrics.scripts_dependency_parsed);
    insert_metrics.bind_int64(48, metrics.scripts_dependency_skipped_clean);
    insert_metrics.bind_int64(49, metrics.script_lines_scanned);
    insert_metrics.bind_int64(50, metrics.parser_lines_scanned);
    insert_metrics.bind_int64(51, metrics.bytes_read);
    insert_metrics.bind_int64(52, metrics.parser_bytes_read);
    insert_metrics.bind_int64(53, metrics.parser_tokens_generated);
    insert_metrics.bind_int64(54, metrics.parser_limit_exceeded_count);
    insert_metrics.bind_int64(55, metrics.symbol_rows_created);
    insert_metrics.bind_int64(56, metrics.reference_rows_created);
    insert_metrics.bind_int64(57, metrics.doc_comment_rows_created);
    insert_metrics.bind_int64(58, metrics.scene_attachment_rows_created);
    insert_metrics.bind_int64(59, metrics.dependency_records_created);
    insert_metrics.bind_int64(60, metrics.unresolved_dependency_count);
    insert_metrics.bind_int64(61, metrics.dynamic_dependency_count);
    insert_metrics.bind_int64(62, metrics.entry_record_count);
    insert_metrics.bind_int64(63, metrics.path_arena_bytes);
    insert_metrics.bind_int64(64, metrics.existing_snapshot_count);
    insert_metrics.bind_int64(65, metrics.parsed_script_count);
    insert_metrics.bind_int64(66, metrics.sqlite_statement_steps);
    insert_metrics.bind_int64(67, metrics.sqlite_transactions);
    insert_metrics.bind_int64(68, metrics.ui_rows_materialized);
    insert_metrics.bind_int64(69, metrics.cancellation_requested ? 1 : 0);
    insert_metrics.bind_text(70, metrics.scan_result_status);
    insert_metrics.bind_int64(71, finished_at_unix);
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
               platform_file_id, parser_version, dependency_parser_version, scene_parser_version,
               classifier_version, parse_status
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
        snapshot.scene_parser_version = statement.column_int64(9);
        snapshot.classifier_version = statement.column_int64(10);
        snapshot.parse_status = parse_status_from_string(statement.column_text(11));
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
    const std::vector<ParsedSceneRecord> &parsed_scenes,
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
    database_->exec("DROP TABLE IF EXISTS temp.script_symbol_stage;");
    database_->exec("DROP TABLE IF EXISTS temp.script_reference_stage;");
    database_->exec("DROP TABLE IF EXISTS temp.script_doc_comment_stage;");
    database_->exec("DROP TABLE IF EXISTS temp.reparsed_scene_stage;");
    database_->exec("DROP TABLE IF EXISTS temp.scene_external_resource_stage;");
    database_->exec("DROP TABLE IF EXISTS temp.scene_attachment_stage;");
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
            scene_parser_version INTEGER NOT NULL,
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
            scene_parser_version,
            classifier_version
        ) VALUES (
            ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10,
            ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22
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
        insert_stage.bind_int64(21, SCENE_PARSER_VERSION);
        insert_stage.bind_int64(22, CLASSIFIER_VERSION);
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
            dependency_parser_version, scene_parser_version, classifier_version, parse_status, is_deleted, deleted_at_unix
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
            s.scene_parser_version,
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
            scene_parser_version = excluded.scene_parser_version,
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

    database_->exec(R"sql(
        CREATE TEMP TABLE temp.script_symbol_stage (
            source_project_relative_path TEXT NOT NULL,
            symbol_slot INTEGER NOT NULL,
            parent_symbol_slot INTEGER,
            symbol_kind TEXT NOT NULL,
            name TEXT NOT NULL,
            qualified_name TEXT NOT NULL,
            declared_type TEXT NOT NULL,
            return_type TEXT NOT NULL,
            default_value_excerpt TEXT NOT NULL,
            visibility TEXT NOT NULL,
            flags INTEGER NOT NULL,
            doc_comment_state TEXT NOT NULL,
            symbol_name TEXT NOT NULL,
            class_name TEXT NOT NULL,
            language TEXT NOT NULL,
            signature_text TEXT NOT NULL,
            symbol_flags INTEGER NOT NULL,
            line_start INTEGER NOT NULL,
            column_start INTEGER NOT NULL,
            line_end INTEGER NOT NULL,
            column_end INTEGER NOT NULL
        );
    )sql");

    Statement insert_symbol_stage = database_->prepare(R"sql(
        INSERT INTO temp.script_symbol_stage (
            source_project_relative_path,
            symbol_slot,
            parent_symbol_slot,
            symbol_kind,
            name,
            qualified_name,
            declared_type,
            return_type,
            default_value_excerpt,
            visibility,
            flags,
            doc_comment_state,
            symbol_name,
            class_name,
            language,
            signature_text,
            symbol_flags,
            line_start,
            column_start,
            line_end,
            column_end
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21);
    )sql");

    auto symbol_kind_to_text = [](SymbolKind kind) -> const char * {
        switch (kind) {
            case SymbolKind::Script: return "script";
            case SymbolKind::Class: return "class";
            case SymbolKind::Function: return "function";
            case SymbolKind::Property: return "property";
            case SymbolKind::Parameter: return "parameter";
            case SymbolKind::Signal: return "signal";
            case SymbolKind::Constant: return "constant";
            case SymbolKind::Enum: return "enum";
            case SymbolKind::Unknown:
            default:
                return "unknown";
        }
    };

    int64_t symbol_stage_rows = 0;
    for (const ParsedScriptRecord &parsed : parsed_scripts) {
        throw_if_cancel_requested(cancel_requested, metrics);

        bool emitted_any_symbol = false;
        for (const ScriptSymbolRecord &symbol : parsed.parse_result.symbols) {
            const int64_t symbol_slot = symbol.local_symbol_id > 0 ? symbol.local_symbol_id : symbol_stage_rows + 1;

            insert_symbol_stage.reset();
            insert_symbol_stage.clear_bindings();
            insert_symbol_stage.bind_text(1, parsed.project_relative_path);
            insert_symbol_stage.bind_int64(2, symbol_slot);
            if (symbol.parent_local_symbol_id.has_value()) {
                insert_symbol_stage.bind_int64(3, symbol.parent_local_symbol_id.value());
            } else {
                insert_symbol_stage.bind_null(3);
            }
            insert_symbol_stage.bind_text(4, symbol_kind_to_text(symbol.symbol_kind));
            insert_symbol_stage.bind_text(5, symbol.name.empty() ? symbol.symbol_name : symbol.name);
            insert_symbol_stage.bind_text(6, symbol.qualified_name);
            insert_symbol_stage.bind_text(7, symbol.declared_type);
            insert_symbol_stage.bind_text(8, symbol.return_type);
            insert_symbol_stage.bind_text(9, symbol.default_value_excerpt);
            insert_symbol_stage.bind_text(10, symbol.visibility);
            insert_symbol_stage.bind_int64(11, symbol.symbol_flags);
            insert_symbol_stage.bind_text(12, symbol.doc_comment_state);
            insert_symbol_stage.bind_text(13, symbol.symbol_name.empty() ? symbol.name : symbol.symbol_name);
            insert_symbol_stage.bind_text(14, symbol.class_name);
            insert_symbol_stage.bind_text(15, symbol.language.empty() ? to_string(parsed.parse_result.language) : symbol.language);
            insert_symbol_stage.bind_text(16, symbol.signature_text);
            insert_symbol_stage.bind_int64(17, symbol.symbol_flags);
            insert_symbol_stage.bind_int64(18, symbol.range.line_start);
            insert_symbol_stage.bind_int64(19, symbol.range.column_start);
            insert_symbol_stage.bind_int64(20, symbol.range.line_end);
            insert_symbol_stage.bind_int64(21, symbol.range.column_end);
            insert_symbol_stage.step_done();
            ++symbol_stage_rows;
            emitted_any_symbol = true;
        }

        if (!emitted_any_symbol &&
            parsed.parse_result.status == ParseStatus::ParsedClass &&
            !parsed.parse_result.class_name.empty()) {
            insert_symbol_stage.reset();
            insert_symbol_stage.clear_bindings();
            insert_symbol_stage.bind_text(1, parsed.project_relative_path);
            insert_symbol_stage.bind_int64(2, 1);
            insert_symbol_stage.bind_null(3);
            insert_symbol_stage.bind_text(4, "class");
            insert_symbol_stage.bind_text(5, parsed.parse_result.class_name);
            insert_symbol_stage.bind_text(6, parsed.parse_result.class_name);
            insert_symbol_stage.bind_text(7, parsed.parse_result.direct_base_type);
            insert_symbol_stage.bind_text(8, "");
            insert_symbol_stage.bind_text(9, "");
            insert_symbol_stage.bind_text(10, "public");
            insert_symbol_stage.bind_int64(11, SYMBOL_FLAG_PUBLIC);
            insert_symbol_stage.bind_text(12, "none");
            insert_symbol_stage.bind_text(13, parsed.parse_result.class_name);
            insert_symbol_stage.bind_text(14, parsed.parse_result.class_name);
            insert_symbol_stage.bind_text(15, to_string(parsed.parse_result.language));
            insert_symbol_stage.bind_text(16, parsed.parse_result.class_name);
            insert_symbol_stage.bind_int64(17, SYMBOL_FLAG_PUBLIC);
            insert_symbol_stage.bind_int64(18, 1);
            insert_symbol_stage.bind_int64(19, 1);
            insert_symbol_stage.bind_int64(20, 1);
            insert_symbol_stage.bind_int64(21, 1);
            insert_symbol_stage.step_done();
            ++symbol_stage_rows;
        }
    }
    metrics.sqlite_statement_steps += symbol_stage_rows;
    metrics.symbol_rows_created = symbol_stage_rows;

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

    Statement delete_reparsed_references = database_->prepare(R"sql(
        DELETE FROM script_references
        WHERE project_id = ?1
          AND source_script_file_id IN (
              SELECT file_id
              FROM temp.reparsed_script_stage
          );
    )sql");
    delete_reparsed_references.bind_int64(1, project_id);
    delete_reparsed_references.step_done();
    ++metrics.sqlite_statement_steps;

    Statement delete_reparsed_docs = database_->prepare(R"sql(
        DELETE FROM script_doc_comments
        WHERE project_id = ?1
          AND script_file_id IN (
              SELECT file_id
              FROM temp.reparsed_script_stage
          );
    )sql");
    delete_reparsed_docs.bind_int64(1, project_id);
    delete_reparsed_docs.step_done();
    ++metrics.sqlite_statement_steps;

    Statement insert_symbols = database_->prepare(R"sql(
        INSERT INTO script_symbols (
            project_id,
            script_file_id,
            symbol_slot,
            parent_symbol_slot,
            parent_symbol_id,
            symbol_kind,
            name,
            qualified_name,
            declared_type,
            return_type,
            default_value_excerpt,
            visibility,
            flags,
            doc_comment_state,
            symbol_name,
            class_name,
            language,
            signature_text,
            symbol_flags,
            line_start,
            column_start,
            line_end,
            column_end,
            parser_version,
            last_parsed_generation,
            last_seen_scan_run_id,
            created_at_unix,
            updated_at_unix
        )
        SELECT
            ?1,
            f.id,
            s.symbol_slot,
            s.parent_symbol_slot,
            NULL,
            s.symbol_kind,
            s.name,
            s.qualified_name,
            s.declared_type,
            s.return_type,
            s.default_value_excerpt,
            s.visibility,
            s.flags,
            s.doc_comment_state,
            s.symbol_name,
            s.class_name,
            s.language,
            s.signature_text,
            s.symbol_flags,
            s.line_start,
            s.column_start,
            s.line_end,
            s.column_end,
            ?2,
            ?3,
            ?4,
            ?5,
            ?5
        FROM temp.script_symbol_stage s
        JOIN project_files f
          ON f.project_id = ?1
         AND f.project_relative_path = s.source_project_relative_path
        WHERE f.is_deleted = 0;
    )sql");
    insert_symbols.bind_int64(1, project_id);
    insert_symbols.bind_int64(2, PARSER_VERSION);
    insert_symbols.bind_int64(3, generation);
    insert_symbols.bind_int64(4, scan_run_id);
    insert_symbols.bind_int64(5, observed_at_unix);
    insert_symbols.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_symbol_parents = database_->prepare(R"sql(
        UPDATE script_symbols
        SET parent_symbol_id = (
            SELECT parent.id
            FROM script_symbols parent
            WHERE parent.project_id = script_symbols.project_id
              AND parent.script_file_id = script_symbols.script_file_id
              AND parent.symbol_slot = script_symbols.parent_symbol_slot
            LIMIT 1
        )
        WHERE project_id = ?1
          AND script_file_id IN (
              SELECT file_id
              FROM temp.reparsed_script_stage
          )
          AND parent_symbol_slot IS NOT NULL;
    )sql");
    resolve_symbol_parents.bind_int64(1, project_id);
    resolve_symbol_parents.step_done();
    ++metrics.sqlite_statement_steps;

    database_->exec(R"sql(
        CREATE TEMP TABLE temp.script_reference_stage (
            source_project_relative_path TEXT NOT NULL,
            script_file_id INTEGER,
            source_script_file_id INTEGER,
            source_symbol_slot INTEGER,
            source_symbol_id INTEGER,
            target_file_id INTEGER,
            target_symbol_id INTEGER,
            target_project_relative_path TEXT,
            target_class_name TEXT,
            target_symbol_name TEXT,
            target_resource_uid TEXT,
            reference_kind TEXT NOT NULL,
            reference_text TEXT NOT NULL,
            source_line INTEGER NOT NULL,
            source_column INTEGER NOT NULL,
            source_line_end INTEGER NOT NULL,
            source_column_end INTEGER NOT NULL,
            confidence INTEGER NOT NULL,
            is_dynamic INTEGER NOT NULL,
            is_resolved INTEGER NOT NULL DEFAULT 0,
            is_unresolved INTEGER NOT NULL DEFAULT 0,
            parser_version INTEGER NOT NULL,
            scan_generation INTEGER NOT NULL
        );
    )sql");

    Statement insert_reference_stage = database_->prepare(R"sql(
        INSERT INTO temp.script_reference_stage (
            source_project_relative_path,
            script_file_id,
            source_script_file_id,
            source_symbol_slot,
            source_symbol_id,
            target_file_id,
            target_symbol_id,
            target_project_relative_path,
            target_class_name,
            target_symbol_name,
            target_resource_uid,
            reference_kind,
            reference_text,
            source_line,
            source_column,
            source_line_end,
            source_column_end,
            confidence,
            is_dynamic,
            is_resolved,
            is_unresolved,
            parser_version,
            scan_generation
        ) VALUES (
            ?1, NULL, NULL, ?2, NULL, NULL, NULL, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, 0, ?15, ?16, ?17
        );
    )sql");

    int64_t reference_stage_rows = 0;
    for (const ParsedScriptRecord &parsed : parsed_scripts) {
        for (const ScriptReferenceRecord &reference : parsed.parse_result.references) {
            const double bounded_confidence = std::max(0.0, std::min(1.0, reference.confidence));
            const int64_t confidence_scaled = static_cast<int64_t>(bounded_confidence * 1000.0);

            insert_reference_stage.reset();
            insert_reference_stage.clear_bindings();
            insert_reference_stage.bind_text(1, parsed.project_relative_path);
            if (reference.source_symbol_local_id.has_value()) {
                insert_reference_stage.bind_int64(2, reference.source_symbol_local_id.value());
            } else {
                insert_reference_stage.bind_null(2);
            }
            if (reference.target_project_relative_path.has_value()) {
                insert_reference_stage.bind_text(3, reference.target_project_relative_path.value());
            } else {
                insert_reference_stage.bind_null(3);
            }
            if (reference.target_class_name.has_value()) {
                insert_reference_stage.bind_text(4, reference.target_class_name.value());
            } else {
                insert_reference_stage.bind_null(4);
            }
            if (reference.target_symbol_name.has_value()) {
                insert_reference_stage.bind_text(5, reference.target_symbol_name.value());
            } else {
                insert_reference_stage.bind_null(5);
            }
            if (reference.target_resource_uid.has_value()) {
                insert_reference_stage.bind_text(6, reference.target_resource_uid.value());
            } else {
                insert_reference_stage.bind_null(6);
            }
            insert_reference_stage.bind_text(7, reference.reference_kind);
            insert_reference_stage.bind_text(8, reference.reference_text);
            insert_reference_stage.bind_int64(9, reference.range.line_start);
            insert_reference_stage.bind_int64(10, reference.range.column_start);
            insert_reference_stage.bind_int64(11, reference.range.line_end);
            insert_reference_stage.bind_int64(12, reference.range.column_end);
            insert_reference_stage.bind_int64(13, confidence_scaled);
            insert_reference_stage.bind_int64(14, reference.is_dynamic ? 1 : 0);
            insert_reference_stage.bind_int64(15, reference.is_unresolved ? 1 : 0);
            insert_reference_stage.bind_int64(16, PARSER_VERSION);
            insert_reference_stage.bind_int64(17, generation);
            insert_reference_stage.step_done();
            ++reference_stage_rows;
        }
    }
    metrics.sqlite_statement_steps += reference_stage_rows;
    metrics.reference_rows_created = reference_stage_rows;

    Statement resolve_reference_source_files = database_->prepare(R"sql(
        UPDATE temp.script_reference_stage
        SET source_script_file_id = (
            SELECT r.file_id
            FROM temp.reparsed_script_stage r
            WHERE r.project_relative_path = source_project_relative_path
            LIMIT 1
        ),
            script_file_id = (
                SELECT r.file_id
                FROM temp.reparsed_script_stage r
                WHERE r.project_relative_path = source_project_relative_path
                LIMIT 1
            );
    )sql");
    resolve_reference_source_files.step_done();
    ++metrics.sqlite_statement_steps;

    Statement drop_unbound_references = database_->prepare(
        "DELETE FROM temp.script_reference_stage WHERE source_script_file_id IS NULL;"
    );
    drop_unbound_references.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_reference_source_symbols = database_->prepare(R"sql(
        UPDATE temp.script_reference_stage
        SET source_symbol_id = (
            SELECT s.id
            FROM script_symbols s
            WHERE s.project_id = ?1
              AND s.script_file_id = source_script_file_id
              AND source_symbol_slot IS NOT NULL
              AND s.symbol_slot = source_symbol_slot
            LIMIT 1
        );
    )sql");
    resolve_reference_source_symbols.bind_int64(1, project_id);
    resolve_reference_source_symbols.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_reference_targets_by_path = database_->prepare(R"sql(
        UPDATE temp.script_reference_stage
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
    resolve_reference_targets_by_path.bind_int64(1, project_id);
    resolve_reference_targets_by_path.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_reference_targets_by_class = database_->prepare(R"sql(
        UPDATE temp.script_reference_stage
        SET target_file_id = (
            SELECT s.script_file_id
            FROM script_symbols s
            JOIN project_files f
              ON f.id = s.script_file_id
             AND f.project_id = s.project_id
             AND f.is_deleted = 0
            WHERE s.project_id = ?1
              AND s.symbol_kind = 'class'
              AND (s.name = target_class_name OR s.symbol_name = target_class_name OR s.class_name = target_class_name)
            LIMIT 1
        )
        WHERE target_file_id IS NULL
          AND target_class_name IS NOT NULL
          AND target_class_name <> '';
    )sql");
    resolve_reference_targets_by_class.bind_int64(1, project_id);
    resolve_reference_targets_by_class.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_reference_target_symbols = database_->prepare(R"sql(
        UPDATE temp.script_reference_stage
        SET target_symbol_id = (
            SELECT s.id
            FROM script_symbols s
            WHERE s.project_id = ?1
              AND target_symbol_name IS NOT NULL
              AND target_symbol_name <> ''
              AND (s.name = target_symbol_name OR s.symbol_name = target_symbol_name)
              AND (
                  target_file_id IS NULL OR
                  s.script_file_id = target_file_id
              )
            LIMIT 1
        );
    )sql");
    resolve_reference_target_symbols.bind_int64(1, project_id);
    resolve_reference_target_symbols.step_done();
    ++metrics.sqlite_statement_steps;

    Statement mark_reference_resolved = database_->prepare(R"sql(
        UPDATE temp.script_reference_stage
        SET is_resolved = CASE
                WHEN target_file_id IS NOT NULL OR target_symbol_id IS NOT NULL THEN 1
                ELSE 0
            END,
            is_unresolved = CASE
                WHEN is_dynamic <> 0 THEN 0
                WHEN target_file_id IS NOT NULL OR target_symbol_id IS NOT NULL THEN 0
                ELSE 1
            END;
    )sql");
    mark_reference_resolved.step_done();
    ++metrics.sqlite_statement_steps;

    Statement insert_references = database_->prepare(R"sql(
        INSERT INTO script_references (
            project_id,
            script_file_id,
            source_script_file_id,
            source_symbol_id,
            target_file_id,
            target_symbol_id,
            target_project_relative_path,
            target_class_name,
            target_symbol_name,
            target_resource_uid,
            reference_kind,
            reference_text,
            source_line,
            source_column,
            source_line_end,
            source_column_end,
            confidence,
            is_dynamic,
            is_resolved,
            is_unresolved,
            parser_version,
            scan_generation,
            created_at_unix
        )
        SELECT
            ?1,
            script_file_id,
            source_script_file_id,
            source_symbol_id,
            target_file_id,
            target_symbol_id,
            target_project_relative_path,
            target_class_name,
            target_symbol_name,
            target_resource_uid,
            reference_kind,
            reference_text,
            source_line,
            source_column,
            source_line_end,
            source_column_end,
            confidence,
            is_dynamic,
            is_resolved,
            is_unresolved,
            parser_version,
            scan_generation,
            ?2
        FROM temp.script_reference_stage;
    )sql");
    insert_references.bind_int64(1, project_id);
    insert_references.bind_int64(2, observed_at_unix);
    insert_references.step_done();
    ++metrics.sqlite_statement_steps;

    database_->exec(R"sql(
        CREATE TEMP TABLE temp.script_doc_comment_stage (
            source_project_relative_path TEXT NOT NULL,
            script_file_id INTEGER,
            symbol_slot INTEGER,
            target_symbol_id INTEGER,
            target_kind TEXT NOT NULL,
            text_hash TEXT NOT NULL,
            text_excerpt TEXT NOT NULL,
            start_line INTEGER NOT NULL,
            end_line INTEGER NOT NULL,
            is_attached INTEGER NOT NULL,
            symbol_id INTEGER,
            comment_style TEXT NOT NULL,
            comment_text TEXT NOT NULL,
            summary_text TEXT NOT NULL,
            line_start INTEGER NOT NULL,
            column_start INTEGER NOT NULL,
            line_end INTEGER NOT NULL,
            column_end INTEGER NOT NULL,
            parser_version INTEGER NOT NULL,
            scan_generation INTEGER NOT NULL
        );
    )sql");

    Statement insert_doc_stage = database_->prepare(R"sql(
        INSERT INTO temp.script_doc_comment_stage (
            source_project_relative_path,
            script_file_id,
            symbol_slot,
            target_symbol_id,
            target_kind,
            text_hash,
            text_excerpt,
            start_line,
            end_line,
            is_attached,
            symbol_id,
            comment_style,
            comment_text,
            summary_text,
            line_start,
            column_start,
            line_end,
            column_end,
            parser_version,
            scan_generation
        ) VALUES (
            ?1, NULL, ?2, NULL, ?3, ?4, ?5, ?6, ?7, ?8, NULL, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17
        );
    )sql");

    int64_t doc_stage_rows = 0;
    for (const ParsedScriptRecord &parsed : parsed_scripts) {
        for (const ScriptDocCommentRecord &comment : parsed.parse_result.doc_comments) {
            insert_doc_stage.reset();
            insert_doc_stage.clear_bindings();
            insert_doc_stage.bind_text(1, parsed.project_relative_path);
            if (comment.symbol_local_id.has_value()) {
                insert_doc_stage.bind_int64(2, comment.symbol_local_id.value());
            } else {
                insert_doc_stage.bind_null(2);
            }
            insert_doc_stage.bind_text(3, comment.target_kind);
            insert_doc_stage.bind_text(4, comment.text_hash);
            insert_doc_stage.bind_text(5, comment.text_excerpt);
            insert_doc_stage.bind_int64(6, comment.range.line_start);
            insert_doc_stage.bind_int64(7, comment.range.line_end);
            insert_doc_stage.bind_int64(8, comment.is_attached ? 1 : 0);
            insert_doc_stage.bind_text(9, comment.comment_style);
            insert_doc_stage.bind_text(10, comment.comment_text);
            insert_doc_stage.bind_text(11, comment.summary_text);
            insert_doc_stage.bind_int64(12, comment.range.line_start);
            insert_doc_stage.bind_int64(13, comment.range.column_start);
            insert_doc_stage.bind_int64(14, comment.range.line_end);
            insert_doc_stage.bind_int64(15, comment.range.column_end);
            insert_doc_stage.bind_int64(16, PARSER_VERSION);
            insert_doc_stage.bind_int64(17, generation);
            insert_doc_stage.step_done();
            ++doc_stage_rows;
        }
    }
    metrics.sqlite_statement_steps += doc_stage_rows;
    metrics.doc_comment_rows_created = doc_stage_rows;

    Statement resolve_doc_source_files = database_->prepare(R"sql(
        UPDATE temp.script_doc_comment_stage
        SET script_file_id = (
            SELECT r.file_id
            FROM temp.reparsed_script_stage r
            WHERE r.project_relative_path = source_project_relative_path
            LIMIT 1
        );
    )sql");
    resolve_doc_source_files.step_done();
    ++metrics.sqlite_statement_steps;

    Statement drop_unbound_doc_rows = database_->prepare(
        "DELETE FROM temp.script_doc_comment_stage WHERE script_file_id IS NULL;"
    );
    drop_unbound_doc_rows.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_doc_symbol_ids = database_->prepare(R"sql(
        UPDATE temp.script_doc_comment_stage
        SET symbol_id = (
            SELECT s.id
            FROM script_symbols s
            WHERE s.project_id = ?1
              AND s.script_file_id = script_file_id
              AND symbol_slot IS NOT NULL
              AND s.symbol_slot = symbol_slot
            LIMIT 1
        ),
            target_symbol_id = (
                SELECT s.id
                FROM script_symbols s
                WHERE s.project_id = ?1
                  AND s.script_file_id = script_file_id
                  AND symbol_slot IS NOT NULL
                  AND s.symbol_slot = symbol_slot
                LIMIT 1
            );
    )sql");
    resolve_doc_symbol_ids.bind_int64(1, project_id);
    resolve_doc_symbol_ids.step_done();
    ++metrics.sqlite_statement_steps;

    Statement insert_doc_comments = database_->prepare(R"sql(
        INSERT INTO script_doc_comments (
            project_id,
            script_file_id,
            target_symbol_id,
            target_kind,
            text_hash,
            text_excerpt,
            start_line,
            end_line,
            is_attached,
            symbol_id,
            comment_style,
            comment_text,
            summary_text,
            line_start,
            column_start,
            line_end,
            column_end,
            parser_version,
            scan_generation,
            created_at_unix
        )
        SELECT
            ?1,
            script_file_id,
            target_symbol_id,
            target_kind,
            text_hash,
            text_excerpt,
            start_line,
            end_line,
            is_attached,
            symbol_id,
            comment_style,
            comment_text,
            summary_text,
            line_start,
            column_start,
            line_end,
            column_end,
            parser_version,
            scan_generation,
            ?2
        FROM temp.script_doc_comment_stage;
    )sql");
    insert_doc_comments.bind_int64(1, project_id);
    insert_doc_comments.bind_int64(2, observed_at_unix);
    insert_doc_comments.step_done();
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

    Statement delete_stale_doc_comments = database_->prepare(R"sql(
        DELETE FROM script_doc_comments
        WHERE project_id = ?1
          AND NOT EXISTS (
              SELECT 1
              FROM project_files f
              WHERE f.id = script_doc_comments.script_file_id
                AND f.project_id = script_doc_comments.project_id
                AND f.is_deleted = 0
          );
    )sql");
    delete_stale_doc_comments.bind_int64(1, project_id);
    delete_stale_doc_comments.step_done();
    ++metrics.sqlite_statement_steps;

    Statement delete_stale_references = database_->prepare(R"sql(
        DELETE FROM script_references
        WHERE project_id = ?1
          AND NOT EXISTS (
              SELECT 1
              FROM project_files f
              WHERE f.id = script_references.source_script_file_id
                AND f.project_id = script_references.project_id
                AND f.is_deleted = 0
          );
    )sql");
    delete_stale_references.bind_int64(1, project_id);
    delete_stale_references.step_done();
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
            ?1, NULL, NULL, NULL, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13
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
            insert_dependency_stage.bind_int64(7, dependency.range.line_start);
            insert_dependency_stage.bind_int64(8, dependency.range.column_start);
            insert_dependency_stage.bind_int64(9, confidence_scaled);
            insert_dependency_stage.bind_int64(10, dependency.is_dynamic ? 1 : 0);
            insert_dependency_stage.bind_int64(11, dependency.is_resolved ? 1 : 0);
            insert_dependency_stage.bind_int64(12, DEPENDENCY_PARSER_VERSION);
            insert_dependency_stage.bind_int64(13, generation);
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
                            AND s.symbol_kind = 'class'
                            AND (s.name = target_class_name OR s.symbol_name = target_class_name OR s.class_name = target_class_name)
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
                            AND s.symbol_kind = 'class'
                        ORDER BY s.symbol_slot ASC
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

    database_->exec(R"sql(
        CREATE TEMP TABLE temp.reparsed_scene_stage (
            file_id INTEGER PRIMARY KEY,
            project_relative_path TEXT NOT NULL UNIQUE
        ) WITHOUT ROWID;
    )sql");

    Statement insert_reparsed_scenes = database_->prepare(R"sql(
        INSERT INTO temp.reparsed_scene_stage (file_id, project_relative_path)
        SELECT f.id, ?2
        FROM project_files f
        WHERE f.project_id = ?1
          AND f.project_relative_path = ?2
          AND f.is_deleted = 0
        LIMIT 1;
    )sql");
    for (const ParsedSceneRecord &parsed_scene : parsed_scenes) {
        insert_reparsed_scenes.reset();
        insert_reparsed_scenes.clear_bindings();
        insert_reparsed_scenes.bind_int64(1, project_id);
        insert_reparsed_scenes.bind_text(2, parsed_scene.project_relative_path);
        insert_reparsed_scenes.step_done();
    }
    metrics.sqlite_statement_steps += static_cast<int64_t>(parsed_scenes.size());

    database_->exec(R"sql(
        CREATE TEMP TABLE temp.scene_external_resource_stage (
            source_scene_project_relative_path TEXT NOT NULL,
            scene_file_id INTEGER,
            ext_resource_id TEXT NOT NULL,
            resource_slot TEXT NOT NULL,
            resource_type TEXT NOT NULL,
            resource_path TEXT NOT NULL,
            resource_uid TEXT NOT NULL,
            target_file_id INTEGER,
            is_script_resource INTEGER NOT NULL,
            is_resolved INTEGER NOT NULL DEFAULT 0,
            source_line INTEGER NOT NULL,
            source_column INTEGER NOT NULL,
            scene_parser_version INTEGER NOT NULL,
            parser_version INTEGER NOT NULL,
            scan_generation INTEGER NOT NULL
        );
    )sql");

    database_->exec(R"sql(
        CREATE TEMP TABLE temp.scene_attachment_stage (
            source_scene_project_relative_path TEXT NOT NULL,
            scene_file_id INTEGER,
            node_path TEXT NOT NULL,
            node_name TEXT NOT NULL,
            node_type TEXT NOT NULL,
            attachment_kind TEXT NOT NULL,
            ext_resource_id TEXT NOT NULL,
            ext_resource_slot TEXT NOT NULL,
            script_resource_path TEXT NOT NULL,
            script_uid TEXT NOT NULL,
            script_project_relative_path TEXT NOT NULL,
            script_resource_uid TEXT NOT NULL,
            script_file_id INTEGER,
            script_symbol_id INTEGER,
            is_dynamic INTEGER NOT NULL,
            is_resolved INTEGER NOT NULL DEFAULT 0,
            source_line INTEGER NOT NULL,
            source_column INTEGER NOT NULL,
            scene_parser_version INTEGER NOT NULL,
            parser_version INTEGER NOT NULL,
            scan_generation INTEGER NOT NULL
        );
    )sql");

    Statement insert_scene_resource_stage = database_->prepare(R"sql(
        INSERT INTO temp.scene_external_resource_stage (
            source_scene_project_relative_path,
            scene_file_id,
            ext_resource_id,
            resource_slot,
            resource_type,
            resource_path,
            resource_uid,
            target_file_id,
            is_script_resource,
            is_resolved,
            source_line,
            source_column,
            scene_parser_version,
            parser_version,
            scan_generation
        ) VALUES (?1, NULL, ?2, ?3, ?4, ?5, ?6, NULL, ?7, 0, ?8, ?9, ?10, ?11, ?12);
    )sql");

    Statement insert_scene_attachment_stage = database_->prepare(R"sql(
        INSERT INTO temp.scene_attachment_stage (
            source_scene_project_relative_path,
            scene_file_id,
            node_path,
            node_name,
            node_type,
            attachment_kind,
            ext_resource_id,
            ext_resource_slot,
            script_resource_path,
            script_uid,
            script_project_relative_path,
            script_resource_uid,
            script_file_id,
            script_symbol_id,
            is_dynamic,
            is_resolved,
            source_line,
            source_column,
            scene_parser_version,
            parser_version,
            scan_generation
        ) VALUES (?1, NULL, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, NULL, NULL, ?12, 0, ?13, ?14, ?15, ?16, ?17);
    )sql");

    int64_t scene_attachment_stage_rows = 0;
    for (const ParsedSceneRecord &parsed_scene : parsed_scenes) {
        for (const SceneExternalResourceRecord &resource : parsed_scene.parse_result.external_resources) {
            insert_scene_resource_stage.reset();
            insert_scene_resource_stage.clear_bindings();
            insert_scene_resource_stage.bind_text(1, parsed_scene.project_relative_path);
            insert_scene_resource_stage.bind_text(2, resource.ext_resource_id);
            insert_scene_resource_stage.bind_text(3, resource.ext_resource_id);
            insert_scene_resource_stage.bind_text(4, resource.resource_type);
            insert_scene_resource_stage.bind_text(5, resource.resource_path);
            insert_scene_resource_stage.bind_text(6, resource.resource_uid);
            insert_scene_resource_stage.bind_int64(7, resource.is_script_resource ? 1 : 0);
            insert_scene_resource_stage.bind_int64(8, resource.range.line_start);
            insert_scene_resource_stage.bind_int64(9, resource.range.column_start);
            insert_scene_resource_stage.bind_int64(10, SCENE_PARSER_VERSION);
            insert_scene_resource_stage.bind_int64(11, SCENE_PARSER_VERSION);
            insert_scene_resource_stage.bind_int64(12, generation);
            insert_scene_resource_stage.step_done();
        }

        for (const SceneScriptAttachmentRecord &attachment : parsed_scene.parse_result.script_attachments) {
            insert_scene_attachment_stage.reset();
            insert_scene_attachment_stage.clear_bindings();
            insert_scene_attachment_stage.bind_text(1, parsed_scene.project_relative_path);
            insert_scene_attachment_stage.bind_text(2, attachment.node_path);
            insert_scene_attachment_stage.bind_text(3, attachment.node_name);
            insert_scene_attachment_stage.bind_text(4, attachment.node_type);
            insert_scene_attachment_stage.bind_text(5, attachment.attachment_kind);
            insert_scene_attachment_stage.bind_text(6, attachment.ext_resource_id);
            insert_scene_attachment_stage.bind_text(7, attachment.ext_resource_id);
            insert_scene_attachment_stage.bind_text(8, attachment.script_resource_path);
            insert_scene_attachment_stage.bind_text(9, attachment.script_uid);
            insert_scene_attachment_stage.bind_text(10, attachment.script_project_relative_path);
            insert_scene_attachment_stage.bind_text(11, attachment.script_resource_uid);
            insert_scene_attachment_stage.bind_int64(12, attachment.is_dynamic ? 1 : 0);
            insert_scene_attachment_stage.bind_int64(13, attachment.range.line_start);
            insert_scene_attachment_stage.bind_int64(14, attachment.range.column_start);
            insert_scene_attachment_stage.bind_int64(15, SCENE_PARSER_VERSION);
            insert_scene_attachment_stage.bind_int64(16, SCENE_PARSER_VERSION);
            insert_scene_attachment_stage.bind_int64(17, generation);
            insert_scene_attachment_stage.step_done();
            ++scene_attachment_stage_rows;
        }
    }
    metrics.scene_attachment_rows_created = scene_attachment_stage_rows;
    metrics.sqlite_statement_steps += scene_attachment_stage_rows;

    Statement resolve_scene_resource_file_ids = database_->prepare(R"sql(
        UPDATE temp.scene_external_resource_stage
        SET scene_file_id = (
            SELECT r.file_id
            FROM temp.reparsed_scene_stage r
            WHERE r.project_relative_path = source_scene_project_relative_path
            LIMIT 1
        );
    )sql");
    resolve_scene_resource_file_ids.step_done();
    ++metrics.sqlite_statement_steps;

    Statement drop_unbound_scene_resources = database_->prepare(
        "DELETE FROM temp.scene_external_resource_stage WHERE scene_file_id IS NULL;"
    );
    drop_unbound_scene_resources.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_scene_resource_targets = database_->prepare(R"sql(
        UPDATE temp.scene_external_resource_stage
        SET target_file_id = (
            SELECT f.id
            FROM project_files f
            WHERE f.project_id = ?1
              AND f.project_relative_path = resource_path
              AND f.is_deleted = 0
            LIMIT 1
        )
        WHERE resource_path <> '';
    )sql");
    resolve_scene_resource_targets.bind_int64(1, project_id);
    resolve_scene_resource_targets.step_done();
    ++metrics.sqlite_statement_steps;

    Statement mark_scene_resources_resolved = database_->prepare(R"sql(
        UPDATE temp.scene_external_resource_stage
        SET is_resolved = CASE WHEN target_file_id IS NOT NULL THEN 1 ELSE 0 END;
    )sql");
    mark_scene_resources_resolved.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_scene_attachment_file_ids = database_->prepare(R"sql(
        UPDATE temp.scene_attachment_stage
        SET scene_file_id = (
            SELECT r.file_id
            FROM temp.reparsed_scene_stage r
            WHERE r.project_relative_path = source_scene_project_relative_path
            LIMIT 1
        );
    )sql");
    resolve_scene_attachment_file_ids.step_done();
    ++metrics.sqlite_statement_steps;

    Statement drop_unbound_scene_attachments = database_->prepare(
        "DELETE FROM temp.scene_attachment_stage WHERE scene_file_id IS NULL;"
    );
    drop_unbound_scene_attachments.step_done();
    ++metrics.sqlite_statement_steps;

    Statement fill_attachment_paths_from_ext = database_->prepare(R"sql(
        UPDATE temp.scene_attachment_stage
        SET script_project_relative_path = (
                SELECT r.resource_path
                FROM temp.scene_external_resource_stage r
                WHERE r.source_scene_project_relative_path = temp.scene_attachment_stage.source_scene_project_relative_path
                  AND (r.ext_resource_id = temp.scene_attachment_stage.ext_resource_id OR
                       r.resource_slot = temp.scene_attachment_stage.ext_resource_slot)
                LIMIT 1
            ),
            script_resource_path = (
                SELECT r.resource_path
                FROM temp.scene_external_resource_stage r
                WHERE r.source_scene_project_relative_path = temp.scene_attachment_stage.source_scene_project_relative_path
                  AND (r.ext_resource_id = temp.scene_attachment_stage.ext_resource_id OR
                       r.resource_slot = temp.scene_attachment_stage.ext_resource_slot)
                LIMIT 1
            ),
            script_resource_uid = (
                SELECT r.resource_uid
                FROM temp.scene_external_resource_stage r
                WHERE r.source_scene_project_relative_path = temp.scene_attachment_stage.source_scene_project_relative_path
                  AND (r.ext_resource_id = temp.scene_attachment_stage.ext_resource_id OR
                       r.resource_slot = temp.scene_attachment_stage.ext_resource_slot)
                LIMIT 1
            ),
            script_uid = (
                SELECT r.resource_uid
                FROM temp.scene_external_resource_stage r
                WHERE r.source_scene_project_relative_path = temp.scene_attachment_stage.source_scene_project_relative_path
                  AND (r.ext_resource_id = temp.scene_attachment_stage.ext_resource_id OR
                       r.resource_slot = temp.scene_attachment_stage.ext_resource_slot)
                LIMIT 1
            )
        WHERE script_project_relative_path = ''
          AND (ext_resource_slot <> '' OR ext_resource_id <> '');
    )sql");
    fill_attachment_paths_from_ext.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_attachment_script_file = database_->prepare(R"sql(
        UPDATE temp.scene_attachment_stage
        SET script_file_id = (
            SELECT f.id
            FROM project_files f
            WHERE f.project_id = ?1
              AND f.project_relative_path = script_project_relative_path
              AND f.is_deleted = 0
            LIMIT 1
        )
        WHERE script_project_relative_path <> '';
    )sql");
    resolve_attachment_script_file.bind_int64(1, project_id);
    resolve_attachment_script_file.step_done();
    ++metrics.sqlite_statement_steps;

    Statement resolve_attachment_script_symbol = database_->prepare(R"sql(
        UPDATE temp.scene_attachment_stage
        SET script_symbol_id = (
            SELECT s.id
            FROM script_symbols s
            WHERE s.project_id = ?1
              AND s.script_file_id = script_file_id
              AND s.symbol_kind = 'class'
            LIMIT 1
        )
        WHERE script_file_id IS NOT NULL;
    )sql");
    resolve_attachment_script_symbol.bind_int64(1, project_id);
    resolve_attachment_script_symbol.step_done();
    ++metrics.sqlite_statement_steps;

    Statement mark_attachments_resolved = database_->prepare(R"sql(
        UPDATE temp.scene_attachment_stage
        SET is_resolved = CASE WHEN script_file_id IS NOT NULL THEN 1 ELSE 0 END;
    )sql");
    mark_attachments_resolved.step_done();
    ++metrics.sqlite_statement_steps;

    Statement delete_old_scene_resources = database_->prepare(R"sql(
        DELETE FROM scene_external_resources
        WHERE project_id = ?1
          AND scene_file_id IN (
              SELECT file_id
              FROM temp.reparsed_scene_stage
          );
    )sql");
    delete_old_scene_resources.bind_int64(1, project_id);
    delete_old_scene_resources.step_done();
    ++metrics.sqlite_statement_steps;

    Statement delete_old_scene_attachments = database_->prepare(R"sql(
        DELETE FROM scene_script_attachments
        WHERE project_id = ?1
          AND scene_file_id IN (
              SELECT file_id
              FROM temp.reparsed_scene_stage
          );
    )sql");
    delete_old_scene_attachments.bind_int64(1, project_id);
    delete_old_scene_attachments.step_done();
    ++metrics.sqlite_statement_steps;

    Statement insert_scene_resources = database_->prepare(R"sql(
        INSERT INTO scene_external_resources (
            project_id,
            scene_file_id,
            ext_resource_id,
            resource_slot,
            resource_type,
            resource_path,
            resource_uid,
            target_file_id,
            is_script_resource,
            is_resolved,
            source_line,
            source_column,
            scene_parser_version,
            parser_version,
            scan_generation,
            created_at_unix
        )
        SELECT
            ?1,
            scene_file_id,
            ext_resource_id,
            resource_slot,
            resource_type,
            resource_path,
            resource_uid,
            target_file_id,
            is_script_resource,
            is_resolved,
            source_line,
            source_column,
            scene_parser_version,
            parser_version,
            scan_generation,
            ?2
        FROM temp.scene_external_resource_stage;
    )sql");
    insert_scene_resources.bind_int64(1, project_id);
    insert_scene_resources.bind_int64(2, observed_at_unix);
    insert_scene_resources.step_done();
    ++metrics.sqlite_statement_steps;

    Statement insert_scene_attachments = database_->prepare(R"sql(
        INSERT INTO scene_script_attachments (
            project_id,
            scene_file_id,
            node_path,
            node_name,
            node_type,
            attachment_kind,
            ext_resource_id,
            ext_resource_slot,
            script_resource_path,
            script_uid,
            script_project_relative_path,
            script_resource_uid,
            script_file_id,
            script_symbol_id,
            is_dynamic,
            is_resolved,
            source_line,
            source_column,
            scene_parser_version,
            parser_version,
            scan_generation,
            created_at_unix
        )
        SELECT
            ?1,
            scene_file_id,
            node_path,
            node_name,
            node_type,
            attachment_kind,
            ext_resource_id,
            ext_resource_slot,
            script_resource_path,
            script_uid,
            script_project_relative_path,
            script_resource_uid,
            script_file_id,
            script_symbol_id,
            is_dynamic,
            is_resolved,
            source_line,
            source_column,
            scene_parser_version,
            parser_version,
            scan_generation,
            ?2
        FROM temp.scene_attachment_stage;
    )sql");
    insert_scene_attachments.bind_int64(1, project_id);
    insert_scene_attachments.bind_int64(2, observed_at_unix);
    insert_scene_attachments.step_done();
    ++metrics.sqlite_statement_steps;

    Statement delete_stale_scene_resources = database_->prepare(R"sql(
        DELETE FROM scene_external_resources
        WHERE project_id = ?1
          AND NOT EXISTS (
              SELECT 1
              FROM project_files f
              WHERE f.id = scene_external_resources.scene_file_id
                AND f.project_id = scene_external_resources.project_id
                AND f.is_deleted = 0
          );
    )sql");
    delete_stale_scene_resources.bind_int64(1, project_id);
    delete_stale_scene_resources.step_done();
    ++metrics.sqlite_statement_steps;

    Statement delete_stale_scene_attachments = database_->prepare(R"sql(
        DELETE FROM scene_script_attachments
        WHERE project_id = ?1
          AND NOT EXISTS (
              SELECT 1
              FROM project_files f
              WHERE f.id = scene_script_attachments.scene_file_id
                AND f.project_id = scene_script_attachments.project_id
                AND f.is_deleted = 0
          );
    )sql");
    delete_stale_scene_attachments.bind_int64(1, project_id);
    delete_stale_scene_attachments.step_done();
    ++metrics.sqlite_statement_steps;

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

        Statement null_missing_dependency_source_symbols = database_->prepare(R"sql(
                UPDATE script_dependencies
                SET source_symbol_id = NULL
                WHERE project_id = ?1
                    AND source_symbol_id IS NOT NULL
                    AND NOT EXISTS (
                            SELECT 1
                            FROM script_symbols s
                            WHERE s.id = script_dependencies.source_symbol_id
                                AND s.project_id = script_dependencies.project_id
                    );
        )sql");
        null_missing_dependency_source_symbols.bind_int64(1, project_id);
        null_missing_dependency_source_symbols.step_done();
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

        Statement delete_deleted_source_references = database_->prepare(R"sql(
                DELETE FROM script_references
                WHERE project_id = ?1
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = script_references.source_script_file_id
                                AND f.project_id = script_references.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        delete_deleted_source_references.bind_int64(1, project_id);
        delete_deleted_source_references.step_done();
        ++metrics.sqlite_statement_steps;

        Statement null_missing_reference_source_symbols = database_->prepare(R"sql(
                UPDATE script_references
                SET source_symbol_id = NULL
                WHERE project_id = ?1
                    AND source_symbol_id IS NOT NULL
                    AND NOT EXISTS (
                            SELECT 1
                            FROM script_symbols s
                            WHERE s.id = script_references.source_symbol_id
                                AND s.project_id = script_references.project_id
                    );
        )sql");
        null_missing_reference_source_symbols.bind_int64(1, project_id);
        null_missing_reference_source_symbols.step_done();
        ++metrics.sqlite_statement_steps;

        Statement invalidate_deleted_reference_targets = database_->prepare(R"sql(
                UPDATE script_references
                SET target_file_id = NULL,
                        target_symbol_id = NULL,
                        is_resolved = 0,
                        is_unresolved = 1
                WHERE project_id = ?1
                    AND target_file_id IS NOT NULL
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = script_references.target_file_id
                                AND f.project_id = script_references.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        invalidate_deleted_reference_targets.bind_int64(1, project_id);
        invalidate_deleted_reference_targets.step_done();
        ++metrics.sqlite_statement_steps;

        Statement invalidate_missing_reference_target_symbols = database_->prepare(R"sql(
                UPDATE script_references
                SET target_symbol_id = NULL,
                        is_resolved = 0,
                        is_unresolved = 1
                WHERE project_id = ?1
                    AND target_symbol_id IS NOT NULL
                    AND NOT EXISTS (
                            SELECT 1
                            FROM script_symbols s
                            WHERE s.id = script_references.target_symbol_id
                                AND s.project_id = script_references.project_id
                    );
        )sql");
        invalidate_missing_reference_target_symbols.bind_int64(1, project_id);
        invalidate_missing_reference_target_symbols.step_done();
        ++metrics.sqlite_statement_steps;

        Statement delete_stale_doc_comments_after_tombstone = database_->prepare(R"sql(
                DELETE FROM script_doc_comments
                WHERE project_id = ?1
                    AND (
                            NOT EXISTS (
                                    SELECT 1
                                    FROM project_files f
                                    WHERE f.id = script_doc_comments.script_file_id
                                        AND f.project_id = script_doc_comments.project_id
                                        AND f.is_deleted = 0
                            )
                            OR (
                                    symbol_id IS NOT NULL
                                    AND NOT EXISTS (
                                            SELECT 1
                                            FROM script_symbols s
                                            WHERE s.id = script_doc_comments.symbol_id
                                                AND s.project_id = script_doc_comments.project_id
                                    )
                            )
                            OR (
                                    target_symbol_id IS NOT NULL
                                    AND NOT EXISTS (
                                            SELECT 1
                                            FROM script_symbols s
                                            WHERE s.id = script_doc_comments.target_symbol_id
                                                AND s.project_id = script_doc_comments.project_id
                                    )
                            )
                    );
        )sql");
                    delete_stale_doc_comments_after_tombstone.bind_int64(1, project_id);
                    delete_stale_doc_comments_after_tombstone.step_done();
        ++metrics.sqlite_statement_steps;

        Statement delete_stale_scene_resources_after_tombstone = database_->prepare(R"sql(
                DELETE FROM scene_external_resources
                WHERE project_id = ?1
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = scene_external_resources.scene_file_id
                                AND f.project_id = scene_external_resources.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        delete_stale_scene_resources_after_tombstone.bind_int64(1, project_id);
        delete_stale_scene_resources_after_tombstone.step_done();
        ++metrics.sqlite_statement_steps;

        Statement invalidate_deleted_scene_resource_targets = database_->prepare(R"sql(
                UPDATE scene_external_resources
                SET target_file_id = NULL,
                        is_resolved = 0
                WHERE project_id = ?1
                    AND target_file_id IS NOT NULL
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = scene_external_resources.target_file_id
                                AND f.project_id = scene_external_resources.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        invalidate_deleted_scene_resource_targets.bind_int64(1, project_id);
        invalidate_deleted_scene_resource_targets.step_done();
        ++metrics.sqlite_statement_steps;

        Statement delete_stale_scene_attachments_after_tombstone = database_->prepare(R"sql(
                DELETE FROM scene_script_attachments
                WHERE project_id = ?1
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = scene_script_attachments.scene_file_id
                                AND f.project_id = scene_script_attachments.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        delete_stale_scene_attachments_after_tombstone.bind_int64(1, project_id);
        delete_stale_scene_attachments_after_tombstone.step_done();
        ++metrics.sqlite_statement_steps;

        Statement invalidate_deleted_scene_attachment_scripts = database_->prepare(R"sql(
                UPDATE scene_script_attachments
                SET script_file_id = NULL,
                        script_symbol_id = NULL,
                        is_resolved = 0
                WHERE project_id = ?1
                    AND script_file_id IS NOT NULL
                    AND NOT EXISTS (
                            SELECT 1
                            FROM project_files f
                            WHERE f.id = scene_script_attachments.script_file_id
                                AND f.project_id = scene_script_attachments.project_id
                                AND f.is_deleted = 0
                    );
        )sql");
        invalidate_deleted_scene_attachment_scripts.bind_int64(1, project_id);
        invalidate_deleted_scene_attachment_scripts.step_done();
        ++metrics.sqlite_statement_steps;

        Statement invalidate_missing_scene_attachment_symbols = database_->prepare(R"sql(
                UPDATE scene_script_attachments
                SET script_symbol_id = NULL
                WHERE project_id = ?1
                    AND script_symbol_id IS NOT NULL
                    AND NOT EXISTS (
                            SELECT 1
                            FROM script_symbols s
                            WHERE s.id = scene_script_attachments.script_symbol_id
                                AND s.project_id = scene_script_attachments.project_id
                    );
        )sql");
        invalidate_missing_scene_attachment_symbols.bind_int64(1, project_id);
        invalidate_missing_scene_attachment_symbols.step_done();
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

std::vector<ScriptSymbolRow> ScanRepository::list_symbols_for_script(
    int64_t project_id,
    int64_t script_file_id,
    const SymbolQueryFilter &filter
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id,
            project_id,
            script_file_id,
            symbol_slot,
            parent_symbol_slot,
            parent_symbol_id,
            symbol_kind,
            COALESCE(name, ''),
            COALESCE(qualified_name, ''),
            COALESCE(declared_type, ''),
            COALESCE(return_type, ''),
            COALESCE(default_value_excerpt, ''),
            COALESCE(visibility, ''),
            COALESCE(flags, 0),
            COALESCE(doc_comment_state, 'none'),
            symbol_name,
            class_name,
            language,
            signature_text,
            symbol_flags,
            line_start,
            column_start,
            line_end,
            column_end,
            parser_version,
            last_parsed_generation,
            last_seen_scan_run_id,
            created_at_unix,
            updated_at_unix
        FROM script_symbols
        WHERE project_id = ?1
          AND script_file_id = ?2
          AND (?3 = '' OR symbol_kind = ?3)
          AND (
              ?4 = '' OR
              COALESCE(name, symbol_name, '') LIKE ?5 OR
              COALESCE(qualified_name, '') LIKE ?5 OR
              symbol_name LIKE ?5 OR
              class_name LIKE ?5 OR
              signature_text LIKE ?5
          )
        ORDER BY line_start ASC, column_start ASC, symbol_slot ASC, id ASC;
    )sql");
    const std::string search_pattern = "%" + filter.search + "%";
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, script_file_id);
    statement.bind_text(3, filter.symbol_kind);
    statement.bind_text(4, filter.search);
    statement.bind_text(5, search_pattern);
    return collect_script_symbol_rows(statement);
}

std::vector<ScriptSymbolRow> ScanRepository::list_functions_for_script(int64_t project_id, int64_t script_file_id) const {
    SymbolQueryFilter filter;
    filter.symbol_kind = "function";
    return list_symbols_for_script(project_id, script_file_id, filter);
}

std::vector<ScriptSymbolRow> ScanRepository::list_properties_for_script(int64_t project_id, int64_t script_file_id) const {
    SymbolQueryFilter filter;
    filter.symbol_kind = "property";
    return list_symbols_for_script(project_id, script_file_id, filter);
}

std::vector<ScriptSymbolRow> ScanRepository::list_parameters_for_function(
    int64_t project_id,
    int64_t function_symbol_id
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            s.id,
            s.project_id,
            s.script_file_id,
            s.symbol_slot,
            s.parent_symbol_slot,
            s.parent_symbol_id,
            s.symbol_kind,
            COALESCE(s.name, ''),
            COALESCE(s.qualified_name, ''),
            COALESCE(s.declared_type, ''),
            COALESCE(s.return_type, ''),
            COALESCE(s.default_value_excerpt, ''),
            COALESCE(s.visibility, ''),
            COALESCE(s.flags, 0),
            COALESCE(s.doc_comment_state, 'none'),
            s.symbol_name,
            s.class_name,
            s.language,
            s.signature_text,
            s.symbol_flags,
            s.line_start,
            s.column_start,
            s.line_end,
            s.column_end,
            s.parser_version,
            s.last_parsed_generation,
            s.last_seen_scan_run_id,
            s.created_at_unix,
            s.updated_at_unix
        FROM script_symbols s
        JOIN script_symbols fn
          ON fn.id = ?2
         AND fn.project_id = ?1
        WHERE s.project_id = ?1
          AND s.script_file_id = fn.script_file_id
          AND s.symbol_kind = 'parameter'
          AND (
              s.parent_symbol_id = fn.id
              OR (s.parent_symbol_id IS NULL AND s.parent_symbol_slot = fn.symbol_slot)
          )
        ORDER BY s.symbol_slot ASC, s.id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, function_symbol_id);
    return collect_script_symbol_rows(statement);
}

std::vector<ScriptSymbolRow> ScanRepository::list_doc_comment_gaps(
    int64_t project_id,
    const DocCommentGapFilter &filter
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            s.id,
            s.project_id,
            s.script_file_id,
            s.symbol_slot,
            s.parent_symbol_slot,
            s.parent_symbol_id,
            s.symbol_kind,
            COALESCE(s.name, ''),
            COALESCE(s.qualified_name, ''),
            COALESCE(s.declared_type, ''),
            COALESCE(s.return_type, ''),
            COALESCE(s.default_value_excerpt, ''),
            COALESCE(s.visibility, ''),
            COALESCE(s.flags, 0),
            COALESCE(s.doc_comment_state, 'none'),
            s.symbol_name,
            s.class_name,
            s.language,
            s.signature_text,
            s.symbol_flags,
            s.line_start,
            s.column_start,
            s.line_end,
            s.column_end,
            s.parser_version,
            s.last_parsed_generation,
            s.last_seen_scan_run_id,
            s.created_at_unix,
            s.updated_at_unix
        FROM script_symbols s
        JOIN project_files f
          ON f.id = s.script_file_id
         AND f.project_id = s.project_id
         AND f.is_deleted = 0
        WHERE s.project_id = ?1
          AND (?2 = '' OR s.symbol_kind = ?2)
          AND NOT EXISTS (
              SELECT 1
              FROM script_doc_comments d
              WHERE d.project_id = s.project_id
                AND d.script_file_id = s.script_file_id
                AND (d.symbol_id = s.id OR d.target_symbol_id = s.id)
          )
          AND (
              ?3 = '' OR
              COALESCE(s.name, s.symbol_name, '') LIKE ?4 OR
              COALESCE(s.qualified_name, '') LIKE ?4 OR
              s.symbol_name LIKE ?4 OR
              s.class_name LIKE ?4 OR
              f.project_relative_path LIKE ?4
          )
        ORDER BY s.script_file_id ASC, s.line_start ASC, s.column_start ASC, s.id ASC;
    )sql");
    const std::string search_pattern = "%" + filter.search + "%";
    statement.bind_int64(1, project_id);
    statement.bind_text(2, filter.symbol_kind);
    statement.bind_text(3, filter.search);
    statement.bind_text(4, search_pattern);
    return collect_script_symbol_rows(statement);
}

std::vector<ScriptReferenceRow> ScanRepository::list_references_for_script(
    int64_t project_id,
    int64_t script_file_id
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id,
            project_id,
            COALESCE(script_file_id, source_script_file_id),
            source_script_file_id,
            source_symbol_id,
            target_file_id,
            target_symbol_id,
            COALESCE(target_project_relative_path, ''),
            COALESCE(target_class_name, ''),
            COALESCE(target_symbol_name, ''),
            COALESCE(target_resource_uid, ''),
            reference_kind,
            reference_text,
            source_line,
            source_column,
            source_line_end,
            source_column_end,
            CAST(confidence AS INTEGER),
            is_dynamic,
            is_resolved,
            is_unresolved,
            parser_version,
            scan_generation,
            created_at_unix
        FROM script_references
        WHERE project_id = ?1
          AND COALESCE(script_file_id, source_script_file_id) = ?2
        ORDER BY source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, script_file_id);
    return collect_script_reference_rows(statement);
}

std::vector<ScriptReferenceRow> ScanRepository::list_references_from_symbol(
    int64_t project_id,
    int64_t symbol_id
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id,
            project_id,
            COALESCE(script_file_id, source_script_file_id),
            source_script_file_id,
            source_symbol_id,
            target_file_id,
            target_symbol_id,
            COALESCE(target_project_relative_path, ''),
            COALESCE(target_class_name, ''),
            COALESCE(target_symbol_name, ''),
            COALESCE(target_resource_uid, ''),
            reference_kind,
            reference_text,
            source_line,
            source_column,
            source_line_end,
            source_column_end,
            CAST(confidence AS INTEGER),
            is_dynamic,
            is_resolved,
            is_unresolved,
            parser_version,
            scan_generation,
            created_at_unix
        FROM script_references
        WHERE project_id = ?1
          AND source_symbol_id = ?2
        ORDER BY source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, symbol_id);
    return collect_script_reference_rows(statement);
}

std::vector<ScriptReferenceRow> ScanRepository::list_unresolved_references(
    int64_t project_id,
    const ReferenceQueryFilter &filter
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id,
            project_id,
            COALESCE(script_file_id, source_script_file_id),
            source_script_file_id,
            source_symbol_id,
            target_file_id,
            target_symbol_id,
            COALESCE(target_project_relative_path, ''),
            COALESCE(target_class_name, ''),
            COALESCE(target_symbol_name, ''),
            COALESCE(target_resource_uid, ''),
            reference_kind,
            reference_text,
            source_line,
            source_column,
            source_line_end,
            source_column_end,
            CAST(confidence AS INTEGER),
            is_dynamic,
            is_resolved,
            is_unresolved,
            parser_version,
            scan_generation,
            created_at_unix
        FROM script_references
        WHERE project_id = ?1
          AND is_unresolved <> 0
          AND (?2 <= 0 OR COALESCE(script_file_id, source_script_file_id) = ?2)
          AND (?3 = '' OR reference_kind = ?3)
        ORDER BY source_script_file_id ASC, source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, filter.script_file_id.value_or(0));
    statement.bind_text(3, filter.reference_kind);
    return collect_script_reference_rows(statement);
}

std::vector<ScriptReferenceRow> ScanRepository::list_dynamic_references(
    int64_t project_id,
    const ReferenceQueryFilter &filter
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id,
            project_id,
            COALESCE(script_file_id, source_script_file_id),
            source_script_file_id,
            source_symbol_id,
            target_file_id,
            target_symbol_id,
            COALESCE(target_project_relative_path, ''),
            COALESCE(target_class_name, ''),
            COALESCE(target_symbol_name, ''),
            COALESCE(target_resource_uid, ''),
            reference_kind,
            reference_text,
            source_line,
            source_column,
            source_line_end,
            source_column_end,
            CAST(confidence AS INTEGER),
            is_dynamic,
            is_resolved,
            is_unresolved,
            parser_version,
            scan_generation,
            created_at_unix
        FROM script_references
        WHERE project_id = ?1
          AND is_dynamic <> 0
          AND (?2 <= 0 OR COALESCE(script_file_id, source_script_file_id) = ?2)
          AND (?3 = '' OR reference_kind = ?3)
        ORDER BY source_script_file_id ASC, source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, filter.script_file_id.value_or(0));
    statement.bind_text(3, filter.reference_kind);
    return collect_script_reference_rows(statement);
}

std::vector<SceneScriptAttachmentRow> ScanRepository::list_scene_script_attachments(
    int64_t project_id,
    const SceneAttachmentQueryFilter &filter
) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id,
            project_id,
            scene_file_id,
            node_path,
            COALESCE(node_name, ''),
            COALESCE(node_type, ''),
            COALESCE(attachment_kind, 'unknown'),
            COALESCE(ext_resource_id, ext_resource_slot, ''),
            ext_resource_slot,
            COALESCE(script_resource_path, script_project_relative_path, ''),
            COALESCE(script_uid, script_resource_uid, ''),
            script_project_relative_path,
            script_resource_uid,
            script_file_id,
            script_symbol_id,
            is_dynamic,
            is_resolved,
            source_line,
            source_column,
            parser_version,
            scan_generation,
            created_at_unix
        FROM scene_script_attachments
        WHERE project_id = ?1
          AND (?2 <= 0 OR scene_file_id = ?2)
          AND (?3 <= 0 OR script_file_id = ?3)
        ORDER BY scene_file_id ASC, source_line ASC, source_column ASC, id ASC;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, filter.scene_file_id.value_or(0));
    statement.bind_int64(3, filter.script_file_id.value_or(0));
    return collect_scene_attachment_rows(statement);
}

std::vector<SceneScriptAttachmentRow> ScanRepository::list_scenes_using_script(
    int64_t project_id,
    int64_t script_file_id
) const {
    SceneAttachmentQueryFilter filter;
    filter.script_file_id = script_file_id;
    return list_scene_script_attachments(project_id, filter);
}

std::vector<SceneScriptAttachmentRow> ScanRepository::list_scripts_attached_to_scene(
    int64_t project_id,
    int64_t scene_file_id
) const {
    SceneAttachmentQueryFilter filter;
    filter.scene_file_id = scene_file_id;
    return list_scene_script_attachments(project_id, filter);
}

std::optional<ScriptSymbolRow> ScanRepository::get_symbol_details(int64_t project_id, int64_t symbol_id) const {
    Statement statement = database_->prepare(R"sql(
        SELECT
            id,
            project_id,
            script_file_id,
            symbol_slot,
            parent_symbol_slot,
            parent_symbol_id,
            symbol_kind,
            COALESCE(name, ''),
            COALESCE(qualified_name, ''),
            COALESCE(declared_type, ''),
            COALESCE(return_type, ''),
            COALESCE(default_value_excerpt, ''),
            COALESCE(visibility, ''),
            COALESCE(flags, 0),
            COALESCE(doc_comment_state, 'none'),
            symbol_name,
            class_name,
            language,
            signature_text,
            symbol_flags,
            line_start,
            column_start,
            line_end,
            column_end,
            parser_version,
            last_parsed_generation,
            last_seen_scan_run_id,
            created_at_unix,
            updated_at_unix
        FROM script_symbols
        WHERE project_id = ?1
          AND id = ?2
        LIMIT 1;
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, symbol_id);
    if (statement.step() != Statement::StepResult::Row) {
        return std::nullopt;
    }
    return read_script_symbol_row(statement);
}

ScriptIntelligenceSummaryRow ScanRepository::get_script_intelligence_summary(
    int64_t project_id,
    int64_t script_file_id
) const {
    ScriptIntelligenceSummaryRow row;
    row.script_file_id = script_file_id;

    Statement statement = database_->prepare(R"sql(
        SELECT
            ?2,
            (SELECT COUNT(*) FROM script_symbols WHERE project_id = ?1 AND script_file_id = ?2),
            (SELECT COUNT(*) FROM script_symbols WHERE project_id = ?1 AND script_file_id = ?2 AND symbol_kind = 'function'),
            (SELECT COUNT(*) FROM script_symbols WHERE project_id = ?1 AND script_file_id = ?2 AND symbol_kind = 'property'),
            (SELECT COUNT(*) FROM script_symbols WHERE project_id = ?1 AND script_file_id = ?2 AND symbol_kind = 'parameter'),
            (SELECT COUNT(*) FROM script_doc_comments WHERE project_id = ?1 AND script_file_id = ?2),
            (SELECT COUNT(*) FROM script_references WHERE project_id = ?1 AND COALESCE(script_file_id, source_script_file_id) = ?2),
            (SELECT COUNT(*) FROM script_references WHERE project_id = ?1 AND COALESCE(script_file_id, source_script_file_id) = ?2 AND is_unresolved <> 0),
            (SELECT COUNT(*) FROM script_references WHERE project_id = ?1 AND COALESCE(script_file_id, source_script_file_id) = ?2 AND is_dynamic <> 0);
    )sql");
    statement.bind_int64(1, project_id);
    statement.bind_int64(2, script_file_id);
    if (statement.step() != Statement::StepResult::Row) {
        return row;
    }

    row.script_file_id = statement.column_int64(0);
    row.symbol_count = statement.column_int64(1);
    row.function_count = statement.column_int64(2);
    row.property_count = statement.column_int64(3);
    row.parameter_count = statement.column_int64(4);
    row.doc_comment_count = statement.column_int64(5);
    row.reference_count = statement.column_int64(6);
    row.unresolved_reference_count = statement.column_int64(7);
    row.dynamic_reference_count = statement.column_int64(8);
    return row;
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
            dependency_parse_ms, full_symbol_parse_ms, doc_comment_parse_ms, scene_attachment_parse_ms, tokenizer_ms,
            sqlite_write_ms, sqlite_stage_insert_ms, sqlite_file_merge_ms, sqlite_clean_refresh_ms,
            sqlite_parent_resolve_ms, sqlite_parse_status_ms, sqlite_custom_class_ms,
            dependency_sqlite_stage_ms, dependency_resolution_ms,
            sqlite_tombstone_ms, sqlite_deleted_reconcile_ms, sqlite_metrics_write_ms,
            godot_materialization_ms,
            files_seen, dirs_seen, dirs_skipped,
            entries_clean, entries_dirty, entries_new, entries_deleted,
            rows_inserted, rows_updated, rows_clean_refreshed, rows_tombstoned,
            scripts_candidates, scripts_parsed, scripts_skipped_clean, symbols_skipped_clean, scenes_skipped_clean,
            scripts_dependency_parsed, scripts_dependency_skipped_clean,
            script_lines_scanned, parser_lines_scanned,
            bytes_read, parser_bytes_read, parser_tokens_generated, parser_limit_exceeded_count,
            symbol_rows_created, reference_rows_created, doc_comment_rows_created, scene_attachment_rows_created,
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
    metrics.full_symbol_parse_ms = statement.column_int64(10);
    metrics.doc_comment_parse_ms = statement.column_int64(11);
    metrics.scene_attachment_parse_ms = statement.column_int64(12);
    metrics.tokenizer_ms = statement.column_int64(13);
    metrics.sqlite_write_ms = statement.column_int64(14);
    metrics.sqlite_stage_insert_ms = statement.column_int64(15);
    metrics.sqlite_file_merge_ms = statement.column_int64(16);
    metrics.sqlite_clean_refresh_ms = statement.column_int64(17);
    metrics.sqlite_parent_resolve_ms = statement.column_int64(18);
    metrics.sqlite_parse_status_ms = statement.column_int64(19);
    metrics.sqlite_custom_class_ms = statement.column_int64(20);
    metrics.dependency_sqlite_stage_ms = statement.column_int64(21);
    metrics.dependency_resolution_ms = statement.column_int64(22);
    metrics.sqlite_tombstone_ms = statement.column_int64(23);
    metrics.sqlite_deleted_reconcile_ms = statement.column_int64(24);
    metrics.sqlite_metrics_write_ms = statement.column_int64(25);
    metrics.godot_materialization_ms = statement.column_int64(26);
    metrics.files_seen = statement.column_int64(27);
    metrics.dirs_seen = statement.column_int64(28);
    metrics.dirs_skipped = statement.column_int64(29);
    metrics.entries_clean = statement.column_int64(30);
    metrics.entries_dirty = statement.column_int64(31);
    metrics.entries_new = statement.column_int64(32);
    metrics.entries_deleted = statement.column_int64(33);
    metrics.rows_inserted = statement.column_int64(34);
    metrics.rows_updated = statement.column_int64(35);
    metrics.rows_clean_refreshed = statement.column_int64(36);
    metrics.rows_tombstoned = statement.column_int64(37);
    metrics.scripts_candidates = statement.column_int64(38);
    metrics.scripts_parsed = statement.column_int64(39);
    metrics.scripts_skipped_clean = statement.column_int64(40);
    metrics.symbols_skipped_clean = statement.column_int64(41);
    metrics.scenes_skipped_clean = statement.column_int64(42);
    metrics.scripts_dependency_parsed = statement.column_int64(43);
    metrics.scripts_dependency_skipped_clean = statement.column_int64(44);
    metrics.script_lines_scanned = statement.column_int64(45);
    metrics.parser_lines_scanned = statement.column_int64(46);
    metrics.bytes_read = statement.column_int64(47);
    metrics.parser_bytes_read = statement.column_int64(48);
    metrics.parser_tokens_generated = statement.column_int64(49);
    metrics.parser_limit_exceeded_count = statement.column_int64(50);
    metrics.symbol_rows_created = statement.column_int64(51);
    metrics.reference_rows_created = statement.column_int64(52);
    metrics.doc_comment_rows_created = statement.column_int64(53);
    metrics.scene_attachment_rows_created = statement.column_int64(54);
    metrics.dependency_records_created = statement.column_int64(55);
    metrics.unresolved_dependency_count = statement.column_int64(56);
    metrics.dynamic_dependency_count = statement.column_int64(57);
    metrics.entry_record_count = statement.column_int64(58);
    metrics.path_arena_bytes = statement.column_int64(59);
    metrics.existing_snapshot_count = statement.column_int64(60);
    metrics.parsed_script_count = statement.column_int64(61);
    metrics.sqlite_statement_steps = statement.column_int64(62);
    metrics.sqlite_transactions = statement.column_int64(63);
    metrics.ui_rows_materialized = statement.column_int64(64);
    metrics.cancellation_requested = statement.column_int64(65) != 0;
    metrics.scan_result_status = statement.column_text(66);
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

            if (record.entry_kind == EntryKind::File &&
                (record.extension_id == ExtensionId::TSCN || record.extension_id == ExtensionId::SCN) &&
                snapshot.has_value() &&
                snapshot->scene_parser_version != SCENE_PARSER_VERSION) {
                record.dirty_state = DirtyState::Dirty;
                record.dirty_reason = DirtyReason::SceneParserVersionChanged;
            }

            if (record.dirty_state == DirtyState::Clean) {
                ++metrics.entries_clean;
            } else {
                ++metrics.entries_dirty;
                if (record.dirty_reason == DirtyReason::NewPath) {
                    ++metrics.entries_new;
                }
            }
        }
        metrics.dirty_check_ms = elapsed_ms(dirty_start, std::chrono::steady_clock::now());

        throw_if_cancel_requested(options.cancel_requested, metrics);

        const auto candidate_start = std::chrono::steady_clock::now();
        std::vector<size_t> script_candidates;
        std::vector<size_t> scene_candidates;
        script_candidates.reserve(256);
        scene_candidates.reserve(128);
        const bool parse_script_metadata = options.collect_custom_classes || options.collect_script_dependencies;
        const bool parse_scene_metadata = options.persist_to_database;

        if (parse_script_metadata || parse_scene_metadata) {
            for (size_t i = 0; i < result.records.size(); ++i) {
                if ((i % 1024) == 0) {
                    throw_if_cancel_requested(options.cancel_requested, metrics);
                }

                const EntryRecord &record = result.records[i];
                if (record.entry_kind != EntryKind::File) {
                    continue;
                }

                const std::string_view extension = result.arena.view(record.extension_offset, record.extension_length);

                const bool is_script = is_script_extension(extension);
                const bool is_scene = record.extension_id == ExtensionId::TSCN || record.extension_id == ExtensionId::SCN;

                if (parse_script_metadata && is_script) {
                    ++metrics.scripts_candidates;
                    if (record.dirty_state == DirtyState::Clean) {
                        ++metrics.scripts_skipped_clean;
                        ++metrics.symbols_skipped_clean;
                        if (options.collect_script_dependencies) {
                            ++metrics.scripts_dependency_skipped_clean;
                        }
                    } else {
                        script_candidates.push_back(i);
                    }
                }

                if (parse_scene_metadata && is_scene) {
                    if (record.dirty_state == DirtyState::Clean) {
                        ++metrics.scenes_skipped_clean;
                    } else {
                        scene_candidates.push_back(i);
                    }
                }
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
                parsed.parse_result = parse_script_intelligence(
                    options.project_root / std::filesystem::u8path(project_path),
                    extension_view,
                    ParseTier::FullSymbols
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
                metrics.full_symbol_parse_ms += parsed.parse_result.full_symbol_parse_ms;
                metrics.doc_comment_parse_ms += parsed.parse_result.doc_comment_parse_ms;
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

        if (parse_scene_metadata) {
            for (size_t record_index : scene_candidates) {
                throw_if_cancel_requested(options.cancel_requested, metrics);

                const EntryRecord &record = result.records[record_index];
                const std::string_view project_path_view =
                    result.arena.view(record.path_offset, record.path_length);

                ParsedSceneRecord parsed_scene;
                parsed_scene.record_index = record_index;
                parsed_scene.project_relative_path = std::string(project_path_view);
                parsed_scene.parse_result = parse_scene_attachments(
                    options.project_root / std::filesystem::u8path(parsed_scene.project_relative_path)
                );

                metrics.bytes_read += parsed_scene.parse_result.bytes_read;
                metrics.scene_attachment_parse_ms += parsed_scene.parse_result.parse_ms;
                if (parsed_scene.parse_result.limit_exceeded) {
                    ++metrics.parser_limit_exceeded_count;
                }

                result.parsed_scenes.push_back(std::move(parsed_scene));
            }
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
                result.parsed_scenes,
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
