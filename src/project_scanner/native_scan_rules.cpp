#include "project_scanner/native_scan_rules.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace gotool::project_scanner {

namespace {

static constexpr uint32_t ENTRY_FLAG_HIDDEN = 1u << 0u;
static constexpr uint32_t ENTRY_FLAG_HAS_PLATFORM_FILE_ID = 1u << 1u;

bool matches_any(std::string_view value, std::initializer_list<std::string_view> candidates) {
    for (const std::string_view candidate : candidates) {
        if (value == candidate) {
            return true;
        }
    }

    return false;
}

bool ends_with_any(std::string_view value, std::initializer_list<std::string_view> candidates) {
    for (const std::string_view candidate : candidates) {
        if (value.size() >= candidate.size() &&
            value.compare(value.size() - candidate.size(), candidate.size(), candidate) == 0) {
            return true;
        }
    }

    return false;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

} // namespace

uint32_t PathArena::append(std::string_view value) {
    const uint32_t offset = static_cast<uint32_t>(data_.size());
    data_.insert(data_.end(), value.begin(), value.end());
    return offset;
}

std::string_view PathArena::view(uint32_t offset, uint32_t length) const {
    if (offset > data_.size() || static_cast<size_t>(offset) + static_cast<size_t>(length) > data_.size()) {
        return {};
    }

    return std::string_view(data_.data() + offset, length);
}

std::string PathArena::string_at(uint32_t offset, uint32_t length) const {
    const std::string_view value = view(offset, length);
    return std::string(value.begin(), value.end());
}

void PathArena::clear() {
    data_.clear();
}

size_t PathArena::size() const {
    return data_.size();
}

bool EntryRecord::is_hidden() const {
    return (flags & ENTRY_FLAG_HIDDEN) != 0;
}

void EntryRecord::set_hidden(bool hidden) {
    if (hidden) {
        flags |= ENTRY_FLAG_HIDDEN;
    } else {
        flags &= ~ENTRY_FLAG_HIDDEN;
    }
}

bool EntryRecord::has_platform_file_id() const {
    return (flags & ENTRY_FLAG_HAS_PLATFORM_FILE_ID) != 0;
}

void EntryRecord::clear_platform_file_id() {
    flags &= ~ENTRY_FLAG_HAS_PLATFORM_FILE_ID;
    platform_file_id_high = 0;
    platform_file_id_low = 0;
}

SkipPolicy::SkipPolicy() {
    prefixes_.push_back("addons/GoToolCenter");
    prefixes_.push_back(".godot/gotool_center");
    prefixes_.push_back(".godot/imported");
    prefixes_.push_back(".godot/shader_cache");
}

bool SkipPolicy::should_skip(std::string_view project_relative_path) const {
    const std::string normalized = normalize_project_path(project_relative_path);

    for (const std::string &prefix : prefixes_) {
        if (normalized == prefix || starts_with(normalized, prefix + "/")) {
            return true;
        }
    }

    return false;
}

void SkipPolicy::add_prefix(std::string prefix) {
    prefixes_.push_back(normalize_project_path(prefix));
}

std::string normalize_project_path(std::string_view path) {
    std::string text(path.begin(), path.end());
    std::replace(text.begin(), text.end(), '\\', '/');

    if (starts_with(text, "res://")) {
        text.erase(0, 6);
    }

    while (starts_with(text, "./")) {
        text.erase(0, 2);
    }

    while (!text.empty() && text.front() == '/') {
        text.erase(text.begin());
    }

    std::vector<std::string> parts;
    size_t cursor = 0;

    while (cursor <= text.size()) {
        const size_t slash = text.find('/', cursor);
        const size_t end = slash == std::string::npos ? text.size() : slash;
        const std::string part = text.substr(cursor, end - cursor);

        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
            } else {
                parts.push_back(part);
            }
        }

        if (slash == std::string::npos) {
            break;
        }
        cursor = slash + 1;
    }

    std::string normalized;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            normalized.push_back('/');
        }
        normalized += parts[i];
    }

    return normalized;
}

std::string lower_ascii(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());

    for (const char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    return lowered;
}

std::string extension_from_path(std::string_view path) {
    const std::string name = lower_ascii(file_name_from_path(path));
    const size_t dot = name.rfind('.');

    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) {
        return "";
    }

    return name.substr(dot);
}

