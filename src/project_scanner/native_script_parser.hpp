#pragma once

#include "project_scanner/native_scan_rules.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gotool::project_scanner {

enum class ParseTier : uint8_t {
    HeaderFast = 0,
    FullSymbols,
    SceneAttachments
};

enum class SymbolKind : uint8_t {
    Unknown = 0,
    Script,
    Class,
    Function,
    Property,
    Parameter,
    Signal,
    Constant,
    Enum
};

static constexpr int64_t SYMBOL_FLAG_EXPORTED = 1LL << 0;
static constexpr int64_t SYMBOL_FLAG_STATIC = 1LL << 1;
static constexpr int64_t SYMBOL_FLAG_PUBLIC = 1LL << 2;
static constexpr int64_t SYMBOL_FLAG_TOOL_SCRIPT = 1LL << 3;
static constexpr int64_t SYMBOL_FLAG_ONREADY = 1LL << 4;
static constexpr int64_t SYMBOL_FLAG_PARTIAL = 1LL << 5;
static constexpr int64_t SYMBOL_FLAG_GLOBAL_CLASS = 1LL << 6;
static constexpr int64_t SYMBOL_FLAG_PARSER_INCOMPLETE = 1LL << 7;
static constexpr int64_t SYMBOL_FLAG_HAS_DYNAMIC_LOADS = 1LL << 8;
static constexpr int64_t SYMBOL_FLAG_HAS_UNRESOLVED_REFS = 1LL << 9;
static constexpr int64_t SYMBOL_FLAG_HAS_DOC_COMMENT = 1LL << 10;
static constexpr int64_t SYMBOL_FLAG_READY_CALLBACK = 1LL << 11;
static constexpr int64_t SYMBOL_FLAG_PROCESS_CALLBACK = 1LL << 12;
static constexpr int64_t SYMBOL_FLAG_PHYSICS_PROCESS_CALLBACK = 1LL << 13;
static constexpr int64_t SYMBOL_FLAG_EXTENDS_NODE = 1LL << 14;
static constexpr int64_t SYMBOL_FLAG_EXTENDS_RESOURCE = 1LL << 15;

static constexpr int64_t SCRIPT_FLAG_HAS_CLASS_NAME = 1LL << 0;
static constexpr int64_t SCRIPT_FLAG_IS_TOOL_SCRIPT = 1LL << 1;
static constexpr int64_t SCRIPT_FLAG_EXTENDS_NODE = 1LL << 2;
static constexpr int64_t SCRIPT_FLAG_EXTENDS_RESOURCE = 1LL << 3;
static constexpr int64_t SCRIPT_FLAG_HAS_EXPORTS = 1LL << 4;
static constexpr int64_t SCRIPT_FLAG_HAS_SIGNALS = 1LL << 5;
static constexpr int64_t SCRIPT_FLAG_HAS_READY_CALLBACK = 1LL << 6;
static constexpr int64_t SCRIPT_FLAG_HAS_PROCESS_CALLBACK = 1LL << 7;
static constexpr int64_t SCRIPT_FLAG_HAS_PHYSICS_PROCESS_CALLBACK = 1LL << 8;
static constexpr int64_t SCRIPT_FLAG_HAS_DOC_COMMENT = 1LL << 9;
static constexpr int64_t SCRIPT_FLAG_PARSER_INCOMPLETE = 1LL << 10;
static constexpr int64_t SCRIPT_FLAG_HAS_DYNAMIC_LOADS = 1LL << 11;
static constexpr int64_t SCRIPT_FLAG_HAS_UNRESOLVED_REFS = 1LL << 12;

struct SourceRange {
    int64_t line_start = 0;
    int64_t column_start = 0;
    int64_t line_end = 0;
    int64_t column_end = 0;
};

struct ScriptDependencyRecord {
    DependencyKind dependency_kind = DependencyKind::Unknown;
    std::optional<int64_t> source_symbol_local_id;
    std::string reference_text;
    SourceRange range;
    double confidence = 0.0;
    bool is_dynamic = false;
    bool is_resolved = false;
    bool is_unresolved = false;
    std::optional<std::string> target_project_relative_path;
    std::optional<std::string> target_class_name;
    std::optional<std::string> target_symbol_name;
    std::optional<std::string> target_resource_uid;
};

