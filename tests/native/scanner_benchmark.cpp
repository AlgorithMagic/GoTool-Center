#include "database/gotool_database.hpp"
#include "database/gotool_schema.hpp"
#include "project_scanner/native_scan_pipeline.hpp"

#include "doctest.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using gotool::database::Database;
using gotool::project_scanner::NativeScanPipeline;
using gotool::project_scanner::ScanOptions;
using gotool::project_scanner::ScanResultSummary;

std::filesystem::path benchmark_root() {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("gotool_center_benchmark_" + std::to_string(now));
}

void write_text_file(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void append_text_file(const std::filesystem::path &path, const std::string &text) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output << text;
}

void register_benchmark_project(Database &database, const std::filesystem::path &root) {
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'benchmark-project', 'benchmark-project', '" + root.generic_string() + "', '" +
        root.generic_string() + "', '" + (root / "project.godot").generic_string() +
        "', '4.6.0-stable', 'benchmark', 1, 1, 1, 1"
        ");"
    );
}

void generate_synthetic_project(const std::filesystem::path &root) {
    write_text_file(root / "project.godot", "[application]\nconfig/name=\"Benchmark\"\n");

    for (int32_t i = 0; i < 1100; ++i) {
        const std::filesystem::path script =
            root / "scripts" / ("group_" + std::to_string(i % 50)) /
            ("BenchClass" + std::to_string(i) + ".gd");
        write_text_file(
            script,
            "class_name BenchClass" + std::to_string(i) + "\n"
            "extends Resource\n"
        );
    }

    for (int32_t i = 0; i < 78900; ++i) {
        const int32_t bucket = i % 800;
        const std::filesystem::path file =
            root / "assets" / ("bucket_" + std::to_string(bucket)) /
            ("asset_" + std::to_string(i) + (i % 7 == 0 ? ".tscn" : i % 7 == 1 ? ".tres" : i % 7 == 2 ? ".png" : ".json"));
        write_text_file(file, "asset " + std::to_string(i) + "\n");
    }
}

std::string summary_json(const std::string &name, const ScanResultSummary &summary) {
    return
        "    {\n"
        "      \"scenario\": \"" + name + "\",\n"
        "      \"scan_run_id\": " + std::to_string(summary.scan_run_id) + ",\n"
        "      \"scan_generation\": " + std::to_string(summary.scan_generation) + ",\n"
        "      \"status\": \"" + summary.status + "\",\n"
        "      \"total_wall_ms\": " + std::to_string(summary.total_wall_ms) + ",\n"
        "      \"files_seen\": " + std::to_string(summary.files_seen) + ",\n"
        "      \"dirs_seen\": " + std::to_string(summary.dirs_seen) + ",\n"
        "      \"entries_clean\": " + std::to_string(summary.entries_clean) + ",\n"
        "      \"entries_dirty\": " + std::to_string(summary.entries_dirty) + ",\n"
        "      \"entries_new\": " + std::to_string(summary.entries_new) + ",\n"
        "      \"entries_deleted\": " + std::to_string(summary.entries_deleted) + ",\n"
        "      \"scripts_candidates\": " + std::to_string(summary.scripts_candidates) + ",\n"
        "      \"scripts_parsed\": " + std::to_string(summary.scripts_parsed) + ",\n"
        "      \"scripts_skipped_clean\": " + std::to_string(summary.scripts_skipped_clean) + "\n"
        "    }";
}

} // namespace

TEST_CASE("scanner_benchmark_synthetic_large_project [benchmark][.]") {
    if (std::getenv("GOTOOL_RUN_SCANNER_BENCHMARK") == nullptr) {
        return;
    }

    const std::filesystem::path root = benchmark_root();
    std::filesystem::create_directories(root);

    generate_synthetic_project(root);

    const std::filesystem::path database_path = root.parent_path() / (root.filename().string() + ".sqlite3");
    Database database(database_path.string());
    gotool::database::create_schema(database, 0);
    register_benchmark_project(database, root);

    NativeScanPipeline pipeline(database);
    ScanOptions options;
    options.project_id = 1;
    options.project_root = root;
    options.include_hidden = true;

    std::vector<std::pair<std::string, ScanResultSummary>> results;
    results.push_back({ "cold_scan", pipeline.run(options) });
    results.push_back({ "immediate_no_change_rescan", pipeline.run(options) });

    append_text_file(root / "scripts" / "group_0" / "BenchClass0.gd", "# changed\n");
    results.push_back({ "one_script_changed", pipeline.run(options) });

    for (int32_t i = 0; i < 100; ++i) {
        append_text_file(
            root / "scripts" / ("group_" + std::to_string(i % 50)) /
            ("BenchClass" + std::to_string(i) + ".gd"),
            "# changed " + std::to_string(i) + "\n"
        );
    }
    results.push_back({ "one_hundred_scripts_changed", pipeline.run(options) });

    for (int32_t i = 0; i < 1000; ++i) {
        append_text_file(
            root / "assets" / ("bucket_" + std::to_string(i % 800)) /
            ("asset_" + std::to_string(i) + (i % 7 == 0 ? ".tscn" : i % 7 == 1 ? ".tres" : i % 7 == 2 ? ".png" : ".json")),
            "changed\n"
        );
    }
    results.push_back({ "one_thousand_random_files_changed", pipeline.run(options) });

    std::filesystem::remove_all(root / "assets" / "bucket_1");
    results.push_back({ "delete_subtree", pipeline.run(options) });

    for (int32_t i = 0; i < 250; ++i) {
        write_text_file(root / "added_subtree" / ("added_" + std::to_string(i) + ".tres"), "[resource]\n");
    }
    results.push_back({ "add_subtree", pipeline.run(options) });

    const std::filesystem::path output_dir = std::filesystem::path("build") / "benchmarks";
    std::filesystem::create_directories(output_dir);
    const std::filesystem::path output_path = output_dir / "scanner_benchmark.json";
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    output << "{\n";
    output << "  \"generated_entries\": 80001,\n";
    output << "  \"custom_class_scripts\": 1100,\n";
    output << "  \"scenarios\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        output << summary_json(results[i].first, results[i].second);
        output << (i + 1 == results.size() ? "\n" : ",\n");
    }
    output << "  ]\n";
    output << "}\n";

    CHECK(std::filesystem::exists(output_path));

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::remove(database_path, cleanup_error);
}
