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

std::string absolute_path_for(const std::filesystem::path &root, const std::string &project_relative_path) {
    return (root / std::filesystem::u8path(project_relative_path)).lexically_normal().generic_string();
}

std::string bindable_extension(const std::string &project_relative_path, EntryKind kind) {
    if (kind == EntryKind::Directory) {
        return "";
    }
    return extension_from_path(project_relative_path);
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
    int64_t finished_at_unix
) {
    Statement update_run = database_->prepare(
        "UPDATE project_scan_runs "
        "SET finished_at_unix = ?1, status = ?2, files_found = ?3, folders_found = ?4, error_message = NULL "
        "WHERE project_id = ?5 AND id = ?6;"
    );
    update_run.bind_int64(1, finished_at_unix);
    update_run.bind_text(2, metrics.scan_result_status);
    update_run.bind_int64(3, metrics.files_seen);
    update_run.bind_int64(4, metrics.dirs_seen);
    update_run.bind_int64(5, project_id);
    update_run.bind_int64(6, scan_run_id);
    update_run.step_done();

    Statement insert_metrics = database_->prepare(R"sql(
        INSERT INTO scan_metrics (
            project_id, scan_run_id, scan_generation, total_wall_ms, traversal_ms, metadata_ms,
            dirty_check_ms, classification_ms, script_parse_ms, sqlite_write_ms, godot_materialization_ms,
            files_seen, dirs_seen, dirs_skipped, entries_clean, entries_dirty, entries_new, entries_deleted,
            rows_inserted, rows_updated, rows_tombstoned, scripts_candidates, scripts_parsed,
            scripts_skipped_clean, bytes_read, sqlite_transactions, ui_rows_materialized,
            cancellation_requested, scan_result_status, created_at_unix
        ) VALUES (
            ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15,
            ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25, ?26, ?27, ?28, ?29, ?30
        );
    )sql");
    insert_metrics.bind_int64(1, project_id);
    insert_metrics.bind_int64(2, scan_run_id);
    insert_metrics.bind_int64(3, generation);
    insert_metrics.bind_int64(4, metrics.total_wall_ms);
    insert_metrics.bind_int64(5, metrics.traversal_ms);
    insert_metrics.bind_int64(6, metrics.metadata_ms);
    insert_metrics.bind_int64(7, metrics.dirty_check_ms);
    insert_metrics.bind_int64(8, metrics.classification_ms);
    insert_metrics.bind_int64(9, metrics.script_parse_ms);
    insert_metrics.bind_int64(10, metrics.sqlite_write_ms);
    insert_metrics.bind_int64(11, metrics.godot_materialization_ms);
    insert_metrics.bind_int64(12, metrics.files_seen);
    insert_metrics.bind_int64(13, metrics.dirs_seen);
    insert_metrics.bind_int64(14, metrics.dirs_skipped);
    insert_metrics.bind_int64(15, metrics.entries_clean);
    insert_metrics.bind_int64(16, metrics.entries_dirty);
    insert_metrics.bind_int64(17, metrics.entries_new);
    insert_metrics.bind_int64(18, metrics.entries_deleted);
    insert_metrics.bind_int64(19, metrics.rows_inserted);
    insert_metrics.bind_int64(20, metrics.rows_updated);
    insert_metrics.bind_int64(21, metrics.rows_tombstoned);
    insert_metrics.bind_int64(22, metrics.scripts_candidates);
    insert_metrics.bind_int64(23, metrics.scripts_parsed);
    insert_metrics.bind_int64(24, metrics.scripts_skipped_clean);
    insert_metrics.bind_int64(25, metrics.bytes_read);
    insert_metrics.bind_int64(26, metrics.sqlite_transactions);
    insert_metrics.bind_int64(27, metrics.ui_rows_materialized);
    insert_metrics.bind_int64(28, metrics.cancellation_requested ? 1 : 0);
    insert_metrics.bind_text(29, metrics.scan_result_status);
    insert_metrics.bind_int64(30, finished_at_unix);
    insert_metrics.step_done();
}

