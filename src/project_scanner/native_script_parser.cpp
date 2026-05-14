#include "project_scanner/native_script_parser.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gotool::project_scanner {

namespace {

enum class TokenKind : uint8_t {
    Identifier,
    StringLiteral,
    NumberLiteral,
    Symbol
};

struct Token {
    TokenKind kind = TokenKind::Symbol;
    std::string_view text;
    int64_t line = 0;
    int64_t column = 0;
};

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool equals_case_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }

    return true;
}

bool is_identifier_start(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool is_identifier_body(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool is_triple_quote(std::string_view value, size_t index, char quote) {
    return index + 2 < value.size() &&
           value[index] == quote &&
           value[index + 1] == quote &&
           value[index + 2] == quote;
}

bool token_is_identifier(const Token &token, std::string_view expected) {
    return token.kind == TokenKind::Identifier && token.text == expected;
}

bool token_is_identifier_ci(const Token &token, std::string_view expected) {
    return token.kind == TokenKind::Identifier && equals_case_insensitive(token.text, expected);
}

bool token_is_symbol(const Token &token, std::string_view expected) {
    return token.kind == TokenKind::Symbol && token.text == expected;
}

std::string strip_quotes(std::string_view literal) {
    if (literal.size() >= 6 && starts_with(literal, "\"\"\"") &&
        literal.substr(literal.size() - 3) == "\"\"\"") {
        return std::string(literal.substr(3, literal.size() - 6));
    }

    if (literal.size() >= 6 && starts_with(literal, "'''") &&
        literal.substr(literal.size() - 3) == "'''") {
        return std::string(literal.substr(3, literal.size() - 6));
    }

    if (literal.size() >= 2 &&
        ((literal.front() == '"' && literal.back() == '"') ||
         (literal.front() == '\'' && literal.back() == '\''))) {
        return std::string(literal.substr(1, literal.size() - 2));
    }

    return std::string(literal);
}

bool is_primitive_type_name(std::string_view type_name) {
    if (type_name.empty()) {
        return true;
    }

    const std::string lowered = lower_ascii(type_name);
    static const std::unordered_set<std::string> primitive_names = {
        "void", "bool", "boolean", "int", "int32", "int64", "float", "double", "decimal",
        "short", "ushort", "byte", "sbyte", "uint", "uint32", "long", "ulong",
        "string", "stringname", "char", "var", "variant", "object", "nint", "nuint",
        "array", "dictionary", "nodepath", "callable", "signal"
    };

    return primitive_names.find(lowered) != primitive_names.end();
}

std::optional<std::string> normalized_res_path_from_string_literal(std::string_view literal) {
    const std::string value = strip_quotes(literal);
    if (!starts_with(value, "res://")) {
        return std::nullopt;
    }

    const std::string normalized = normalize_project_path(value);
    if (normalized.empty()) {
        return std::nullopt;
    }

    return normalized;
}

std::optional<std::string> uid_from_string_literal(std::string_view literal) {
    const std::string value = strip_quotes(literal);
    if (starts_with(value, "uid://")) {
        return value;
    }
    return std::nullopt;
}

void add_dependency(
    ScriptParseResult &result,
    ScriptDependencyRecord dependency,
    int64_t max_dependencies
) {
    if (static_cast<int64_t>(result.dependencies.size()) >= max_dependencies) {
        result.limit_exceeded = true;
        return;
    }

    result.dependencies.push_back(std::move(dependency));
}

void add_class_dependency(
    ScriptParseResult &result,
    DependencyKind dependency_kind,
    const Token &anchor,
    std::string target_class_name,
    int64_t max_dependencies,
    double confidence = 0.90
) {
    if (target_class_name.empty()) {
        return;
    }

    ScriptDependencyRecord dependency;
    dependency.dependency_kind = dependency_kind;
    dependency.reference_text = target_class_name;
    dependency.range.line_start = anchor.line;
    dependency.range.column_start = anchor.column;
    dependency.range.line_end = anchor.line;
    dependency.range.column_end = anchor.column;
    dependency.confidence = confidence;
    dependency.target_symbol_name = target_class_name;
    dependency.target_class_name = std::move(target_class_name);
    add_dependency(result, std::move(dependency), max_dependencies);
}

void add_path_dependency(
    ScriptParseResult &result,
    DependencyKind dependency_kind,
    const Token &anchor,
    std::string target_path,
    int64_t max_dependencies,
    double confidence = 1.0
) {
    if (target_path.empty()) {
        return;
    }

    ScriptDependencyRecord dependency;
    dependency.dependency_kind = dependency_kind;
    dependency.reference_text = "res://" + target_path;
    dependency.range.line_start = anchor.line;
    dependency.range.column_start = anchor.column;
    dependency.range.line_end = anchor.line;
    dependency.range.column_end = anchor.column;
    dependency.confidence = confidence;
    dependency.target_project_relative_path = std::move(target_path);
    add_dependency(result, std::move(dependency), max_dependencies);
}

void add_dynamic_load_dependency(
    ScriptParseResult &result,
    const Token &anchor,
    std::string reference_text,
    int64_t max_dependencies
) {
    ScriptDependencyRecord dependency;
    dependency.dependency_kind = DependencyKind::DynamicLoad;
    dependency.reference_text = std::move(reference_text);
    dependency.range.line_start = anchor.line;
    dependency.range.column_start = anchor.column;
    dependency.range.line_end = anchor.line;
    dependency.range.column_end = anchor.column;
    dependency.confidence = 0.35;
    dependency.is_dynamic = true;
    add_dependency(result, std::move(dependency), max_dependencies);
}

void add_uid_dependency(
    ScriptParseResult &result,
    const Token &anchor,
    std::string uid,
    int64_t max_dependencies
) {
    ScriptDependencyRecord dependency;
    dependency.dependency_kind = DependencyKind::ResourceUIDRef;
    dependency.reference_text = uid;
    dependency.range.line_start = anchor.line;
    dependency.range.column_start = anchor.column;
    dependency.range.line_end = anchor.line;
    dependency.range.column_end = anchor.column;
    dependency.confidence = 1.0;
    dependency.target_resource_uid = std::move(uid);
    add_dependency(result, std::move(dependency), max_dependencies);
}

void add_scene_node_path_dependency(
    ScriptParseResult &result,
    const Token &anchor,
    std::string value,
    int64_t max_dependencies
) {
    ScriptDependencyRecord dependency;
    dependency.dependency_kind = DependencyKind::SceneNodePath;
    dependency.reference_text = std::move(value);
    dependency.range.line_start = anchor.line;
    dependency.range.column_start = anchor.column;
    dependency.range.line_end = anchor.line;
    dependency.range.column_end = anchor.column;
    dependency.confidence = 0.95;
    add_dependency(result, std::move(dependency), max_dependencies);
}

std::string strip_gdscript_comments_preserving_strings(std::string_view source) {
    std::string output(source);
    bool in_single = false;
    bool in_double = false;
    bool in_triple_single = false;
    bool in_triple_double = false;
    bool escaped = false;

    size_t index = 0;
    while (index < source.size()) {
        const char ch = source[index];

        if (in_triple_single) {
            if (is_triple_quote(source, index, '\'')) {
                in_triple_single = false;
                index += 3;
                continue;
            }
            ++index;
            continue;
        }

        if (in_triple_double) {
            if (is_triple_quote(source, index, '"')) {
                in_triple_double = false;
                index += 3;
                continue;
            }
            ++index;
            continue;
        }

        if (in_single || in_double) {
            if (escaped) {
                escaped = false;
                ++index;
                continue;
            }

            if (ch == '\\') {
                escaped = true;
                ++index;
                continue;
            }

            if ((in_single && ch == '\'') || (in_double && ch == '"')) {
                in_single = false;
                in_double = false;
            }

            ++index;
            continue;
        }

        if (is_triple_quote(source, index, '\'')) {
            in_triple_single = true;
            index += 3;
            continue;
        }

        if (is_triple_quote(source, index, '"')) {
            in_triple_double = true;
            index += 3;
            continue;
        }

        if (ch == '\'') {
            in_single = true;
            ++index;
            continue;
        }

        if (ch == '"') {
            in_double = true;
            ++index;
            continue;
        }

        if (ch == '#') {
            while (index < source.size() && source[index] != '\n') {
                output[index] = ' ';
                ++index;
            }
            continue;
        }

        ++index;
    }

    return output;
}

std::string strip_csharp_comments_preserving_strings(std::string_view source) {
    std::string output(source);
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_regular_string = false;
    bool in_verbatim_string = false;
    bool in_char_literal = false;
    bool escaped = false;

    size_t index = 0;
    while (index < source.size()) {
        const char ch = source[index];
        const char next = index + 1 < source.size() ? source[index + 1] : '\0';

        if (in_line_comment) {
            if (ch == '\n') {
                in_line_comment = false;
                ++index;
                continue;
            }

            output[index] = ' ';
            ++index;
            continue;
        }

        if (in_block_comment) {
            if (ch == '*' && next == '/') {
                output[index] = ' ';
                output[index + 1] = ' ';
                in_block_comment = false;
                index += 2;
                continue;
            }

            if (ch != '\n') {
                output[index] = ' ';
            }
            ++index;
            continue;
        }

        if (in_regular_string) {
            if (escaped) {
                escaped = false;
                ++index;
                continue;
            }

            if (ch == '\\') {
                escaped = true;
                ++index;
                continue;
            }

            if (ch == '"') {
                in_regular_string = false;
            }

            ++index;
            continue;
        }

        if (in_verbatim_string) {
            if (ch == '"' && next == '"') {
                index += 2;
                continue;
            }

            if (ch == '"') {
                in_verbatim_string = false;
            }

            ++index;
            continue;
        }

        if (in_char_literal) {
            if (escaped) {
                escaped = false;
                ++index;
                continue;
            }

            if (ch == '\\') {
                escaped = true;
                ++index;
                continue;
            }

            if (ch == '\'') {
                in_char_literal = false;
            }

            ++index;
            continue;
        }

        if (ch == '/' && next == '/') {
            output[index] = ' ';
            output[index + 1] = ' ';
            in_line_comment = true;
            index += 2;
            continue;
        }

        if (ch == '/' && next == '*') {
            output[index] = ' ';
            output[index + 1] = ' ';
            in_block_comment = true;
            index += 2;
            continue;
        }

        if (ch == '@' && next == '"') {
            in_verbatim_string = true;
            index += 2;
            continue;
        }

        if (ch == '"') {
            in_regular_string = true;
            ++index;
            continue;
        }

        if (ch == '\'') {
            in_char_literal = true;
            ++index;
            continue;
        }

        ++index;
    }

    return output;
}

std::string read_file_with_limit(const std::filesystem::path &absolute_path, int64_t max_bytes, bool &limit_exceeded) {
    std::ifstream input(absolute_path, std::ios::binary);
    if (!input.is_open()) {
        return "";
    }

    std::string contents;
    const int64_t bounded_max = std::max<int64_t>(max_bytes, 1);
    contents.reserve(static_cast<size_t>(std::min<int64_t>(bounded_max, 256 * 1024)));

    static constexpr size_t BUFFER_SIZE = 8 * 1024;
    char buffer[BUFFER_SIZE];

    while (input.good()) {
        input.read(buffer, static_cast<std::streamsize>(BUFFER_SIZE));
        const std::streamsize read_count = input.gcount();
        if (read_count <= 0) {
            break;
        }

        const int64_t remaining = bounded_max - static_cast<int64_t>(contents.size());
        if (remaining <= 0) {
            limit_exceeded = true;
            break;
        }

        if (read_count > remaining) {
            contents.append(buffer, static_cast<size_t>(remaining));
            limit_exceeded = true;
            break;
        }

        contents.append(buffer, static_cast<size_t>(read_count));
    }

    return contents;
}

std::vector<Token> tokenize_source(
    std::string_view source,
    ScriptLanguage language,
    int64_t max_lines,
    int64_t max_tokens,
    bool &limit_exceeded,
    int64_t &lines_scanned
) {
    std::vector<Token> tokens;
    if (max_tokens <= 0) {
        limit_exceeded = true;
        lines_scanned = 0;
        return tokens;
    }

    tokens.reserve(static_cast<size_t>(std::min<int64_t>(max_tokens, 4096)));

    int64_t line = 1;
    int64_t column = 1;
    size_t index = 0;

    const auto push_token = [&](TokenKind kind, size_t start, size_t length, int64_t token_line, int64_t token_col) -> bool {
        if (static_cast<int64_t>(tokens.size()) >= max_tokens) {
            return false;
        }
        tokens.push_back(Token { kind, source.substr(start, length), token_line, token_col });
        return true;
    };

    while (index < source.size()) {
        if (line > max_lines) {
            limit_exceeded = true;
            break;
        }

        const char ch = source[index];

        if (ch == '\r') {
            ++index;
            continue;
        }

        if (ch == '\n') {
            ++line;
            column = 1;
            ++index;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            ++index;
            ++column;
            continue;
        }

        if (is_identifier_start(ch)) {
            const size_t start = index;
            const int64_t token_line = line;
            const int64_t token_col = column;
            ++index;
            ++column;

            while (index < source.size() && is_identifier_body(source[index])) {
                ++index;
                ++column;
            }

            if (!push_token(TokenKind::Identifier, start, index - start, token_line, token_col)) {
                limit_exceeded = true;
                break;
            }
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            const size_t start = index;
            const int64_t token_line = line;
            const int64_t token_col = column;
            ++index;
            ++column;

            while (index < source.size() &&
                   (std::isalnum(static_cast<unsigned char>(source[index])) != 0 || source[index] == '_')) {
                ++index;
                ++column;
            }

            if (!push_token(TokenKind::NumberLiteral, start, index - start, token_line, token_col)) {
                limit_exceeded = true;
                break;
            }
            continue;
        }

        if (ch == '"' || ch == '\'') {
            const size_t start = index;
            const int64_t token_line = line;
            const int64_t token_col = column;
            const char quote = ch;
            bool closed = false;

            if (language == ScriptLanguage::GDScript && is_triple_quote(source, index, quote)) {
                index += 3;
                column += 3;
                while (index < source.size()) {
                    if (source[index] == '\n') {
                        ++line;
                        column = 1;
                        ++index;
                        continue;
                    }

                    if (is_triple_quote(source, index, quote)) {
                        index += 3;
                        column += 3;
                        closed = true;
                        break;
                    }

                    ++index;
                    ++column;
                }
            } else {
                const bool verbatim_csharp_string =
                    language == ScriptLanguage::CSharp &&
                    start > 0 &&
                    source[start - 1] == '@';

                bool escaped = false;
                ++index;
                ++column;

                while (index < source.size()) {
                    const char current = source[index];

                    if (current == '\n') {
                        ++line;
                        column = 1;
                        ++index;
                        if (verbatim_csharp_string) {
                            continue;
                        }
                        break;
                    }

                    if (verbatim_csharp_string) {
                        if (current == '"' && index + 1 < source.size() && source[index + 1] == '"') {
                            index += 2;
                            column += 2;
                            continue;
                        }

                        if (current == quote) {
                            ++index;
                            ++column;
                            closed = true;
                            break;
                        }

                        ++index;
                        ++column;
                        continue;
                    }

                    if (escaped) {
                        escaped = false;
                        ++index;
                        ++column;
                        continue;
                    }

                    if (current == '\\') {
                        escaped = true;
                        ++index;
                        ++column;
                        continue;
                    }

                    if (current == quote) {
                        ++index;
                        ++column;
                        closed = true;
                        break;
                    }

                    ++index;
                    ++column;
                }
            }

            if (!closed) {
                limit_exceeded = true;
            }

            if (!push_token(TokenKind::StringLiteral, start, index - start, token_line, token_col)) {
                limit_exceeded = true;
                break;
            }
            continue;
        }

        const size_t start = index;
        const int64_t token_line = line;
        const int64_t token_col = column;

        if (index + 1 < source.size()) {
            const char next = source[index + 1];
            if ((ch == '-' && next == '>') || (ch == ':' && next == ':')) {
                index += 2;
                column += 2;
                if (!push_token(TokenKind::Symbol, start, 2, token_line, token_col)) {
                    limit_exceeded = true;
                    break;
                }
                continue;
            }
        }

        ++index;
        ++column;
        if (!push_token(TokenKind::Symbol, start, 1, token_line, token_col)) {
            limit_exceeded = true;
            break;
        }
    }

    lines_scanned = std::max<int64_t>(1, std::min<int64_t>(line, max_lines));
    return tokens;
}

void emit_type_dependency(
    const std::vector<Token> &tokens,
    size_t type_index,
    DependencyKind default_kind,
    ScriptParseResult &result,
    int64_t max_dependencies
) {
    if (type_index >= tokens.size() || tokens[type_index].kind != TokenKind::Identifier) {
        return;
    }

    const Token &type_token = tokens[type_index];
    const std::string type_name(type_token.text);
    const std::string lowered = lower_ascii(type_name);
    if (lowered == "array" && type_index + 2 < tokens.size() && token_is_symbol(tokens[type_index + 1], "[")) {
        size_t cursor = type_index + 2;
        while (cursor < tokens.size() && !token_is_symbol(tokens[cursor], "]")) {
            if (tokens[cursor].kind == TokenKind::Identifier &&
                !is_primitive_type_name(tokens[cursor].text)) {
                add_class_dependency(
                    result,
                    DependencyKind::TypedArrayElementRef,
                    tokens[cursor],
                    std::string(tokens[cursor].text),
                    max_dependencies,
                    0.85
                );
                return;
            }
            ++cursor;
        }
        return;
    }

    if (is_primitive_type_name(type_name)) {
        return;
    }

    if (lowered == "dictionary" && type_index + 2 < tokens.size() && token_is_symbol(tokens[type_index + 1], "[")) {
        size_t cursor = type_index + 2;
        bool after_comma = false;
        size_t fallback_identifier = tokens.size();

        while (cursor < tokens.size() && !token_is_symbol(tokens[cursor], "]")) {
            if (tokens[cursor].kind == TokenKind::Identifier &&
                !is_primitive_type_name(tokens[cursor].text)) {
                if (fallback_identifier == tokens.size()) {
                    fallback_identifier = cursor;
                }
                if (after_comma) {
                    add_class_dependency(
                        result,
                        DependencyKind::TypedDictionaryRef,
                        tokens[cursor],
                        std::string(tokens[cursor].text),
                        max_dependencies,
                        0.85
                    );
                    return;
                }
            }

            if (token_is_symbol(tokens[cursor], ",")) {
                after_comma = true;
            }

            ++cursor;
        }

        if (fallback_identifier < tokens.size()) {
            add_class_dependency(
                result,
                DependencyKind::TypedDictionaryRef,
                tokens[fallback_identifier],
                std::string(tokens[fallback_identifier].text),
                max_dependencies,
                0.80
            );
        }
        return;
    }

    add_class_dependency(result, default_kind, type_token, type_name, max_dependencies, 0.90);
}

void extract_gdscript_dependencies(
    const std::vector<Token> &tokens,
    ScriptParseResult &result,
    int64_t max_dependencies
) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token &token = tokens[i];

        if (token.kind == TokenKind::StringLiteral) {
            const std::optional<std::string> uid = uid_from_string_literal(token.text);
            if (uid.has_value()) {
                add_uid_dependency(result, token, uid.value(), max_dependencies);
            }
            continue;
        }

        if (token.kind != TokenKind::Identifier) {
            continue;
        }

        if (token_is_identifier(token, "class_name") && i + 1 < tokens.size() &&
            tokens[i + 1].kind == TokenKind::Identifier) {
            if (result.class_name.empty()) {
                result.class_name = std::string(tokens[i + 1].text);
            }
            add_class_dependency(
                result,
                DependencyKind::ClassNameDeclaration,
                tokens[i + 1],
                std::string(tokens[i + 1].text),
                max_dependencies,
                1.0
            );
            continue;
        }

        if (token_is_identifier(token, "extends") && i + 1 < tokens.size()) {
            const Token &target = tokens[i + 1];
            if (target.kind == TokenKind::StringLiteral) {
                const std::optional<std::string> path = normalized_res_path_from_string_literal(target.text);
                if (path.has_value()) {
                    add_path_dependency(result, DependencyKind::ExtendsPath, target, path.value(), max_dependencies, 1.0);
                    if (result.direct_base_type.empty()) {
                        result.direct_base_type = "ScriptPath";
                    }
                }
            } else if (target.kind == TokenKind::Identifier) {
                add_class_dependency(
                    result,
                    DependencyKind::ExtendsClass,
                    target,
                    std::string(target.text),
                    max_dependencies,
                    0.95
                );
                if (result.direct_base_type.empty()) {
                    result.direct_base_type = std::string(target.text);
                }
            }
            continue;
        }

        if (token_is_identifier(token, "const") && i + 5 < tokens.size() &&
            tokens[i + 1].kind == TokenKind::Identifier &&
            token_is_symbol(tokens[i + 2], "=") &&
            token_is_identifier(tokens[i + 3], "preload") &&
            token_is_symbol(tokens[i + 4], "(") &&
            tokens[i + 5].kind == TokenKind::StringLiteral) {
            const std::optional<std::string> path = normalized_res_path_from_string_literal(tokens[i + 5].text);
            if (path.has_value()) {
                ScriptDependencyRecord dependency;
                dependency.dependency_kind = DependencyKind::ConstPreloadAlias;
                dependency.reference_text = std::string(tokens[i + 1].text);
                dependency.range.line_start = tokens[i + 1].line;
                dependency.range.column_start = tokens[i + 1].column;
                dependency.range.line_end = tokens[i + 1].line;
                dependency.range.column_end = tokens[i + 1].column;
                dependency.confidence = 1.0;
                dependency.target_project_relative_path = path.value();
                add_dependency(result, std::move(dependency), max_dependencies);
            }
            continue;
        }

        if (token_is_identifier(token, "preload") && i + 2 < tokens.size() &&
            token_is_symbol(tokens[i + 1], "(")) {
            const Token &argument = tokens[i + 2];
            if (argument.kind == TokenKind::StringLiteral) {
                const std::optional<std::string> path = normalized_res_path_from_string_literal(argument.text);
                if (path.has_value()) {
                    add_path_dependency(result, DependencyKind::PreloadPath, argument, path.value(), max_dependencies, 1.0);
                }
            } else {
                add_dynamic_load_dependency(result, token, "preload(dynamic)", max_dependencies);
            }
            continue;
        }

        if (token_is_identifier(token, "ResourceLoader") && i + 4 < tokens.size() &&
            token_is_symbol(tokens[i + 1], ".") &&
            token_is_identifier(tokens[i + 2], "load") &&
            token_is_symbol(tokens[i + 3], "(")) {
            const Token &argument = tokens[i + 4];
            if (argument.kind == TokenKind::StringLiteral) {
                const std::optional<std::string> path = normalized_res_path_from_string_literal(argument.text);
                if (path.has_value()) {
                    add_path_dependency(
                        result,
                        DependencyKind::ResourceLoaderLoadPath,
                        argument,
                        path.value(),
                        max_dependencies,
                        1.0
                    );
                }
            } else {
                add_dynamic_load_dependency(result, token, "ResourceLoader.load(dynamic)", max_dependencies);
            }
            continue;
        }

        if (token_is_identifier(token, "load") && i + 2 < tokens.size() && token_is_symbol(tokens[i + 1], "(")) {
            const bool is_member_call = i >= 2 && token_is_symbol(tokens[i - 1], ".");
            if (is_member_call) {
                continue;
            }

            const Token &argument = tokens[i + 2];
            if (argument.kind == TokenKind::StringLiteral) {
                const std::optional<std::string> path = normalized_res_path_from_string_literal(argument.text);
                if (path.has_value()) {
                    add_path_dependency(result, DependencyKind::LoadPath, argument, path.value(), max_dependencies, 1.0);
                }
            } else {
                add_dynamic_load_dependency(result, token, "load(dynamic)", max_dependencies);
            }
            continue;
        }

        if (token_is_identifier(token, "var")) {
            size_t cursor = i + 1;
            while (cursor < tokens.size() && tokens[cursor].line == token.line) {
                if (token_is_symbol(tokens[cursor], ":") && cursor + 1 < tokens.size()) {
                    emit_type_dependency(tokens, cursor + 1, DependencyKind::TypedVarRef, result, max_dependencies);
                    break;
                }
                ++cursor;
            }
            continue;
        }

        if (token_is_identifier(token, "func")) {
            size_t cursor = i + 1;
            while (cursor < tokens.size() && !token_is_symbol(tokens[cursor], "(")) {
                ++cursor;
            }

            if (cursor < tokens.size()) {
                size_t parameter_cursor = cursor + 1;
                while (parameter_cursor < tokens.size() && !token_is_symbol(tokens[parameter_cursor], ")")) {
                    if (token_is_symbol(tokens[parameter_cursor], ":") && parameter_cursor + 1 < tokens.size()) {
                        emit_type_dependency(
                            tokens,
                            parameter_cursor + 1,
                            DependencyKind::TypedParamRef,
                            result,
                            max_dependencies
                        );
                    }
                    ++parameter_cursor;
                }

                while (parameter_cursor < tokens.size()) {
                    if (token_is_symbol(tokens[parameter_cursor], "->") && parameter_cursor + 1 < tokens.size()) {
                        emit_type_dependency(
                            tokens,
                            parameter_cursor + 1,
                            DependencyKind::TypedReturnRef,
                            result,
                            max_dependencies
                        );
                        break;
                    }

                    if (tokens[parameter_cursor].line > token.line + 2) {
                        break;
                    }
                    ++parameter_cursor;
                }
            }
            continue;
        }

        if (token_is_identifier(token, "signal")) {
            size_t cursor = i + 1;
            while (cursor < tokens.size() && !token_is_symbol(tokens[cursor], "(")) {
                ++cursor;
            }

            if (cursor < tokens.size()) {
                size_t parameter_cursor = cursor + 1;
                while (parameter_cursor < tokens.size() && !token_is_symbol(tokens[parameter_cursor], ")")) {
                    if (token_is_symbol(tokens[parameter_cursor], ":") && parameter_cursor + 1 < tokens.size()) {
                        emit_type_dependency(
                            tokens,
                            parameter_cursor + 1,
                            DependencyKind::SignalTypeRef,
                            result,
                            max_dependencies
                        );
                    }
                    ++parameter_cursor;
                }
            }
            continue;
        }

        if (token_is_identifier(token, "NodePath") && i + 2 < tokens.size() &&
            token_is_symbol(tokens[i + 1], "(") &&
            tokens[i + 2].kind == TokenKind::StringLiteral) {
            add_scene_node_path_dependency(result, tokens[i + 2], strip_quotes(tokens[i + 2].text), max_dependencies);
            continue;
        }

        if (i + 3 < tokens.size() && token_is_symbol(tokens[i + 1], ".") &&
            token_is_identifier(tokens[i + 2], "new") && token_is_symbol(tokens[i + 3], "(")) {
            if (!is_primitive_type_name(token.text)) {
                add_class_dependency(
                    result,
                    DependencyKind::NewClassInstantiation,
                    token,
                    std::string(token.text),
                    max_dependencies,
                    0.80
                );
            }
            continue;
        }

        if (token_is_identifier(token, "export") && i + 1 < tokens.size()) {
            for (size_t cursor = i + 1; cursor < tokens.size() && tokens[cursor].line <= token.line + 1; ++cursor) {
                if (token_is_symbol(tokens[cursor], ":") && cursor + 1 < tokens.size()) {
                    emit_type_dependency(tokens, cursor + 1, DependencyKind::ExportTypeRef, result, max_dependencies);
                    break;
                }
            }
            continue;
        }

        if (i > 0 && token_is_symbol(tokens[i - 1], "@") && token_is_identifier(token, "export")) {
            for (size_t cursor = i + 1; cursor < tokens.size() && tokens[cursor].line <= token.line + 2; ++cursor) {
                if (token_is_symbol(tokens[cursor], ":") && cursor + 1 < tokens.size()) {
                    emit_type_dependency(tokens, cursor + 1, DependencyKind::ExportTypeRef, result, max_dependencies);
                    break;
                }
            }
            continue;
        }
    }
}

