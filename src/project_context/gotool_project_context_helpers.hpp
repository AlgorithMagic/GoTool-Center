// Copyright 2026 AlgorithMagic

#pragma once

#include "database/gotool_project_registry_repository.hpp"
#include "project_scanner/native_scan_pipeline.hpp"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace godot {

struct ParsedAutoload {
  std::string autoload_name;
  std::string target_path;
  bool is_singleton = false;
};

Dictionary to_project_dictionary(const gotool::database::ProjectListItem &item);

gotool::project_scanner::FileQuery
file_query_from_dictionary(const Dictionary &filter);

gotool::project_scanner::CustomClassQuery
custom_class_query_from_dictionary(const Dictionary &filter);

gotool::project_scanner::SymbolQueryFilter
symbol_query_filter_from_dictionary(const Dictionary &filter);

gotool::project_scanner::ReferenceQueryFilter
reference_query_filter_from_dictionary(const Dictionary &filter);

gotool::project_scanner::SceneAttachmentQueryFilter
scene_attachment_filter_from_dictionary(const Dictionary &filter);

gotool::project_scanner::DocCommentGapFilter
doc_comment_gap_filter_from_dictionary(const Dictionary &filter);

Dictionary file_row_to_dictionary(const gotool::project_scanner::FileRow &row);

Dictionary custom_class_row_to_dictionary(
    const gotool::project_scanner::CustomClassRow &row);

Dictionary dependency_row_to_dictionary(
    const gotool::project_scanner::ScriptDependencyRow &row);

Dictionary dependency_cycle_row_to_dictionary(
    const gotool::project_scanner::DependencyCycleRow &row);

Dictionary
symbol_row_to_dictionary(const gotool::project_scanner::ScriptSymbolRow &row);

Dictionary reference_row_to_dictionary(
    const gotool::project_scanner::ScriptReferenceRow &row);

Dictionary scene_attachment_row_to_dictionary(
    const gotool::project_scanner::SceneScriptAttachmentRow &row);

Dictionary script_intelligence_summary_row_to_dictionary(
    const gotool::project_scanner::ScriptIntelligenceSummaryRow &row);

Dictionary
metrics_to_dictionary(const gotool::project_scanner::ScanMetrics &metrics);

Dictionary scan_summary_to_dictionary(
    const gotool::project_scanner::ScanResultSummary &summary);

std::vector<ParsedAutoload>
parse_project_autoloads(const std::filesystem::path &project_root);

} // namespace godot