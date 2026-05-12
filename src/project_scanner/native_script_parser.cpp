#include "project_scanner/native_script_parser.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace gotool::project_scanner {

namespace {

std::string trim(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

bool starts_with_keyword(std::string_view value, std::string_view keyword) {
    if (value.size() < keyword.size() || value.substr(0, keyword.size()) != keyword) {
        return false;
    }

    return value.size() == keyword.size() ||
           std::isspace(static_cast<unsigned char>(value[keyword.size()]));
}

std::string strip_gdscript_comment(std::string_view line) {
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    std::string output;
    output.reserve(line.size());

    for (const char ch : line) {
        if (escaped) {
            output.push_back(ch);
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            output.push_back(ch);
            escaped = true;
            continue;
        }

        if (ch == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            output.push_back(ch);
            continue;
        }

        if (ch == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            output.push_back(ch);
            continue;
        }

        if (ch == '#' && !in_single_quote && !in_double_quote) {
            break;
        }

        output.push_back(ch);
    }

    return output;
}

std::string strip_csharp_line_comment(std::string_view line) {
    bool in_string = false;
    bool in_char = false;
    bool escaped = false;
    std::string output;
    output.reserve(line.size());

    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];

        if (escaped) {
            output.push_back(ch);
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            output.push_back(ch);
            escaped = true;
            continue;
        }

        if (ch == '"' && !in_char) {
            in_string = !in_string;
            output.push_back(ch);
            continue;
        }

        if (ch == '\'' && !in_string) {
            in_char = !in_char;
            output.push_back(ch);
            continue;
        }

        if (!in_string && !in_char && ch == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            break;
        }

        output.push_back(ch);
    }

    return output;
}

std::string first_token(std::string_view value) {
    const std::string trimmed = trim(value);
    size_t end = 0;
    while (end < trimmed.size() && !std::isspace(static_cast<unsigned char>(trimmed[end])) &&
           trimmed[end] != '{' && trimmed[end] != ',' && trimmed[end] != ':') {
        ++end;
    }

    std::string token = trimmed.substr(0, end);
    if (token.size() >= 2 &&
        ((token.front() == '"' && token.back() == '"') || (token.front() == '\'' && token.back() == '\''))) {
        return "ScriptPath";
    }

    return token;
}

std::vector<std::string> split_tokens(std::string value) {
    for (char &ch : value) {
        if (ch == '{' || ch == ':' || ch == ',' || ch == '<' || ch == '>' || ch == '(' || ch == ')') {
            ch = ' ';
        }
    }

    std::istringstream input(value);
    std::vector<std::string> tokens;
    std::string token;
    while (input >> token) {
        tokens.push_back(token);
    }

    return tokens;
}

ScriptParseResult parse_gdscript(std::ifstream &input, int64_t max_lines, int64_t max_bytes) {
    ScriptParseResult result;
    result.language = ScriptLanguage::GDScript;
    result.status = ParseStatus::NoClass;

    std::string line;
    while (result.lines_scanned < max_lines && result.bytes_read < max_bytes && std::getline(input, line)) {
        result.bytes_read += static_cast<int64_t>(line.size()) + 1;
        ++result.lines_scanned;

        const std::string cleaned = trim(strip_gdscript_comment(line));
        if (cleaned.empty()) {
            continue;
        }

        if (result.class_name.empty() && starts_with_keyword(cleaned, "class_name")) {
            result.class_name = first_token(std::string_view(cleaned).substr(10));
        } else if (result.direct_base_type.empty() && starts_with_keyword(cleaned, "extends")) {
            result.direct_base_type = first_token(std::string_view(cleaned).substr(7));
        }

        if (!result.class_name.empty() && !result.direct_base_type.empty()) {
            break;
        }
    }

    if (!result.class_name.empty()) {
        result.status = ParseStatus::ParsedClass;
    }

    return result;
}

ScriptParseResult parse_csharp(std::ifstream &input, int64_t max_lines, int64_t max_bytes) {
    ScriptParseResult result;
    result.language = ScriptLanguage::CSharp;
    result.status = ParseStatus::NoClass;

    bool saw_global_class = false;
    bool in_block_comment = false;
    std::string line;

    while (result.lines_scanned < max_lines && result.bytes_read < max_bytes && std::getline(input, line)) {
        result.bytes_read += static_cast<int64_t>(line.size()) + 1;
        ++result.lines_scanned;

        std::string cleaned = strip_csharp_line_comment(line);

        if (in_block_comment) {
            const size_t close = cleaned.find("*/");
            if (close == std::string::npos) {
                continue;
            }
            cleaned.erase(0, close + 2);
            in_block_comment = false;
        }

        const size_t open = cleaned.find("/*");
        if (open != std::string::npos) {
            const size_t close = cleaned.find("*/", open + 2);
            if (close == std::string::npos) {
                cleaned.erase(open);
                in_block_comment = true;
            } else {
                cleaned.erase(open, close + 2 - open);
            }
        }

        cleaned = trim(cleaned);
        if (cleaned.empty()) {
            continue;
        }

        if (cleaned.find("[GlobalClass]") != std::string::npos ||
            cleaned.find("[GlobalClassAttribute]") != std::string::npos) {
            saw_global_class = true;
        }

        if (!saw_global_class || cleaned.find("class") == std::string::npos) {
            continue;
        }

        std::string token_line = cleaned;
        std::replace(token_line.begin(), token_line.end(), ':', ' ');
        const std::vector<std::string> tokens = split_tokens(token_line);

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == "class" && i + 1 < tokens.size()) {
                result.class_name = tokens[i + 1];
                if (i + 2 < tokens.size()) {
                    result.direct_base_type = tokens[i + 2];
                }
                break;
            }
        }

        if (!result.class_name.empty()) {
            result.status = ParseStatus::ParsedClass;
            break;
        }
    }

    return result;
}

} // namespace

ScriptParseResult parse_script_header(
    const std::filesystem::path &absolute_path,
    std::string_view extension,
    int64_t max_lines,
    int64_t max_bytes
) {
    ScriptParseResult result;
    result.language = language_from_extension(extension);

    if (result.language == ScriptLanguage::Unknown) {
        result.status = ParseStatus::UnsupportedLanguage;
        return result;
    }

    std::ifstream input(absolute_path, std::ios::binary);
    if (!input.is_open()) {
        result.status = ParseStatus::IoError;
        result.parse_error = "failed_to_open";
        return result;
    }

    if (result.language == ScriptLanguage::GDScript) {
        return parse_gdscript(input, max_lines, max_bytes);
    }

    return parse_csharp(input, max_lines, max_bytes);
}

} // namespace gotool::project_scanner