void extract_csharp_dependencies(
    const std::vector<Token> &tokens,
    ScriptParseResult &result,
    int64_t max_dependencies
) {
    bool saw_global_class = false;
    bool pending_export = false;
    bool past_export_attribute = false;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token &token = tokens[i];

        if (token.kind == TokenKind::StringLiteral) {
            const std::optional<std::string> uid = uid_from_string_literal(token.text);
            if (uid.has_value()) {
                add_uid_dependency(result, token, uid.value(), max_dependencies);
            }
            continue;
        }

        if (token.kind == TokenKind::Symbol && token_is_symbol(token, "[") && i + 1 < tokens.size() &&
            tokens[i + 1].kind == TokenKind::Identifier) {
            if (token_is_identifier_ci(tokens[i + 1], "GlobalClass") ||
                token_is_identifier_ci(tokens[i + 1], "GlobalClassAttribute")) {
                saw_global_class = true;
            }

            if (token_is_identifier_ci(tokens[i + 1], "Export") ||
                token_is_identifier_ci(tokens[i + 1], "ExportAttribute")) {
                pending_export = true;
                past_export_attribute = false;
            }
            continue;
        }

        if (token.kind == TokenKind::Symbol && token_is_symbol(token, "]")) {
            if (pending_export) {
                past_export_attribute = true;
            }
            continue;
        }

        if (token.kind != TokenKind::Identifier) {
            continue;
        }

        if (token_is_identifier_ci(token, "class") && i + 1 < tokens.size() &&
            tokens[i + 1].kind == TokenKind::Identifier) {
            if (saw_global_class) {
                if (result.class_name.empty()) {
                    result.class_name = std::string(tokens[i + 1].text);
                }
                add_class_dependency(
                    result,
                    DependencyKind::ClassNameDeclaration,
                    tokens[i + 1],
                    std::string(tokens[i + 1].text),
                    max_dependencies,
                    1.0
                );
            }

            size_t cursor = i + 2;
            if (cursor < tokens.size() && token_is_symbol(tokens[cursor], ":")) {
                ++cursor;
                while (cursor < tokens.size()) {
                    if (token_is_symbol(tokens[cursor], "{")) {
                        break;
                    }

                    if (tokens[cursor].kind == TokenKind::Identifier) {
                        if (result.direct_base_type.empty()) {
                            result.direct_base_type = std::string(tokens[cursor].text);
                        }
                        add_class_dependency(
                            result,
                            DependencyKind::ExtendsClass,
                            tokens[cursor],
                            std::string(tokens[cursor].text),
                            max_dependencies,
                            0.95
                        );
                    }

                    ++cursor;
                }
            }

            continue;
        }

        if (pending_export && past_export_attribute) {
            const std::string lowered = lower_ascii(token.text);
            static const std::unordered_set<std::string> modifiers = {
                "public", "private", "protected", "internal", "static",
                "readonly", "const", "partial", "virtual", "override", "new"
            };

            if (modifiers.find(lowered) == modifiers.end()) {
                emit_type_dependency(tokens, i, DependencyKind::ExportTypeRef, result, max_dependencies);
                pending_export = false;
                past_export_attribute = false;
            }
        }

        if (token_is_identifier_ci(token, "GD") && i + 3 < tokens.size() &&
            token_is_symbol(tokens[i + 1], ".") &&
            token_is_identifier_ci(tokens[i + 2], "Load")) {
            size_t cursor = i + 3;
            if (cursor < tokens.size() && token_is_symbol(tokens[cursor], "<")) {
                ++cursor;
                if (cursor < tokens.size() && tokens[cursor].kind == TokenKind::Identifier) {
                    add_class_dependency(
                        result,
                        DependencyKind::TypedReturnRef,
                        tokens[cursor],
                        std::string(tokens[cursor].text),
                        max_dependencies,
                        0.70
                    );
                }
                while (cursor < tokens.size() && !token_is_symbol(tokens[cursor], ">")) {
                    ++cursor;
                }
                if (cursor < tokens.size()) {
                    ++cursor;
                }
            }

            if (cursor < tokens.size() && token_is_symbol(tokens[cursor], "(") && cursor + 1 < tokens.size()) {
                const Token &argument = tokens[cursor + 1];
                if (argument.kind == TokenKind::StringLiteral) {
                    const std::optional<std::string> path = normalized_res_path_from_string_literal(argument.text);
                    if (path.has_value()) {
                        add_path_dependency(result, DependencyKind::GDLoadPath, argument, path.value(), max_dependencies, 1.0);
                    }
                } else {
                    add_dynamic_load_dependency(result, token, "GD.Load(dynamic)", max_dependencies);
                }
            }
            continue;
        }

        if (token_is_identifier_ci(token, "ResourceLoader") && i + 3 < tokens.size() &&
            token_is_symbol(tokens[i + 1], ".") &&
            token_is_identifier_ci(tokens[i + 2], "Load")) {
            size_t cursor = i + 3;
            if (cursor < tokens.size() && token_is_symbol(tokens[cursor], "<")) {
                ++cursor;
                if (cursor < tokens.size() && tokens[cursor].kind == TokenKind::Identifier) {
                    add_class_dependency(
                        result,
                        DependencyKind::TypedReturnRef,
                        tokens[cursor],
                        std::string(tokens[cursor].text),
                        max_dependencies,
                        0.70
                    );
                }
                while (cursor < tokens.size() && !token_is_symbol(tokens[cursor], ">")) {
                    ++cursor;
                }
                if (cursor < tokens.size()) {
                    ++cursor;
                }
            }

            if (cursor < tokens.size() && token_is_symbol(tokens[cursor], "(") && cursor + 1 < tokens.size()) {
                const Token &argument = tokens[cursor + 1];
                if (argument.kind == TokenKind::StringLiteral) {
                    const std::optional<std::string> path = normalized_res_path_from_string_literal(argument.text);
                    if (path.has_value()) {
                        add_path_dependency(
                            result,
                            DependencyKind::ResourceLoaderLoadPath,
                            argument,
                            path.value(),
                            max_dependencies,
                            1.0
                        );
                    }
                } else {
                    add_dynamic_load_dependency(result, token, "ResourceLoader.Load(dynamic)", max_dependencies);
                }
            }
            continue;
        }

        if (token_is_identifier_ci(token, "PackedScene") && i + 4 < tokens.size() &&
            token_is_symbol(tokens[i + 1], ".") &&
            token_is_identifier_ci(tokens[i + 2], "Instantiate") &&
            token_is_symbol(tokens[i + 3], "<")) {
            if (tokens[i + 4].kind == TokenKind::Identifier && !is_primitive_type_name(tokens[i + 4].text)) {
                add_class_dependency(
                    result,
                    DependencyKind::NewClassInstantiation,
                    tokens[i + 4],
                    std::string(tokens[i + 4].text),
                    max_dependencies,
                    0.90
                );
            }
            continue;
        }
    }
}

