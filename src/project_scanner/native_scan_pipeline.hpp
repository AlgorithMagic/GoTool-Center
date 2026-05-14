#pragma once

#include "database/gotool_database.hpp"
#include "project_scanner/native_directory_enumerator.hpp"
#include "project_scanner/native_script_parser.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gotool::project_scanner {

struct ParsedScriptRecord {
    size_t record_index = 0;
    std::string project_relative_path;
    ScriptParseResult parse_result;
};

struct ParsedSceneRecord {
    size_t record_index = 0;
    std::string project_relative_path;
    SceneParseResult parse_result;
};

struct NativeScanResult {
    ScanResultSummary summary;
    ScanMetrics metrics;
    std::string error_message;
    PathArena arena;
    std::vector<EntryRecord> records;
    std::vector<ParsedScriptRecord> parsed_scripts;
    std::vector<ParsedSceneRecord> parsed_scenes;
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

struct SymbolQueryFilter {
    std::string symbol_kind;
    std::string search;
};

struct ReferenceQueryFilter {
    std::optional<int64_t> script_file_id;
    std::string reference_kind;
};

struct SceneAttachmentQueryFilter {
    std::optional<int64_t> scene_file_id;
    std::optional<int64_t> script_file_id;
};

struct DocCommentGapFilter {
    std::string symbol_kind;
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

struct ScriptDependencyRow {
    int64_t id = 0;
    int64_t project_id = 0;
    int64_t source_script_file_id = 0;
    std::optional<int64_t> source_symbol_id;
    std::optional<int64_t> target_file_id;
    std::string target_project_relative_path;
    std::string target_class_name;
    std::string target_resource_uid;
    std::string dependency_kind;
    std::string reference_text;
    int64_t source_line = 0;
    int64_t source_column = 0;
    double confidence = 0.0;
    bool is_dynamic = false;
    bool is_resolved = false;
    int64_t parser_version = 0;
    ScanGeneration scan_generation = 0;
    int64_t created_at_unix = 0;
};

struct ScriptSymbolRow {
    int64_t id = 0;
    int64_t project_id = 0;
    int64_t script_file_id = 0;
    int64_t symbol_slot = 0;
    std::optional<int64_t> parent_symbol_slot;
    std::optional<int64_t> parent_symbol_id;
    std::string symbol_kind;
    std::string name;
    std::string qualified_name;
    std::string declared_type;
    std::string return_type;
    std::string default_value_excerpt;
    std::string visibility;
    int64_t flags = 0;
    std::string doc_comment_state;
    std::string symbol_name;
    std::string class_name;
    std::string language;
    std::string signature_text;
    int64_t symbol_flags = 0;
    int64_t line_start = 0;
    int64_t column_start = 0;
    int64_t line_end = 0;
    int64_t column_end = 0;
    int64_t parser_version = 0;
    ScanGeneration last_parsed_generation = 0;
    int64_t last_seen_scan_run_id = 0;
    int64_t created_at_unix = 0;
    int64_t updated_at_unix = 0;
};

struct ScriptReferenceRow {
    int64_t id = 0;
    int64_t project_id = 0;
    int64_t script_file_id = 0;
    int64_t source_script_file_id = 0;
    std::optional<int64_t> source_symbol_id;
    std::optional<int64_t> target_file_id;
    std::optional<int64_t> target_symbol_id;
    std::string target_project_relative_path;
    std::string target_class_name;
    std::string target_symbol_name;
    std::string target_resource_uid;
    std::string reference_kind;
    std::string reference_text;
    int64_t source_line = 0;
    int64_t source_column = 0;
    int64_t source_line_end = 0;
    int64_t source_column_end = 0;
    double confidence = 0.0;
    bool is_dynamic = false;
    bool is_resolved = false;
    bool is_unresolved = false;
    int64_t parser_version = 0;
    ScanGeneration scan_generation = 0;
    int64_t created_at_unix = 0;
};

struct ScriptDocCommentRow {
    int64_t id = 0;
    int64_t project_id = 0;
    int64_t script_file_id = 0;
    std::optional<int64_t> target_symbol_id;
    std::string target_kind;
    std::string text_hash;
    std::string text_excerpt;
    std::optional<int64_t> symbol_id;
    std::string comment_style;
    std::string comment_text;
    std::string summary_text;
    int64_t start_line = 0;
    int64_t end_line = 0;
    bool is_attached = false;
    int64_t line_start = 0;
    int64_t column_start = 0;
    int64_t line_end = 0;
    int64_t column_end = 0;
    int64_t parser_version = 0;
    ScanGeneration scan_generation = 0;
    int64_t created_at_unix = 0;
};

struct SceneScriptAttachmentRow {
    int64_t id = 0;
    int64_t project_id = 0;
    int64_t scene_file_id = 0;
    std::string node_path;
    std::string node_name;
    std::string node_type;
    std::string attachment_kind;
    std::string ext_resource_id;
    std::string ext_resource_slot;
    std::string script_resource_path;
    std::string script_uid;
    std::string script_project_relative_path;
    std::string script_resource_uid;
    std::optional<int64_t> script_file_id;
    std::optional<int64_t> script_symbol_id;
    bool is_dynamic = false;
    bool is_resolved = false;
    int64_t source_line = 0;
    int64_t source_column = 0;
    int64_t parser_version = 0;
    ScanGeneration scan_generation = 0;
    int64_t created_at_unix = 0;
};

struct ScriptIntelligenceSummaryRow {
    int64_t script_file_id = 0;
    int64_t symbol_count = 0;
    int64_t function_count = 0;
    int64_t property_count = 0;
    int64_t parameter_count = 0;
    int64_t doc_comment_count = 0;
    int64_t reference_count = 0;
    int64_t unresolved_reference_count = 0;
    int64_t dynamic_reference_count = 0;
};

struct DependencyCycleRow {
    int64_t source_script_file_id = 0;
    int64_t cycle_to_script_file_id = 0;
    std::string cycle_path;
    int64_t hop_count = 0;
};

struct TransparentStringHash {
    using is_transparent = void;

