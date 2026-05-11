#include "gotool_schema.hpp"

namespace gotool::database {

void create_schema(Database& database) {
    database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS project_scan_runs (
            id INTEGER PRIMARY KEY,
            started_at_unix INTEGER NOT NULL DEFAULT 0,
            finished_at_unix INTEGER,
            status TEXT NOT NULL CHECK (
                status IN ('running', 'completed', 'failed', 'cancelled')
            ),
            files_found INTEGER NOT NULL DEFAULT 0,
            folders_found INTEGER NOT NULL DEFAULT 0,
            error_message TEXT
        );
    )sql");

    database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS project_files (
            id INTEGER PRIMARY KEY,

            project_relative_path TEXT NOT NULL UNIQUE,
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

            first_seen_scan_run_id INTEGER,
            last_seen_scan_run_id INTEGER,

            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,

            FOREIGN KEY (first_seen_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE SET NULL,

            FOREIGN KEY (last_seen_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE SET NULL
        );
    )sql");

    database.exec(R"sql(
        UPDATE project_files
        SET godot_type = 'NGT'
        WHERE godot_type IS NULL OR godot_type = '';
    )sql");

    database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS project_autoloads (
            id INTEGER PRIMARY KEY,

            autoload_name TEXT NOT NULL UNIQUE,
            target_path TEXT NOT NULL,
            target_project_relative_path TEXT NOT NULL,

            is_singleton INTEGER NOT NULL DEFAULT 1,

            target_file_id INTEGER,
            last_seen_scan_run_id INTEGER,

            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,

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

            class_name TEXT NOT NULL UNIQUE,
            script_path TEXT NOT NULL,
            script_project_relative_path TEXT NOT NULL,

            language TEXT NOT NULL,
            base_type TEXT NOT NULL DEFAULT '',

            is_resource_type INTEGER NOT NULL DEFAULT 0,
            is_node_type INTEGER NOT NULL DEFAULT 0,

            script_file_id INTEGER,
            last_seen_scan_run_id INTEGER,

            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,

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

            project_relative_path TEXT NOT NULL,
            file_name TEXT NOT NULL,
            extension TEXT NOT NULL DEFAULT '',
            observed_file_type TEXT NOT NULL DEFAULT 'Unknown',
            observed_godot_type TEXT NOT NULL DEFAULT 'NGT',

            last_seen_scan_run_id INTEGER,

            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,

            UNIQUE(project_relative_path, extension),

            FOREIGN KEY (last_seen_scan_run_id)
                REFERENCES project_scan_runs(id)
                ON DELETE SET NULL
        );
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_relative_path
        ON project_files(project_relative_path);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_extension
        ON project_files(extension);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_file_type
        ON project_files(file_type);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_godot_type
        ON project_files(godot_type);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_content_hash
        ON project_files(content_hash);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_files_is_directory
        ON project_files(is_directory);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_autoloads_target_path
        ON project_autoloads(target_path);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_autoloads_target_project_relative_path
        ON project_autoloads(target_project_relative_path);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_custom_classes_script_path
        ON project_custom_classes(script_path);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_custom_classes_base_type
        ON project_custom_classes(base_type);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_custom_classes_is_resource_type
        ON project_custom_classes(is_resource_type);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_custom_classes_is_node_type
        ON project_custom_classes(is_node_type);
    )sql");

    database.exec(R"sql(
        CREATE INDEX IF NOT EXISTS idx_project_scan_unknowns_extension
        ON project_scan_unknowns(extension);
    )sql");

    database.exec("PRAGMA user_version = 1;");
}

} // namespace gotool::database