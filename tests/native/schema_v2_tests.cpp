#include "database/gotool_database.hpp"
#include "database/gotool_project_registry_repository.hpp"
#include "database/gotool_schema.hpp"

#include "doctest.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using gotool::database::Database;
using gotool::database::ProjectRegistrationInput;
using gotool::database::ProjectRegistryRepository;
using gotool::database::Statement;

std::filesystem::path make_temp_database_path(const std::string &test_name) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "gotool_center_native_tests";
    std::filesystem::create_directories(root);

    return root / (test_name + "_" + std::to_string(now) + ".sqlite3");
}

struct TemporaryDatabaseFile {
    explicit TemporaryDatabaseFile(std::filesystem::path value) :
        path(std::move(value)) {}

    ~TemporaryDatabaseFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    std::filesystem::path path;
};

int64_t query_single_int64(Database &database, const std::string &sql) {
    Statement statement = database.prepare(sql);
    const Statement::StepResult result = statement.step();
    if (result != Statement::StepResult::Row) {
        throw std::runtime_error("Expected row while querying single int64.");
    }
    return statement.column_int64(0);
}

int64_t query_single_int64_with_bind(Database &database, const std::string &sql, int64_t value) {
    Statement statement = database.prepare(sql);
    statement.bind_int64(1, value);
    const Statement::StepResult result = statement.step();
    if (result != Statement::StepResult::Row) {
        throw std::runtime_error("Expected row while querying single int64 with bind.");
    }
    return statement.column_int64(0);
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

bool table_exists(Database &database, const std::string &table_name) {
    Statement statement = database.prepare(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1 LIMIT 1;"
    );
    statement.bind_text(1, table_name);
    return statement.step() == Statement::StepResult::Row;
}

int64_t register_project(
    ProjectRegistryRepository &registry,
    const std::string &project_uid,
    const std::filesystem::path &root_absolute_path,
    const std::filesystem::path &root_canonical_path,
    const std::filesystem::path &project_file_absolute_path,
    const std::string &identity_source = "uid_file",
    const std::string &identity_warning = ""
) {
    ProjectRegistrationInput input;
    input.project_uid = project_uid;
    input.display_name = project_uid;
    input.root_absolute_path = root_absolute_path;
    input.root_canonical_path = root_canonical_path;
    input.project_file_absolute_path = project_file_absolute_path;
    input.godot_version = "4.6.0-stable";
    input.identity_source = identity_source;
    input.identity_warning = identity_warning;
    input.observed_at_unix = 1'700'000'000;

    return registry.register_project(input).project_id;
}

} // namespace

TEST_CASE("schema_v2_creation_is_idempotent") {
    TemporaryDatabaseFile temp_db(make_temp_database_path("schema_idempotent"));

    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);
    gotool::database::create_schema(database, 0);

    CHECK(query_single_int64(database, "PRAGMA user_version;") == gotool::database::GOTOOL_SCHEMA_VERSION);

    CHECK(table_has_column(database, "project_scan_runs", "project_id"));
    CHECK(table_has_column(database, "project_files", "project_id"));
    CHECK(table_has_column(database, "project_autoloads", "project_id"));
    CHECK(table_has_column(database, "project_custom_classes", "project_id"));
    CHECK(table_has_column(database, "project_scan_unknowns", "project_id"));
}