    size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view> {}(value);
    }

    size_t operator()(const std::string &value) const noexcept {
        return operator()(std::string_view(value));
    }
};

struct TransparentStringEqual {
    using is_transparent = void;

    bool operator()(std::string_view left, std::string_view right) const noexcept {
        return left == right;
    }

    bool operator()(const std::string &left, const std::string &right) const noexcept {
        return left == right;
    }
};

using ExistingEntryMap =
    std::unordered_map<std::string, ExistingEntrySnapshot, TransparentStringHash, TransparentStringEqual>;

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
        int64_t finished_at_unix,
        const std::string &error_message = ""
    );
    ExistingEntryMap load_existing_entries(int64_t project_id);
    void write_scan_results(
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
    std::vector<ScriptDependencyRow> list_dependencies_for_script(int64_t project_id, int64_t script_file_id) const;
    std::vector<ScriptDependencyRow> list_dependents_of_file(int64_t project_id, int64_t target_file_id) const;
    std::vector<ScriptDependencyRow> list_dependents_of_class(int64_t project_id, const std::string &class_name) const;
    std::vector<ScriptDependencyRow> list_unresolved_dependencies(int64_t project_id) const;
    std::vector<ScriptDependencyRow> list_dynamic_dependencies(int64_t project_id) const;
    std::vector<ScriptSymbolRow> list_symbols_for_script(
        int64_t project_id,
        int64_t script_file_id,
        const SymbolQueryFilter &filter
    ) const;
    std::vector<ScriptSymbolRow> list_functions_for_script(int64_t project_id, int64_t script_file_id) const;
    std::vector<ScriptSymbolRow> list_properties_for_script(int64_t project_id, int64_t script_file_id) const;
    std::vector<ScriptSymbolRow> list_parameters_for_function(int64_t project_id, int64_t function_symbol_id) const;
    std::vector<ScriptSymbolRow> list_doc_comment_gaps(int64_t project_id, const DocCommentGapFilter &filter) const;
    std::vector<ScriptReferenceRow> list_references_for_script(int64_t project_id, int64_t script_file_id) const;
    std::vector<ScriptReferenceRow> list_references_from_symbol(int64_t project_id, int64_t symbol_id) const;
    std::vector<ScriptReferenceRow> list_unresolved_references(
        int64_t project_id,
        const ReferenceQueryFilter &filter
    ) const;
    std::vector<ScriptReferenceRow> list_dynamic_references(
        int64_t project_id,
        const ReferenceQueryFilter &filter
    ) const;
    std::vector<SceneScriptAttachmentRow> list_scene_script_attachments(
        int64_t project_id,
        const SceneAttachmentQueryFilter &filter
    ) const;
    std::vector<SceneScriptAttachmentRow> list_scenes_using_script(int64_t project_id, int64_t script_file_id) const;
    std::vector<SceneScriptAttachmentRow> list_scripts_attached_to_scene(int64_t project_id, int64_t scene_file_id) const;
    std::optional<ScriptSymbolRow> get_symbol_details(int64_t project_id, int64_t symbol_id) const;
    ScriptIntelligenceSummaryRow get_script_intelligence_summary(int64_t project_id, int64_t script_file_id) const;
    std::vector<DependencyCycleRow> list_dependency_cycles(int64_t project_id) const;
    std::vector<ScriptDependencyRow> get_dependency_graph_slice(
        int64_t project_id,
        int64_t root_script_file_id,
        int64_t depth
    ) const;
    ScanMetrics get_scan_metrics(int64_t project_id, int64_t scan_run_id) const;
    std::string get_scan_status(int64_t project_id, int64_t scan_run_id) const;

private:
    gotool::database::Database *database_ = nullptr;
};

class NativeScanPipeline {
public:
    NativeScanPipeline() = default;
    explicit NativeScanPipeline(gotool::database::Database &database);

    NativeScanResult run_detailed(const ScanOptions &options);
    ScanResultSummary run(const ScanOptions &options);

private:
    gotool::database::Database *database_ = nullptr;
};

} // namespace gotool::project_scanner
