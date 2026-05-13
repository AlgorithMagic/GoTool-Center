#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gotool::project_scanner {

using ScanGeneration = int64_t;

static constexpr int64_t PARSER_VERSION = 1;
static constexpr int64_t CLASSIFIER_VERSION = 2;
static constexpr int64_t SCANNER_SCHEMA_VERSION = 4;

enum class EntryKind : uint8_t {
    File = 0,
    Directory = 1
};

enum class FileTypeId : uint16_t {
    Unknown = 0,
    Folder,
    GodotScene,
    GodotResource,
    Script,
    Shader,
    Asset,
    Config,
    GodotImportMetadata,
    GodotImportHash,
    GodotShaderCache,
    GodotEditorMetadata,
    Data,
    GeneratedArtifact,
    SourceArt,
    ColorPalette,
    SourceCode,
    Documentation,
    Image,
    Texture,
    Audio,
    Video,
    Font,
    Model,
    ModelBackup,
    ModelCache,
    MaterialSource,
    BinaryData,
    Archive,
    Database,
    BuildArtifact
};

enum class GodotTypeHint : uint16_t {
    NotGodotTyped = 0,
    PackedScene,
    Resource,
    GDScript,
    CSharpScript,
    Shader,
    ShaderInclude,
    Texture2D,
    AudioStreamWAV,
    AudioStreamMP3,
    AudioStreamOggVorbis,
    FontFile,
    MeshLibrary,
    GDExtension,
    ConfigFile,
    ResourceUID,
    ProjectSettings
};

enum class TypeHintSource : uint8_t {
    None = 0,
    Extension,
    Path,
    ImportMetadata,
    EditorFileSystem,
    ExplicitInspection
};

enum class DirtyState : uint8_t {
    Clean = 0,
    Dirty,
    Deleted
};

enum class DirtyReason : uint16_t {
    None = 0,
    NewPath,
    DeletedPath,
    KindChanged,
    SizeChanged,
    ModifiedTimeChanged,
    FileIdentityChanged,
    ParserVersionChanged,
    ClassifierVersionChanged,
    PriorParseFailedRetry,
    WatcherInvalidated,
    ForceRescan
};

enum class ParseStatus : uint8_t {
    NotParsed = 0,
    ParsedClass,
    NoClass,
    IoError,
    UnsupportedLanguage,
    Malformed
};

enum class ScriptLanguage : uint8_t {
    Unknown = 0,
    GDScript,
    CSharp
};

struct ScanMetrics {
    int64_t total_wall_ms = 0;
    int64_t traversal_ms = 0;
    int64_t metadata_ms = 0;
    int64_t existing_snapshot_load_ms = 0;
    int64_t reserve_setup_ms = 0;
    int64_t dirty_check_ms = 0;
    int64_t script_candidate_ms = 0;
    int64_t classification_ms = 0;
    int64_t script_parse_ms = 0;
    int64_t sqlite_write_ms = 0;
    int64_t sqlite_stage_insert_ms = 0;
    int64_t sqlite_file_merge_ms = 0;
    int64_t sqlite_clean_refresh_ms = 0;
    int64_t sqlite_parent_resolve_ms = 0;
    int64_t sqlite_parse_status_ms = 0;
    int64_t sqlite_custom_class_ms = 0;
    int64_t sqlite_tombstone_ms = 0;
    int64_t sqlite_deleted_reconcile_ms = 0;
    int64_t sqlite_metrics_write_ms = 0;
    int64_t godot_materialization_ms = 0;
    int64_t files_seen = 0;
    int64_t dirs_seen = 0;
    int64_t dirs_skipped = 0;
    int64_t entries_clean = 0;
    int64_t entries_dirty = 0;
    int64_t entries_new = 0;
    int64_t entries_deleted = 0;
    int64_t rows_inserted = 0;
    int64_t rows_updated = 0;
    int64_t rows_clean_refreshed = 0;
    int64_t rows_tombstoned = 0;
    int64_t scripts_candidates = 0;
    int64_t scripts_parsed = 0;
    int64_t scripts_skipped_clean = 0;
    int64_t script_lines_scanned = 0;
    int64_t bytes_read = 0;
    int64_t entry_record_count = 0;
    int64_t path_arena_bytes = 0;
    int64_t existing_snapshot_count = 0;
    int64_t parsed_script_count = 0;
    int64_t sqlite_statement_steps = 0;
    int64_t sqlite_transactions = 0;
    int64_t ui_rows_materialized = 0;
    bool cancellation_requested = false;
    std::string scan_result_status = "completed";
};