void extract_dependencies(
    const std::vector<Token> &tokens,
    ScriptParseResult &result,
    int64_t max_dependencies
) {
    if (result.language == ScriptLanguage::GDScript) {
        extract_gdscript_dependencies(tokens, result, max_dependencies);
        return;
    }

    if (result.language == ScriptLanguage::CSharp) {
        extract_csharp_dependencies(tokens, result, max_dependencies);
    }
}

void fill_class_fields_from_tokens(
    const std::vector<Token> &tokens,
    ScriptParseResult &result
) {
    if (result.language == ScriptLanguage::GDScript) {
        if (result.class_name.empty()) {
            for (size_t i = 0; i + 1 < tokens.size(); ++i) {
                if (token_is_identifier(tokens[i], "class_name") && tokens[i + 1].kind == TokenKind::Identifier) {
                    result.class_name = std::string(tokens[i + 1].text);
                    break;
                }
            }
        }

        if (result.direct_base_type.empty()) {
            for (size_t i = 0; i + 1 < tokens.size(); ++i) {
                if (!token_is_identifier(tokens[i], "extends")) {
                    continue;
                }

                if (tokens[i + 1].kind == TokenKind::Identifier) {
                    result.direct_base_type = std::string(tokens[i + 1].text);
                } else if (tokens[i + 1].kind == TokenKind::StringLiteral) {
                    result.direct_base_type = "ScriptPath";
                }
                break;
            }
        }

        return;
    }

    if (result.language == ScriptLanguage::CSharp) {
        bool saw_global_class = false;
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (token_is_symbol(tokens[i], "[") &&
                (token_is_identifier_ci(tokens[i + 1], "GlobalClass") ||
                 token_is_identifier_ci(tokens[i + 1], "GlobalClassAttribute"))) {
                saw_global_class = true;
            }

            if (saw_global_class && token_is_identifier_ci(tokens[i], "class") &&
                i + 1 < tokens.size() && tokens[i + 1].kind == TokenKind::Identifier) {
                if (result.class_name.empty()) {
                    result.class_name = std::string(tokens[i + 1].text);
                }

                size_t cursor = i + 2;
                while (cursor < tokens.size() && !token_is_symbol(tokens[cursor], ":") &&
                       !token_is_symbol(tokens[cursor], "{")) {
                    ++cursor;
                }

                if (cursor < tokens.size() && token_is_symbol(tokens[cursor], ":")) {
                    ++cursor;
                    while (cursor < tokens.size()) {
                        if (tokens[cursor].kind == TokenKind::Identifier) {
                            if (result.direct_base_type.empty()) {
                                result.direct_base_type = std::string(tokens[cursor].text);
                            }
                            break;
                        }
                        if (token_is_symbol(tokens[cursor], "{")) {
                            break;
                        }
                        ++cursor;
                    }
                }

                break;
            }
        }
    }
}

