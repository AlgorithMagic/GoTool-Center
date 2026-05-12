#pragma once

#include "project_scanner/native_scan_rules.hpp"

#include <filesystem>
#include <string>

namespace gotool::project_scanner {

struct ScriptParseResult {
    ScriptLanguage language = ScriptLanguage::Unknown;
    ParseStatus status = ParseStatus::NotParsed;
    std::string class_name;
    std::string direct_base_type;
    std::string parse_error;
    int64_t bytes_read = 0;
    int64_t lines_scanned = 0;
};

ScriptParseResult parse_script_header(
    const std::filesystem::path &absolute_path,
    std::string_view extension,
    int64_t max_lines = 240,
    int64_t max_bytes = 64 * 1024
);

} // namespace gotool::project_scanner