TEST_CASE("schema_v5_dependency_tables_and_metrics_columns_exist") {
    TemporaryDatabaseFile temp_db(make_temp_database_path("schema_v5_dependency"));

    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);

    CHECK(query_single_int64(database, "PRAGMA user_version;") == gotool::database::GOTOOL_SCHEMA_VERSION);
    CHECK(table_exists(database, "script_symbols"));
    CHECK(table_exists(database, "script_dependencies"));
    CHECK(table_has_column(database, "project_files", "dependency_parser_version"));

    CHECK(table_has_column(database, "script_dependencies", "source_script_file_id"));
    CHECK(table_has_column(database, "script_dependencies", "source_symbol_id"));
    CHECK(table_has_column(database, "script_dependencies", "target_file_id"));
    CHECK(table_has_column(database, "script_dependencies", "target_project_relative_path"));
    CHECK(table_has_column(database, "script_dependencies", "target_class_name"));
    CHECK(table_has_column(database, "script_dependencies", "target_resource_uid"));
    CHECK(table_has_column(database, "script_dependencies", "dependency_kind"));
    CHECK(table_has_column(database, "script_dependencies", "is_dynamic"));
    CHECK(table_has_column(database, "script_dependencies", "is_resolved"));
    CHECK(table_has_column(database, "script_dependencies", "parser_version"));
    CHECK(table_has_column(database, "script_dependencies", "scan_generation"));

    CHECK(table_has_column(database, "scan_metrics", "dependency_parse_ms"));
    CHECK(table_has_column(database, "scan_metrics", "tokenizer_ms"));
    CHECK(table_has_column(database, "scan_metrics", "dependency_sqlite_stage_ms"));
    CHECK(table_has_column(database, "scan_metrics", "dependency_resolution_ms"));
    CHECK(table_has_column(database, "scan_metrics", "dependency_records_created"));
    CHECK(table_has_column(database, "scan_metrics", "unresolved_dependency_count"));
    CHECK(table_has_column(database, "scan_metrics", "dynamic_dependency_count"));
    CHECK(table_has_column(database, "scan_metrics", "scripts_dependency_parsed"));
    CHECK(table_has_column(database, "scan_metrics", "scripts_dependency_skipped_clean"));
    CHECK(table_has_column(database, "scan_metrics", "parser_bytes_read"));
    CHECK(table_has_column(database, "scan_metrics", "parser_lines_scanned"));
    CHECK(table_has_column(database, "scan_metrics", "parser_tokens_generated"));
    CHECK(table_has_column(database, "scan_metrics", "parser_limit_exceeded_count"));
}

TEST_CASE("same_relative_path_can_exist_across_multiple_projects") {
    TemporaryDatabaseFile temp_db(make_temp_database_path("multi_project_path"));
    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);

    ProjectRegistryRepository registry(database);
    const int64_t project_a_id = register_project(
        registry,
        "uid-a",
        "/tmp/project_a",
        "/tmp/project_a",
        "/tmp/project_a/project.godot"
    );
    const int64_t project_b_id = register_project(
        registry,
        "uid-b",
        "/tmp/project_b",
        "/tmp/project_b",
        "/tmp/project_b/project.godot"
    );

    Statement scan_run_a = database.prepare(
        "INSERT INTO project_scan_runs (project_id, started_at_unix, status, files_found, folders_found) "
        "VALUES (?1, 1, 'completed', 1, 0);"
    );
    scan_run_a.bind_int64(1, project_a_id);
    scan_run_a.step_done();
    const int64_t scan_run_a_id = database.last_insert_row_id();

    Statement scan_run_b = database.prepare(
        "INSERT INTO project_scan_runs (project_id, started_at_unix, status, files_found, folders_found) "
        "VALUES (?1, 1, 'completed', 1, 0);"
    );
    scan_run_b.bind_int64(1, project_b_id);
    scan_run_b.step_done();
    const int64_t scan_run_b_id = database.last_insert_row_id();

    Statement insert_file_a = database.prepare(R"sql(
        INSERT INTO project_files (
            project_id, project_relative_path, absolute_path, file_name, extension, file_type, godot_type,
            size_bytes, modified_time_unix, is_directory, is_hidden,
            first_seen_scan_run_id, last_seen_scan_run_id, created_at_unix, updated_at_unix
        ) VALUES (
            ?1, ?2, ?3, 'script.gd', '.gd', 'GDScript', 'GDScript',
            1, 1, 0, 0, ?4, ?4, 1, 1
        );
    )sql");
    insert_file_a.bind_int64(1, project_a_id);
    insert_file_a.bind_text(2, "scripts/player.gd");
    insert_file_a.bind_text(3, "/tmp/project_a/scripts/player.gd");
    insert_file_a.bind_int64(4, scan_run_a_id);
    insert_file_a.step_done();

    Statement insert_file_b = database.prepare(R"sql(
        INSERT INTO project_files (
            project_id, project_relative_path, absolute_path, file_name, extension, file_type, godot_type,
            size_bytes, modified_time_unix, is_directory, is_hidden,
            first_seen_scan_run_id, last_seen_scan_run_id, created_at_unix, updated_at_unix
        ) VALUES (
            ?1, ?2, ?3, 'script.gd', '.gd', 'GDScript', 'GDScript',
            1, 1, 0, 0, ?4, ?4, 1, 1
        );
    )sql");
    insert_file_b.bind_int64(1, project_b_id);
    insert_file_b.bind_text(2, "scripts/player.gd");
    insert_file_b.bind_text(3, "/tmp/project_b/scripts/player.gd");
    insert_file_b.bind_int64(4, scan_run_b_id);
    insert_file_b.step_done();

    CHECK(
        query_single_int64(
            database,
            "SELECT COUNT(*) FROM project_files WHERE project_relative_path = 'scripts/player.gd';"
        ) == 2
    );
}