std::string_view trim_left_ascii_view(std::string_view value) {
    size_t cursor = 0;
    while (cursor < value.size() && std::isspace(static_cast<unsigned char>(value[cursor])) != 0) {
        ++cursor;
    }
    return value.substr(cursor);
}

std::string trim_ascii_copy(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

std::vector<std::string_view> split_lines_view(const std::string &source) {
    std::vector<std::string_view> lines;
    lines.reserve(256);

    size_t start = 0;
    while (start <= source.size()) {
        const size_t newline = source.find('\n', start);
        const size_t end = newline == std::string::npos ? source.size() : newline;
        std::string_view line(source.data() + start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);

        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }

    return lines;
}

bool is_identifier_text(std::string_view value) {
    if (value.empty() || !is_identifier_start(value.front())) {
        return false;
    }

    for (size_t i = 1; i < value.size(); ++i) {
        if (!is_identifier_body(value[i])) {
            return false;
        }
    }

    return true;
}

std::string extract_identifier_token(std::string_view value) {
    value = trim_left_ascii_view(value);
    if (value.empty() || !is_identifier_start(value.front())) {
        return "";
    }

    size_t end = 1;
    while (end < value.size() && is_identifier_body(value[end])) {
        ++end;
    }

    return std::string(value.substr(0, end));
}

std::string extract_identifier_before_paren(std::string_view line) {
    const size_t open = line.find('(');
    if (open == std::string::npos || open == 0) {
        return "";
    }

    size_t cursor = open;
    while (cursor > 0 && std::isspace(static_cast<unsigned char>(line[cursor - 1])) != 0) {
        --cursor;
    }

    size_t end = cursor;
    while (cursor > 0 && is_identifier_body(line[cursor - 1])) {
        --cursor;
    }

    if (cursor == end) {
        return "";
    }

    const std::string_view candidate = line.substr(cursor, end - cursor);
    if (!is_identifier_text(candidate)) {
        return "";
    }

    return std::string(candidate);
}

std::vector<std::string> parse_parameter_names(std::string_view signature, ScriptLanguage language) {
    std::vector<std::string> names;

    const size_t open = signature.find('(');
    const size_t close = signature.find(')', open == std::string::npos ? 0 : open + 1);
    if (open == std::string::npos || close == std::string::npos || close <= open + 1) {
        return names;
    }

    const std::string_view params = signature.substr(open + 1, close - open - 1);
    size_t cursor = 0;
    while (cursor <= params.size()) {
        const size_t comma = params.find(',', cursor);
        const size_t end = comma == std::string::npos ? params.size() : comma;
        std::string part = trim_ascii_copy(params.substr(cursor, end - cursor));

        if (!part.empty()) {
            const size_t equals = part.find('=');
            if (equals != std::string::npos) {
                part = trim_ascii_copy(part.substr(0, equals));
            }

            if (language == ScriptLanguage::GDScript) {
                const size_t colon = part.find(':');
                if (colon != std::string::npos) {
                    part = trim_ascii_copy(part.substr(0, colon));
                }
                if (starts_with(part, "var ")) {
                    part = trim_ascii_copy(std::string_view(part).substr(4));
                }

                const std::string name = extract_identifier_token(part);
                if (!name.empty()) {
                    names.push_back(name);
                }
            } else {
                size_t token_cursor = 0;
                std::string last_identifier;
                while (token_cursor < part.size()) {
                    while (token_cursor < part.size() &&
                           std::isspace(static_cast<unsigned char>(part[token_cursor])) != 0) {
                        ++token_cursor;
                    }
                    if (token_cursor >= part.size()) {
                        break;
                    }

                    size_t token_end = token_cursor;
                    while (token_end < part.size() &&
                           std::isspace(static_cast<unsigned char>(part[token_end])) == 0) {
                        ++token_end;
                    }

                    const std::string token = trim_ascii_copy(std::string_view(part).substr(token_cursor, token_end - token_cursor));
                    if (is_identifier_text(token)) {
                        last_identifier = token;
                    }
                    token_cursor = token_end;
                }

                if (!last_identifier.empty()) {
                    names.push_back(last_identifier);
                }
            }
        }

        if (comma == std::string::npos) {
            break;
        }
        cursor = comma + 1;
    }

    return names;
}

std::optional<ScriptDocCommentRecord> collect_doc_comment(
    const std::vector<std::string_view> &lines,
    size_t declaration_line,
    std::string_view marker,
    std::string style,
    std::optional<int64_t> symbol_local_id,
    std::string target_kind = ""
) {
    if (declaration_line == 0 || declaration_line > lines.size()) {
        return std::nullopt;
    }

    std::vector<std::string> collected;
    size_t cursor = declaration_line - 1;
    size_t first_line = declaration_line;
    size_t last_line = 0;

    while (cursor > 0) {
        const std::string_view raw = lines[cursor - 1];
        const std::string_view trimmed = trim_left_ascii_view(raw);
        if (trimmed.empty()) {
            break;
        }

        if (!starts_with(trimmed, marker)) {
            break;
        }

        size_t text_start = marker.size();
        while (text_start < trimmed.size() &&
               std::isspace(static_cast<unsigned char>(trimmed[text_start])) != 0) {
            ++text_start;
        }

        collected.push_back(std::string(trimmed.substr(text_start)));
        first_line = cursor;
        last_line = declaration_line - 1;
        --cursor;
    }

    if (collected.empty()) {
        return std::nullopt;
    }

    std::reverse(collected.begin(), collected.end());

    ScriptDocCommentRecord comment;
    comment.symbol_local_id = symbol_local_id;
    comment.target_kind = std::move(target_kind);
    comment.comment_style = std::move(style);
    comment.is_attached = true;
    for (size_t i = 0; i < collected.size(); ++i) {
        if (i > 0) {
            comment.comment_text += '\n';
        }
        comment.comment_text += collected[i];
        if (comment.summary_text.empty() && !trim_ascii_copy(collected[i]).empty()) {
            comment.summary_text = trim_ascii_copy(collected[i]);
        }
    }
    comment.text_excerpt = comment.summary_text.empty() ? comment.comment_text : comment.summary_text;
    comment.text_hash = std::to_string(std::hash<std::string> {}(comment.comment_text));

    comment.range.line_start = static_cast<int64_t>(first_line);
    comment.range.column_start = 1;
    comment.range.line_end = static_cast<int64_t>(last_line);
    comment.range.column_end = 1;
    return comment;
}

int64_t append_symbol(
    ScriptParseResult &result,
    int64_t &next_symbol_id,
    SymbolKind symbol_kind,
    std::string symbol_name,
    std::optional<int64_t> parent_symbol_id,
    int64_t symbol_flags,
    int64_t line_number,
    std::string signature_text,
    std::string declared_type = "",
    std::string return_type = "",
    std::string default_value_excerpt = "",
    std::string visibility = ""
) {
    if (symbol_name.empty()) {
        return 0;
    }

    ScriptSymbolRecord symbol;
    symbol.local_symbol_id = next_symbol_id++;
    symbol.parent_local_symbol_id = parent_symbol_id;
    symbol.symbol_kind = symbol_kind;
    symbol.name = symbol_name;
    symbol.symbol_name = symbol_name;
    symbol.class_name = result.class_name;
    symbol.language = to_string(result.language);
    symbol.declared_type = std::move(declared_type);
    symbol.return_type = std::move(return_type);
    symbol.default_value_excerpt = std::move(default_value_excerpt);
    symbol.visibility = std::move(visibility);
    symbol.signature_text = std::move(signature_text);
    symbol.symbol_flags = symbol_flags;
    symbol.range.line_start = line_number;
    symbol.range.column_start = 1;
    symbol.range.line_end = line_number;
    symbol.range.column_end = 1;
    if (!symbol.class_name.empty()) {
        symbol.qualified_name = symbol.class_name + "::" + symbol.name;
    } else {
        symbol.qualified_name = symbol.name;
    }
    result.symbols.push_back(std::move(symbol));
    return result.symbols.back().local_symbol_id;
}

std::optional<ScriptDocCommentRecord> collect_plain_adjacent_comment(
    const std::vector<std::string_view> &lines,
    size_t declaration_line,
    std::string style,
    std::optional<int64_t> symbol_local_id,
    std::string target_kind = ""
) {
    if (declaration_line == 0 || declaration_line > lines.size()) {
        return std::nullopt;
    }

    std::vector<std::string> collected;
    size_t cursor = declaration_line - 1;
    size_t first_line = declaration_line;
    size_t last_line = 0;

    while (cursor > 0) {
        const std::string_view raw = lines[cursor - 1];
        const std::string_view trimmed = trim_left_ascii_view(raw);
        if (trimmed.empty()) {
            break;
        }

        if (starts_with(trimmed, "##")) {
            break;
        }

        if (!starts_with(trimmed, "#")) {
            break;
        }

        std::string_view text = trimmed.substr(1);
        text = trim_left_ascii_view(text);
        collected.push_back(std::string(text));
        first_line = cursor;
        last_line = declaration_line - 1;
        --cursor;
    }

    if (collected.empty()) {
        return std::nullopt;
    }

    std::reverse(collected.begin(), collected.end());

    ScriptDocCommentRecord comment;
    comment.symbol_local_id = symbol_local_id;
    comment.target_kind = std::move(target_kind);
    comment.comment_style = std::move(style);
    comment.is_attached = false;
    for (size_t i = 0; i < collected.size(); ++i) {
        if (i > 0) {
            comment.comment_text += '\n';
        }
        comment.comment_text += collected[i];
        if (comment.summary_text.empty() && !trim_ascii_copy(collected[i]).empty()) {
            comment.summary_text = trim_ascii_copy(collected[i]);
        }
    }
    comment.text_excerpt = comment.summary_text.empty() ? comment.comment_text : comment.summary_text;
    comment.text_hash = std::to_string(std::hash<std::string> {}(comment.comment_text));
    comment.range.line_start = static_cast<int64_t>(first_line);
    comment.range.column_start = 1;
    comment.range.line_end = static_cast<int64_t>(last_line);
    comment.range.column_end = 1;
    return comment;
}

void set_symbol_doc_comment_state(
    ScriptParseResult &result,
    int64_t symbol_local_id,
    std::string_view state
) {
    for (ScriptSymbolRecord &symbol : result.symbols) {
        if (symbol.local_symbol_id != symbol_local_id) {
            continue;
        }

        symbol.doc_comment_state = std::string(state);
        if (state == "present") {
            symbol.symbol_flags |= SYMBOL_FLAG_HAS_DOC_COMMENT;
        }
        return;
    }
}

struct ParsedParameter {
    std::string name;
    std::string declared_type;
    std::string default_value_excerpt;
};

std::string_view trim_right_ascii_view(std::string_view value) {
    size_t end = value.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(0, end);
}

std::string extract_declared_type_after_colon(std::string_view declaration) {
    const size_t colon = declaration.find(':');
    if (colon == std::string::npos) {
        return "";
    }

    std::string_view tail = trim_left_ascii_view(declaration.substr(colon + 1));
    const size_t equals = tail.find('=');
    if (equals != std::string::npos) {
        tail = trim_right_ascii_view(tail.substr(0, equals));
    }

    if (!tail.empty() && tail.back() == ':') {
        tail.remove_suffix(1);
    }

    return trim_ascii_copy(tail);
}

std::string extract_default_value_excerpt(std::string_view declaration) {
    const size_t equals = declaration.find('=');
    if (equals == std::string::npos) {
        return "";
    }

    std::string value = trim_ascii_copy(declaration.substr(equals + 1));
    if (value.size() > 120) {
        value.resize(120);
    }
    return value;
}

std::string extract_gdscript_return_type(std::string_view declaration) {
    const size_t arrow = declaration.find("->");
    if (arrow == std::string::npos) {
        return "";
    }

    std::string_view tail = trim_left_ascii_view(declaration.substr(arrow + 2));
    size_t end = tail.find(':');
    if (end != std::string::npos) {
        tail = tail.substr(0, end);
    }
    return trim_ascii_copy(tail);
}

std::vector<ParsedParameter> parse_gdscript_parameters(std::string_view signature) {
    std::vector<ParsedParameter> parameters;

    const size_t open = signature.find('(');
    const size_t close = signature.find(')', open == std::string::npos ? 0 : open + 1);
    if (open == std::string::npos || close == std::string::npos || close <= open + 1) {
        return parameters;
    }

    const std::string_view params = signature.substr(open + 1, close - open - 1);
    size_t cursor = 0;
    while (cursor <= params.size()) {
        const size_t comma = params.find(',', cursor);
        const size_t end = comma == std::string::npos ? params.size() : comma;
        std::string part = trim_ascii_copy(params.substr(cursor, end - cursor));

        if (!part.empty()) {
            ParsedParameter parameter;
            parameter.default_value_excerpt = extract_default_value_excerpt(part);

            std::string head = part;
            const size_t equals = head.find('=');
            if (equals != std::string::npos) {
                head = trim_ascii_copy(head.substr(0, equals));
            }

            parameter.declared_type = extract_declared_type_after_colon(head);
            if (!parameter.declared_type.empty()) {
                const size_t colon = head.find(':');
                if (colon != std::string::npos) {
                    head = trim_ascii_copy(std::string_view(head).substr(0, colon));
                }
            }
            if (starts_with(head, "var ")) {
                head = trim_ascii_copy(std::string_view(head).substr(4));
            }
            parameter.name = extract_identifier_token(head);

            if (!parameter.name.empty()) {
                parameters.push_back(std::move(parameter));
            }
        }

        if (comma == std::string::npos) {
            break;
        }
        cursor = comma + 1;
    }

    return parameters;
}

std::vector<ParsedParameter> parse_csharp_parameters(std::string_view signature) {
    std::vector<ParsedParameter> parameters;
    const size_t open = signature.find('(');
    const size_t close = signature.find(')', open == std::string::npos ? 0 : open + 1);
    if (open == std::string::npos || close == std::string::npos || close <= open + 1) {
        return parameters;
    }

    const std::string_view params = signature.substr(open + 1, close - open - 1);
    size_t cursor = 0;
    while (cursor <= params.size()) {
        const size_t comma = params.find(',', cursor);
        const size_t end = comma == std::string::npos ? params.size() : comma;
        std::string part = trim_ascii_copy(params.substr(cursor, end - cursor));
        if (!part.empty()) {
            ParsedParameter parameter;
            parameter.default_value_excerpt = extract_default_value_excerpt(part);

            const size_t equals = part.find('=');
            if (equals != std::string::npos) {
                part = trim_ascii_copy(std::string_view(part).substr(0, equals));
            }

            size_t token_cursor = 0;
            std::vector<std::string> tokens;
            while (token_cursor < part.size()) {
                while (token_cursor < part.size() &&
                       std::isspace(static_cast<unsigned char>(part[token_cursor])) != 0) {
                    ++token_cursor;
                }
                size_t token_end = token_cursor;
                while (token_end < part.size() &&
                       std::isspace(static_cast<unsigned char>(part[token_end])) == 0) {
                    ++token_end;
                }
                if (token_end > token_cursor) {
                    tokens.push_back(trim_ascii_copy(std::string_view(part).substr(token_cursor, token_end - token_cursor)));
                }
                token_cursor = token_end;
            }

            if (!tokens.empty()) {
                parameter.name = tokens.back();
                tokens.pop_back();
                for (const std::string &token : tokens) {
                    const std::string lowered = lower_ascii(token);
                    if (lowered == "ref" || lowered == "out" || lowered == "in" || lowered == "params" || lowered == "this") {
                        continue;
                    }
                    if (!parameter.declared_type.empty()) {
                        parameter.declared_type += " ";
                    }
                    parameter.declared_type += token;
                }
            }

            if (is_identifier_text(parameter.name)) {
                parameters.push_back(std::move(parameter));
            }
        }

        if (comma == std::string::npos) {
            break;
        }
        cursor = comma + 1;
    }

    return parameters;
}

std::string extract_csharp_type_prefix(std::string_view declaration, std::string_view member_name) {
    const size_t member = declaration.find(member_name);
    if (member == std::string::npos) {
        return "";
    }

    std::string prefix = trim_ascii_copy(declaration.substr(0, member));
    static const std::unordered_set<std::string> modifiers = {
        "public", "private", "protected", "internal", "static", "readonly", "const",
        "virtual", "override", "partial", "new", "sealed", "async"
    };

    std::vector<std::string> kept;
    size_t cursor = 0;
    while (cursor < prefix.size()) {
        while (cursor < prefix.size() && std::isspace(static_cast<unsigned char>(prefix[cursor])) != 0) {
            ++cursor;
        }
        size_t end = cursor;
        while (end < prefix.size() && std::isspace(static_cast<unsigned char>(prefix[end])) == 0) {
            ++end;
        }
        if (end > cursor) {
            const std::string token = trim_ascii_copy(std::string_view(prefix).substr(cursor, end - cursor));
            if (modifiers.find(lower_ascii(token)) == modifiers.end()) {
                kept.push_back(token);
            }
        }
        cursor = end;
    }

    std::string type_name;
    for (size_t i = 0; i < kept.size(); ++i) {
        if (i > 0) {
            type_name += " ";
        }
        type_name += kept[i];
    }
    return type_name;
}

int64_t ensure_class_symbol(ScriptParseResult &result, int64_t &next_symbol_id) {
    for (const ScriptSymbolRecord &symbol : result.symbols) {
        if (symbol.symbol_kind == SymbolKind::Class) {
            return symbol.local_symbol_id;
        }
    }

    if (result.class_name.empty()) {
        return 0;
    }

    return append_symbol(
        result,
        next_symbol_id,
        SymbolKind::Class,
        result.class_name,
        std::nullopt,
        SYMBOL_FLAG_PUBLIC,
        1,
        result.class_name
    );
}

void extract_gdscript_symbols_and_docs(
    const std::string &source,
    ScriptParseResult &result
) {
    const std::vector<std::string_view> lines = split_lines_view(source);
    int64_t next_symbol_id = 1;
    int64_t class_symbol_id = ensure_class_symbol(result, next_symbol_id);

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string_view trimmed = trim_left_ascii_view(lines[i]);
        if (trimmed.empty()) {
            continue;
        }

        if (starts_with(trimmed, "@tool")) {
            result.script_flags |= SCRIPT_FLAG_IS_TOOL_SCRIPT;
        }

        if (starts_with(trimmed, "extends ")) {
            const std::string base_name = extract_identifier_token(trimmed.substr(8));
            const std::string lowered = lower_ascii(base_name);
            if (lowered.find("node") != std::string::npos) {
                result.script_flags |= SCRIPT_FLAG_EXTENDS_NODE;
            } else if (lowered.find("resource") != std::string::npos || trimmed.find("res://") != std::string::npos) {
                result.script_flags |= SCRIPT_FLAG_EXTENDS_RESOURCE;
            }
        }

        bool exported = false;
        bool onready = false;
        bool static_member = false;

        while (starts_with(trimmed, "@export ")) {
            exported = true;
            result.script_flags |= SCRIPT_FLAG_HAS_EXPORTS;
            trimmed = trim_left_ascii_view(trimmed.substr(8));
        }
        while (starts_with(trimmed, "@onready ")) {
            onready = true;
            trimmed = trim_left_ascii_view(trimmed.substr(9));
        }
        while (starts_with(trimmed, "export ")) {
            exported = true;
            result.script_flags |= SCRIPT_FLAG_HAS_EXPORTS;
            trimmed = trim_left_ascii_view(trimmed.substr(7));
        }
        while (starts_with(trimmed, "onready ")) {
            onready = true;
            trimmed = trim_left_ascii_view(trimmed.substr(8));
        }
        while (starts_with(trimmed, "static ")) {
            static_member = true;
            trimmed = trim_left_ascii_view(trimmed.substr(7));
        }

        if (starts_with(trimmed, "class_name ")) {
            const std::string symbol_name = extract_identifier_token(trimmed.substr(11));
            if (!symbol_name.empty()) {
                result.script_flags |= SCRIPT_FLAG_HAS_CLASS_NAME;
                if (result.class_name.empty()) {
                    result.class_name = symbol_name;
                }
                if (class_symbol_id == 0) {
                    class_symbol_id = append_symbol(
                        result,
                        next_symbol_id,
                        SymbolKind::Class,
                        symbol_name,
                        std::nullopt,
                        SYMBOL_FLAG_PUBLIC,
                        static_cast<int64_t>(i + 1),
                        std::string(trimmed),
                        "",
                        "",
                        "",
                        "public"
                    );
                }
                std::optional<ScriptDocCommentRecord> comment =
                    collect_doc_comment(lines, i + 1, "##", "gd_doc", class_symbol_id, "class");
                if (!comment.has_value()) {
                    comment = collect_plain_adjacent_comment(
                        lines,
                        i + 1,
                        "gd_plain_adjacent",
                        class_symbol_id,
                        "class"
                    );
                }
                if (comment.has_value()) {
                    result.doc_comments.push_back(comment.value());
                    set_symbol_doc_comment_state(
                        result,
                        class_symbol_id,
                        comment->is_attached ? "present" : "detached"
                    );
                    result.script_flags |= SCRIPT_FLAG_HAS_DOC_COMMENT;
                }
            }
            continue;
        }

        if (starts_with(trimmed, "func ")) {
            const std::string function_name = extract_identifier_token(trimmed.substr(5));
            if (function_name.empty()) {
                continue;
            }

            int64_t function_flags = 0;
            if (static_member) {
                function_flags |= SYMBOL_FLAG_STATIC;
            }
            if (function_name == "_ready") {
                function_flags |= SYMBOL_FLAG_READY_CALLBACK;
                result.script_flags |= SCRIPT_FLAG_HAS_READY_CALLBACK;
            } else if (function_name == "_process") {
                function_flags |= SYMBOL_FLAG_PROCESS_CALLBACK;
                result.script_flags |= SCRIPT_FLAG_HAS_PROCESS_CALLBACK;
            } else if (function_name == "_physics_process") {
                function_flags |= SYMBOL_FLAG_PHYSICS_PROCESS_CALLBACK;
                result.script_flags |= SCRIPT_FLAG_HAS_PHYSICS_PROCESS_CALLBACK;
            }

            const int64_t function_id = append_symbol(
                result,
                next_symbol_id,
                SymbolKind::Function,
                function_name,
                class_symbol_id > 0 ? std::optional<int64_t>(class_symbol_id) : std::nullopt,
                function_flags,
                static_cast<int64_t>(i + 1),
                std::string(trimmed),
                "",
                extract_gdscript_return_type(trimmed),
                "",
                "public"
            );

            const std::vector<ParsedParameter> parameters = parse_gdscript_parameters(trimmed);
            for (const ParsedParameter &parameter : parameters) {
                append_symbol(
                    result,
                    next_symbol_id,
                    SymbolKind::Parameter,
                    parameter.name,
                    function_id,
                    0,
                    static_cast<int64_t>(i + 1),
                    parameter.name,
                    parameter.declared_type,
                    "",
                    parameter.default_value_excerpt,
                    ""
                );
            }

            std::optional<ScriptDocCommentRecord> comment =
                collect_doc_comment(lines, i + 1, "##", "gd_doc", function_id, "function");
            if (!comment.has_value()) {
                comment = collect_plain_adjacent_comment(
                    lines,
                    i + 1,
                    "gd_plain_adjacent",
                    function_id,
                    "function"
                );
            }
            if (comment.has_value()) {
                result.doc_comments.push_back(comment.value());
                set_symbol_doc_comment_state(
                    result,
                    function_id,
                    comment->is_attached ? "present" : "detached"
                );
                result.script_flags |= SCRIPT_FLAG_HAS_DOC_COMMENT;
            }
            continue;
        }

        if (starts_with(trimmed, "signal ")) {
            result.script_flags |= SCRIPT_FLAG_HAS_SIGNALS;
            const std::string signal_name = extract_identifier_token(trimmed.substr(7));
            const int64_t signal_id = append_symbol(
                result,
                next_symbol_id,
                SymbolKind::Signal,
                signal_name,
                class_symbol_id > 0 ? std::optional<int64_t>(class_symbol_id) : std::nullopt,
                0,
                static_cast<int64_t>(i + 1),
                std::string(trimmed)
            );
            if (signal_id > 0) {
                std::optional<ScriptDocCommentRecord> comment =
                    collect_doc_comment(lines, i + 1, "##", "gd_doc", signal_id, "signal");
                if (!comment.has_value()) {
                    comment = collect_plain_adjacent_comment(
                        lines,
                        i + 1,
                        "gd_plain_adjacent",
                        signal_id,
                        "signal"
                    );
                }
                if (comment.has_value()) {
                    result.doc_comments.push_back(comment.value());
                    set_symbol_doc_comment_state(
                        result,
                        signal_id,
                        comment->is_attached ? "present" : "detached"
                    );
                    result.script_flags |= SCRIPT_FLAG_HAS_DOC_COMMENT;
                }
            }
            continue;
        }

        if (starts_with(trimmed, "enum ")) {
            const std::string enum_name = extract_identifier_token(trimmed.substr(5));
            append_symbol(
                result,
                next_symbol_id,
                SymbolKind::Enum,
                enum_name.empty() ? "enum" : enum_name,
                class_symbol_id > 0 ? std::optional<int64_t>(class_symbol_id) : std::nullopt,
                0,
                static_cast<int64_t>(i + 1),
                std::string(trimmed)
            );
            continue;
        }

        if (starts_with(trimmed, "var ") || starts_with(trimmed, "const ")) {
            const bool is_const = starts_with(trimmed, "const ");
            const std::string symbol_name =
                extract_identifier_token(trimmed.substr(is_const ? 6 : 4));
            int64_t flags = exported ? SYMBOL_FLAG_EXPORTED : 0;
            if (static_member) {
                flags |= SYMBOL_FLAG_STATIC;
            }
            if (onready) {
                flags |= SYMBOL_FLAG_ONREADY;
            }
            const int64_t symbol_id = append_symbol(
                result,
                next_symbol_id,
                is_const ? SymbolKind::Constant : SymbolKind::Property,
                symbol_name,
                class_symbol_id > 0 ? std::optional<int64_t>(class_symbol_id) : std::nullopt,
                flags,
                static_cast<int64_t>(i + 1),
                std::string(trimmed),
                extract_declared_type_after_colon(trimmed),
                "",
                extract_default_value_excerpt(trimmed),
                "public"
            );

            if (symbol_id > 0) {
                std::optional<ScriptDocCommentRecord> comment =
                    collect_doc_comment(
                        lines,
                        i + 1,
                        "##",
                        "gd_doc",
                        symbol_id,
                        is_const ? "constant" : "property"
                    );
                if (!comment.has_value()) {
                    comment = collect_plain_adjacent_comment(
                        lines,
                        i + 1,
                        "gd_plain_adjacent",
                        symbol_id,
                        is_const ? "constant" : "property"
                    );
                }
                if (comment.has_value()) {
                    result.doc_comments.push_back(comment.value());
                    set_symbol_doc_comment_state(
                        result,
                        symbol_id,
                        comment->is_attached ? "present" : "detached"
                    );
                    result.script_flags |= SCRIPT_FLAG_HAS_DOC_COMMENT;
                }
            }
        }
    }
}