struct ScriptSymbolRecord {
    int64_t local_symbol_id = 0;
    std::optional<int64_t> parent_local_symbol_id;
    SymbolKind symbol_kind = SymbolKind::Unknown;
    std::string name;
    std::string qualified_name;
    std::string class_name;
    std::string language;
    std::string declared_type;
    std::string return_type;
    std::string default_value_excerpt;
    std::string visibility;
    std::string signature_text;
    std::string doc_comment_state = "none";
    std::string symbol_name;
    int64_t symbol_flags = 0;
    SourceRange range;
};

struct ScriptReferenceRecord {
    std::optional<int64_t> source_symbol_local_id;
    std::string reference_kind;
    std::string reference_text;
    std::optional<std::string> target_project_relative_path;
    std::optional<std::string> target_class_name;
    std::optional<std::string> target_symbol_name;
    std::optional<std::string> target_resource_uid;
    bool is_dynamic = false;
    bool is_resolved = false;
    bool is_unresolved = false;
    double confidence = 0.0;
    SourceRange range;
};

struct ScriptDocCommentRecord {
    std::optional<int64_t> symbol_local_id;
    std::string target_kind;
    std::string comment_style;
    std::string text_hash;
    std::string text_excerpt;
    std::string comment_text;
    std::string summary_text;
    bool is_attached = false;
    SourceRange range;
};

struct ScriptParseResult {
    ScriptLanguage language = ScriptLanguage::Unknown;
    ParseStatus status = ParseStatus::NotParsed;
    ParseTier parse_tier = ParseTier::HeaderFast;
    std::string class_name;
    std::string direct_base_type;
    std::string parse_error;
    int64_t bytes_read = 0;
    int64_t lines_scanned = 0;
    int64_t tokens_generated = 0;
    int64_t tokenizer_ms = 0;
    int64_t dependency_parse_ms = 0;
    int64_t full_symbol_parse_ms = 0;
    int64_t doc_comment_parse_ms = 0;
    int64_t script_flags = 0;
    bool limit_exceeded = false;
    bool parser_incomplete = false;
    std::vector<ScriptDependencyRecord> dependencies;
    std::vector<ScriptSymbolRecord> symbols;
    std::vector<ScriptReferenceRecord> references;
    std::vector<ScriptDocCommentRecord> doc_comments;
};

struct SceneExternalResourceRecord {
    std::string ext_resource_id;
    std::string resource_type;
    std::string resource_path;
    std::string resource_uid;
    bool is_script_resource = false;
    bool is_resolved = false;
    SourceRange range;
};

struct SceneScriptAttachmentRecord {
    std::string node_path;
    std::string node_name;
    std::string node_type;
    std::string ext_resource_id;
    std::string attachment_kind = "unknown";
    std::string script_project_relative_path;
    std::string script_resource_path;
    std::string script_uid;
    std::string script_resource_uid;
    bool is_dynamic = false;
    bool is_resolved = false;
    SourceRange range;
};

struct SceneParseResult {
    ParseStatus status = ParseStatus::NotParsed;
    std::string parse_error;
    int64_t bytes_read = 0;
    int64_t lines_scanned = 0;
    int64_t parse_ms = 0;
    bool limit_exceeded = false;
    std::vector<SceneExternalResourceRecord> external_resources;
    std::vector<SceneScriptAttachmentRecord> script_attachments;
};

ScriptParseResult parse_script_intelligence(
    const std::filesystem::path &absolute_path,
    std::string_view extension,
    ParseTier parse_tier = ParseTier::FullSymbols,
    int64_t max_lines = 240,
    int64_t max_bytes = 64 * 1024,
    int64_t max_tokens = 16 * 1024,
    int64_t max_dependencies = 2 * 1024
);

ScriptParseResult parse_script_header(
    const std::filesystem::path &absolute_path,
    std::string_view extension,
    int64_t max_lines = 240,
    int64_t max_bytes = 64 * 1024,
    int64_t max_tokens = 16 * 1024,
    int64_t max_dependencies = 2 * 1024
);

SceneParseResult parse_scene_attachments(
    const std::filesystem::path &absolute_path,
    int64_t max_lines = 2048,
    int64_t max_bytes = 256 * 1024
);

} // namespace gotool::project_scanner
