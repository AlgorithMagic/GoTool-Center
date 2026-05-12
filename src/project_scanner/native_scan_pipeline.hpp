#pragma once

#include "database/gotool_database.hpp"
#include "project_scanner/native_directory_enumerator.hpp"
#include "project_scanner/native_script_parser.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gotool::project_scanner {

struct ParsedScriptRecord {
    size_t record_index = 0;
    std::string project_relative_path;
    std::string extension;
    ScriptParseResult parse_result;
};

struct FileQuery {
    bool include_deleted = false;
    std::optional<int64_t> parent_id;
    std::optional<bool> is_directory;
    std::string extension;
    std::string file_type;
    std::string godot_type;
    std::string search;
};

struct CustomClassQuery {
    std::string language;
    std::string base_type;
    std::string search;
};

struct FileRow {
    int64_t id = 0;
    int64_t parent_id = 0;
    std::string project_relative_path;
    std::string file_name;
    std::string extension;
    std::string file_type;
    std::string godot_type;
    std::string type_hint_source;
    int64_t size_bytes = 0;
    int64_t modified_time_ns = 0;
    bool is_directory = false;
    bool is_hidden = false;
    bool is_deleted = false;
    ScanGeneration scan_generation = 0;
    ScanGeneration last_seen_generation = 0;
    std::string dirty_state;
    std::string dirty_reason;
};

struct CustomClassRow {
    int64_t id = 0;
    std::string class_name;
    std::string script_path;
    std::string script_project_relative_path;
    std::string language;
    std::string direct_base_type;
    bool is_resource_type = false;
    bool is_node_type = false;
    int64_t script_file_id = 0;
    int64_t parser_version = 0;
    std::string parse_status;
    std::string parse_error;
    ScanGeneration last_parsed_generation = 0;
};

class ScanRepository {
public:
    explicit ScanRepository(gotool::database::Database &database);

    ScanGeneration next_generation(int64_t project_id);
    int64_t create_scan_run(int64_t project_id, ScanGeneration generation, int64_t started_at_unix);
    void complete_scan_run(
        int64_t project_id,
        int64_t scan_run_id,
        ScanGeneration generation,
        const ScanMetrics &metrics,
        int64_t finished_at_unix
    );
    std::unordered_map<std::string, ExistingEntrySnapshot> load_existing_entries(int64_t project_id);
    void write_scan_results(
        int64_t project_id,
        int64_t scan_run_id,
        ScanGeneration generation,
        const std::filesystem::path &project_root,
        PathArena &arena,
        std::vector<EntryRecord> &records,
        const std::vector<ParsedScriptRecord> &parsed_scripts,
        ScanMetrics &metrics
    );

    int64_t count_files(int64_t project_id, const FileQuery &query) const;
    std::vector<FileRow> list_files(
        int64_t project_id,
        const FileQuery &query,
        int64_t offset,
        int64_t limit,
        const std::string &sort
    ) const;
    std::optional<FileRow> get_file_details(int64_t project_id, int64_t file_id) const;
    int64_t count_custom_classes(int64_t project_id, const CustomClassQuery &query) const;
    std::vector<CustomClassRow> list_custom_classes(
        int64_t project_id,
        const CustomClassQuery &query,
        int64_t offset,
        int64_t limit,
        const std::string &sort
    ) const;
    ScanMetrics get_scan_metrics(int64_t project_id, int64_t scan_run_id) const;
    std::string get_scan_status(int64_t project_id, int64_t scan_run_id) const;

private:
    gotool::database::Database *database_ = nullptr;
};

class NativeScanPipeline {
public:
    explicit NativeScanPipeline(gotool::database::Database &database);

    ScanResultSummary run(const ScanOptions &options);

private:
    gotool::database::Database *database_ = nullptr;
};

} // namespace gotool::project_scanner