bool is_probable_csharp_method_line(std::string_view trimmed) {
    if (trimmed.empty() || trimmed.find('(') == std::string::npos || trimmed.find(')') == std::string::npos) {
        return false;
    }

    const std::string lowered = lower_ascii(trimmed);
    if (starts_with(lowered, "if ") || starts_with(lowered, "for ") || starts_with(lowered, "while ") ||
        starts_with(lowered, "switch ") || starts_with(lowered, "return ") || starts_with(lowered, "new ") ||
        lowered.find(" class ") != std::string::npos) {
        return false;
    }

    return true;
}

void extract_csharp_symbols_and_docs(
    const std::string &source,
    ScriptParseResult &result
) {
    const std::vector<std::string_view> lines = split_lines_view(source);
    int64_t next_symbol_id = 1;
    int64_t class_symbol_id = ensure_class_symbol(result, next_symbol_id);
    bool pending_global_class_attribute = false;
    bool pending_export_attribute = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string_view trimmed = trim_left_ascii_view(lines[i]);
        if (trimmed.empty()) {
            continue;
        }

        const std::string lowered = lower_ascii(trimmed);
        if (starts_with(lowered, "[globalclass")) {
            pending_global_class_attribute = true;
            continue;
        }
        if (starts_with(lowered, "[export")) {
            pending_export_attribute = true;
            result.script_flags |= SCRIPT_FLAG_HAS_EXPORTS;
            continue;
        }

        size_t class_pos = lowered.find(" class ");
        if (class_pos == std::string::npos && starts_with(lowered, "class ")) {
            class_pos = 0;
        }
        if (class_pos != std::string::npos) {
            const size_t name_offset = class_pos == 0 ? 6 : class_pos + 7;
            const std::string class_name = extract_identifier_token(trimmed.substr(name_offset));
            if (!class_name.empty()) {
                result.class_name = class_name;
                result.script_flags |= SCRIPT_FLAG_HAS_CLASS_NAME;

                int64_t class_flags = 0;
                if (lowered.find("public") != std::string::npos) {
                    class_flags |= SYMBOL_FLAG_PUBLIC;
                }
                if (lowered.find("partial") != std::string::npos) {
                    class_flags |= SYMBOL_FLAG_PARTIAL;
                }
                if (pending_global_class_attribute) {
                    class_flags |= SYMBOL_FLAG_GLOBAL_CLASS;
                }

                std::string base_type;
                const size_t colon = trimmed.find(':');
                if (colon != std::string::npos) {
                    base_type = extract_identifier_token(trimmed.substr(colon + 1));
                    const std::string lowered_base = lower_ascii(base_type);
                    if (lowered_base.find("node") != std::string::npos) {
                        result.script_flags |= SCRIPT_FLAG_EXTENDS_NODE;
                    } else if (lowered_base.find("resource") != std::string::npos) {
                        result.script_flags |= SCRIPT_FLAG_EXTENDS_RESOURCE;
                    }
                }

                if (class_symbol_id == 0) {
                    class_symbol_id = append_symbol(
                        result,
                        next_symbol_id,
                        SymbolKind::Class,
                        class_name,
                        std::nullopt,
                        class_flags,
                        static_cast<int64_t>(i + 1),
                        std::string(trimmed),
                        base_type,
                        "",
                        "",
                        "public"
                    );
                }

                std::optional<ScriptDocCommentRecord> comment =
                    collect_doc_comment(lines, i + 1, "///", "csharp_xml", class_symbol_id, "class");
                if (comment.has_value()) {
                    result.doc_comments.push_back(comment.value());
                    set_symbol_doc_comment_state(
                        result,
                        class_symbol_id,
                        comment->is_attached ? "present" : "detached"
                    );
                    result.script_flags |= SCRIPT_FLAG_HAS_DOC_COMMENT;
                }
            }
            pending_global_class_attribute = false;
            continue;
        }

        if (lowered.find(" event ") != std::string::npos) {
            result.script_flags |= SCRIPT_FLAG_HAS_SIGNALS;
            size_t semicolon = trimmed.find(';');
            if (semicolon == std::string::npos) {
                semicolon = trimmed.size();
            }
            std::string signal_name = extract_identifier_before_paren(trimmed.substr(0, semicolon));
            if (signal_name.empty()) {
                size_t cursor = semicolon;
                while (cursor > 0 && std::isspace(static_cast<unsigned char>(trimmed[cursor - 1])) != 0) {
                    --cursor;
                }
                size_t start = cursor;
                while (start > 0 && is_identifier_body(trimmed[start - 1])) {
                    --start;
                }
                if (start < cursor) {
                    signal_name = std::string(trimmed.substr(start, cursor - start));
                }
            }

            const int64_t signal_id = append_symbol(
                result,
                next_symbol_id,
                SymbolKind::Signal,
                signal_name,
                class_symbol_id > 0 ? std::optional<int64_t>(class_symbol_id) : std::nullopt,
                lowered.find("public") != std::string::npos ? SYMBOL_FLAG_PUBLIC : 0,
                static_cast<int64_t>(i + 1),
                std::string(trimmed),
                "",
                "",
                "",
                "public"
            );

            if (signal_id > 0) {
                std::optional<ScriptDocCommentRecord> comment =
                    collect_doc_comment(lines, i + 1, "///", "csharp_xml", signal_id, "signal");
                if (comment.has_value()) {
                    result.doc_comments.push_back(comment.value());
                    set_symbol_doc_comment_state(
                        result,
                        signal_id,
                        comment->is_attached ? "present" : "detached"
                    );
                    result.script_flags |= SCRIPT_FLAG_HAS_DOC_COMMENT;
                }
            }
            continue;
        }

        if (is_probable_csharp_method_line(trimmed)) {
            const std::string method_name = extract_identifier_before_paren(trimmed);
            if (!method_name.empty()) {
                const int64_t flags =
                    (lowered.find("public") != std::string::npos ? SYMBOL_FLAG_PUBLIC : 0) |
                    (lowered.find("static") != std::string::npos ? SYMBOL_FLAG_STATIC : 0);

                const int64_t method_id = append_symbol(
                    result,
                    next_symbol_id,
                    SymbolKind::Function,
                    method_name,
                    class_symbol_id > 0 ? std::optional<int64_t>(class_symbol_id) : std::nullopt,
                    flags,
                    static_cast<int64_t>(i + 1),
                    std::string(trimmed),
                    "",
                    extract_csharp_type_prefix(trimmed, method_name),
                    "",
                    lowered.find("public") != std::string::npos ? "public" : ""
                );

                const std::vector<ParsedParameter> parameters = parse_csharp_parameters(trimmed);
                for (const ParsedParameter &parameter : parameters) {
                    append_symbol(
                        result,
                        next_symbol_id,
                        SymbolKind::Parameter,
                        parameter.name,
                        method_id,
                        0,
                        static_cast<int64_t>(i + 1),
                        parameter.name,
                        parameter.declared_type,
                        "",
                        parameter.default_value_excerpt,
                        ""
                    );
                }

                std::optional<ScriptDocCommentRecord> comment =
                    collect_doc_comment(lines, i + 1, "///", "csharp_xml", method_id, "function");
                if (comment.has_value()) {
                    result.doc_comments.push_back(comment.value());
                    set_symbol_doc_comment_state(
                        result,
                        method_id,
                        comment->is_attached ? "present" : "detached"
                    );
                    result.script_flags |= SCRIPT_FLAG_HAS_DOC_COMMENT;
                }
                continue;
            }
        }

        if (trimmed.find('{') != std::string::npos &&
            (lowered.find(" get;") != std::string::npos || lowered.find(" set;") != std::string::npos) &&
            trimmed.find('(') == std::string::npos) {
            size_t brace = trimmed.find('{');
            std::string property_name;
            size_t cursor = brace;
            while (cursor > 0 && std::isspace(static_cast<unsigned char>(trimmed[cursor - 1])) != 0) {
                --cursor;
            }
            size_t start = cursor;
            while (start > 0 && is_identifier_body(trimmed[start - 1])) {
                --start;
            }
            if (start < cursor) {
                property_name = std::string(trimmed.substr(start, cursor - start));
            }

            const int64_t property_id = append_symbol(
                result,
                next_symbol_id,
                SymbolKind::Property,
                property_name,
                class_symbol_id > 0 ? std::optional<int64_t>(class_symbol_id) : std::nullopt,
                (lowered.find("public") != std::string::npos ? SYMBOL_FLAG_PUBLIC : 0) |
                    (pending_export_attribute ? SYMBOL_FLAG_EXPORTED : 0),
                static_cast<int64_t>(i + 1),
                std::string(trimmed),
                extract_csharp_type_prefix(trimmed, property_name),
                "",
                "",
                lowered.find("public") != std::string::npos ? "public" : ""
            );

            if (property_id > 0) {
                std::optional<ScriptDocCommentRecord> comment =
                    collect_doc_comment(lines, i + 1, "///", "csharp_xml", property_id, "property");
                if (comment.has_value()) {
                    result.doc_comments.push_back(comment.value());
                    set_symbol_doc_comment_state(
                        result,
                        property_id,
                        comment->is_attached ? "present" : "detached"
                    );
                    result.script_flags |= SCRIPT_FLAG_HAS_DOC_COMMENT;
                }
            }
            pending_export_attribute = false;
        }
    }
}