TEST_CASE("cleanup_queries_are_scoped_by_project_id") {
    TemporaryDatabaseFile temp_db(make_temp_database_path("scoped_cleanup"));
    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);

    ProjectRegistryRepository registry(database);
    const int64_t project_a_id = register_project(
        registry,
        "uid-cleanup-a",
        "/tmp/cleanup_a",
        "/tmp/cleanup_a",
        "/tmp/cleanup_a/project.godot"
    );
    const int64_t project_b_id = register_project(
        registry,
        "uid-cleanup-b",
        "/tmp/cleanup_b",
        "/tmp/cleanup_b",
        "/tmp/cleanup_b/project.godot"
    );

    Statement scan_run_a_old = database.prepare(
        "INSERT INTO project_scan_runs (project_id, started_at_unix, status, files_found, folders_found) "
        "VALUES (?1, 1, 'completed', 1, 0);"
    );
    scan_run_a_old.bind_int64(1, project_a_id);
    scan_run_a_old.step_done();
    const int64_t scan_run_a_old_id = database.last_insert_row_id();

    Statement scan_run_a_new = database.prepare(
        "INSERT INTO project_scan_runs (project_id, started_at_unix, status, files_found, folders_found) "
        "VALUES (?1, 2, 'completed', 1, 0);"
    );
    scan_run_a_new.bind_int64(1, project_a_id);
    scan_run_a_new.step_done();
    const int64_t scan_run_a_new_id = database.last_insert_row_id();

    Statement scan_run_b = database.prepare(
        "INSERT INTO project_scan_runs (project_id, started_at_unix, status, files_found, folders_found) "
        "VALUES (?1, 3, 'completed', 1, 0);"
    );
    scan_run_b.bind_int64(1, project_b_id);
    scan_run_b.step_done();
    const int64_t scan_run_b_id = database.last_insert_row_id();

    Statement insert_file_a_old = database.prepare(R"sql(
        INSERT INTO project_files (
            project_id, project_relative_path, absolute_path, file_name, extension, file_type, godot_type,
            size_bytes, modified_time_unix, is_directory, is_hidden,
            first_seen_scan_run_id, last_seen_scan_run_id, created_at_unix, updated_at_unix
        ) VALUES (
            ?1, 'stale_a.gd', '/tmp/cleanup_a/stale_a.gd', 'stale_a.gd', '.gd', 'GDScript', 'GDScript',
            1, 1, 0, 0, ?2, ?2, 1, 1
        );
    )sql");
    insert_file_a_old.bind_int64(1, project_a_id);
    insert_file_a_old.bind_int64(2, scan_run_a_old_id);
    insert_file_a_old.step_done();

    Statement insert_file_a_new = database.prepare(R"sql(
        INSERT INTO project_files (
            project_id, project_relative_path, absolute_path, file_name, extension, file_type, godot_type,
            size_bytes, modified_time_unix, is_directory, is_hidden,
            first_seen_scan_run_id, last_seen_scan_run_id, created_at_unix, updated_at_unix
        ) VALUES (
            ?1, 'fresh_a.gd', '/tmp/cleanup_a/fresh_a.gd', 'fresh_a.gd', '.gd', 'GDScript', 'GDScript',
            1, 1, 0, 0, ?2, ?3, 1, 1
        );
    )sql");
    insert_file_a_new.bind_int64(1, project_a_id);
    insert_file_a_new.bind_int64(2, scan_run_a_old_id);
    insert_file_a_new.bind_int64(3, scan_run_a_new_id);
    insert_file_a_new.step_done();

    Statement insert_file_b = database.prepare(R"sql(
        INSERT INTO project_files (
            project_id, project_relative_path, absolute_path, file_name, extension, file_type, godot_type,
            size_bytes, modified_time_unix, is_directory, is_hidden,
            first_seen_scan_run_id, last_seen_scan_run_id, created_at_unix, updated_at_unix
        ) VALUES (
            ?1, 'keep_b.gd', '/tmp/cleanup_b/keep_b.gd', 'keep_b.gd', '.gd', 'GDScript', 'GDScript',
            1, 1, 0, 0, ?2, ?2, 1, 1
        );
    )sql");
    insert_file_b.bind_int64(1, project_b_id);
    insert_file_b.bind_int64(2, scan_run_b_id);
    insert_file_b.step_done();

    Statement cleanup_statement = database.prepare(
        "DELETE FROM project_files "
        "WHERE project_id = ?1 "
        "  AND (last_seen_scan_run_id IS NULL OR last_seen_scan_run_id <> ?2);"
    );
    cleanup_statement.bind_int64(1, project_a_id);
    cleanup_statement.bind_int64(2, scan_run_a_new_id);
    cleanup_statement.step_done();

    CHECK(
        query_single_int64_with_bind(
            database,
            "SELECT COUNT(*) FROM project_files WHERE project_id = ?1;",
            project_a_id
        ) == 1
    );
    CHECK(
        query_single_int64_with_bind(
            database,
            "SELECT COUNT(*) FROM project_files WHERE project_id = ?1;",
            project_b_id
        ) == 1
    );
    CHECK(query_single_int64(database, "SELECT COUNT(*) FROM project_files WHERE project_relative_path = 'fresh_a.gd';") == 1);
    CHECK(query_single_int64(database, "SELECT COUNT(*) FROM project_files WHERE project_relative_path = 'keep_b.gd';") == 1);
}