std::string file_name_from_path(std::string_view path) {
    const std::string normalized = normalize_project_path(path);
    const size_t slash = normalized.rfind('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

bool is_script_extension(std::string_view extension) {
    return extension == ".gd" || extension == ".cs";
}

ScriptLanguage language_from_extension(std::string_view extension) {
    if (extension == ".gd") {
        return ScriptLanguage::GDScript;
    }

    if (extension == ".cs") {
        return ScriptLanguage::CSharp;
    }

    return ScriptLanguage::Unknown;
}

FileTypeId classify_entry(std::string_view project_relative_path, EntryKind kind) {
    if (kind == EntryKind::Directory) {
        return FileTypeId::Folder;
    }

    const std::string path = lower_ascii(normalize_project_path(project_relative_path));
    const std::string name = file_name_from_path(path);
    const std::string extension = extension_from_path(path);

    if (matches_any(extension, { ".tscn", ".scn" })) {
        return FileTypeId::GodotScene;
    }
    if (matches_any(extension, { ".tres", ".res", ".meshlib" })) {
        return FileTypeId::GodotResource;
    }
    if (matches_any(extension, { ".gd", ".cs", ".sh" })) {
        return FileTypeId::Script;
    }
    if (matches_any(extension, { ".gdshader", ".gdshaderinc", ".shader" })) {
        return FileTypeId::Shader;
    }
    if (matches_any(extension, { ".cfg", ".ini", ".conf", ".config", ".import", ".godot", ".uid", ".gdextension" }) ||
        matches_any(name, { "plugin.cfg", "project.godot", ".editorconfig", ".gitignore", ".gitattributes" })) {
        return FileTypeId::Config;
    }
    if (matches_any(extension, { ".json", ".csv", ".yaml", ".yml", ".toml", ".xml", ".dat", ".bytes" })) {
        return FileTypeId::Data;
    }
    if (matches_any(extension, { ".png", ".jpg", ".jpeg", ".webp", ".svg", ".bmp", ".gif", ".tga" })) {
        return FileTypeId::Asset;
    }
    if (matches_any(extension, { ".wav", ".mp3", ".ogg", ".opus", ".flac" })) {
        return FileTypeId::Audio;
    }
    if (matches_any(extension, { ".ttf", ".otf", ".fnt", ".woff", ".woff2" })) {
        return FileTypeId::Font;
    }
    if (matches_any(extension, { ".glb", ".gltf", ".fbx", ".obj", ".dae", ".blend" })) {
        return FileTypeId::Model;
    }
    if (matches_any(extension, { ".zip", ".7z", ".rar", ".tar", ".gz" })) {
        return FileTypeId::Archive;
    }
    if (matches_any(extension, { ".db", ".sqlite", ".sqlite3" })) {
        return FileTypeId::Database;
    }
    if (matches_any(extension, { ".dll", ".so", ".dylib", ".lib", ".a", ".obj", ".o", ".pdb", ".exp" })) {
        return FileTypeId::BuildArtifact;
    }
    if (matches_any(extension, { ".txt", ".md", ".rst", ".adoc" })) {
        return FileTypeId::Documentation;
    }
    if (matches_any(extension, { ".c", ".h", ".cpp", ".hpp", ".py", ".js", ".ts", ".rs", ".go", ".java" })) {
        return FileTypeId::SourceCode;
    }
    if (matches_any(extension, { ".xcf", ".psd", ".aseprite", ".kra" })) {
        return FileTypeId::SourceArt;
    }
    if (ends_with_any(name, { ".x86_64" })) {
        return FileTypeId::GeneratedArtifact;
    }

    return FileTypeId::Unknown;
}

GodotTypeHint detect_godot_type_hint(std::string_view project_relative_path, FileTypeId file_type) {
    const std::string path = lower_ascii(normalize_project_path(project_relative_path));
    const std::string name = file_name_from_path(path);
    const std::string extension = extension_from_path(path);

    if (file_type == FileTypeId::Folder) {
        return GodotTypeHint::NotGodotTyped;
    }

    if (matches_any(extension, { ".tscn", ".scn", ".glb", ".gltf", ".fbx", ".dae", ".blend" })) {
        return GodotTypeHint::PackedScene;
    }
    if (matches_any(extension, { ".tres", ".res" })) {
        return GodotTypeHint::Resource;
    }
    if (extension == ".meshlib") {
        return GodotTypeHint::MeshLibrary;
    }
    if (extension == ".gd") {
        return GodotTypeHint::GDScript;
    }
    if (extension == ".cs") {
        return GodotTypeHint::CSharpScript;
    }
    if (extension == ".gdshader" || extension == ".shader") {
        return GodotTypeHint::Shader;
    }
    if (extension == ".gdshaderinc") {
        return GodotTypeHint::ShaderInclude;
    }
    if (matches_any(extension, { ".png", ".jpg", ".jpeg", ".webp", ".svg", ".bmp", ".gif", ".tga" })) {
        return GodotTypeHint::Texture2D;
    }
    if (extension == ".wav") {
        return GodotTypeHint::AudioStreamWAV;
    }
    if (extension == ".mp3") {
        return GodotTypeHint::AudioStreamMP3;
    }
    if (extension == ".ogg" || extension == ".opus") {
        return GodotTypeHint::AudioStreamOggVorbis;
    }
    if (matches_any(extension, { ".ttf", ".otf", ".fnt", ".woff", ".woff2" })) {
        return GodotTypeHint::FontFile;
    }
    if (extension == ".gdextension") {
        return GodotTypeHint::GDExtension;
    }
    if (extension == ".uid") {
        return GodotTypeHint::ResourceUID;
    }
    if (matches_any(extension, { ".cfg", ".ini", ".conf", ".config", ".import", ".remap" }) || name == "plugin.cfg") {
        return GodotTypeHint::ConfigFile;
    }
    if (name == "project.godot") {
        return GodotTypeHint::ProjectSettings;
    }

    return GodotTypeHint::NotGodotTyped;
}

TypeHintSource type_hint_source_for(GodotTypeHint hint) {
    return hint == GodotTypeHint::NotGodotTyped ? TypeHintSource::None : TypeHintSource::Extension;
}

DirtyCheckResult detect_dirty_state(
    EntryKind kind,
    int64_t size_bytes,
    int64_t modified_time_ns,
    std::string_view platform_file_id,
    const ExistingEntrySnapshot &existing,
    bool force_rescan
) {
    if (force_rescan) {
        return { DirtyState::Dirty, DirtyReason::ForceRescan };
    }

    if (existing.entry_kind != kind) {
        return { DirtyState::Dirty, DirtyReason::KindChanged };
    }

    if (existing.size_bytes != size_bytes) {
        return { DirtyState::Dirty, DirtyReason::SizeChanged };
    }

    if (existing.modified_time_ns != modified_time_ns) {
        return { DirtyState::Dirty, DirtyReason::ModifiedTimeChanged };
    }

    if (!existing.platform_file_id.empty() && !platform_file_id.empty() &&
        existing.platform_file_id != platform_file_id) {
        return { DirtyState::Dirty, DirtyReason::FileIdentityChanged };
    }

    if (existing.parser_version != PARSER_VERSION) {
        return { DirtyState::Dirty, DirtyReason::ParserVersionChanged };
    }

    if (existing.classifier_version != CLASSIFIER_VERSION) {
        return { DirtyState::Dirty, DirtyReason::ClassifierVersionChanged };
    }

    if (existing.parse_status == ParseStatus::IoError || existing.parse_status == ParseStatus::Malformed) {
        return { DirtyState::Dirty, DirtyReason::PriorParseFailedRetry };
    }

    return { DirtyState::Clean, DirtyReason::None };
}

DirtyCheckResult detect_dirty_state(
    EntryKind kind,
    int64_t size_bytes,
    int64_t modified_time_ns,
    std::string_view platform_file_id,
    const std::optional<ExistingEntrySnapshot> &existing,
    bool force_rescan
) {
    if (!existing.has_value()) {
        (void)kind;
        (void)size_bytes;
        (void)modified_time_ns;
        (void)platform_file_id;
        return { DirtyState::Dirty, DirtyReason::NewPath };
    }

    return detect_dirty_state(kind, size_bytes, modified_time_ns, platform_file_id, existing.value(), force_rescan);
}

std::string platform_file_id_to_string(const EntryRecord &record) {
    if (!record.has_platform_file_id()) {
        return "";
    }

    std::ostringstream output;
    output << std::hex << record.platform_file_id_high << ":" << record.platform_file_id_low;
    return output.str();
}

bool is_builtin_node_type_hint(std::string_view type_name) {
    return matches_any(type_name, {
        "Node", "Node2D", "Node3D", "Control", "CanvasItem", "CharacterBody2D", "CharacterBody3D",
        "RigidBody2D", "RigidBody3D", "StaticBody2D", "StaticBody3D", "Area2D", "Area3D",
        "Sprite2D", "AnimatedSprite2D", "Camera2D", "Camera3D", "Label", "Button", "Panel",
        "TextureRect", "VBoxContainer", "HBoxContainer", "MarginContainer", "EditorPlugin"
    });
}

bool is_builtin_resource_type_hint(std::string_view type_name) {
    return matches_any(type_name, {
        "Resource", "Script", "GDScript", "CSharpScript", "Shader", "Texture2D", "ImageTexture",
        "PackedScene", "Material", "StandardMaterial3D", "AudioStream", "Font", "Mesh", "MeshLibrary"
    });
}

const char *to_string(EntryKind value) {
    return value == EntryKind::Directory ? "directory" : "file";
}

const char *to_string(FileTypeId value) {
    switch (value) {
        case FileTypeId::Folder: return "Folder";
        case FileTypeId::GodotScene: return "GodotScene";
        case FileTypeId::GodotResource: return "GodotResource";
        case FileTypeId::Script: return "Script";
        case FileTypeId::Shader: return "Shader";
        case FileTypeId::Asset: return "Asset";
        case FileTypeId::Config: return "Config";
        case FileTypeId::Data: return "Data";
        case FileTypeId::GeneratedArtifact: return "GeneratedArtifact";
        case FileTypeId::SourceArt: return "SourceArt";
        case FileTypeId::SourceCode: return "SourceCode";
        case FileTypeId::Documentation: return "Documentation";
        case FileTypeId::Image: return "Image";
        case FileTypeId::Texture: return "Texture";
        case FileTypeId::Audio: return "Audio";
        case FileTypeId::Video: return "Video";
        case FileTypeId::Font: return "Font";
        case FileTypeId::Model: return "Model";
        case FileTypeId::Archive: return "Archive";
        case FileTypeId::Database: return "Database";
        case FileTypeId::BuildArtifact: return "BuildArtifact";
        case FileTypeId::Unknown:
        default: return "Unknown";
    }
}

const char *to_string(GodotTypeHint value) {
    switch (value) {
        case GodotTypeHint::PackedScene: return "PackedScene";
        case GodotTypeHint::Resource: return "Resource";
        case GodotTypeHint::GDScript: return "GDScript";
        case GodotTypeHint::CSharpScript: return "CSharpScript";
        case GodotTypeHint::Shader: return "Shader";
        case GodotTypeHint::ShaderInclude: return "ShaderInclude";
        case GodotTypeHint::Texture2D: return "Texture2D";
        case GodotTypeHint::AudioStreamWAV: return "AudioStreamWAV";
        case GodotTypeHint::AudioStreamMP3: return "AudioStreamMP3";
        case GodotTypeHint::AudioStreamOggVorbis: return "AudioStreamOggVorbis";
        case GodotTypeHint::FontFile: return "FontFile";
        case GodotTypeHint::MeshLibrary: return "MeshLibrary";
        case GodotTypeHint::GDExtension: return "GDExtension";
        case GodotTypeHint::ConfigFile: return "ConfigFile";
        case GodotTypeHint::ResourceUID: return "ResourceUID";
        case GodotTypeHint::ProjectSettings: return "ProjectSettings";
        case GodotTypeHint::NotGodotTyped:
        default: return "NGT";
    }
}

const char *to_string(TypeHintSource value) {
    switch (value) {
        case TypeHintSource::Extension: return "extension";
        case TypeHintSource::Path: return "path";
        case TypeHintSource::ImportMetadata: return "import_metadata";
        case TypeHintSource::EditorFileSystem: return "editor_filesystem";
        case TypeHintSource::ExplicitInspection: return "explicit_inspection";
        case TypeHintSource::None:
        default: return "none";
    }
}

const char *to_string(DirtyState value) {
    switch (value) {
        case DirtyState::Clean: return "clean";
        case DirtyState::Deleted: return "deleted";
        case DirtyState::Dirty:
        default: return "dirty";
    }
}

const char *to_string(DirtyReason value) {
    switch (value) {
        case DirtyReason::None: return "none";
        case DirtyReason::NewPath: return "new_path";
        case DirtyReason::DeletedPath: return "deleted_path";
        case DirtyReason::KindChanged: return "kind_changed";
        case DirtyReason::SizeChanged: return "size_changed";
        case DirtyReason::ModifiedTimeChanged: return "modified_time_changed";
        case DirtyReason::FileIdentityChanged: return "file_identity_changed";
        case DirtyReason::ParserVersionChanged: return "parser_version_changed";
        case DirtyReason::ClassifierVersionChanged: return "classifier_version_changed";
        case DirtyReason::PriorParseFailedRetry: return "prior_parse_failed_retry";
        case DirtyReason::WatcherInvalidated: return "watcher_invalidated";
        case DirtyReason::ForceRescan: return "force_rescan";
        default: return "unknown";
    }
}

const char *to_string(ParseStatus value) {
    switch (value) {
        case ParseStatus::ParsedClass: return "parsed_class";
        case ParseStatus::NoClass: return "no_class";
        case ParseStatus::IoError: return "io_error";
        case ParseStatus::UnsupportedLanguage: return "unsupported_language";
        case ParseStatus::Malformed: return "malformed";
        case ParseStatus::NotParsed:
        default: return "not_parsed";
    }
}

const char *to_string(ScriptLanguage value) {
    switch (value) {
        case ScriptLanguage::GDScript: return "GDScript";
        case ScriptLanguage::CSharp: return "CSharp";
        case ScriptLanguage::Unknown:
        default: return "";
    }
}

} // namespace gotool::project_scanner