std::unordered_map<std::string, ExistingEntrySnapshot> ScanRepository::load_existing_entries(int64_t project_id) {
    std::unordered_map<std::string, ExistingEntrySnapshot> existing;
    Statement statement = database_->prepare(R"sql(
        SELECT id, project_relative_path, entry_kind, is_directory, size_bytes, modified_time_ns,
               platform_file_id, parser_version, classifier_version, parse_status
        FROM project_files
        WHERE project_id = ?1 AND is_deleted = 0;
    )sql");
    statement.bind_int64(1, project_id);

    while (statement.step() == Statement::StepResult::Row) {
        ExistingEntrySnapshot snapshot;
        snapshot.id = statement.column_int64(0);
        const std::string path = statement.column_text(1);
        snapshot.entry_kind = entry_kind_from_string(statement.column_text(2), statement.column_int64(3));
        snapshot.size_bytes = statement.column_int64(4);
        snapshot.modified_time_ns = statement.column_int64(5);
        snapshot.platform_file_id = statement.column_text(6);
        snapshot.parser_version = statement.column_int64(7);
        snapshot.classifier_version = statement.column_int64(8);
        snapshot.parse_status = parse_status_from_string(statement.column_text(9));
        existing[path] = snapshot;
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
    ScanMetrics &metrics
) {
    const auto write_start = std::chrono::steady_clock::now();
    const int64_t observed_at_unix = current_unix_time();

    Transaction transaction(*database_);
    ++metrics.sqlite_transactions;

    Statement upsert_file = database_->prepare(R"sql(
        INSERT INTO project_files (
            project_id, project_relative_path, absolute_path, file_name, extension, file_type, godot_type,
            size_bytes, modified_time_unix, is_directory, is_hidden, first_seen_scan_run_id,
            last_seen_scan_run_id, created_at_unix, updated_at_unix, parent_id, entry_kind,
            godot_type_hint, type_hint_source, type_hint_confidence, modified_time_ns, platform_file_id,
            scan_generation, last_seen_generation, dirty_state, dirty_reason, parser_version,
            classifier_version, parse_status, is_deleted, deleted_at_unix
        ) VALUES (
            ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?12, ?13, ?13, ?14,
            ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?21, ?22, ?23, ?24, ?25, 'not_parsed', 0, NULL
        )
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
            parent_id = excluded.parent_id,
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
            classifier_version = excluded.classifier_version,
            is_deleted = 0,
            deleted_at_unix = NULL,
            first_seen_scan_run_id = COALESCE(project_files.first_seen_scan_run_id, excluded.first_seen_scan_run_id)
        RETURNING id;
    )sql");

    for (size_t i = 0; i < records.size(); ++i) {
        EntryRecord &record = records[i];
        const std::string project_path = arena.string_at(record.path_offset, record.path_length);
        const std::string name = arena.string_at(record.name_offset, record.name_length);
        const std::string extension = bindable_extension(project_path, record.entry_kind);
        const int64_t parent_id =
            record.parent_record_index >= 0
                ? records[static_cast<size_t>(record.parent_record_index)].database_id
                : 0;
        record.parent_db_id = parent_id;

        upsert_file.clear_bindings();
        upsert_file.reset();
        upsert_file.bind_int64(1, project_id);
        upsert_file.bind_text(2, project_path);
        upsert_file.bind_text(3, absolute_path_for(project_root, project_path));
        upsert_file.bind_text(4, name);
        upsert_file.bind_text(5, extension);
        upsert_file.bind_text(6, to_string(record.file_type_id));
        upsert_file.bind_text(7, to_string(record.godot_type_hint));
        upsert_file.bind_int64(8, record.size_bytes);
        upsert_file.bind_int64(9, record.modified_time_ns / 1'000'000'000LL);
        upsert_file.bind_int64(10, record.entry_kind == EntryKind::Directory ? 1 : 0);
        upsert_file.bind_int64(11, record.is_hidden() ? 1 : 0);
        upsert_file.bind_int64(12, scan_run_id);
        upsert_file.bind_int64(13, observed_at_unix);
        upsert_file.bind_int64(14, parent_id);
        upsert_file.bind_text(15, to_string(record.entry_kind));
        upsert_file.bind_text(16, to_string(record.godot_type_hint));
        upsert_file.bind_text(17, to_string(record.type_hint_source));
        upsert_file.bind_int64(18, record.godot_type_hint == GodotTypeHint::NotGodotTyped ? 0 : 50);
        upsert_file.bind_int64(19, record.modified_time_ns);
        upsert_file.bind_text(20, platform_file_id_to_string(record));
        upsert_file.bind_int64(21, generation);
        upsert_file.bind_text(22, to_string(record.dirty_state));
        upsert_file.bind_text(23, to_string(record.dirty_reason));
        upsert_file.bind_int64(24, PARSER_VERSION);
        upsert_file.bind_int64(25, CLASSIFIER_VERSION);

        if (upsert_file.step() == Statement::StepResult::Row) {
            record.database_id = upsert_file.column_int64(0);
        }
        upsert_file.step_done();
    }

    for (const ParsedScriptRecord &parsed : parsed_scripts) {
        if (parsed.record_index >= records.size()) {
            continue;
        }

        const EntryRecord &record = records[parsed.record_index];
        Statement update_parse_status = database_->prepare(
            "UPDATE project_files SET parse_status = ?1, parser_version = ?2, updated_at_unix = ?3 "
            "WHERE project_id = ?4 AND id = ?5;"
        );
        update_parse_status.bind_text(1, to_string(parsed.parse_result.status));
        update_parse_status.bind_int64(2, PARSER_VERSION);
        update_parse_status.bind_int64(3, observed_at_unix);
        update_parse_status.bind_int64(4, project_id);
        update_parse_status.bind_int64(5, record.database_id);
        update_parse_status.step_done();

        if (parsed.parse_result.status != ParseStatus::ParsedClass || parsed.parse_result.class_name.empty()) {
            Statement delete_old_class = database_->prepare(
                "DELETE FROM project_custom_classes WHERE project_id = ?1 AND script_project_relative_path = ?2;"
            );
            delete_old_class.bind_int64(1, project_id);
            delete_old_class.bind_text(2, parsed.project_relative_path);
            delete_old_class.step_done();
            continue;
        }

        const std::string script_path = "res://" + parsed.project_relative_path;
        const std::string direct_base = parsed.parse_result.direct_base_type;
        Statement upsert_class = database_->prepare(R"sql(
            INSERT INTO project_custom_classes (
                project_id, class_name, script_path, script_project_relative_path, language, base_type,
                direct_base_type, is_resource_type, is_node_type, parser_version, parse_status, parse_error,
                last_parsed_generation, script_file_id, last_seen_scan_run_id, created_at_unix, updated_at_unix
            ) VALUES (
                ?1, ?2, ?3, ?4, ?5, ?6, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?15
            )
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
        upsert_class.bind_text(2, parsed.parse_result.class_name);
        upsert_class.bind_text(3, script_path);
        upsert_class.bind_text(4, parsed.project_relative_path);
        upsert_class.bind_text(5, to_string(parsed.parse_result.language));
        upsert_class.bind_text(6, direct_base);
        upsert_class.bind_int64(7, is_builtin_resource_type_hint(direct_base) ? 1 : 0);
        upsert_class.bind_int64(8, is_builtin_node_type_hint(direct_base) ? 1 : 0);
        upsert_class.bind_int64(9, PARSER_VERSION);
        upsert_class.bind_text(10, to_string(parsed.parse_result.status));
        upsert_class.bind_text(11, parsed.parse_result.parse_error);
        upsert_class.bind_int64(12, generation);
        upsert_class.bind_int64(13, record.database_id);
        upsert_class.bind_int64(14, scan_run_id);
        upsert_class.bind_int64(15, observed_at_unix);
        upsert_class.step_done();
    }

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
    metrics.rows_tombstoned = database_->changes();
    metrics.entries_deleted = metrics.rows_tombstoned;

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

    metrics.rows_inserted = metrics.entries_new;
    metrics.rows_updated = static_cast<int64_t>(records.size()) - metrics.entries_new;
    metrics.sqlite_write_ms = elapsed_ms(write_start, std::chrono::steady_clock::now());
    complete_scan_run(project_id, scan_run_id, generation, metrics, observed_at_unix);

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
    FileQuery query;
    query.include_deleted = true;
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

ScanMetrics ScanRepository::get_scan_metrics(int64_t project_id, int64_t scan_run_id) const {
    ScanMetrics metrics;
    Statement statement = database_->prepare(R"sql(
        SELECT total_wall_ms, traversal_ms, metadata_ms, dirty_check_ms, classification_ms,
               script_parse_ms, sqlite_write_ms, godot_materialization_ms, files_seen, dirs_seen,
               dirs_skipped, entries_clean, entries_dirty, entries_new, entries_deleted, rows_inserted,
               rows_updated, rows_tombstoned, scripts_candidates, scripts_parsed, scripts_skipped_clean,
               bytes_read, sqlite_transactions, ui_rows_materialized, cancellation_requested, scan_result_status
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
    metrics.dirty_check_ms = statement.column_int64(3);
    metrics.classification_ms = statement.column_int64(4);
    metrics.script_parse_ms = statement.column_int64(5);
    metrics.sqlite_write_ms = statement.column_int64(6);
    metrics.godot_materialization_ms = statement.column_int64(7);
    metrics.files_seen = statement.column_int64(8);
    metrics.dirs_seen = statement.column_int64(9);
    metrics.dirs_skipped = statement.column_int64(10);
    metrics.entries_clean = statement.column_int64(11);
    metrics.entries_dirty = statement.column_int64(12);
    metrics.entries_new = statement.column_int64(13);
    metrics.entries_deleted = statement.column_int64(14);
    metrics.rows_inserted = statement.column_int64(15);
    metrics.rows_updated = statement.column_int64(16);
    metrics.rows_tombstoned = statement.column_int64(17);
    metrics.scripts_candidates = statement.column_int64(18);
    metrics.scripts_parsed = statement.column_int64(19);
    metrics.scripts_skipped_clean = statement.column_int64(20);
    metrics.bytes_read = statement.column_int64(21);
    metrics.sqlite_transactions = statement.column_int64(22);
    metrics.ui_rows_materialized = statement.column_int64(23);
    metrics.cancellation_requested = statement.column_int64(24) != 0;
    metrics.scan_result_status = statement.column_text(25);
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

ScanResultSummary NativeScanPipeline::run(const ScanOptions &options) {
    if (database_ == nullptr) {
        throw std::runtime_error("NativeScanPipeline requires a database.");
    }
    if (options.project_id <= 0) {
        throw std::runtime_error("NativeScanPipeline requires a valid project_id.");
    }
    if (options.project_root.empty()) {
        throw std::runtime_error("NativeScanPipeline requires a project root.");
    }

    const auto scan_start = std::chrono::steady_clock::now();
    ScanMetrics metrics;
    ScanRepository repository(*database_);
    const ScanGeneration generation = repository.next_generation(options.project_id);
    const int64_t scan_run_id = repository.create_scan_run(options.project_id, generation, current_unix_time());

    PathArena arena;
    std::vector<EntryRecord> records;
    records.reserve(4096);

    SkipPolicy skip_policy;
    NativeDirectoryEnumerator enumerator;
    EnumerationOptions enumeration_options;
    enumeration_options.root = options.project_root;
    enumeration_options.include_hidden = options.include_hidden;
    enumeration_options.skip_policy = &skip_policy;

    const auto traversal_start = std::chrono::steady_clock::now();
    const EnumerationResult enumeration_result = enumerator.enumerate(enumeration_options, arena, records);
    const auto traversal_end = std::chrono::steady_clock::now();
    metrics.traversal_ms = elapsed_ms(traversal_start, traversal_end);
    metrics.files_seen = enumeration_result.files_seen;
    metrics.dirs_seen = enumeration_result.dirs_seen;
    metrics.dirs_skipped = enumeration_result.dirs_skipped;
    metrics.cancellation_requested = !enumeration_result.completed;
    metrics.scan_result_status = enumeration_result.completed ? "completed" : "cancelled";

    const auto dirty_start = std::chrono::steady_clock::now();
    const std::unordered_map<std::string, ExistingEntrySnapshot> existing =
        repository.load_existing_entries(options.project_id);

    std::vector<ParsedScriptRecord> parsed_scripts;
    parsed_scripts.reserve(256);

    for (size_t i = 0; i < records.size(); ++i) {
        EntryRecord &record = records[i];
        const std::string project_path = arena.string_at(record.path_offset, record.path_length);
        const std::string platform_id = platform_file_id_to_string(record);

        std::optional<ExistingEntrySnapshot> snapshot;
        const auto found = existing.find(project_path);
        if (found != existing.end()) {
            snapshot = found->second;
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

        if (record.entry_kind == EntryKind::File) {
            const std::string extension = extension_from_path(project_path);
            if (is_script_extension(extension)) {
                ++metrics.scripts_candidates;
                if (dirty.state == DirtyState::Clean) {
                    ++metrics.scripts_skipped_clean;
                } else {
                    const auto parse_start = std::chrono::steady_clock::now();
                    ParsedScriptRecord parsed;
                    parsed.record_index = i;
                    parsed.project_relative_path = project_path;
                    parsed.extension = extension;
                    parsed.parse_result = parse_script_header(options.project_root / std::filesystem::u8path(project_path), extension);
                    metrics.script_parse_ms += elapsed_ms(parse_start, std::chrono::steady_clock::now());
                    metrics.bytes_read += parsed.parse_result.bytes_read;
                    ++metrics.scripts_parsed;
                    parsed_scripts.push_back(std::move(parsed));
                }
            }
        }
    }
    metrics.dirty_check_ms = elapsed_ms(dirty_start, std::chrono::steady_clock::now());

    repository.write_scan_results(
        options.project_id,
        scan_run_id,
        generation,
        options.project_root,
        arena,
        records,
        parsed_scripts,
        metrics
    );

    metrics.total_wall_ms = elapsed_ms(scan_start, std::chrono::steady_clock::now());

    // Persist the final wall time after the transactional write has inserted the metrics row.
    Statement update_metrics = database_->prepare(
        "UPDATE scan_metrics SET total_wall_ms = ?1 WHERE project_id = ?2 AND scan_run_id = ?3;"
    );
    update_metrics.bind_int64(1, metrics.total_wall_ms);
    update_metrics.bind_int64(2, options.project_id);
    update_metrics.bind_int64(3, scan_run_id);
    update_metrics.step_done();

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

} // namespace gotool::project_scanner
