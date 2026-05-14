// Copyright 2026 AlgorithMagic

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "database/gotool_database.hpp"
#include "database/gotool_project_inventory_repository.hpp"
#include "database/gotool_project_registry_repository.hpp"
#include "database/gotool_schema.hpp"
#include "doctest.h"

namespace {

using gotool::database::Database;
using gotool::database::PersistedInventorySummary;
using gotool::database::ProjectInventoryRepository;
using gotool::database::ProjectRegistrationInput;
using gotool::database::ProjectRegistryRepository;
using gotool::database::Statement;
using gotool::database::Transaction;

std::filesystem::path make_temp_database_path(const std::string& test_name)
{
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "gotool_center_native_tests";
    std::filesystem::create_directories(root);
    return root / (test_name + "_" + std::to_string(now) + ".sqlite3");
}

struct TemporaryDatabaseFile
{
    explicit TemporaryDatabaseFile(std::filesystem::path value) : path(std::move(value)) {}

    TemporaryDatabaseFile(const TemporaryDatabaseFile&) = delete;
    TemporaryDatabaseFile& operator=(const TemporaryDatabaseFile&) = delete;
    TemporaryDatabaseFile(TemporaryDatabaseFile&&) = delete;
    TemporaryDatabaseFile& operator=(TemporaryDatabaseFile&&) = delete;

    ~TemporaryDatabaseFile()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    std::filesystem::path path;
};

int64_t query_single_int64(Database& database, const std::string& sql)
{
    Statement statement = database.prepare(sql);
    const Statement::StepResult result = statement.step();
    if (result != Statement::StepResult::Row) {
        throw std::runtime_error("Expected sqlite row while querying int64.");
    }
    return statement.column_int64(0);
}

int64_t query_single_int64_with_bind(Database& database, const std::string& sql, int64_t value)
{
    Statement statement = database.prepare(sql);
    statement.bind_int64(1, value);
    const Statement::StepResult result = statement.step();
    if (result != Statement::StepResult::Row) {
        throw std::runtime_error("Expected sqlite row while querying int64 with bind.");
    }
    return statement.column_int64(0);
}

ProjectRegistrationInput make_registration(const std::string& uid, const std::string& root)
{
    ProjectRegistrationInput input;
    input.project_uid = uid;
    input.display_name = uid;
    input.root_absolute_path = root;
    input.root_canonical_path = root;
    input.project_file_absolute_path = root + "/project.godot";
    input.godot_version = "4.6.0-stable";
    input.identity_source = "native_test";
    input.identity_warning = "";
    input.observed_at_unix = 1'700'000'000;
    return input;
}

int64_t register_project(ProjectRegistryRepository& registry, const std::string& uid,
                         const std::string& root)
{
    return registry.register_project(make_registration(uid, root)).project_id;
}

} // namespace

// These integration-style tests intentionally keep extensive setup and verification in one case.
// NOLINTBEGIN(readability-function-size,modernize-type-traits,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
TEST_CASE("project_inventory_repository_rejects_invalid_project_id_and_missing_payload_keys")
{
    TemporaryDatabaseFile temp_db(make_temp_database_path("inventory_validation"));
    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);

    ProjectRegistryRepository registry(database);
    const int64_t project_id =
        register_project(registry, "inventory-validation", "/tmp/inventory-validation");
    REQUIRE(project_id > 0);

    ProjectInventoryRepository inventory_repository(database);

    CHECK_THROWS_AS(inventory_repository.persist_inventory(0, godot::Dictionary()),
                    std::runtime_error);
    CHECK_THROWS_AS(inventory_repository.persist_inventory(project_id, godot::Dictionary()),
                    std::runtime_error);

    godot::Dictionary inventory_with_explicit_empty_files;
    inventory_with_explicit_empty_files["files"] = godot::Array();

    const PersistedInventorySummary summary =
        inventory_repository.persist_inventory(project_id, inventory_with_explicit_empty_files);

    CHECK(summary.scan_run_id > 0);
    CHECK(summary.files_found == 0);
    CHECK(summary.folders_found == 0);
    CHECK(summary.unknown_entries == 0);
    CHECK(summary.sqlite_prepare_count == 10);
}