int64_t find_class_symbol_id(const ScriptParseResult &result) {
    for (const ScriptSymbolRecord &symbol : result.symbols) {
        if (symbol.symbol_kind == SymbolKind::Class || symbol.symbol_kind == SymbolKind::Script) {
            return symbol.local_symbol_id;
        }
    }
    return 0;
}

int64_t map_script_flags_to_symbol_flags(int64_t script_flags) {
    int64_t mapped = 0;
    if ((script_flags & SCRIPT_FLAG_IS_TOOL_SCRIPT) != 0) {
        mapped |= SYMBOL_FLAG_TOOL_SCRIPT;
    }
    if ((script_flags & SCRIPT_FLAG_EXTENDS_NODE) != 0) {
        mapped |= SYMBOL_FLAG_EXTENDS_NODE;
    }
    if ((script_flags & SCRIPT_FLAG_EXTENDS_RESOURCE) != 0) {
        mapped |= SYMBOL_FLAG_EXTENDS_RESOURCE;
    }
    if ((script_flags & SCRIPT_FLAG_HAS_DOC_COMMENT) != 0) {
        mapped |= SYMBOL_FLAG_HAS_DOC_COMMENT;
    }
    if ((script_flags & SCRIPT_FLAG_HAS_READY_CALLBACK) != 0) {
        mapped |= SYMBOL_FLAG_READY_CALLBACK;
    }
    if ((script_flags & SCRIPT_FLAG_HAS_PROCESS_CALLBACK) != 0) {
        mapped |= SYMBOL_FLAG_PROCESS_CALLBACK;
    }
    if ((script_flags & SCRIPT_FLAG_HAS_PHYSICS_PROCESS_CALLBACK) != 0) {
        mapped |= SYMBOL_FLAG_PHYSICS_PROCESS_CALLBACK;
    }
    if ((script_flags & SCRIPT_FLAG_PARSER_INCOMPLETE) != 0) {
        mapped |= SYMBOL_FLAG_PARSER_INCOMPLETE;
    }
    if ((script_flags & SCRIPT_FLAG_HAS_DYNAMIC_LOADS) != 0) {
        mapped |= SYMBOL_FLAG_HAS_DYNAMIC_LOADS;
    }
    if ((script_flags & SCRIPT_FLAG_HAS_UNRESOLVED_REFS) != 0) {
        mapped |= SYMBOL_FLAG_HAS_UNRESOLVED_REFS;
    }
    return mapped;
}