struct ScanOptions {
    int64_t project_id = 0;
    std::filesystem::path project_root;
    const std::atomic_bool *cancel_requested = nullptr;
    bool include_hidden = true;
    bool force_rescan = false;
    bool persist_to_database = true;
    bool collect_custom_classes = true;
    bool include_deleted = false;
    int64_t result_limit = 0;
    int64_t scan_run_id = 0;
    ScanGeneration scan_generation = 0;
    int64_t started_at_unix = 0;
    int64_t batch_size = 1000;
};

struct ScanResultSummary {
    int64_t scan_run_id = 0;
    ScanGeneration scan_generation = 0;
    std::string status = "completed";
    int64_t files_seen = 0;
    int64_t dirs_seen = 0;
    int64_t entries_clean = 0;
    int64_t entries_dirty = 0;
    int64_t entries_new = 0;
    int64_t entries_deleted = 0;
    int64_t scripts_candidates = 0;
    int64_t scripts_parsed = 0;
    int64_t scripts_skipped_clean = 0;
    int64_t total_wall_ms = 0;
};

struct ExistingEntrySnapshot {
    int64_t id = 0;
    EntryKind entry_kind = EntryKind::File;
    int64_t size_bytes = 0;
    int64_t modified_time_ns = 0;
    std::string platform_file_id;
    int64_t parser_version = PARSER_VERSION;
    int64_t classifier_version = CLASSIFIER_VERSION;
    ParseStatus parse_status = ParseStatus::NotParsed;
};

struct DirtyCheckResult {
    DirtyState state = DirtyState::Dirty;
    DirtyReason reason = DirtyReason::NewPath;
};

class PathArena {
public:
    uint32_t append(std::string_view value);
    std::string_view view(uint32_t offset, uint32_t length) const;
    std::string string_at(uint32_t offset, uint32_t length) const;
    void reserve(size_t bytes);
    void clear();
    size_t size() const;
    size_t capacity() const;

private:
    std::vector<char> data_;
};

struct EntryRecord {
    uint32_t path_offset = 0;
    uint32_t path_length = 0;
    uint32_t name_offset = 0;
    uint32_t name_length = 0;
    uint32_t extension_offset = 0;
    uint32_t extension_length = 0;
    int64_t parent_record_index = -1;
    int64_t parent_db_id = 0;
    int64_t database_id = 0;
    int64_t size_bytes = 0;
    int64_t modified_time_ns = 0;
    uint64_t platform_file_id_high = 0;
    uint64_t platform_file_id_low = 0;
    uint32_t flags = 0;
    EntryKind entry_kind = EntryKind::File;
    FileTypeId file_type_id = FileTypeId::Unknown;
    GodotTypeHint godot_type_hint = GodotTypeHint::NotGodotTyped;
    TypeHintSource type_hint_source = TypeHintSource::None;
    DirtyState dirty_state = DirtyState::Dirty;
    DirtyReason dirty_reason = DirtyReason::NewPath;

    bool is_hidden() const;
    void set_hidden(bool hidden);
    bool has_platform_file_id() const;
    void clear_platform_file_id();
};

class SkipPolicy {
public:
    SkipPolicy();

    bool should_skip(std::string_view project_relative_path) const;
    void add_prefix(std::string prefix);

private:
    std::vector<std::string> prefixes_;
};

std::string normalize_project_path(std::string_view path);
std::string extension_from_path(std::string_view path);
std::string file_name_from_path(std::string_view path);
std::string lower_ascii(std::string_view value);
bool is_script_extension(std::string_view extension);
ScriptLanguage language_from_extension(std::string_view extension);
FileTypeId classify_entry(std::string_view project_relative_path, EntryKind kind);
GodotTypeHint detect_godot_type_hint(std::string_view project_relative_path, FileTypeId file_type);
TypeHintSource type_hint_source_for(GodotTypeHint hint);
DirtyCheckResult detect_dirty_state(
    EntryKind kind,
    int64_t size_bytes,
    int64_t modified_time_ns,
    std::string_view platform_file_id,
    const ExistingEntrySnapshot &existing,
    bool force_rescan
);
DirtyCheckResult detect_dirty_state(
    EntryKind kind,
    int64_t size_bytes,
    int64_t modified_time_ns,
    std::string_view platform_file_id,
    const std::optional<ExistingEntrySnapshot> &existing,
    bool force_rescan
);
std::string platform_file_id_to_string(const EntryRecord &record);
bool is_builtin_node_type_hint(std::string_view type_name);
bool is_builtin_resource_type_hint(std::string_view type_name);

const char *to_string(EntryKind value);
const char *to_string(FileTypeId value);
const char *to_string(GodotTypeHint value);
const char *to_string(TypeHintSource value);
const char *to_string(DirtyState value);
const char *to_string(DirtyReason value);
const char *to_string(ParseStatus value);
const char *to_string(ScriptLanguage value);

} // namespace gotool::project_scanner