TEST_CASE("project_inventory_repository_persists_then_prunes_files_autoloads_classes_and_unknowns")
{
    TemporaryDatabaseFile temp_db(make_temp_database_path("inventory_persist_prune"));
    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);

    ProjectRegistryRepository registry(database);
    const int64_t project_id =
        register_project(registry, "inventory-persist", "/tmp/inventory-persist");
    REQUIRE(project_id > 0);

    ProjectInventoryRepository inventory_repository(database);

    godot::Dictionary script_file;
    script_file["project_relative_path"] = "scripts/player.gd";
    script_file["absolute_path"] = "/tmp/inventory-persist/scripts/player.gd";
    script_file["file_name"] = "player.gd";
    script_file["extension"] = ".gd";
    script_file["file_type"] = "Script";
    script_file["godot_type"] = "GDScript";
    script_file["size_bytes"] = 42.5;
    script_file["modified_time_unix"] = 1001;
    script_file["is_directory"] = false;
    script_file["is_hidden"] = 1;

    godot::Dictionary unknown_file;
    unknown_file["project_relative_path"] = "misc/blob.xyz";
    unknown_file["absolute_path"] = "/tmp/inventory-persist/misc/blob.xyz";
    unknown_file["file_name"] = 77;
    unknown_file["extension"] = ".xyz";
    unknown_file["file_type"] = "Unknown";
    unknown_file["godot_type"] = godot::Variant();
    unknown_file["size_bytes"] = true;
    unknown_file["modified_time_unix"] = "not-a-number";
    unknown_file["is_directory"] = 0;
    unknown_file["is_hidden"] = "yes";

    godot::Dictionary folder;
    folder["project_relative_path"] = "scenes";
    folder["absolute_path"] = "/tmp/inventory-persist/scenes";
    folder["file_name"] = "scenes";
    folder["extension"] = "";
    folder["file_type"] = "Folder";
    folder["godot_type"] = "NGT";
    folder["size_bytes"] = 0;
    folder["modified_time_unix"] = 2002;
    folder["is_directory"] = true;
    folder["is_hidden"] = false;

    godot::Array files;
    files.append(script_file);
    files.append(unknown_file);
    files.append(folder);
    files.append(12345);

    godot::Dictionary autoload;
    autoload["autoload_name"] = "PlayerAuto";
    autoload["target_path"] = "res://scripts/player.gd";
    autoload["target_project_relative_path"] = "scripts/player.gd";
    autoload["is_singleton"] = true;

    godot::Array autoloads;
    autoloads.append(autoload);
    autoloads.append("skip-me");

    godot::Dictionary custom_class;
    custom_class["class_name"] = "PlayerAgent";
    custom_class["script_path"] = "res://scripts/player.gd";
    custom_class["script_project_relative_path"] = "scripts/player.gd";
    custom_class["language"] = "GDScript";
    custom_class["base_type"] = "Node";
    custom_class["is_resource_type"] = false;
    custom_class["is_node_type"] = 1;

    godot::Array custom_classes;
    custom_classes.append(custom_class);
    custom_classes.append(9.5);

    godot::Dictionary first_inventory;
    first_inventory["files"] = files;
    first_inventory["autoloads"] = autoloads;
    first_inventory["custom_classes"] = custom_classes;

    const PersistedInventorySummary first =
        inventory_repository.persist_inventory(project_id, first_inventory);
    CHECK(first.scan_run_id > 0);
    CHECK(first.files_found == 2);
    CHECK(first.folders_found == 1);
    CHECK(first.unknown_entries == 1);
    CHECK(first.sqlite_prepare_count == 10);
    CHECK(first.sqlite_step_count >= 10);

    CHECK(query_single_int64_with_bind(database,
                                       "SELECT COUNT(*) FROM project_files WHERE project_id = ?1;",
                                       project_id) == 3);
    CHECK(query_single_int64_with_bind(
              database, "SELECT COUNT(*) FROM project_autoloads WHERE project_id = ?1;",
              project_id) == 1);
    CHECK(query_single_int64_with_bind(
              database, "SELECT COUNT(*) FROM project_custom_classes WHERE project_id = ?1;",
              project_id) == 1);
    CHECK(query_single_int64_with_bind(
              database, "SELECT COUNT(*) FROM project_scan_unknowns WHERE project_id = ?1;",
              project_id) == 1);

    CHECK(query_single_int64(database, "SELECT size_bytes FROM project_files WHERE "
                                       "project_relative_path = 'scripts/player.gd';") == 42);
    CHECK(query_single_int64(database, "SELECT size_bytes FROM project_files WHERE "
                                       "project_relative_path = 'misc/blob.xyz';") == 1);
    CHECK(query_single_int64(database, "SELECT modified_time_unix FROM project_files WHERE "
                                       "project_relative_path = 'misc/blob.xyz';") == 0);
    CHECK(
        query_single_int64(
            database,
            "SELECT is_hidden FROM project_files WHERE project_relative_path = 'misc/blob.xyz';") ==
        0);

    Statement unknown_type = database.prepare(
        "SELECT observed_godot_type FROM project_scan_unknowns WHERE project_id = ?1 LIMIT 1;");
    unknown_type.bind_int64(1, project_id);
    REQUIRE(unknown_type.step() == Statement::StepResult::Row);
    CHECK(unknown_type.column_text(0) == "NGT");

    CHECK(query_single_int64_with_bind(database,
                                       "SELECT target_file_id FROM project_autoloads WHERE "
                                       "project_id = ?1 AND autoload_name = 'PlayerAuto';",
                                       project_id) > 0);
    CHECK(query_single_int64_with_bind(database,
                                       "SELECT script_file_id FROM project_custom_classes WHERE "
                                       "project_id = ?1 AND class_name = 'PlayerAgent';",
                                       project_id) > 0);

    godot::Dictionary updated_script_file;
    updated_script_file["project_relative_path"] = "scripts/player.gd";
    updated_script_file["absolute_path"] = "/tmp/inventory-persist/scripts/player.gd";
    updated_script_file["file_name"] = "player.gd";
    updated_script_file["extension"] = ".gd";
    updated_script_file["file_type"] = "Script";
    updated_script_file["godot_type"] = "GDScript";
    updated_script_file["size_bytes"] = 84;
    updated_script_file["modified_time_unix"] = 3003;
    updated_script_file["is_directory"] = false;
    updated_script_file["is_hidden"] = false;

    godot::Array second_files;
    second_files.append(updated_script_file);

    godot::Dictionary second_inventory;
    second_inventory["files"] = second_files;
    second_inventory["autoloads"] = godot::Array();
    second_inventory["custom_classes"] = godot::Array();

    const PersistedInventorySummary second =
        inventory_repository.persist_inventory(project_id, second_inventory);
    CHECK(second.scan_run_id > first.scan_run_id);
    CHECK(second.files_found == 1);
    CHECK(second.folders_found == 0);
    CHECK(second.unknown_entries == 0);

    CHECK(query_single_int64_with_bind(database,
                                       "SELECT COUNT(*) FROM project_files WHERE project_id = ?1;",
                                       project_id) == 1);
    CHECK(query_single_int64_with_bind(
              database, "SELECT COUNT(*) FROM project_autoloads WHERE project_id = ?1;",
              project_id) == 0);
    CHECK(query_single_int64_with_bind(
              database, "SELECT COUNT(*) FROM project_custom_classes WHERE project_id = ?1;",
              project_id) == 0);
    CHECK(query_single_int64_with_bind(
              database, "SELECT COUNT(*) FROM project_scan_unknowns WHERE project_id = ?1;",
              project_id) == 0);

    Statement scan_marker =
        database.prepare("SELECT first_seen_scan_run_id, last_seen_scan_run_id, size_bytes "
                         "FROM project_files WHERE project_id = ?1 AND project_relative_path = "
                         "'scripts/player.gd' LIMIT 1;");
    scan_marker.bind_int64(1, project_id);
    REQUIRE(scan_marker.step() == Statement::StepResult::Row);
    CHECK(scan_marker.column_int64(0) == first.scan_run_id);
    CHECK(scan_marker.column_int64(1) == second.scan_run_id);
    CHECK(scan_marker.column_int64(2) == 84);
}