void apply_script_flags_to_root_symbol(ScriptParseResult &result) {
    if (result.symbols.empty()) {
        return;
    }

    const int64_t mapped_flags = map_script_flags_to_symbol_flags(result.script_flags);
    if (mapped_flags == 0) {
        return;
    }

    for (ScriptSymbolRecord &symbol : result.symbols) {
        if (symbol.symbol_kind == SymbolKind::Class || symbol.symbol_kind == SymbolKind::Script) {
            symbol.symbol_flags |= mapped_flags;
            return;
        }
    }

    result.symbols.front().symbol_flags |= mapped_flags;
}

void bind_dependency_source_symbols(ScriptParseResult &result) {
    const int64_t class_symbol_id = find_class_symbol_id(result);
    for (ScriptDependencyRecord &dependency : result.dependencies) {
        int64_t best_symbol_id = class_symbol_id;
        int64_t best_line = 0;
        for (const ScriptSymbolRecord &symbol : result.symbols) {
            if (symbol.local_symbol_id <= 0) {
                continue;
            }
            if (symbol.symbol_kind == SymbolKind::Parameter) {
                continue;
            }
            if (symbol.range.line_start <= 0 || symbol.range.line_start > dependency.range.line_start) {
                continue;
            }
            if (symbol.range.line_start >= best_line) {
                best_line = symbol.range.line_start;
                best_symbol_id = symbol.local_symbol_id;
            }
        }

        if (best_symbol_id > 0) {
            dependency.source_symbol_local_id = best_symbol_id;
        }

        dependency.is_unresolved = !dependency.is_dynamic &&
            !dependency.target_project_relative_path.has_value() &&
            !dependency.target_class_name.has_value() &&
            !dependency.target_symbol_name.has_value() &&
            !dependency.target_resource_uid.has_value();

        if (dependency.is_dynamic) {
            result.script_flags |= SCRIPT_FLAG_HAS_DYNAMIC_LOADS;
        }
        if (dependency.is_unresolved) {
            result.script_flags |= SCRIPT_FLAG_HAS_UNRESOLVED_REFS;
        }
    }
}

void build_references_from_dependencies(ScriptParseResult &result) {
    int64_t class_symbol_id = 0;
    for (const ScriptSymbolRecord &symbol : result.symbols) {
        if (symbol.symbol_kind == SymbolKind::Class) {
            class_symbol_id = symbol.local_symbol_id;
            break;
        }
    }

    for (const ScriptDependencyRecord &dependency : result.dependencies) {
        ScriptReferenceRecord reference;
        reference.reference_kind = to_string(dependency.dependency_kind);
        reference.reference_text = dependency.reference_text;
        reference.target_project_relative_path = dependency.target_project_relative_path;
        reference.target_class_name = dependency.target_class_name;
        reference.target_symbol_name = dependency.target_symbol_name;
        reference.target_resource_uid = dependency.target_resource_uid;
        reference.is_dynamic = dependency.is_dynamic;
        reference.is_resolved = dependency.is_resolved;
        reference.confidence = dependency.confidence;
        reference.range = dependency.range;
        reference.source_symbol_local_id = dependency.source_symbol_local_id.has_value()
            ? dependency.source_symbol_local_id
            : (class_symbol_id > 0 ? std::optional<int64_t>(class_symbol_id) : std::nullopt);
        reference.is_unresolved = dependency.is_unresolved || (
            !dependency.is_dynamic &&
            !dependency.target_project_relative_path.has_value() &&
            !dependency.target_class_name.has_value() &&
            !dependency.target_symbol_name.has_value() &&
            !dependency.target_resource_uid.has_value()
        );
        result.references.push_back(std::move(reference));
    }
}

