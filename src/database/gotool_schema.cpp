#include "gotool_schema.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace gotool::database {

namespace {

int64_t read_user_version(Database &database) {
  Statement statement = database.prepare("PRAGMA user_version;");

  if (statement.step() != Statement::StepResult::Row) {
    throw std::runtime_error("Failed to read SQLite PRAGMA user_version.");
  }

  return statement.column_int64(0);
}

void write_user_version(Database &database, int64_t version) {
  database.exec("PRAGMA user_version = " + std::to_string(version) + ";");
}

bool table_exists(Database &database, const std::string &table_name) {
  Statement statement =
      database.prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND "
                       "name = ?1 LIMIT 1;");
  statement.bind_text(1, table_name);
  return statement.step() == Statement::StepResult::Row;
}

bool table_has_column(Database &database, const std::string &table_name,
                      const std::string &column_name) {
  Statement statement =
      database.prepare("PRAGMA table_info(" + table_name + ");");

  while (statement.step() == Statement::StepResult::Row) {
    if (statement.column_text(1) == column_name) {
      return true;
    }
  }

  return false;
}

void add_column_if_missing(Database &database, const std::string &table_name,
                           const std::string &column_sql) {
  const size_t first_space = column_sql.find(' ');
  if (first_space == std::string::npos) {
    throw std::runtime_error("Column SQL must start with a column name.");
  }

  const std::string column_name = column_sql.substr(0, first_space);
  if (!table_has_column(database, table_name, column_name)) {
    database.exec("ALTER TABLE " + table_name + " ADD COLUMN " + column_sql +
                  ";");
  }
}

void create_projects_table(Database &database) {
  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS projects (
            id INTEGER PRIMARY KEY,
            project_uid TEXT NOT NULL UNIQUE,
            display_name TEXT NOT NULL,
            root_absolute_path TEXT NOT NULL,
            root_canonical_path TEXT NOT NULL UNIQUE,
            project_file_absolute_path TEXT NOT NULL,
            godot_version TEXT,
            identity_source TEXT NOT NULL DEFAULT 'uid_file',
            identity_warning TEXT NOT NULL DEFAULT '',
            first_seen_unix INTEGER NOT NULL DEFAULT 0,
            last_seen_unix INTEGER NOT NULL DEFAULT 0,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0
        );
    )sql");

  if (!table_has_column(database, "projects", "identity_source")) {
    database.exec("ALTER TABLE projects ADD COLUMN identity_source TEXT NOT "
                  "NULL DEFAULT 'uid_file';");
  }

  if (!table_has_column(database, "projects", "identity_warning")) {
    database.exec("ALTER TABLE projects ADD COLUMN identity_warning TEXT NOT "
                  "NULL DEFAULT '';");
  }
}