TEST_CASE("project_registry_repository_validates_inputs_and_detects_uid_root_conflicts")
{
    TemporaryDatabaseFile temp_db(make_temp_database_path("registry_validation_conflicts"));
    Database database(temp_db.path.string());
    gotool::database::create_schema(database, 0);

    ProjectRegistryRepository registry(database);

    ProjectRegistrationInput empty_uid = make_registration("uid-valid", "/tmp/registry-empty-uid");
    empty_uid.project_uid.clear();
    CHECK_THROWS_AS(registry.register_project(empty_uid), std::runtime_error);

    ProjectRegistrationInput empty_canonical =
        make_registration("uid-empty-canonical", "/tmp/registry-empty-canonical");
    empty_canonical.root_canonical_path = std::filesystem::path();
    CHECK_THROWS_AS(registry.register_project(empty_canonical), std::runtime_error);

    ProjectRegistrationInput empty_project_file =
        make_registration("uid-empty-project-file", "/tmp/registry-empty-project-file");
    empty_project_file.project_file_absolute_path = std::filesystem::path();
    CHECK_THROWS_AS(registry.register_project(empty_project_file), std::runtime_error);

    const int64_t project_a = register_project(registry, "uid-a", "/tmp/registry-a");
    const int64_t project_b = register_project(registry, "uid-b", "/tmp/registry-b");
    CHECK(project_a > 0);
    CHECK(project_b > 0);

    ProjectRegistrationInput conflicting = make_registration("uid-a", "/tmp/registry-b");
    CHECK_THROWS_AS(registry.register_project(conflicting), std::runtime_error);

    CHECK_FALSE(registry.get_project_summary(999999).has_value());

    const std::vector<gotool::database::ProjectListItem> listed = registry.list_projects();
    CHECK(listed.size() >= 2);
}

