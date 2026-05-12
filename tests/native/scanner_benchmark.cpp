#include "database/gotool_database.hpp"
#include "database/gotool_schema.hpp"
#include "project_scanner/native_scan_pipeline.hpp"

#include "doctest.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using gotool::database::Database;
using gotool::project_scanner::NativeScanPipeline;
using gotool::project_scanner::NativeScanResult;
using gotool::project_scanner::ScanMetrics;
using gotool::project_scanner::ScanOptions;
using gotool::project_scanner::ScanRepository;
using gotool::project_scanner::ScanResultSummary;

struct BenchmarkScenarioResult {
    std::string scenario;
    ScanResultSummary summary;
    ScanMetrics metrics;
};

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
            ("asset_" + std::to_string(i) +
             (i % 7 == 0 ? ".tscn" : i % 7 == 1 ? ".tres" : i % 7 == 2 ? ".png" : ".json"));
        write_text_file(file, "asset " + std::to_string(i) + "\n");
    }
}

BenchmarkScenarioResult run_persisted_scenario(
    NativeScanPipeline &pipeline,
    Database &database,
    const std::string &name,
    const ScanOptions &options
) {
    BenchmarkScenarioResult result;
    result.scenario = name;
    result.summary = pipeline.run(options);

    if (result.summary.scan_run_id > 0) {
        ScanRepository repository(database);
        result.metrics = repository.get_scan_metrics(options.project_id, result.summary.scan_run_id);
    }

    return result;
}

BenchmarkScenarioResult run_native_no_db_scenario(
    NativeScanPipeline &pipeline,
    const std::string &name,
    ScanOptions options
) {
    options.persist_to_database = false;
    options.scan_run_id = 0;
    options.scan_generation = 0;

    BenchmarkScenarioResult result;
    result.scenario = name;

    const NativeScanResult native_result = pipeline.run_detailed(options);
    result.summary = native_result.summary;
    result.metrics = native_result.metrics;
    return result;
}

std::string scenario_json(const BenchmarkScenarioResult &scenario) {
    return
        "    {\n"
        "      \"scenario\": \"" + scenario.scenario + "\",\n"
        "      \"scan_run_id\": " + std::to_string(scenario.summary.scan_run_id) + ",\n"
        "      \"scan_generation\": " + std::to_string(scenario.summary.scan_generation) + ",\n"
        "      \"status\": \"" + scenario.summary.status + "\",\n"
        "      \"total_wall_ms\": " + std::to_string(scenario.metrics.total_wall_ms) + ",\n"
        "      \"traversal_ms\": " + std::to_string(scenario.metrics.traversal_ms) + ",\n"
        "      \"dirty_check_ms\": " + std::to_string(scenario.metrics.dirty_check_ms) + ",\n"
        "      \"script_parse_ms\": " + std::to_string(scenario.metrics.script_parse_ms) + ",\n"
        "      \"sqlite_write_ms\": " + std::to_string(scenario.metrics.sqlite_write_ms) + ",\n"
        "      \"files_seen\": " + std::to_string(scenario.metrics.files_seen) + ",\n"
        "      \"dirs_seen\": " + std::to_string(scenario.metrics.dirs_seen) + ",\n"
        "      \"entries_clean\": " + std::to_string(scenario.metrics.entries_clean) + ",\n"
        "      \"entries_dirty\": " + std::to_string(scenario.metrics.entries_dirty) + ",\n"
        "      \"entries_new\": " + std::to_string(scenario.metrics.entries_new) + ",\n"
        "      \"entries_deleted\": " + std::to_string(scenario.metrics.entries_deleted) + ",\n"
        "      \"scripts_candidates\": " + std::to_string(scenario.metrics.scripts_candidates) + ",\n"
        "      \"scripts_parsed\": " + std::to_string(scenario.metrics.scripts_parsed) + ",\n"
        "      \"scripts_skipped_clean\": " + std::to_string(scenario.metrics.scripts_skipped_clean) + "\n"
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

    const std::filesystem::path database_path =
        root.parent_path() / (root.filename().string() + ".sqlite3");

    Database database(database_path.string());
    gotool::database::create_schema(database, 0);
    register_benchmark_project(database, root);

    NativeScanPipeline pipeline(database);
    ScanOptions options;
    options.project_id = 1;
    options.project_root = root;
    options.include_hidden = true;
    options.persist_to_database = true;

    std::vector<BenchmarkScenarioResult> scenarios;
    scenarios.push_back(run_native_no_db_scenario(pipeline, "native_fast_no_db_cold", options));
    scenarios.push_back(run_persisted_scenario(pipeline, database, "db_cold_scan", options));
    scenarios.push_back(run_persisted_scenario(pipeline, database, "db_immediate_no_change", options));

    append_text_file(root / "scripts" / "group_0" / "BenchClass0.gd", "# changed\n");
    scenarios.push_back(run_persisted_scenario(pipeline, database, "db_one_script_changed", options));

    std::atomic_bool cancel_requested = false;
    std::thread cancel_thread([&cancel_requested]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        cancel_requested.store(true, std::memory_order_relaxed);
    });

    ScanOptions cancellation_options = options;
    cancellation_options.cancel_requested = &cancel_requested;
    const NativeScanResult cancelled = pipeline.run_detailed(cancellation_options);
    cancel_thread.join();

    BenchmarkScenarioResult cancelled_scenario;
    cancelled_scenario.scenario = "db_cancellation_responsiveness";
    cancelled_scenario.summary = cancelled.summary;
    cancelled_scenario.metrics = cancelled.metrics;
    scenarios.push_back(std::move(cancelled_scenario));

    const std::filesystem::path output_dir = std::filesystem::path("build") / "benchmarks";
    std::filesystem::create_directories(output_dir);
    const std::filesystem::path output_path = output_dir / "scanner_benchmark.json";

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    output << "{\n";
    output << "  \"generated_entries\": 80001,\n";
    output << "  \"custom_class_scripts\": 1100,\n";
    output << "  \"scenarios\": [\n";
    for (size_t i = 0; i < scenarios.size(); ++i) {
        output << scenario_json(scenarios[i]);
        output << (i + 1 == scenarios.size() ? "\n" : ",\n");
    }
    output << "  ]\n";
    output << "}\n";

    CHECK(std::filesystem::exists(output_path));

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::remove(database_path, cleanup_error);
}