void create_schema_v2_tables(Database &database) {
  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS project_scan_runs (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            started_at_unix INTEGER NOT NULL DEFAULT 0,
            finished_at_unix INTEGER,
            status TEXT NOT NULL CHECK (
                status IN ('running', 'completed', 'failed', 'cancelled')
            ),
            files_found INTEGER NOT NULL DEFAULT 0,
            folders_found INTEGER NOT NULL DEFAULT 0,
            error_message TEXT,
            scan_generation INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS project_files (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            project_relative_path TEXT NOT NULL,
            absolute_path TEXT NOT NULL,
            file_name TEXT NOT NULL,
            extension TEXT NOT NULL DEFAULT '',
            file_type TEXT NOT NULL,
            godot_type TEXT NOT NULL DEFAULT 'NGT' CHECK (godot_type <> ''),
            size_bytes INTEGER NOT NULL DEFAULT 0,
            content_hash TEXT,
            modified_time_unix INTEGER,
            is_directory INTEGER NOT NULL DEFAULT 0,
            is_hidden INTEGER NOT NULL DEFAULT 0,
            parent_id INTEGER NOT NULL DEFAULT 0,
            entry_kind TEXT NOT NULL DEFAULT 'file',
            godot_type_hint TEXT NOT NULL DEFAULT 'NGT',
            type_hint_source TEXT NOT NULL DEFAULT 'none',
            type_hint_confidence INTEGER NOT NULL DEFAULT 0,
            modified_time_ns INTEGER NOT NULL DEFAULT 0,
            platform_file_id TEXT NOT NULL DEFAULT '',
            scan_generation INTEGER NOT NULL DEFAULT 0,
            last_seen_generation INTEGER NOT NULL DEFAULT 0,
            dirty_state TEXT NOT NULL DEFAULT 'dirty',
            dirty_reason TEXT NOT NULL DEFAULT 'new_path',
            parser_version INTEGER NOT NULL DEFAULT 1,
            dependency_parser_version INTEGER NOT NULL DEFAULT 1,
            scene_parser_version INTEGER NOT NULL DEFAULT 1,
            classifier_version INTEGER NOT NULL DEFAULT 1,
            parse_status TEXT NOT NULL DEFAULT 'not_parsed',
            is_deleted INTEGER NOT NULL DEFAULT 0,
            deleted_at_unix INTEGER,
            first_seen_scan_run_id INTEGER,
            last_seen_scan_run_id INTEGER,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,
            UNIQUE(project_id, project_relative_path),
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (first_seen_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE SET NULL,
            FOREIGN KEY (last_seen_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS project_autoloads (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            autoload_name TEXT NOT NULL,
            target_path TEXT NOT NULL,
            target_project_relative_path TEXT NOT NULL,
            is_singleton INTEGER NOT NULL DEFAULT 1,
            target_file_id INTEGER,
            last_seen_scan_run_id INTEGER,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,
            UNIQUE(project_id, autoload_name),
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (target_file_id)
                REFERENCES project_files(id)
                ON DELETE SET NULL,
            FOREIGN KEY (last_seen_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS project_custom_classes (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            class_name TEXT NOT NULL,
            script_path TEXT NOT NULL,
            script_project_relative_path TEXT NOT NULL,
            language TEXT NOT NULL,
            base_type TEXT NOT NULL DEFAULT '',
            direct_base_type TEXT NOT NULL DEFAULT '',
            is_resource_type INTEGER NOT NULL DEFAULT 0,
            is_node_type INTEGER NOT NULL DEFAULT 0,
            parser_version INTEGER NOT NULL DEFAULT 1,
            parse_status TEXT NOT NULL DEFAULT 'not_parsed',
            parse_error TEXT NOT NULL DEFAULT '',
            last_parsed_generation INTEGER NOT NULL DEFAULT 0,
            script_file_id INTEGER,
            last_seen_scan_run_id INTEGER,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,
            UNIQUE(project_id, class_name),
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (script_file_id)
                REFERENCES project_files(id)
                ON DELETE SET NULL,
            FOREIGN KEY (last_seen_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS script_symbols (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            script_file_id INTEGER NOT NULL,
            symbol_slot INTEGER NOT NULL DEFAULT 0,
            parent_symbol_slot INTEGER,
            parent_symbol_id INTEGER,
            symbol_kind TEXT NOT NULL DEFAULT 'unknown',
            name TEXT NOT NULL DEFAULT '',
            qualified_name TEXT NOT NULL DEFAULT '',
            declared_type TEXT NOT NULL DEFAULT '',
            return_type TEXT NOT NULL DEFAULT '',
            default_value_excerpt TEXT NOT NULL DEFAULT '',
            visibility TEXT NOT NULL DEFAULT '',
            flags INTEGER NOT NULL DEFAULT 0,
            doc_comment_state TEXT NOT NULL DEFAULT 'none',
            symbol_name TEXT NOT NULL DEFAULT '',
            class_name TEXT NOT NULL DEFAULT '',
            language TEXT NOT NULL DEFAULT '',
            signature_text TEXT NOT NULL DEFAULT '',
            symbol_flags INTEGER NOT NULL DEFAULT 0,
            line_start INTEGER NOT NULL DEFAULT 0,
            column_start INTEGER NOT NULL DEFAULT 0,
            line_end INTEGER NOT NULL DEFAULT 0,
            column_end INTEGER NOT NULL DEFAULT 0,
            parser_version INTEGER NOT NULL DEFAULT 1,
            last_parsed_generation INTEGER NOT NULL DEFAULT 0,
            last_seen_scan_run_id INTEGER,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,
            UNIQUE(project_id, script_file_id, symbol_slot),
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (script_file_id)
                REFERENCES project_files(id)
                ON DELETE CASCADE,
            FOREIGN KEY (parent_symbol_id)
                REFERENCES script_symbols(id)
                ON DELETE SET NULL,
            FOREIGN KEY (last_seen_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS script_dependencies (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            source_script_file_id INTEGER NOT NULL,
            source_symbol_id INTEGER,
            target_file_id INTEGER,
            target_project_relative_path TEXT,
            target_class_name TEXT,
            target_resource_uid TEXT,
            dependency_kind TEXT NOT NULL CHECK (
                dependency_kind IN (
                    'preload_path',
                    'load_path',
                    'resource_loader_load_path',
                    'gd_load_path',
                    'extends_path',
                    'extends_class',
                    'class_name_declaration',
                    'const_preload_alias',
                    'typed_var_ref',
                    'typed_param_ref',
                    'typed_return_ref',
                    'typed_array_element_ref',
                    'typed_dictionary_ref',
                    'export_type_ref',
                    'signal_type_ref',
                    'new_class_instantiation',
                    'scene_node_path',
                    'resource_uid_ref',
                    'dynamic_load',
                    'unresolved_symbol',
                    'unknown'
                )
            ),
            reference_text TEXT NOT NULL DEFAULT '',
            source_line INTEGER NOT NULL DEFAULT 0,
            source_column INTEGER NOT NULL DEFAULT 0,
            confidence REAL NOT NULL DEFAULT 0,
            is_dynamic INTEGER NOT NULL DEFAULT 0,
            is_resolved INTEGER NOT NULL DEFAULT 0,
            parser_version INTEGER NOT NULL DEFAULT 1,
            scan_generation INTEGER NOT NULL DEFAULT 0,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (source_script_file_id)
                REFERENCES project_files(id)
                ON DELETE CASCADE,
            FOREIGN KEY (source_symbol_id)
                REFERENCES script_symbols(id)
                ON DELETE SET NULL,
            FOREIGN KEY (target_file_id)
                REFERENCES project_files(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS script_references (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            script_file_id INTEGER NOT NULL,
            source_script_file_id INTEGER NOT NULL,
            source_symbol_id INTEGER,
            target_file_id INTEGER,
            target_symbol_id INTEGER,
            target_project_relative_path TEXT,
            target_class_name TEXT,
            target_symbol_name TEXT,
            target_resource_uid TEXT,
            reference_kind TEXT NOT NULL DEFAULT 'unknown',
            reference_text TEXT NOT NULL DEFAULT '',
            source_line INTEGER NOT NULL DEFAULT 0,
            source_column INTEGER NOT NULL DEFAULT 0,
            source_line_end INTEGER NOT NULL DEFAULT 0,
            source_column_end INTEGER NOT NULL DEFAULT 0,
            confidence REAL NOT NULL DEFAULT 0,
            is_dynamic INTEGER NOT NULL DEFAULT 0,
            is_resolved INTEGER NOT NULL DEFAULT 0,
            is_unresolved INTEGER NOT NULL DEFAULT 0,
            parser_version INTEGER NOT NULL DEFAULT 1,
            scan_generation INTEGER NOT NULL DEFAULT 0,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (script_file_id)
                REFERENCES project_files(id)
                ON DELETE CASCADE,
            FOREIGN KEY (source_script_file_id)
                REFERENCES project_files(id)
                ON DELETE CASCADE,
            FOREIGN KEY (source_symbol_id)
                REFERENCES script_symbols(id)
                ON DELETE SET NULL,
            FOREIGN KEY (target_file_id)
                REFERENCES project_files(id)
                ON DELETE SET NULL,
            FOREIGN KEY (target_symbol_id)
                REFERENCES script_symbols(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS script_doc_comments (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            script_file_id INTEGER NOT NULL,
            target_symbol_id INTEGER,
            target_kind TEXT NOT NULL DEFAULT '',
            symbol_id INTEGER,
            comment_style TEXT NOT NULL DEFAULT '',
            text_hash TEXT NOT NULL DEFAULT '',
            text_excerpt TEXT NOT NULL DEFAULT '',
            comment_text TEXT NOT NULL DEFAULT '',
            summary_text TEXT NOT NULL DEFAULT '',
            start_line INTEGER NOT NULL DEFAULT 0,
            end_line INTEGER NOT NULL DEFAULT 0,
            is_attached INTEGER NOT NULL DEFAULT 0,
            line_start INTEGER NOT NULL DEFAULT 0,
            column_start INTEGER NOT NULL DEFAULT 0,
            line_end INTEGER NOT NULL DEFAULT 0,
            column_end INTEGER NOT NULL DEFAULT 0,
            parser_version INTEGER NOT NULL DEFAULT 1,
            scan_generation INTEGER NOT NULL DEFAULT 0,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (script_file_id)
                REFERENCES project_files(id)
                ON DELETE CASCADE,
            FOREIGN KEY (target_symbol_id)
                REFERENCES script_symbols(id)
                ON DELETE SET NULL,
            FOREIGN KEY (symbol_id)
                REFERENCES script_symbols(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS scene_external_resources (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            scene_file_id INTEGER NOT NULL,
            ext_resource_id TEXT NOT NULL DEFAULT '',
            resource_slot TEXT NOT NULL DEFAULT '',
            resource_type TEXT NOT NULL DEFAULT '',
            resource_path TEXT NOT NULL DEFAULT '',
            resource_uid TEXT NOT NULL DEFAULT '',
            target_file_id INTEGER,
            is_script_resource INTEGER NOT NULL DEFAULT 0,
            is_resolved INTEGER NOT NULL DEFAULT 0,
            source_line INTEGER NOT NULL DEFAULT 0,
            source_column INTEGER NOT NULL DEFAULT 0,
            scene_parser_version INTEGER NOT NULL DEFAULT 1,
            parser_version INTEGER NOT NULL DEFAULT 1,
            scan_generation INTEGER NOT NULL DEFAULT 0,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            UNIQUE(project_id, scene_file_id, resource_slot),
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (scene_file_id)
                REFERENCES project_files(id)
                ON DELETE CASCADE,
            FOREIGN KEY (target_file_id)
                REFERENCES project_files(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS scene_script_attachments (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            scene_file_id INTEGER NOT NULL,
            node_path TEXT NOT NULL DEFAULT '',
            node_name TEXT NOT NULL DEFAULT '',
            node_type TEXT NOT NULL DEFAULT '',
            attachment_kind TEXT NOT NULL DEFAULT 'unknown',
            ext_resource_id TEXT NOT NULL DEFAULT '',
            ext_resource_slot TEXT NOT NULL DEFAULT '',
            script_resource_path TEXT NOT NULL DEFAULT '',
            script_uid TEXT NOT NULL DEFAULT '',
            script_project_relative_path TEXT NOT NULL DEFAULT '',
            script_resource_uid TEXT NOT NULL DEFAULT '',
            script_file_id INTEGER,
            script_symbol_id INTEGER,
            is_dynamic INTEGER NOT NULL DEFAULT 0,
            is_resolved INTEGER NOT NULL DEFAULT 0,
            source_line INTEGER NOT NULL DEFAULT 0,
            source_column INTEGER NOT NULL DEFAULT 0,
            scene_parser_version INTEGER NOT NULL DEFAULT 1,
            parser_version INTEGER NOT NULL DEFAULT 1,
            scan_generation INTEGER NOT NULL DEFAULT 0,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (scene_file_id)
                REFERENCES project_files(id)
                ON DELETE CASCADE,
            FOREIGN KEY (script_file_id)
                REFERENCES project_files(id)
                ON DELETE SET NULL,
            FOREIGN KEY (script_symbol_id)
                REFERENCES script_symbols(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS project_scan_unknowns (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            project_relative_path TEXT NOT NULL,
            file_name TEXT NOT NULL,
            extension TEXT NOT NULL DEFAULT '',
            observed_file_type TEXT NOT NULL DEFAULT 'Unknown',
            observed_godot_type TEXT NOT NULL DEFAULT 'NGT',
            last_seen_scan_run_id INTEGER,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,
            UNIQUE(project_id, project_relative_path, extension),
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (last_seen_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE SET NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS scanner_meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS scan_metrics (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            scan_run_id INTEGER NOT NULL,
            scan_generation INTEGER NOT NULL DEFAULT 0,
            total_wall_ms INTEGER NOT NULL DEFAULT 0,
            traversal_ms INTEGER NOT NULL DEFAULT 0,
            metadata_ms INTEGER NOT NULL DEFAULT 0,
            existing_snapshot_load_ms INTEGER NOT NULL DEFAULT 0,
            reserve_setup_ms INTEGER NOT NULL DEFAULT 0,
            dirty_check_ms INTEGER NOT NULL DEFAULT 0,
            script_candidate_ms INTEGER NOT NULL DEFAULT 0,
            classification_ms INTEGER NOT NULL DEFAULT 0,
            script_parse_ms INTEGER NOT NULL DEFAULT 0,
            dependency_parse_ms INTEGER NOT NULL DEFAULT 0,
            full_symbol_parse_ms INTEGER NOT NULL DEFAULT 0,
            doc_comment_parse_ms INTEGER NOT NULL DEFAULT 0,
            scene_attachment_parse_ms INTEGER NOT NULL DEFAULT 0,
            tokenizer_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_write_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_stage_insert_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_file_merge_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_clean_refresh_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_parent_resolve_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_parse_status_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_custom_class_ms INTEGER NOT NULL DEFAULT 0,
            dependency_sqlite_stage_ms INTEGER NOT NULL DEFAULT 0,
            dependency_resolution_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_tombstone_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_deleted_reconcile_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_metrics_write_ms INTEGER NOT NULL DEFAULT 0,
            godot_materialization_ms INTEGER NOT NULL DEFAULT 0,
            files_seen INTEGER NOT NULL DEFAULT 0,
            dirs_seen INTEGER NOT NULL DEFAULT 0,
            dirs_skipped INTEGER NOT NULL DEFAULT 0,
            entries_clean INTEGER NOT NULL DEFAULT 0,
            entries_dirty INTEGER NOT NULL DEFAULT 0,
            entries_new INTEGER NOT NULL DEFAULT 0,
            entries_deleted INTEGER NOT NULL DEFAULT 0,
            rows_inserted INTEGER NOT NULL DEFAULT 0,
            rows_updated INTEGER NOT NULL DEFAULT 0,
            rows_clean_refreshed INTEGER NOT NULL DEFAULT 0,
            rows_tombstoned INTEGER NOT NULL DEFAULT 0,
            scripts_candidates INTEGER NOT NULL DEFAULT 0,
            scripts_parsed INTEGER NOT NULL DEFAULT 0,
            scripts_skipped_clean INTEGER NOT NULL DEFAULT 0,
            symbols_skipped_clean INTEGER NOT NULL DEFAULT 0,
            scenes_skipped_clean INTEGER NOT NULL DEFAULT 0,
            scripts_dependency_parsed INTEGER NOT NULL DEFAULT 0,
            scripts_dependency_skipped_clean INTEGER NOT NULL DEFAULT 0,
            script_lines_scanned INTEGER NOT NULL DEFAULT 0,
            parser_lines_scanned INTEGER NOT NULL DEFAULT 0,
            bytes_read INTEGER NOT NULL DEFAULT 0,
            parser_bytes_read INTEGER NOT NULL DEFAULT 0,
            parser_tokens_generated INTEGER NOT NULL DEFAULT 0,
            parser_limit_exceeded_count INTEGER NOT NULL DEFAULT 0,
            symbol_rows_created INTEGER NOT NULL DEFAULT 0,
            reference_rows_created INTEGER NOT NULL DEFAULT 0,
            doc_comment_rows_created INTEGER NOT NULL DEFAULT 0,
            scene_attachment_rows_created INTEGER NOT NULL DEFAULT 0,
            dependency_records_created INTEGER NOT NULL DEFAULT 0,
            unresolved_dependency_count INTEGER NOT NULL DEFAULT 0,
            dynamic_dependency_count INTEGER NOT NULL DEFAULT 0,
            entry_record_count INTEGER NOT NULL DEFAULT 0,
            path_arena_bytes INTEGER NOT NULL DEFAULT 0,
            existing_snapshot_count INTEGER NOT NULL DEFAULT 0,
            parsed_script_count INTEGER NOT NULL DEFAULT 0,
            sqlite_statement_steps INTEGER NOT NULL DEFAULT 0,
            sqlite_transactions INTEGER NOT NULL DEFAULT 0,
            ui_rows_materialized INTEGER NOT NULL DEFAULT 0,
            cancellation_requested INTEGER NOT NULL DEFAULT 0,
            scan_result_status TEXT NOT NULL DEFAULT 'completed',
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE CASCADE
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS deleted_entries (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            file_id INTEGER,
            project_relative_path TEXT NOT NULL,
            deleted_scan_run_id INTEGER NOT NULL,
            scan_generation INTEGER NOT NULL,
            deleted_at_unix INTEGER NOT NULL DEFAULT 0,
            UNIQUE(project_id, project_relative_path, scan_generation),
            FOREIGN KEY (project_id)
                REFERENCES projects(id)
                ON DELETE CASCADE,
            FOREIGN KEY (file_id)
                REFERENCES project_files(id)
                ON DELETE SET NULL,
            FOREIGN KEY (deleted_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE CASCADE
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS classification_policy (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            classifier_version INTEGER NOT NULL DEFAULT 1,
            updated_at_unix INTEGER NOT NULL DEFAULT 0
        );
    )sql");

  database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS parser_policy (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            parser_version INTEGER NOT NULL DEFAULT 1,
            updated_at_unix INTEGER NOT NULL DEFAULT 0
        );
    )sql");
}

void create_schema_v2_indexes(Database &database) {
  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_projects_last_seen_unix
        ON projects(last_seen_unix);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_scan_runs_project_id_started_at_unix
        ON project_scan_runs(project_id, started_at_unix);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_relative_path
        ON project_files(project_id, project_relative_path);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_extension
        ON project_files(project_id, extension);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_file_type
        ON project_files(project_id, file_type);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_godot_type
        ON project_files(project_id, godot_type);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_content_hash
        ON project_files(project_id, content_hash);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_is_directory
        ON project_files(project_id, is_directory);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_parent_id
        ON project_files(project_id, parent_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_parent_id_is_deleted
        ON project_files(project_id, parent_id, is_deleted);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_last_seen_generation
        ON project_files(project_id, last_seen_generation);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_is_deleted
        ON project_files(project_id, is_deleted);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_scene_parser_version
        ON project_files(project_id, scene_parser_version);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_autoloads_project_id_target_project_relative_path
        ON project_autoloads(project_id, target_project_relative_path);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_custom_classes_project_id_script_project_relative_path
        ON project_custom_classes(project_id, script_project_relative_path);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_custom_classes_project_id_base_type
        ON project_custom_classes(project_id, base_type);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_custom_classes_project_id_last_parsed_generation
        ON project_custom_classes(project_id, last_parsed_generation);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_symbols_project_id_class_name
        ON script_symbols(project_id, class_name);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_symbols_project_id_script_file_id
        ON script_symbols(project_id, script_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_symbols_project_id_name
        ON script_symbols(project_id, name);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_symbols_project_id_qualified_name
        ON script_symbols(project_id, qualified_name);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_symbols_project_id_symbol_kind
        ON script_symbols(project_id, symbol_kind);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_symbols_project_id_parent_symbol_id
        ON script_symbols(project_id, parent_symbol_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_dependencies_project_id_source_script_file_id
        ON script_dependencies(project_id, source_script_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_dependencies_project_id_target_file_id
        ON script_dependencies(project_id, target_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_dependencies_project_id_target_class_name
        ON script_dependencies(project_id, target_class_name);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_dependencies_project_id_dependency_kind
        ON script_dependencies(project_id, dependency_kind);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_dependencies_project_id_is_resolved
        ON script_dependencies(project_id, is_resolved);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_symbols_project_id_scan_generation
        ON script_symbols(project_id, last_parsed_generation);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_script_file_id
        ON script_references(project_id, script_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_source_script_file_id
        ON script_references(project_id, source_script_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_source_symbol_id
        ON script_references(project_id, source_symbol_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_target_file_id
        ON script_references(project_id, target_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_target_symbol_id
        ON script_references(project_id, target_symbol_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_target_class_name
        ON script_references(project_id, target_class_name);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_reference_kind
        ON script_references(project_id, reference_kind);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_is_resolved
        ON script_references(project_id, is_resolved);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_is_dynamic
        ON script_references(project_id, is_dynamic);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_references_project_id_scan_generation
        ON script_references(project_id, scan_generation);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_doc_comments_project_id_script_file_id
        ON script_doc_comments(project_id, script_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_doc_comments_project_id_symbol_id
        ON script_doc_comments(project_id, symbol_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_doc_comments_project_id_target_symbol_id
        ON script_doc_comments(project_id, target_symbol_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_script_doc_comments_project_id_scan_generation
        ON script_doc_comments(project_id, scan_generation);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_scene_external_resources_project_id_scene_file_id
        ON scene_external_resources(project_id, scene_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_scene_external_resources_project_id_scan_generation
        ON scene_external_resources(project_id, scan_generation);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_scene_external_resources_project_id_target_file_id
        ON scene_external_resources(project_id, target_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_scene_script_attachments_project_id_scene_file_id
        ON scene_script_attachments(project_id, scene_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_scene_script_attachments_project_id_script_file_id
        ON scene_script_attachments(project_id, script_file_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_scene_script_attachments_project_id_script_file_id_node_path
        ON scene_script_attachments(project_id, script_file_id, node_path);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_scene_script_attachments_project_id_is_resolved
        ON scene_script_attachments(project_id, is_resolved);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_scene_script_attachments_project_id_scan_generation
        ON scene_script_attachments(project_id, scan_generation);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_scan_unknowns_project_id_extension
        ON project_scan_unknowns(project_id, extension);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_scan_metrics_project_id_scan_run_id
        ON scan_metrics(project_id, scan_run_id);
    )sql");

  database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_deleted_entries_project_id_generation
        ON deleted_entries(project_id, scan_generation);
    )sql");
}

void ensure_schema_v3_columns(Database &database) {
  add_column_if_missing(database, "project_scan_runs",
                        "scan_generation INTEGER NOT NULL DEFAULT 0");

  add_column_if_missing(database, "project_files",
                        "parent_id INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "project_files",
                        "entry_kind TEXT NOT NULL DEFAULT 'file'");
  add_column_if_missing(database, "project_files",
                        "godot_type_hint TEXT NOT NULL DEFAULT 'NGT'");
  add_column_if_missing(database, "project_files",
                        "type_hint_source TEXT NOT NULL DEFAULT 'none'");
  add_column_if_missing(database, "project_files",
                        "type_hint_confidence INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "project_files",
                        "modified_time_ns INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "project_files",
                        "platform_file_id TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "project_files",
                        "scan_generation INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "project_files",
                        "last_seen_generation INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "project_files",
                        "dirty_state TEXT NOT NULL DEFAULT 'dirty'");
  add_column_if_missing(database, "project_files",
                        "dirty_reason TEXT NOT NULL DEFAULT 'new_path'");
  add_column_if_missing(database, "project_files",
                        "parser_version INTEGER NOT NULL DEFAULT 1");
  add_column_if_missing(database, "project_files",
                        "dependency_parser_version INTEGER NOT NULL DEFAULT 1");
  add_column_if_missing(database, "project_files",
                        "scene_parser_version INTEGER NOT NULL DEFAULT 1");
  add_column_if_missing(database, "project_files",
                        "classifier_version INTEGER NOT NULL DEFAULT 1");
  add_column_if_missing(database, "project_files",
                        "parse_status TEXT NOT NULL DEFAULT 'not_parsed'");
  add_column_if_missing(database, "project_files",
                        "is_deleted INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "project_files", "deleted_at_unix INTEGER");

  add_column_if_missing(database, "project_custom_classes",
                        "direct_base_type TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "project_custom_classes",
                        "parser_version INTEGER NOT NULL DEFAULT 1");
  add_column_if_missing(database, "project_custom_classes",
                        "parse_status TEXT NOT NULL DEFAULT 'not_parsed'");
  add_column_if_missing(database, "project_custom_classes",
                        "parse_error TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "project_custom_classes",
                        "last_parsed_generation INTEGER NOT NULL DEFAULT 0");

  add_column_if_missing(database, "script_symbols",
                        "name TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "script_symbols",
                        "qualified_name TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "script_symbols",
                        "declared_type TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "script_symbols",
                        "return_type TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "script_symbols",
                        "default_value_excerpt TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "script_symbols",
                        "visibility TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "script_symbols",
                        "flags INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "script_symbols",
                        "doc_comment_state TEXT NOT NULL DEFAULT 'none'");

  add_column_if_missing(database, "script_references",
                        "script_file_id INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "script_doc_comments",
                        "target_symbol_id INTEGER");
  add_column_if_missing(database, "script_doc_comments",
                        "target_kind TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "script_doc_comments",
                        "text_hash TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "script_doc_comments",
                        "text_excerpt TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "script_doc_comments",
                        "start_line INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "script_doc_comments",
                        "end_line INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "script_doc_comments",
                        "is_attached INTEGER NOT NULL DEFAULT 0");

  add_column_if_missing(database, "scene_external_resources",
                        "ext_resource_id TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "scene_external_resources",
                        "scene_parser_version INTEGER NOT NULL DEFAULT 1");

  add_column_if_missing(database, "scene_script_attachments",
                        "node_name TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "scene_script_attachments",
                        "node_type TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "scene_script_attachments",
                        "attachment_kind TEXT NOT NULL DEFAULT 'unknown'");
  add_column_if_missing(database, "scene_script_attachments",
                        "ext_resource_id TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "scene_script_attachments",
                        "script_resource_path TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "scene_script_attachments",
                        "script_uid TEXT NOT NULL DEFAULT ''");
  add_column_if_missing(database, "scene_script_attachments",
                        "scene_parser_version INTEGER NOT NULL DEFAULT 1");

  database.exec(R"sql(
        UPDATE script_symbols
        SET
            name = CASE WHEN name = '' THEN symbol_name ELSE name END,
            qualified_name = CASE
                WHEN qualified_name <> '' THEN qualified_name
                WHEN class_name <> '' AND symbol_name <> '' THEN class_name || '::' || symbol_name
                ELSE symbol_name
            END,
            flags = CASE WHEN flags = 0 THEN symbol_flags ELSE flags END
        WHERE
            name = '' OR
            qualified_name = '' OR
            flags = 0;
    )sql");

  database.exec(R"sql(
        UPDATE script_references
        SET script_file_id = source_script_file_id
        WHERE script_file_id = 0;
    )sql");

  database.exec(R"sql(
        UPDATE script_doc_comments
        SET
            target_symbol_id = COALESCE(target_symbol_id, symbol_id),
            text_excerpt = CASE
                WHEN text_excerpt <> '' THEN text_excerpt
                WHEN summary_text <> '' THEN summary_text
                ELSE comment_text
            END,
            start_line = CASE WHEN start_line = 0 THEN line_start ELSE start_line END,
            end_line = CASE WHEN end_line = 0 THEN line_end ELSE end_line END,
            is_attached = CASE
                WHEN is_attached <> 0 THEN is_attached
                WHEN comment_style IN ('gd_doc', 'csharp_xml') THEN 1
                ELSE 0
            END
        WHERE
            target_symbol_id IS NULL OR
            text_excerpt = '' OR
            start_line = 0 OR
            end_line = 0;
    )sql");

  database.exec(R"sql(
        UPDATE scene_external_resources
        SET
            ext_resource_id = CASE WHEN ext_resource_id = '' THEN resource_slot ELSE ext_resource_id END,
            scene_parser_version = CASE WHEN scene_parser_version = 1 THEN parser_version ELSE scene_parser_version END
        WHERE
            ext_resource_id = '' OR
            scene_parser_version = 1;
    )sql");

  database.exec(R"sql(
        UPDATE scene_script_attachments
        SET
            ext_resource_id = CASE WHEN ext_resource_id = '' THEN ext_resource_slot ELSE ext_resource_id END,
            script_resource_path = CASE
                WHEN script_resource_path <> '' THEN script_resource_path
                ELSE script_project_relative_path
            END,
            script_uid = CASE WHEN script_uid = '' THEN script_resource_uid ELSE script_uid END,
            scene_parser_version = CASE WHEN scene_parser_version = 1 THEN parser_version ELSE scene_parser_version END
        WHERE
            ext_resource_id = '' OR
            script_resource_path = '' OR
            script_uid = '' OR
            scene_parser_version = 1;
    )sql");

  add_column_if_missing(database, "scan_metrics",
                        "existing_snapshot_load_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "reserve_setup_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "script_candidate_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "sqlite_stage_insert_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "sqlite_file_merge_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "sqlite_clean_refresh_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "sqlite_parent_resolve_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "sqlite_parse_status_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "sqlite_custom_class_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(
      database, "scan_metrics",
      "dependency_sqlite_stage_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "dependency_resolution_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "sqlite_tombstone_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(
      database, "scan_metrics",
      "sqlite_deleted_reconcile_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "sqlite_metrics_write_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "dependency_parse_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "full_symbol_parse_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "doc_comment_parse_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "scene_attachment_parse_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "tokenizer_ms INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "rows_clean_refreshed INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "script_lines_scanned INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "parser_lines_scanned INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "entry_record_count INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "path_arena_bytes INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "existing_snapshot_count INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "parsed_script_count INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "symbols_skipped_clean INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "scenes_skipped_clean INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "scripts_dependency_parsed INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(
      database, "scan_metrics",
      "scripts_dependency_skipped_clean INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "parser_bytes_read INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "parser_tokens_generated INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(
      database, "scan_metrics",
      "parser_limit_exceeded_count INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "symbol_rows_created INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "reference_rows_created INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "doc_comment_rows_created INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(
      database, "scan_metrics",
      "scene_attachment_rows_created INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(
      database, "scan_metrics",
      "dependency_records_created INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(
      database, "scan_metrics",
      "unresolved_dependency_count INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "dynamic_dependency_count INTEGER NOT NULL DEFAULT 0");
  add_column_if_missing(database, "scan_metrics",
                        "sqlite_statement_steps INTEGER NOT NULL DEFAULT 0");
}

void ensure_script_symbols_generic_layout(Database &database) {
  if (!table_exists(database, "script_symbols")) {
    return;
  }

  if (table_has_column(database, "script_symbols", "symbol_kind")) {
    return;
  }

  database.exec("PRAGMA foreign_keys = OFF;");

  try {
    database.exec("DROP TABLE IF EXISTS script_symbols_v6;");
    database.exec(R"sql(
            CREATE TABLE script_symbols_v6 (
                id INTEGER PRIMARY KEY,
                project_id INTEGER NOT NULL,
                script_file_id INTEGER NOT NULL,
                symbol_slot INTEGER NOT NULL DEFAULT 0,
                parent_symbol_slot INTEGER,
                parent_symbol_id INTEGER,
                symbol_kind TEXT NOT NULL DEFAULT 'unknown',
                name TEXT NOT NULL DEFAULT '',
                qualified_name TEXT NOT NULL DEFAULT '',
                declared_type TEXT NOT NULL DEFAULT '',
                return_type TEXT NOT NULL DEFAULT '',
                default_value_excerpt TEXT NOT NULL DEFAULT '',
                visibility TEXT NOT NULL DEFAULT '',
                flags INTEGER NOT NULL DEFAULT 0,
                doc_comment_state TEXT NOT NULL DEFAULT 'none',
                symbol_name TEXT NOT NULL DEFAULT '',
                class_name TEXT NOT NULL DEFAULT '',
                language TEXT NOT NULL DEFAULT '',
                signature_text TEXT NOT NULL DEFAULT '',
                symbol_flags INTEGER NOT NULL DEFAULT 0,
                line_start INTEGER NOT NULL DEFAULT 0,
                column_start INTEGER NOT NULL DEFAULT 0,
                line_end INTEGER NOT NULL DEFAULT 0,
                column_end INTEGER NOT NULL DEFAULT 0,
                parser_version INTEGER NOT NULL DEFAULT 1,
                last_parsed_generation INTEGER NOT NULL DEFAULT 0,
                last_seen_scan_run_id INTEGER,
                created_at_unix INTEGER NOT NULL DEFAULT 0,
                updated_at_unix INTEGER NOT NULL DEFAULT 0,
                UNIQUE(project_id, script_file_id, symbol_slot),
                FOREIGN KEY (project_id)
                    REFERENCES projects(id)
                    ON DELETE CASCADE,
                FOREIGN KEY (script_file_id)
                    REFERENCES project_files(id)
                    ON DELETE CASCADE,
                FOREIGN KEY (parent_symbol_id)
                    REFERENCES script_symbols_v6(id)
                    ON DELETE SET NULL,
                FOREIGN KEY (last_seen_scan_run_id)
                    REFERENCES project_scan_runs(id)
                    ON DELETE SET NULL
            );
        )sql");

    Statement copy_symbols = database.prepare(R"sql(
            INSERT INTO script_symbols_v6 (
                id,
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
                id,
                project_id,
                script_file_id,
                0,
                NULL,
                NULL,
                'class',
                COALESCE(NULLIF(class_name, ''), 'script_root'),
                COALESCE(NULLIF(class_name, ''), 'script_root'),
                '',
                '',
                '',
                '',
                0,
                'none',
                COALESCE(NULLIF(class_name, ''), 'script_root'),
                class_name,
                language,
                class_name,
                0,
                1,
                1,
                1,
                1,
                parser_version,
                last_parsed_generation,
                last_seen_scan_run_id,
                created_at_unix,
                updated_at_unix
            FROM script_symbols;
        )sql");
    copy_symbols.step_done();

    database.exec("DROP TABLE script_symbols;");
    database.exec("ALTER TABLE script_symbols_v6 RENAME TO script_symbols;");
  } catch (...) {
    database.exec("PRAGMA foreign_keys = ON;");
    throw;
  }

  database.exec("PRAGMA foreign_keys = ON;");
}

bool needs_legacy_v1_migration(Database &database) {
  if (!table_exists(database, "project_scan_runs")) {
    return false;
  }

  return !table_has_column(database, "project_scan_runs", "project_id");
}

void migrate_v1_to_v2(Database &database, int64_t legacy_project_id) {
  if (legacy_project_id <= 0) {
    throw std::runtime_error(
        "Cannot migrate legacy schema without a registered project_id.");
  }

  database.exec("PRAGMA foreign_keys = OFF;");

  try {
    Transaction transaction(database);

    database.exec(
        "ALTER TABLE project_scan_runs RENAME TO project_scan_runs_legacy;");
    database.exec("ALTER TABLE project_files RENAME TO project_files_legacy;");
    database.exec(
        "ALTER TABLE project_autoloads RENAME TO project_autoloads_legacy;");
    database.exec("ALTER TABLE project_custom_classes RENAME TO "
                  "project_custom_classes_legacy;");
    database.exec("ALTER TABLE project_scan_unknowns RENAME TO "
                  "project_scan_unknowns_legacy;");

    create_schema_v2_tables(database);

    Statement copy_scan_runs = database.prepare(R"sql(
            INSERT INTO project_scan_runs (
                id,
                project_id,
                started_at_unix,
                finished_at_unix,
                status,
                files_found,
                folders_found,
                error_message
            )
            SELECT
                id,
                ?1,
                started_at_unix,
                finished_at_unix,
                status,
                files_found,
                folders_found,
                error_message
            FROM project_scan_runs_legacy;
        )sql");
    copy_scan_runs.bind_int64(1, legacy_project_id);
    copy_scan_runs.step_done();

    Statement copy_project_files = database.prepare(R"sql(
            INSERT INTO project_files (
                id,
                project_id,
                project_relative_path,
                absolute_path,
                file_name,
                extension,
                file_type,
                godot_type,
                size_bytes,
                content_hash,
                modified_time_unix,
                is_directory,
                is_hidden,
                first_seen_scan_run_id,
                last_seen_scan_run_id,
                created_at_unix,
                updated_at_unix
            )
            SELECT
                id,
                ?1,
                project_relative_path,
                absolute_path,
                file_name,
                extension,
                file_type,
                godot_type,
                size_bytes,
                content_hash,
                modified_time_unix,
                is_directory,
                is_hidden,
                first_seen_scan_run_id,
                last_seen_scan_run_id,
                created_at_unix,
                updated_at_unix
            FROM project_files_legacy;
        )sql");
    copy_project_files.bind_int64(1, legacy_project_id);
    copy_project_files.step_done();

    Statement copy_autoloads = database.prepare(R"sql(
            INSERT INTO project_autoloads (
                id,
                project_id,
                autoload_name,
                target_path,
                target_project_relative_path,
                is_singleton,
                target_file_id,
                last_seen_scan_run_id,
                created_at_unix,
                updated_at_unix
            )
            SELECT
                id,
                ?1,
                autoload_name,
                target_path,
                target_project_relative_path,
                is_singleton,
                target_file_id,
                last_seen_scan_run_id,
                created_at_unix,
                updated_at_unix
            FROM project_autoloads_legacy;
        )sql");
    copy_autoloads.bind_int64(1, legacy_project_id);
    copy_autoloads.step_done();

    Statement copy_custom_classes = database.prepare(R"sql(
            INSERT INTO project_custom_classes (
                id,
                project_id,
                class_name,
                script_path,
                script_project_relative_path,
                language,
                base_type,
                is_resource_type,
                is_node_type,
                script_file_id,
                last_seen_scan_run_id,
                created_at_unix,
                updated_at_unix
            )
            SELECT
                id,
                ?1,
                class_name,
                script_path,
                script_project_relative_path,
                language,
                base_type,
                is_resource_type,
                is_node_type,
                script_file_id,
                last_seen_scan_run_id,
                created_at_unix,
                updated_at_unix
            FROM project_custom_classes_legacy;
        )sql");
    copy_custom_classes.bind_int64(1, legacy_project_id);
    copy_custom_classes.step_done();

    Statement copy_unknowns = database.prepare(R"sql(
            INSERT INTO project_scan_unknowns (
                id,
                project_id,
                project_relative_path,
                file_name,
                extension,
                observed_file_type,
                observed_godot_type,
                last_seen_scan_run_id,
                created_at_unix,
                updated_at_unix
            )
            SELECT
                id,
                ?1,
                project_relative_path,
                file_name,
                extension,
                observed_file_type,
                observed_godot_type,
                last_seen_scan_run_id,
                created_at_unix,
                updated_at_unix
            FROM project_scan_unknowns_legacy;
        )sql");
    copy_unknowns.bind_int64(1, legacy_project_id);
    copy_unknowns.step_done();

    database.exec("DROP TABLE project_scan_unknowns_legacy;");
    database.exec("DROP TABLE project_custom_classes_legacy;");
    database.exec("DROP TABLE project_autoloads_legacy;");
    database.exec("DROP TABLE project_files_legacy;");
    database.exec("DROP TABLE project_scan_runs_legacy;");

    create_schema_v2_indexes(database);

    database.exec(R"sql(
            UPDATE project_files
            SET godot_type = 'NGT'
            WHERE godot_type IS NULL OR godot_type = '';
        )sql");

    write_user_version(database, GOTOOL_SCHEMA_VERSION);
    transaction.commit();
  } catch (...) {
    database.exec("PRAGMA foreign_keys = ON;");
    throw;
  }

  database.exec("PRAGMA foreign_keys = ON;");
}

void create_fresh_v2_schema(Database &database) {
  Transaction transaction(database);
  create_projects_table(database);
  create_schema_v2_tables(database);
  ensure_schema_v3_columns(database);
  ensure_script_symbols_generic_layout(database);
  create_schema_v2_indexes(database);

  database.exec(R"sql(
        UPDATE project_files
        SET godot_type = 'NGT'
        WHERE godot_type IS NULL OR godot_type = '';
    )sql");

  write_user_version(database, GOTOOL_SCHEMA_VERSION);
  transaction.commit();
}

void ensure_v2_schema(Database &database) {
  Transaction transaction(database);
  create_projects_table(database);
  create_schema_v2_tables(database);
  ensure_schema_v3_columns(database);
  ensure_script_symbols_generic_layout(database);
  create_schema_v2_indexes(database);

  database.exec(R"sql(
        UPDATE project_files
        SET godot_type = 'NGT'
        WHERE godot_type IS NULL OR godot_type = '';
    )sql");

  write_user_version(database, GOTOOL_SCHEMA_VERSION);
  transaction.commit();
}

} // namespace

void create_schema(Database &database, int64_t legacy_project_id) {
  create_projects_table(database);

  if (needs_legacy_v1_migration(database)) {
    if (legacy_project_id <= 0) {
      return;
    }

    migrate_v1_to_v2(database, legacy_project_id);
    return;
  }

  const bool has_scan_table = table_exists(database, "project_scan_runs");
  const int64_t user_version = read_user_version(database);

  if (!has_scan_table && user_version == 0) {
    create_fresh_v2_schema(database);
    return;
  }

  ensure_v2_schema(database);
}

} // namespace gotool::database
