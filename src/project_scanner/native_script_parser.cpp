#include "project_scanner/native_script_parser.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <optional>
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
    dependency.source_line = anchor.line;
    dependency.source_column = anchor.column;
    dependency.confidence = confidence;
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
    dependency.source_line = anchor.line;
    dependency.source_column = anchor.column;
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
    dependency.source_line = anchor.line;
    dependency.source_column = anchor.column;
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
    dependency.source_line = anchor.line;
    dependency.source_column = anchor.column;
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
    dependency.source_line = anchor.line;
    dependency.source_column = anchor.column;
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
                dependency.source_line = tokens[i + 1].line;
                dependency.source_column = tokens[i + 1].column;
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

} // namespace

ScriptParseResult parse_script_header(
    const std::filesystem::path &absolute_path,
    std::string_view extension,
    int64_t max_lines,
    int64_t max_bytes,
    int64_t max_tokens,
    int64_t max_dependencies
) {
    ScriptParseResult result;
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

    const auto dependency_start = std::chrono::steady_clock::now();
    extract_dependencies(tokens, result, std::max<int64_t>(max_dependencies, 1));
    result.dependency_parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dependency_start
    ).count();

    fill_class_fields_from_tokens(tokens, result);

    if (!result.class_name.empty()) {
        result.status = ParseStatus::ParsedClass;
    } else {
        result.status = ParseStatus::NoClass;
    }

    return result;
}

} // namespace gotool::project_scanner