std::unordered_map<std::string, std::string> parse_scene_section_attributes(std::string_view line) {
    std::unordered_map<std::string, std::string> attributes;
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        return attributes;
    }

    const std::string_view body = line.substr(1, line.size() - 2);
    size_t cursor = 0;

    while (cursor < body.size()) {
        while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
        }

        size_t key_start = cursor;
        while (cursor < body.size() &&
               (std::isalnum(static_cast<unsigned char>(body[cursor])) != 0 || body[cursor] == '_' || body[cursor] == '-')) {
            ++cursor;
        }
        if (cursor == key_start) {
            ++cursor;
            continue;
        }

        const std::string key(body.substr(key_start, cursor - key_start));

        while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= body.size() || body[cursor] != '=') {
            continue;
        }
        ++cursor;

        while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
        }

        std::string value;
        if (cursor < body.size() && (body[cursor] == '"' || body[cursor] == '\'')) {
            const char quote = body[cursor++];
            const size_t value_start = cursor;
            while (cursor < body.size() && body[cursor] != quote) {
                ++cursor;
            }
            value = std::string(body.substr(value_start, cursor - value_start));
            if (cursor < body.size()) {
                ++cursor;
            }
        } else {
            const size_t value_start = cursor;
            while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) == 0) {
                ++cursor;
            }
            value = std::string(body.substr(value_start, cursor - value_start));
        }

        attributes.emplace(key, value);
    }

    return attributes;
}

std::string extract_first_quoted_value(std::string_view value) {
    const size_t quote_start = value.find('"');
    if (quote_start == std::string::npos) {
        return "";
    }
    const size_t quote_end = value.find('"', quote_start + 1);
    if (quote_end == std::string::npos || quote_end <= quote_start + 1) {
        return "";
    }
    return std::string(value.substr(quote_start + 1, quote_end - quote_start - 1));
}

std::string normalize_scene_script_path(std::string_view raw_path) {
    const std::string trimmed = trim_ascii_copy(raw_path);
    if (!starts_with(trimmed, "res://")) {
        return "";
    }
    return normalize_project_path(trimmed);
}

std::string build_scene_node_path(
    const std::unordered_map<std::string, std::string> &attributes,
    const std::string &fallback
) {
    const auto name_it = attributes.find("name");
    const auto parent_it = attributes.find("parent");

    const std::string name = name_it == attributes.end() ? "" : name_it->second;
    const std::string parent = parent_it == attributes.end() ? "" : parent_it->second;

    if (name.empty()) {
        return fallback;
    }

    if (parent.empty() || parent == ".") {
        return name;
    }

    return parent + "/" + name;
}

} // namespace

ScriptParseResult parse_script_intelligence(
    const std::filesystem::path &absolute_path,
    std::string_view extension,
    ParseTier parse_tier,
    int64_t max_lines,
    int64_t max_bytes,
    int64_t max_tokens,
    int64_t max_dependencies
) {
    ScriptParseResult result;
    result.parse_tier = parse_tier;
    result.language = language_from_extension(extension);

    if (result.language == ScriptLanguage::Unknown) {
        result.status = ParseStatus::UnsupportedLanguage;
        return result;
    }

    bool byte_limit_exceeded = false;
    const std::string source = read_file_with_limit(absolute_path, max_bytes, byte_limit_exceeded);
    if (source.empty()) {
        std::ifstream probe(absolute_path, std::ios::binary);
        if (!probe.is_open()) {
            result.status = ParseStatus::IoError;
            result.parse_error = "failed_to_open";
            return result;
        }
    }

    result.bytes_read = static_cast<int64_t>(source.size());
    result.limit_exceeded = byte_limit_exceeded;
    if (byte_limit_exceeded) {
        result.parse_error = "limit_exceeded";
    }

    std::string source_without_comments;
    if (result.language == ScriptLanguage::GDScript) {
        source_without_comments = strip_gdscript_comments_preserving_strings(source);
    } else {
        source_without_comments = strip_csharp_comments_preserving_strings(source);
    }

    const auto tokenizer_start = std::chrono::steady_clock::now();
    int64_t lines_scanned = 0;
    bool tokenizer_limit_exceeded = false;
    const std::vector<Token> tokens = tokenize_source(
        source_without_comments,
        result.language,
        std::max<int64_t>(max_lines, 1),
        std::max<int64_t>(max_tokens, 1),
        tokenizer_limit_exceeded,
        lines_scanned
    );
    result.tokenizer_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tokenizer_start
    ).count();
    result.tokens_generated = static_cast<int64_t>(tokens.size());
    result.lines_scanned = lines_scanned;
    result.limit_exceeded = result.limit_exceeded || tokenizer_limit_exceeded;
    if (result.limit_exceeded && result.parse_error.empty()) {
        result.parse_error = "limit_exceeded";
    }
    result.parser_incomplete = result.limit_exceeded;
    if (result.parser_incomplete) {
        result.script_flags |= SCRIPT_FLAG_PARSER_INCOMPLETE;
    }

    const auto dependency_start = std::chrono::steady_clock::now();
    extract_dependencies(tokens, result, std::max<int64_t>(max_dependencies, 1));
    result.dependency_parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dependency_start
    ).count();

    fill_class_fields_from_tokens(tokens, result);

    if (parse_tier == ParseTier::FullSymbols) {
        const auto symbol_start = std::chrono::steady_clock::now();
        if (result.language == ScriptLanguage::GDScript) {
            extract_gdscript_symbols_and_docs(source, result);
        } else {
            extract_csharp_symbols_and_docs(source, result);
        }
        result.full_symbol_parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - symbol_start
        ).count();

        const auto doc_start = std::chrono::steady_clock::now();
        bind_dependency_source_symbols(result);
        build_references_from_dependencies(result);
        result.doc_comment_parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - doc_start
        ).count();
        if (result.doc_comment_parse_ms == 0 && !result.doc_comments.empty()) {
            result.doc_comment_parse_ms = 1;
        }

        apply_script_flags_to_root_symbol(result);
    }

    if (!result.class_name.empty()) {
        result.status = ParseStatus::ParsedClass;
    } else {
        result.status = ParseStatus::NoClass;
    }

    return result;
}

ScriptParseResult parse_script_header(
    const std::filesystem::path &absolute_path,
    std::string_view extension,
    int64_t max_lines,
    int64_t max_bytes,
    int64_t max_tokens,
    int64_t max_dependencies
) {
    return parse_script_intelligence(
        absolute_path,
        extension,
        ParseTier::HeaderFast,
        max_lines,
        max_bytes,
        max_tokens,
        max_dependencies
    );
}

SceneParseResult parse_scene_attachments(
    const std::filesystem::path &absolute_path,
    int64_t max_lines,
    int64_t max_bytes
) {
    SceneParseResult result;

    bool byte_limit_exceeded = false;
    const std::string source = read_file_with_limit(absolute_path, max_bytes, byte_limit_exceeded);
    if (source.empty()) {
        std::ifstream probe(absolute_path, std::ios::binary);
        if (!probe.is_open()) {
            result.status = ParseStatus::IoError;
            result.parse_error = "failed_to_open";
            return result;
        }
    }

    result.bytes_read = static_cast<int64_t>(source.size());
    result.limit_exceeded = byte_limit_exceeded;
    if (byte_limit_exceeded) {
        result.parse_error = "limit_exceeded";
    }

    const auto parse_start = std::chrono::steady_clock::now();

    std::unordered_map<std::string, SceneExternalResourceRecord> external_resource_by_slot;
    std::istringstream stream(source);
    std::string line;
    int64_t line_number = 0;
    std::string current_node_path;
    std::string current_node_name;
    std::string current_node_type;

    while (std::getline(stream, line)) {
        ++line_number;
        if (line_number > std::max<int64_t>(max_lines, 1)) {
            result.limit_exceeded = true;
            break;
        }

        std::string trimmed = trim_ascii_copy(line);
        if (trimmed.empty() || starts_with(trimmed, ";")) {
            continue;
        }

        if (starts_with(trimmed, "[ext_resource") && trimmed.back() == ']') {
            const std::unordered_map<std::string, std::string> attributes = parse_scene_section_attributes(trimmed);
            SceneExternalResourceRecord resource;
            const auto id_it = attributes.find("id");
            const auto type_it = attributes.find("type");
            const auto path_it = attributes.find("path");
            const auto uid_it = attributes.find("uid");

            if (id_it != attributes.end()) {
                resource.ext_resource_id = id_it->second;
            }
            if (type_it != attributes.end()) {
                resource.resource_type = type_it->second;
            }
            if (path_it != attributes.end()) {
                resource.resource_path = normalize_scene_script_path(path_it->second);
            }
            if (uid_it != attributes.end()) {
                resource.resource_uid = uid_it->second;
            }

            resource.range.line_start = line_number;
            resource.range.column_start = 1;
            resource.range.line_end = line_number;
            resource.range.column_end = 1;

            const std::string lowered_type = lower_ascii(resource.resource_type);
            resource.is_script_resource = lowered_type.find("script") != std::string::npos ||
                                          is_script_extension(extension_from_path(resource.resource_path));
            resource.is_resolved = !resource.resource_path.empty() || !resource.resource_uid.empty();

            if (!resource.ext_resource_id.empty()) {
                external_resource_by_slot[resource.ext_resource_id] = resource;
            }
            result.external_resources.push_back(std::move(resource));
            continue;
        }

        if (starts_with(trimmed, "[node") && trimmed.back() == ']') {
            const std::unordered_map<std::string, std::string> attributes = parse_scene_section_attributes(trimmed);
            current_node_path = build_scene_node_path(attributes, current_node_path);
            const auto name_it = attributes.find("name");
            current_node_name = name_it == attributes.end() ? "" : name_it->second;
            const auto type_it = attributes.find("type");
            current_node_type = type_it == attributes.end() ? "" : type_it->second;
            continue;
        }

        if (starts_with(trimmed, "script") && trimmed.find('=') != std::string::npos) {
            const size_t equals = trimmed.find('=');
            const std::string value = trim_ascii_copy(std::string_view(trimmed).substr(equals + 1));

            SceneScriptAttachmentRecord attachment;
            attachment.node_path = current_node_path;
            attachment.node_name = current_node_name;
            attachment.node_type = current_node_type;
            attachment.range.line_start = line_number;
            attachment.range.column_start = 1;
            attachment.range.line_end = line_number;
            attachment.range.column_end = 1;

            if (starts_with(value, "ExtResource(")) {
                attachment.ext_resource_id = extract_first_quoted_value(value);
                attachment.attachment_kind = "ext_resource";
                const auto found = external_resource_by_slot.find(attachment.ext_resource_id);
                if (found != external_resource_by_slot.end()) {
                    attachment.script_project_relative_path = found->second.resource_path;
                    attachment.script_resource_path = found->second.resource_path;
                    attachment.script_uid = found->second.resource_uid;
                    attachment.script_resource_uid = found->second.resource_uid;
                    attachment.is_resolved = !attachment.script_project_relative_path.empty();
                }
            } else if (starts_with(value, "\"") || starts_with(value, "'")) {
                const std::string direct = extract_first_quoted_value(value);
                attachment.script_project_relative_path = normalize_scene_script_path(direct);
                attachment.script_resource_path = attachment.script_project_relative_path;
                attachment.attachment_kind = "direct";
                attachment.is_resolved = !attachment.script_project_relative_path.empty();
            } else if (starts_with(value, "SubResource(")) {
                attachment.attachment_kind = "sub_resource";
                attachment.is_resolved = false;
            } else {
                attachment.attachment_kind = "unknown";
                attachment.is_dynamic = true;
                attachment.is_resolved = false;
            }

            result.script_attachments.push_back(std::move(attachment));
        }
    }

    result.lines_scanned = line_number;
    result.parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - parse_start
    ).count();
    result.status = ParseStatus::NoClass;
    return result;
}

} // namespace gotool::project_scanner