TEST_CASE("register_project_updates_moved_root_without_duplicate_rows") {
    TemporaryDatabaseFile temp_db(make_temp_database_path("register_upsert"));
    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);

    ProjectRegistryRepository registry(database);

    const int64_t first_id = register_project(
        registry,
        "uid-move",
        "/tmp/move_a",
        "/tmp/move_a",
        "/tmp/move_a/project.godot"
    );

    const int64_t second_id = register_project(
        registry,
        "uid-move",
        "/tmp/move_b",
        "/tmp/move_b",
        "/tmp/move_b/project.godot"
    );

    CHECK(first_id == second_id);
    CHECK(query_single_int64(database, "SELECT COUNT(*) FROM projects;") == 1);

    Statement root_query = database.prepare("SELECT root_canonical_path FROM projects WHERE id = ?1;");
    root_query.bind_int64(1, first_id);
    REQUIRE(root_query.step() == Statement::StepResult::Row);
    const std::filesystem::path stored_root = std::filesystem::path(root_query.column_text(0)).lexically_normal();
    const std::filesystem::path expected_root = std::filesystem::path("/tmp/move_b").lexically_normal();
    CHECK(stored_root == expected_root);
}

TEST_CASE("register_project_rejects_canonical_path_uid_conflict") {
    TemporaryDatabaseFile temp_db(make_temp_database_path("register_conflict"));
    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);

    ProjectRegistryRepository registry(database);

    register_project(
        registry,
        "uid-existing",
        "/tmp/shared_path",
        "/tmp/shared_path",
        "/tmp/shared_path/project.godot"
    );

    CHECK_THROWS_AS(
        register_project(
            registry,
            "uid-conflicting",
            "/tmp/shared_path",
            "/tmp/shared_path",
            "/tmp/shared_path/project.godot"
        ),
        std::runtime_error
    );
}

