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
    Statement statement = database.prepare(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1 LIMIT 1;"
    );
    statement.bind_text(1, table_name);
    return statement.step() == Statement::StepResult::Row;
}

bool table_has_column(Database &database, const std::string &table_name, const std::string &column_name) {
    Statement statement = database.prepare("PRAGMA table_info(" + table_name + ");");

    while (statement.step() == Statement::StepResult::Row) {
        if (statement.column_text(1) == column_name) {
            return true;
        }
    }

    return false;
}

void add_column_if_missing(Database &database, const std::string &table_name, const std::string &column_sql) {
    const size_t first_space = column_sql.find(' ');
    if (first_space == std::string::npos) {
        throw std::runtime_error("Column SQL must start with a column name.");
    }

    const std::string column_name = column_sql.substr(0, first_space);
    if (!table_has_column(database, table_name, column_name)) {
        database.exec("ALTER TABLE " + table_name + " ADD COLUMN " + column_sql + ";");
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
        database.exec(
            "ALTER TABLE projects ADD COLUMN identity_source TEXT NOT NULL DEFAULT 'uid_file';"
        );
    }

    if (!table_has_column(database, "projects", "identity_warning")) {
        database.exec(
            "ALTER TABLE projects ADD COLUMN identity_warning TEXT NOT NULL DEFAULT '';"
        );
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
            dirty_check_ms INTEGER NOT NULL DEFAULT 0,
            classification_ms INTEGER NOT NULL DEFAULT 0,
            script_parse_ms INTEGER NOT NULL DEFAULT 0,
            sqlite_write_ms INTEGER NOT NULL DEFAULT 0,
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
            rows_tombstoned INTEGER NOT NULL DEFAULT 0,
            scripts_candidates INTEGER NOT NULL DEFAULT 0,
            scripts_parsed INTEGER NOT NULL DEFAULT 0,
            scripts_skipped_clean INTEGER NOT NULL DEFAULT 0,
            bytes_read INTEGER NOT NULL DEFAULT 0,
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
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_last_seen_generation
        ON project_files(project_id, last_seen_generation);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_project_id_is_deleted
        ON project_files(project_id, is_deleted);
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
    add_column_if_missing(database, "project_scan_runs", "scan_generation INTEGER NOT NULL DEFAULT 0");

    add_column_if_missing(database, "project_files", "parent_id INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(database, "project_files", "entry_kind TEXT NOT NULL DEFAULT 'file'");
    add_column_if_missing(database, "project_files", "godot_type_hint TEXT NOT NULL DEFAULT 'NGT'");
    add_column_if_missing(database, "project_files", "type_hint_source TEXT NOT NULL DEFAULT 'none'");
    add_column_if_missing(database, "project_files", "type_hint_confidence INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(database, "project_files", "modified_time_ns INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(database, "project_files", "platform_file_id TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(database, "project_files", "scan_generation INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(database, "project_files", "last_seen_generation INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(database, "project_files", "dirty_state TEXT NOT NULL DEFAULT 'dirty'");
    add_column_if_missing(database, "project_files", "dirty_reason TEXT NOT NULL DEFAULT 'new_path'");
    add_column_if_missing(database, "project_files", "parser_version INTEGER NOT NULL DEFAULT 1");
    add_column_if_missing(database, "project_files", "classifier_version INTEGER NOT NULL DEFAULT 1");
    add_column_if_missing(database, "project_files", "parse_status TEXT NOT NULL DEFAULT 'not_parsed'");
    add_column_if_missing(database, "project_files", "is_deleted INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(database, "project_files", "deleted_at_unix INTEGER");

    add_column_if_missing(database, "project_custom_classes", "direct_base_type TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(database, "project_custom_classes", "parser_version INTEGER NOT NULL DEFAULT 1");
    add_column_if_missing(database, "project_custom_classes", "parse_status TEXT NOT NULL DEFAULT 'not_parsed'");
    add_column_if_missing(database, "project_custom_classes", "parse_error TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(database, "project_custom_classes", "last_parsed_generation INTEGER NOT NULL DEFAULT 0");
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
            "Cannot migrate legacy schema without a registered project_id."
        );
    }

    database.exec("PRAGMA foreign_keys = OFF;");

    try {
        Transaction transaction(database);

        database.exec("ALTER TABLE project_scan_runs RENAME TO project_scan_runs_legacy;");
        database.exec("ALTER TABLE project_files RENAME TO project_files_legacy;");
        database.exec("ALTER TABLE project_autoloads RENAME TO project_autoloads_legacy;");
        database.exec("ALTER TABLE project_custom_classes RENAME TO project_custom_classes_legacy;");
        database.exec("ALTER TABLE project_scan_unknowns RENAME TO project_scan_unknowns_legacy;");

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
