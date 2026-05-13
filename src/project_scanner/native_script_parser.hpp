#pragma once

#include "project_scanner/native_scan_rules.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gotool::project_scanner {

struct ScriptDependencyRecord {
    DependencyKind dependency_kind = DependencyKind::Unknown;
    std::string reference_text;
    int64_t source_line = 0;
    int64_t source_column = 0;
    double confidence = 0.0;
    bool is_dynamic = false;
    std::optional<std::string> target_project_relative_path;
    std::optional<std::string> target_class_name;
    std::optional<std::string> target_resource_uid;
};

struct ScriptParseResult {
    ScriptLanguage language = ScriptLanguage::Unknown;
    ParseStatus status = ParseStatus::NotParsed;
    std::string class_name;
    std::string direct_base_type;
    std::string parse_error;
    int64_t bytes_read = 0;
    int64_t lines_scanned = 0;
    int64_t tokens_generated = 0;
    int64_t tokenizer_ms = 0;
    int64_t dependency_parse_ms = 0;
    bool limit_exceeded = false;
    std::vector<ScriptDependencyRecord> dependencies;
};

ScriptParseResult parse_script_header(
    const std::filesystem::path &absolute_path,
    std::string_view extension,
    int64_t max_lines = 240,
    int64_t max_bytes = 64 * 1024,
    int64_t max_tokens = 16 * 1024,
    int64_t max_dependencies = 2 * 1024
);

} // namespace gotool::project_scanner