TEST_CASE("legacy_v1_schema_migrates_into_registered_project_scope") {
    TemporaryDatabaseFile temp_db(make_temp_database_path("legacy_migration"));
    Database database(temp_db.path.string());

    database.exec(R"sql(
        CREATE TABLE project_scan_runs (
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
        CREATE TABLE project_files (
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
        CREATE TABLE project_autoloads (
            id INTEGER PRIMARY KEY,
            autoload_name TEXT NOT NULL UNIQUE,
            target_path TEXT NOT NULL,
            target_project_relative_path TEXT NOT NULL,
            is_singleton INTEGER NOT NULL DEFAULT 1,
            target_file_id INTEGER,
            last_seen_scan_run_id INTEGER,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0
        );
    )sql");

    database.exec(R"sql(
        CREATE TABLE project_custom_classes (
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
            updated_at_unix INTEGER NOT NULL DEFAULT 0
        );
    )sql");

    database.exec(R"sql(
        CREATE TABLE project_scan_unknowns (
            id INTEGER PRIMARY KEY,
            project_relative_path TEXT NOT NULL,
            file_name TEXT NOT NULL,
            extension TEXT NOT NULL DEFAULT '',
            observed_file_type TEXT NOT NULL DEFAULT 'Unknown',
            observed_godot_type TEXT NOT NULL DEFAULT 'NGT',
            last_seen_scan_run_id INTEGER,
            created_at_unix INTEGER NOT NULL DEFAULT 0,
            updated_at_unix INTEGER NOT NULL DEFAULT 0,
            UNIQUE(project_relative_path, extension)
        );
    )sql");

    database.exec(
        "INSERT INTO project_scan_runs (id, started_at_unix, finished_at_unix, status, files_found, folders_found) "
        "VALUES (1, 10, 20, 'completed', 1, 0);"
    );

    database.exec(
        "INSERT INTO project_files (id, project_relative_path, absolute_path, file_name, extension, file_type, godot_type, "
        "size_bytes, modified_time_unix, is_directory, is_hidden, first_seen_scan_run_id, last_seen_scan_run_id, created_at_unix, updated_at_unix) "
        "VALUES (1, 'legacy.gd', '/tmp/legacy/legacy.gd', 'legacy.gd', '.gd', 'GDScript', 'GDScript', 1, 1, 0, 0, 1, 1, 1, 1);"
    );

    database.exec(
        "INSERT INTO project_autoloads (id, autoload_name, target_path, target_project_relative_path, is_singleton, target_file_id, last_seen_scan_run_id, created_at_unix, updated_at_unix) "
        "VALUES (1, 'LegacyAuto', 'res://legacy.gd', 'legacy.gd', 1, 1, 1, 1, 1);"
    );

    database.exec(
        "INSERT INTO project_custom_classes (id, class_name, script_path, script_project_relative_path, language, base_type, is_resource_type, is_node_type, script_file_id, last_seen_scan_run_id, created_at_unix, updated_at_unix) "
        "VALUES (1, 'LegacyClass', 'res://legacy.gd', 'legacy.gd', 'GDScript', 'Node', 0, 1, 1, 1, 1, 1);"
    );

    database.exec(
        "INSERT INTO project_scan_unknowns (id, project_relative_path, file_name, extension, observed_file_type, observed_godot_type, last_seen_scan_run_id, created_at_unix, updated_at_unix) "
        "VALUES (1, 'legacy.bin', 'legacy.bin', '.bin', 'Unknown', 'NGT', 1, 1, 1);"
    );

    database.exec("PRAGMA user_version = 1;");

    gotool::database::create_schema(database, 0);

    ProjectRegistryRepository registry(database);
    const int64_t project_id = register_project(
        registry,
        "legacy-project",
        "/tmp/legacy",
        "/tmp/legacy",
        "/tmp/legacy/project.godot"
    );

    gotool::database::create_schema(database, project_id);

    CHECK(query_single_int64(database, "PRAGMA user_version;") == gotool::database::GOTOOL_SCHEMA_VERSION);
    CHECK(
        query_single_int64_with_bind(
            database,
            "SELECT COUNT(*) FROM project_files WHERE project_id = ?1;",
            project_id
        ) == 1
    );
    CHECK(
        query_single_int64_with_bind(
            database,
            "SELECT COUNT(*) FROM project_autoloads WHERE project_id = ?1;",
            project_id
        ) == 1
    );
    CHECK(
        query_single_int64_with_bind(
            database,
            "SELECT COUNT(*) FROM project_custom_classes WHERE project_id = ?1;",
            project_id
        ) == 1
    );
    CHECK(
        query_single_int64_with_bind(
            database,
            "SELECT COUNT(*) FROM project_scan_unknowns WHERE project_id = ?1;",
            project_id
        ) == 1
    );
}

TEST_CASE("project_registry_normalizes_stored_paths") {
    TemporaryDatabaseFile temp_db(make_temp_database_path("path_normalization"));
    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);

    ProjectRegistryRepository registry(database);

#if defined(_WIN32)
    const std::filesystem::path root_absolute = "C:/games/../games/sample_project";
    const std::filesystem::path root_canonical = "C:/games/sample_project";
    const std::filesystem::path project_file = "C:/games/sample_project/project.godot";
#else
    const std::filesystem::path root_absolute = "/tmp/games/../games/sample_project";
    const std::filesystem::path root_canonical = "/tmp/games/sample_project";
    const std::filesystem::path project_file = "/tmp/games/sample_project/project.godot";
#endif

    const int64_t project_id = register_project(
        registry,
        "uid-normalized",
        root_absolute,
        root_canonical,
        project_file
    );

    Statement statement = database.prepare(
        "SELECT root_absolute_path, root_canonical_path, project_file_absolute_path "
        "FROM projects WHERE id = ?1;"
    );
    statement.bind_int64(1, project_id);
    REQUIRE(statement.step() == Statement::StepResult::Row);

    CHECK(statement.column_text(0).find("..") == std::string::npos);
    CHECK(statement.column_text(1).find("..") == std::string::npos);
    CHECK(statement.column_text(2).find("..") == std::string::npos);
}