TEST_CASE("database_statement_and_transaction_edge_paths_are_safe")
{
    TemporaryDatabaseFile temp_db(make_temp_database_path("database_statement_edges"));
    Database database(temp_db.path.string());

    database.exec("CREATE TABLE values_table ("
                  "id INTEGER PRIMARY KEY,"
                  "text_value TEXT,"
                  "number_value INTEGER"
                  ");");

    Statement insert =
        database.prepare("INSERT INTO values_table (text_value, number_value) VALUES (?1, ?2);");
    insert.bind_text(1, static_cast<const char*>(nullptr));
    insert.bind_null(2);
    insert.step_done();

    insert.reset();
    insert.clear_bindings();
    insert.bind_text(1, std::string(""));
    insert.bind_int64(2, 5);
    insert.step_done();

    Statement first_row =
        database.prepare("SELECT text_value, number_value FROM values_table WHERE id = 1;");
    REQUIRE(first_row.step() == Statement::StepResult::Row);
    CHECK(first_row.column_text(0).empty());
    CHECK(first_row.column_is_null(1));
    CHECK(first_row.column_text(1).empty());

    Statement moved_constructed = std::move(first_row);
    CHECK(moved_constructed.column_is_null(1));

    Statement second_row =
        database.prepare("SELECT text_value, number_value FROM values_table WHERE id = 2;");
    REQUIRE(second_row.step() == Statement::StepResult::Row);
    CHECK(second_row.column_text(0).empty());
    CHECK_FALSE(second_row.column_is_null(1));

    second_row = std::move(moved_constructed);
    CHECK(second_row.column_is_null(1));

    second_row = std::move(second_row);
    CHECK(second_row.column_is_null(1));

    Statement row_statement = database.prepare("SELECT 1;");
    CHECK_THROWS_AS(row_statement.step_done(), std::runtime_error);

    const int64_t baseline_count =
        query_single_int64(database, "SELECT COUNT(*) FROM values_table;");

    {
        Transaction committed(database);
        database.exec(
            "INSERT INTO values_table (text_value, number_value) VALUES ('committed', 11);");
        committed.commit();
        committed.commit();
    }

    {
        Transaction rolled_back(database);
        database.exec(
            "INSERT INTO values_table (text_value, number_value) VALUES ('rolled_back', 22);");
    }

    CHECK(query_single_int64(database, "SELECT COUNT(*) FROM values_table;") == baseline_count + 1);
    CHECK(query_single_int64(
              database, "SELECT COUNT(*) FROM values_table WHERE text_value = 'rolled_back';") ==
          0);

    CHECK_THROWS_AS(database.exec("THIS IS NOT VALID SQL"), std::runtime_error);
    CHECK_THROWS_AS(Statement(nullptr, "SELECT 1;"), std::runtime_error);
    CHECK(query_single_int64_with_bind(database, "SELECT COUNT(*) FROM values_table WHERE id = ?1;",
                                       1) == 1);
}

// NOLINTEND(readability-function-size,modernize-type-traits,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
