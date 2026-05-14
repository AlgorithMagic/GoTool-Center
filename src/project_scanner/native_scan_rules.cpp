// Copyright 2026 AlgorithMagic

#include "project_scanner/native_scan_rules.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

// NOLINTBEGIN(readability-static-definition-in-anonymous-namespace,readability-uppercase-literal-suffix,readability-use-anyofallof,modernize-use-starts-ends-with,modernize-use-ranges,modernize-use-auto,modernize-return-braced-init-list,modernize-use-emplace,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,readability-function-size)
namespace gotool::project_scanner {

namespace {

static constexpr uint32_t ENTRY_FLAG_HIDDEN = 1u << 0u;
static constexpr uint32_t ENTRY_FLAG_HAS_PLATFORM_FILE_ID = 1u << 1u;

bool matches_any(std::string_view value, std::initializer_list<std::string_view> candidates)
{
    for (const std::string_view candidate : candidates) {
        if (value == candidate) {
            return true;
        }
    }

    return false;
}

bool ends_with_any(std::string_view value, std::initializer_list<std::string_view> candidates)
{
    for (const std::string_view candidate : candidates) {
        if (value.size() >= candidate.size() &&
            value.compare(value.size() - candidate.size(), candidate.size(), candidate) == 0) {
            return true;
        }
    }

    return false;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string_view file_name_view_from_normalized_path(std::string_view normalized_path)
{
    const size_t slash = normalized_path.rfind('/');
    return slash == std::string::npos ? normalized_path : normalized_path.substr(slash + 1);
}

std::string extension_from_name_lower(std::string_view lower_name)
{
    const size_t dot = lower_name.rfind('.');

    if (dot == std::string::npos || dot == 0 || dot + 1 >= lower_name.size()) {
        return "";
    }

    return std::string(lower_name.substr(dot));
}

std::string normalize_prefix_for_internal(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');

    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }

    while (starts_with(value, "./")) {
        value.erase(0, 2);
    }

    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }

    return value;
}

} // namespace

uint32_t PathArena::append(std::string_view value)
{
    const uint32_t offset = static_cast<uint32_t>(m_data.size());

    if (value.empty()) {
        return offset;
    }

    const size_t old_size = m_data.size();
    m_data.resize(old_size + value.size());
    std::memcpy(m_data.data() + old_size, value.data(), value.size());
    return offset;
}

std::string_view PathArena::view(uint32_t offset, uint32_t length) const
{
    if (offset > m_data.size() ||
        static_cast<size_t>(offset) + static_cast<size_t>(length) > m_data.size()) {
        return {};
    }

    return std::string_view(m_data.data() + offset, length);
}

std::string PathArena::string_at(uint32_t offset, uint32_t length) const
{
    const std::string_view value = view(offset, length);
    return std::string(value.begin(), value.end());
}

void PathArena::reserve(size_t bytes)
{
    m_data.reserve(bytes);
}

void PathArena::clear()
{
    m_data.clear();
}

size_t PathArena::size() const
{
    return m_data.size();
}

size_t PathArena::capacity() const
{
    return m_data.capacity();
}

bool EntryRecord::is_hidden() const
{
    return (flags & ENTRY_FLAG_HIDDEN) != 0;
}

void EntryRecord::set_hidden(bool hidden)
{
    if (hidden) {
        flags |= ENTRY_FLAG_HIDDEN;
    } else {
        flags &= ~ENTRY_FLAG_HIDDEN;
    }
}

bool EntryRecord::has_platform_file_id() const
{
    return (flags & ENTRY_FLAG_HAS_PLATFORM_FILE_ID) != 0;
}

void EntryRecord::clear_platform_file_id()
{
    flags &= ~ENTRY_FLAG_HAS_PLATFORM_FILE_ID;
    platform_file_id_high = 0;
    platform_file_id_low = 0;
}

SkipPolicy::SkipPolicy()
{
    m_prefixes.push_back("addons/GoToolCenter");
    m_prefixes.push_back(".godot/gotool_center");
    m_prefixes.push_back(".godot/imported");
    m_prefixes.push_back(".godot/shader_cache");
}

bool SkipPolicy::should_skip_normalized(std::string_view project_relative_path) const
{
    const std::string_view normalized = project_relative_path;

    for (const std::string& prefix : m_prefixes) {
        if (normalized == prefix) {
            return true;
        }

        if (normalized.size() > prefix.size() && normalized[prefix.size()] == '/' &&
            starts_with(normalized, prefix)) {
            return true;
        }
    }

    return false;
}

bool SkipPolicy::should_skip_external(std::string_view project_relative_path) const
{
    return should_skip_normalized(normalize_project_path(project_relative_path));
}

bool SkipPolicy::should_skip(std::string_view project_relative_path) const
{
    return should_skip_external(project_relative_path);
}

void SkipPolicy::add_prefix_normalized(std::string prefix)
{
    m_prefixes.push_back(normalize_prefix_for_internal(std::move(prefix)));
}

void SkipPolicy::add_prefix_external(std::string_view prefix)
{
    m_prefixes.push_back(normalize_project_path(prefix));
}

void SkipPolicy::add_prefix(std::string_view prefix)
{
    add_prefix_external(prefix);
}

std::string normalize_project_path(std::string_view path)
{
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

std::string lower_ascii(std::string_view value)
{
    std::string lowered;
    lowered.reserve(value.size());

    for (const char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    return lowered;
}

std::string extension_from_path(std::string_view path)
{
    const std::string normalized = normalize_project_path(path);
    const std::string name = lower_ascii(file_name_view_from_normalized_path(normalized));
    return extension_from_name_lower(name);
}

std::string file_name_from_path(std::string_view path)
{
    const std::string normalized = normalize_project_path(path);
    const size_t slash = normalized.rfind('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

bool is_script_extension(std::string_view extension)
{
    const ExtensionId extension_id = extension_id_from_extension(extension);
    return extension_id == ExtensionId::GD || extension_id == ExtensionId::CS;
}

ScriptLanguage language_from_extension(std::string_view extension)
{
    const ExtensionId extension_id = extension_id_from_extension(extension);

    if (extension_id == ExtensionId::GD) {
        return ScriptLanguage::GDScript;
    }

    if (extension_id == ExtensionId::CS) {
        return ScriptLanguage::CSharp;
    }

    return ScriptLanguage::Unknown;
}

ExtensionId extension_id_from_extension(std::string_view extension)
{
    if (extension.empty()) {
        return ExtensionId::Unknown;
    }

    // Dense table-style chain keeps lookup straightforward and branch-predictable.
    // NOLINTBEGIN(readability-braces-around-statements)
    if (extension == ".tscn")
        return ExtensionId::TSCN;
    if (extension == ".scn")
        return ExtensionId::SCN;
    if (extension == ".tres")
        return ExtensionId::TRES;
    if (extension == ".res")
        return ExtensionId::RES;
    if (extension == ".gd")
        return ExtensionId::GD;
    if (extension == ".cs")
        return ExtensionId::CS;
    if (extension == ".sh")
        return ExtensionId::SH;
    if (extension == ".gdshader")
        return ExtensionId::GDSHADER;
    if (extension == ".gdshaderinc")
        return ExtensionId::GDSHADERINC;
    if (extension == ".shader")
        return ExtensionId::SHADER;
    if (extension == ".import")
        return ExtensionId::IMPORT;
    if (extension == ".uid")
        return ExtensionId::UID;
    if (extension == ".png")
        return ExtensionId::PNG;
    if (extension == ".jpg")
        return ExtensionId::JPG;
    if (extension == ".jpeg")
        return ExtensionId::JPEG;
    if (extension == ".webp")
        return ExtensionId::WEBP;
    if (extension == ".svg")
        return ExtensionId::SVG;
    if (extension == ".bmp")
        return ExtensionId::BMP;
    if (extension == ".gif")
        return ExtensionId::GIF;
    if (extension == ".tga")
        return ExtensionId::TGA;
    if (extension == ".exr")
        return ExtensionId::EXR;
    if (extension == ".wav")
        return ExtensionId::WAV;
    if (extension == ".ogg")
        return ExtensionId::OGG;
    if (extension == ".opus")
        return ExtensionId::OPUS;
    if (extension == ".mp3")
        return ExtensionId::MP3;
    if (extension == ".flac")
        return ExtensionId::FLAC;
    if (extension == ".otf")
        return ExtensionId::OTF;
    if (extension == ".ttf")
        return ExtensionId::TTF;
    if (extension == ".fnt")
        return ExtensionId::FNT;
    if (extension == ".woff")
        return ExtensionId::WOFF;
    if (extension == ".woff2")
        return ExtensionId::WOFF2;
    if (extension == ".glb")
        return ExtensionId::GLB;
    if (extension == ".gltf")
        return ExtensionId::GLTF;
    if (extension == ".fbx")
        return ExtensionId::FBX;
    if (extension == ".dae")
        return ExtensionId::DAE;
    if (extension == ".blend")
        return ExtensionId::BLEND;
    if (extension == ".meshlib")
        return ExtensionId::MESHLIB;
    if (extension == ".godot")
        return ExtensionId::GODOT;
    if (extension == ".cfg")
        return ExtensionId::CFG;
    if (extension == ".ini")
        return ExtensionId::INI_FILE;
    if (extension == ".conf")
        return ExtensionId::CONF_FILE;
    if (extension == ".config")
        return ExtensionId::CONFIG_FILE;
    if (extension == ".gdextension")
        return ExtensionId::GD_EXTENSION_FILE;
    if (extension == ".json")
        return ExtensionId::JSON;
    if (extension == ".csv")
        return ExtensionId::CSV;
    if (extension == ".yaml")
        return ExtensionId::YAML;
    if (extension == ".yml")
        return ExtensionId::YML;
    if (extension == ".toml")
        return ExtensionId::TOML;
    if (extension == ".xml")
        return ExtensionId::XML;
    if (extension == ".dat")
        return ExtensionId::DAT;
    if (extension == ".bytes")
        return ExtensionId::BYTES;
    if (extension == ".md")
        return ExtensionId::MD;
    if (extension == ".txt")
        return ExtensionId::TXT;
    if (extension == ".rst")
        return ExtensionId::RST;
    if (extension == ".adoc")
        return ExtensionId::ADOC;
    if (extension == ".dll")
        return ExtensionId::DLL;
    if (extension == ".so")
        return ExtensionId::SO;
    if (extension == ".dylib")
        return ExtensionId::DYLIB;
    if (extension == ".lib")
        return ExtensionId::LIB;
    if (extension == ".a")
        return ExtensionId::STATICLIB_A;
    if (extension == ".obj")
        return ExtensionId::OBJ;
    if (extension == ".o")
        return ExtensionId::OBJECT_O;
    if (extension == ".pdb")
        return ExtensionId::PDB;
    if (extension == ".exp")
        return ExtensionId::EXP;
    if (extension == ".db")
        return ExtensionId::DB;
    if (extension == ".sqlite")
        return ExtensionId::SQLITE;
    if (extension == ".sqlite3")
        return ExtensionId::SQLITE3;
    if (extension == ".zip")
        return ExtensionId::ZIP;
    if (extension == ".tar")
        return ExtensionId::TAR;
    if (extension == ".gz")
        return ExtensionId::GZ;
    if (extension == ".rar")
        return ExtensionId::RAR;
    if (extension == ".7z")
        return ExtensionId::SEVEN_Z;
    if (extension == ".node")
        return ExtensionId::NODE;
    if (extension == ".object")
        return ExtensionId::OBJECT;
    if (extension == ".resource")
        return ExtensionId::RESOURCE;
    if (extension == ".md5")
        return ExtensionId::MD5;
    if (extension == ".cache")
        return ExtensionId::CACHE;
    if (extension == ".ase")
        return ExtensionId::ASE;
    if (extension == ".aseprite")
        return ExtensionId::ASEPRITE;
    if (extension == ".kra")
        return ExtensionId::KRA;
    if (extension == ".psd")
        return ExtensionId::PSD;
    if (extension == ".xcf")
        return ExtensionId::XCF;
    if (extension == ".spp")
        return ExtensionId::SPP;
    if (extension == ".bin")
        return ExtensionId::BIN;
    if (extension == ".c")
        return ExtensionId::SRC_C;
    if (extension == ".h")
        return ExtensionId::SRC_H;
    if (extension == ".cpp")
        return ExtensionId::CPP;
    if (extension == ".hpp")
        return ExtensionId::HPP;
    if (extension == ".py")
        return ExtensionId::PY;
    if (extension == ".js")
        return ExtensionId::JS;
    if (extension == ".ts")
        return ExtensionId::TS;
    if (extension == ".rs")
        return ExtensionId::RS;
    if (extension == ".go")
        return ExtensionId::GO;
    if (extension == ".java")
        return ExtensionId::JAVA;
    if (extension == ".blend1")
        return ExtensionId::BLEND1;
    if (extension == ".blend2")
        return ExtensionId::BLEND2;
    if (extension == ".assbin")
        return ExtensionId::ASSBIN;
    if (extension == ".remap")
        return ExtensionId::REMAP;
    // NOLINTEND(readability-braces-around-statements)

    return ExtensionId::Unknown;
}

// NOLINTNEXTLINE(readability-function-size)
FileTypeId classify_entry_from_facts(const EntryFacts& facts)
{
    if (facts.entry_kind == EntryKind::Directory) {
        return FileTypeId::Folder;
    }

    const std::string_view path = facts.project_relative_path_lower.empty()
                                      ? facts.project_relative_path
                                      : facts.project_relative_path_lower;
    const std::string_view name = file_name_view_from_normalized_path(path);

    switch (facts.extension_id) {
        case ExtensionId::TSCN:
        case ExtensionId::SCN:
            return FileTypeId::GodotScene;
        case ExtensionId::TRES:
        case ExtensionId::RES:
        case ExtensionId::MESHLIB:
            return FileTypeId::GodotResource;
        case ExtensionId::GD:
        case ExtensionId::CS:
        case ExtensionId::SH:
            return FileTypeId::Script;
        case ExtensionId::GDSHADER:
        case ExtensionId::GDSHADERINC:
        case ExtensionId::SHADER:
            return FileTypeId::Shader;
        case ExtensionId::IMPORT:
            return FileTypeId::GodotImportMetadata;
        case ExtensionId::NODE:
        case ExtensionId::OBJECT:
        case ExtensionId::RESOURCE:
            return FileTypeId::GodotEditorMetadata;
        case ExtensionId::JSON:
        case ExtensionId::CSV:
        case ExtensionId::YAML:
        case ExtensionId::YML:
        case ExtensionId::TOML:
        case ExtensionId::XML:
        case ExtensionId::DAT:
        case ExtensionId::BYTES:
            return FileTypeId::Data;
        case ExtensionId::PNG:
        case ExtensionId::JPG:
        case ExtensionId::JPEG:
        case ExtensionId::WEBP:
        case ExtensionId::SVG:
        case ExtensionId::BMP:
        case ExtensionId::GIF:
        case ExtensionId::TGA:
            return FileTypeId::Asset;
        case ExtensionId::EXR:
            return FileTypeId::Image;
        case ExtensionId::WAV:
        case ExtensionId::MP3:
        case ExtensionId::OGG:
        case ExtensionId::OPUS:
        case ExtensionId::FLAC:
            return FileTypeId::Audio;
        case ExtensionId::OTF:
        case ExtensionId::TTF:
        case ExtensionId::FNT:
        case ExtensionId::WOFF:
        case ExtensionId::WOFF2:
            return FileTypeId::Font;
        case ExtensionId::BLEND1:
        case ExtensionId::BLEND2:
            return FileTypeId::ModelBackup;
        case ExtensionId::ASSBIN:
            return FileTypeId::ModelCache;
        case ExtensionId::GLB:
        case ExtensionId::GLTF:
        case ExtensionId::FBX:
        case ExtensionId::OBJ:
        case ExtensionId::DAE:
        case ExtensionId::BLEND:
            return FileTypeId::Model;
        case ExtensionId::ZIP:
        case ExtensionId::SEVEN_Z:
        case ExtensionId::RAR:
        case ExtensionId::TAR:
        case ExtensionId::GZ:
            return FileTypeId::Archive;
        case ExtensionId::DB:
        case ExtensionId::SQLITE:
        case ExtensionId::SQLITE3:
            return FileTypeId::Database;
        case ExtensionId::DLL:
        case ExtensionId::SO:
        case ExtensionId::DYLIB:
        case ExtensionId::LIB:
        case ExtensionId::STATICLIB_A:
        case ExtensionId::OBJECT_O:
        case ExtensionId::PDB:
        case ExtensionId::EXP:
            return FileTypeId::BuildArtifact;
        case ExtensionId::TXT:
        case ExtensionId::MD:
        case ExtensionId::RST:
        case ExtensionId::ADOC:
            return FileTypeId::Documentation;
        case ExtensionId::ASE:
            return FileTypeId::ColorPalette;
        case ExtensionId::SRC_C:
        case ExtensionId::SRC_H:
        case ExtensionId::CPP:
        case ExtensionId::HPP:
        case ExtensionId::PY:
        case ExtensionId::JS:
        case ExtensionId::TS:
        case ExtensionId::RS:
        case ExtensionId::GO:
        case ExtensionId::JAVA:
            return FileTypeId::SourceCode;
        case ExtensionId::XCF:
        case ExtensionId::PSD:
        case ExtensionId::ASEPRITE:
        case ExtensionId::KRA:
            return FileTypeId::SourceArt;
        case ExtensionId::SPP:
            return FileTypeId::MaterialSource;
        case ExtensionId::BIN:
            return FileTypeId::BinaryData;
        default:
            break;
    }

    if (starts_with(path, ".godot/imported/") && facts.extension_id == ExtensionId::MD5) {
        return FileTypeId::GodotImportHash;
    }

    if (starts_with(path, ".godot/shader_cache/") && facts.extension_id == ExtensionId::CACHE) {
        return FileTypeId::GodotShaderCache;
    }

    if (facts.extension_id == ExtensionId::CFG || facts.extension_id == ExtensionId::INI_FILE ||
        facts.extension_id == ExtensionId::CONF_FILE ||
        facts.extension_id == ExtensionId::CONFIG_FILE ||
        facts.extension_id == ExtensionId::GODOT || facts.extension_id == ExtensionId::UID ||
        facts.extension_id == ExtensionId::GD_EXTENSION_FILE || name == "plugin.cfg" ||
        name == "project.godot" || name == ".editorconfig" || name == ".gitignore" ||
        name == ".gitattributes") {
        return FileTypeId::Config;
    }

    if (ends_with_any(name, {".x86_64"})) {
        return FileTypeId::GeneratedArtifact;
    }

    return FileTypeId::Unknown;
}

GodotTypeHint detect_godot_type_hint_from_facts(const EntryFacts& facts, FileTypeId file_type)
{
    if (file_type == FileTypeId::Folder) {
        return GodotTypeHint::NotGodotTyped;
    }

    const std::string_view path = facts.project_relative_path_lower.empty()
                                      ? facts.project_relative_path
                                      : facts.project_relative_path_lower;
    const std::string_view name = file_name_view_from_normalized_path(path);

    switch (facts.extension_id) {
        case ExtensionId::TSCN:
        case ExtensionId::SCN:
        case ExtensionId::GLB:
        case ExtensionId::GLTF:
        case ExtensionId::FBX:
        case ExtensionId::DAE:
        case ExtensionId::BLEND:
            return GodotTypeHint::PackedScene;
        case ExtensionId::TRES:
        case ExtensionId::RES:
            return GodotTypeHint::Resource;
        case ExtensionId::MESHLIB:
            return GodotTypeHint::MeshLibrary;
        case ExtensionId::GD:
            return GodotTypeHint::GDScript;
        case ExtensionId::CS:
            return GodotTypeHint::CSharpScript;
        case ExtensionId::GDSHADER:
        case ExtensionId::SHADER:
            return GodotTypeHint::Shader;
        case ExtensionId::GDSHADERINC:
            return GodotTypeHint::ShaderInclude;
        case ExtensionId::PNG:
        case ExtensionId::JPG:
        case ExtensionId::JPEG:
        case ExtensionId::WEBP:
        case ExtensionId::SVG:
        case ExtensionId::BMP:
        case ExtensionId::GIF:
        case ExtensionId::TGA:
            return GodotTypeHint::Texture2D;
        case ExtensionId::WAV:
            return GodotTypeHint::AudioStreamWAV;
        case ExtensionId::MP3:
            return GodotTypeHint::AudioStreamMP3;
        case ExtensionId::OGG:
        case ExtensionId::OPUS:
            return GodotTypeHint::AudioStreamOggVorbis;
        case ExtensionId::OTF:
        case ExtensionId::TTF:
        case ExtensionId::FNT:
        case ExtensionId::WOFF:
        case ExtensionId::WOFF2:
            return GodotTypeHint::FontFile;
        case ExtensionId::GD_EXTENSION_FILE:
            return GodotTypeHint::GDExtension;
        case ExtensionId::UID:
            return GodotTypeHint::ResourceUID;
        case ExtensionId::CFG:
        case ExtensionId::INI_FILE:
        case ExtensionId::CONF_FILE:
        case ExtensionId::CONFIG_FILE:
        case ExtensionId::REMAP:
            return GodotTypeHint::ConfigFile;
        default:
            break;
    }

    if (name == "plugin.cfg") {
        return GodotTypeHint::ConfigFile;
    }

    if (name == "project.godot") {
        return GodotTypeHint::ProjectSettings;
    }

    return GodotTypeHint::NotGodotTyped;
}

FileTypeId classify_entry(std::string_view project_relative_path, EntryKind kind)
{
    const std::string normalized = normalize_project_path(project_relative_path);
    const std::string lowered = lower_ascii(normalized);
    const std::string_view name = file_name_view_from_normalized_path(lowered);
    const std::string extension = extension_from_name_lower(name);

    EntryFacts facts;
    facts.project_relative_path = normalized;
    facts.project_relative_path_lower = lowered;
    facts.file_name = name;
    facts.extension = extension;
    facts.entry_kind = kind;
    facts.extension_id = extension_id_from_extension(extension);
    return classify_entry_from_facts(facts);
}

GodotTypeHint detect_godot_type_hint(std::string_view project_relative_path, FileTypeId file_type)
{
    const std::string normalized = normalize_project_path(project_relative_path);
    const std::string lowered = lower_ascii(normalized);
    const std::string_view name = file_name_view_from_normalized_path(lowered);
    const std::string extension = extension_from_name_lower(name);

    EntryFacts facts;
    facts.project_relative_path = normalized;
    facts.project_relative_path_lower = lowered;
    facts.file_name = name;
    facts.extension = extension;
    facts.entry_kind = EntryKind::File;
    facts.extension_id = extension_id_from_extension(extension);
    return detect_godot_type_hint_from_facts(facts, file_type);
}

TypeHintSource type_hint_source_for(GodotTypeHint hint)
{
    return hint == GodotTypeHint::NotGodotTyped ? TypeHintSource::None : TypeHintSource::Extension;
}

DirtyCheckResult detect_dirty_state(EntryKind kind, int64_t size_bytes, int64_t modified_time_ns,
                                    std::string_view platform_file_id,
                                    const ExistingEntrySnapshot& existing, bool force_rescan)
{
    if (force_rescan) {
        return {DirtyState::Dirty, DirtyReason::ForceRescan};
    }

    if (existing.entry_kind != kind) {
        return {DirtyState::Dirty, DirtyReason::KindChanged};
    }

    // Directory timestamps can jitter on some filesystems/OS combinations
    // without representing meaningful inventory changes, so only file entries
    // use size/mtime as dirtiness signals.
    if (kind == EntryKind::File) {
        if (existing.size_bytes != size_bytes) {
            return {DirtyState::Dirty, DirtyReason::SizeChanged};
        }

        if (existing.modified_time_ns != modified_time_ns) {
            return {DirtyState::Dirty, DirtyReason::ModifiedTimeChanged};
        }
    }

    if (!existing.platform_file_id.empty() && !platform_file_id.empty() &&
        existing.platform_file_id != platform_file_id) {
        return {DirtyState::Dirty, DirtyReason::FileIdentityChanged};
    }

    if (existing.parser_version != PARSER_VERSION) {
        return {DirtyState::Dirty, DirtyReason::ParserVersionChanged};
    }

    if (existing.dependency_parser_version != DEPENDENCY_PARSER_VERSION) {
        return {DirtyState::Dirty, DirtyReason::DependencyParserVersionChanged};
    }

    if (existing.classifier_version != CLASSIFIER_VERSION) {
        return {DirtyState::Dirty, DirtyReason::ClassifierVersionChanged};
    }

    if (existing.parse_status == ParseStatus::IoError ||
        existing.parse_status == ParseStatus::Malformed) {
        return {DirtyState::Dirty, DirtyReason::PriorParseFailedRetry};
    }

    return {DirtyState::Clean, DirtyReason::None};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
DirtyCheckResult detect_dirty_state(EntryKind kind, int64_t size_bytes, int64_t modified_time_ns,
                                    std::string_view platform_file_id,
                                    const std::optional<ExistingEntrySnapshot>& existing,
                                    bool force_rescan)
{
    if (!existing.has_value()) {
        (void)kind;
        (void)size_bytes;
        (void)modified_time_ns;
        (void)platform_file_id;
        return {DirtyState::Dirty, DirtyReason::NewPath};
    }

    return detect_dirty_state(kind, size_bytes, modified_time_ns, platform_file_id,
                              existing.value(), force_rescan);
}

std::string platform_file_id_to_string(const EntryRecord& record)
{
    if (!record.has_platform_file_id()) {
        return "";
    }

    std::ostringstream output;
    output << std::hex << record.platform_file_id_high << ":" << record.platform_file_id_low;
    return output.str();
}

bool is_builtin_node_type_hint(std::string_view type_name)
{
    return matches_any(type_name,
                       {"Node",        "Node2D",          "Node3D",           "Control",
                        "CanvasItem",  "CharacterBody2D", "CharacterBody3D",  "RigidBody2D",
                        "RigidBody3D", "StaticBody2D",    "StaticBody3D",     "Area2D",
                        "Area3D",      "Sprite2D",        "AnimatedSprite2D", "Camera2D",
                        "Camera3D",    "Label",           "Button",           "Panel",
                        "TextureRect", "VBoxContainer",   "HBoxContainer",    "MarginContainer",
                        "EditorPlugin"});
}

bool is_builtin_resource_type_hint(std::string_view type_name)
{
    return matches_any(type_name,
                       {"Resource", "Script", "GDScript", "CSharpScript", "Shader", "Texture2D",
                        "ImageTexture", "PackedScene", "Material", "StandardMaterial3D",
                        "AudioStream", "Font", "Mesh", "MeshLibrary"});
}

const char* to_string(EntryKind value)
{
    return value == EntryKind::Directory ? "directory" : "file";
}

const char* to_string(ExtensionId value)
{
    switch (value) {
        case ExtensionId::TSCN:
            return ".tscn";
        case ExtensionId::SCN:
            return ".scn";
        case ExtensionId::TRES:
            return ".tres";
        case ExtensionId::RES:
            return ".res";
        case ExtensionId::GD:
            return ".gd";
        case ExtensionId::CS:
            return ".cs";
        case ExtensionId::SH:
            return ".sh";
        case ExtensionId::GDSHADER:
            return ".gdshader";
        case ExtensionId::GDSHADERINC:
            return ".gdshaderinc";
        case ExtensionId::SHADER:
            return ".shader";
        case ExtensionId::IMPORT:
            return ".import";
        case ExtensionId::UID:
            return ".uid";
        case ExtensionId::PNG:
            return ".png";
        case ExtensionId::JPG:
            return ".jpg";
        case ExtensionId::JPEG:
            return ".jpeg";
        case ExtensionId::WEBP:
            return ".webp";
        case ExtensionId::SVG:
            return ".svg";
        case ExtensionId::WAV:
            return ".wav";
        case ExtensionId::OGG:
            return ".ogg";
        case ExtensionId::MP3:
            return ".mp3";
        case ExtensionId::GLB:
            return ".glb";
        case ExtensionId::GLTF:
            return ".gltf";
        case ExtensionId::FBX:
            return ".fbx";
        case ExtensionId::DAE:
            return ".dae";
        case ExtensionId::BLEND:
            return ".blend";
        case ExtensionId::MESHLIB:
            return ".meshlib";
        case ExtensionId::GODOT:
            return ".godot";
        case ExtensionId::CFG:
            return ".cfg";
        case ExtensionId::JSON:
            return ".json";
        case ExtensionId::CSV:
            return ".csv";
        case ExtensionId::MD:
            return ".md";
        case ExtensionId::DLL:
            return ".dll";
        case ExtensionId::SO:
            return ".so";
        case ExtensionId::DYLIB:
            return ".dylib";
        case ExtensionId::BIN:
            return ".bin";
        case ExtensionId::Unknown:
        default:
            return "";
    }
}

const char* to_string(FileTypeId value)
{
    switch (value) {
        case FileTypeId::Folder:
            return "Folder";
        case FileTypeId::GodotScene:
            return "GodotScene";
        case FileTypeId::GodotResource:
            return "GodotResource";
        case FileTypeId::Script:
            return "Script";
        case FileTypeId::Shader:
            return "Shader";
        case FileTypeId::Asset:
            return "Asset";
        case FileTypeId::Config:
            return "Config";
        case FileTypeId::GodotImportMetadata:
            return "GodotImportMetadata";
        case FileTypeId::GodotImportHash:
            return "GodotImportHash";
        case FileTypeId::GodotShaderCache:
            return "GodotShaderCache";
        case FileTypeId::GodotEditorMetadata:
            return "GodotEditorMetadata";
        case FileTypeId::Data:
            return "Data";
        case FileTypeId::GeneratedArtifact:
            return "GeneratedArtifact";
        case FileTypeId::SourceArt:
            return "SourceArt";
        case FileTypeId::ColorPalette:
            return "ColorPalette";
        case FileTypeId::SourceCode:
            return "SourceCode";
        case FileTypeId::Documentation:
            return "Documentation";
        case FileTypeId::Image:
            return "Image";
        case FileTypeId::Texture:
            return "Texture";
        case FileTypeId::Audio:
            return "Audio";
        case FileTypeId::Video:
            return "Video";
        case FileTypeId::Font:
            return "Font";
        case FileTypeId::Model:
            return "Model";
        case FileTypeId::ModelBackup:
            return "ModelBackup";
        case FileTypeId::ModelCache:
            return "ModelCache";
        case FileTypeId::MaterialSource:
            return "MaterialSource";
        case FileTypeId::BinaryData:
            return "BinaryData";
        case FileTypeId::Archive:
            return "Archive";
        case FileTypeId::Database:
            return "Database";
        case FileTypeId::BuildArtifact:
            return "BuildArtifact";
        case FileTypeId::Unknown:
        default:
            return "Unknown";
    }
}

const char* to_string(GodotTypeHint value)
{
    switch (value) {
        case GodotTypeHint::PackedScene:
            return "PackedScene";
        case GodotTypeHint::Resource:
            return "Resource";
        case GodotTypeHint::GDScript:
            return "GDScript";
        case GodotTypeHint::CSharpScript:
            return "CSharpScript";
        case GodotTypeHint::Shader:
            return "Shader";
        case GodotTypeHint::ShaderInclude:
            return "ShaderInclude";
        case GodotTypeHint::Texture2D:
            return "Texture2D";
        case GodotTypeHint::AudioStreamWAV:
            return "AudioStreamWAV";
        case GodotTypeHint::AudioStreamMP3:
            return "AudioStreamMP3";
        case GodotTypeHint::AudioStreamOggVorbis:
            return "AudioStreamOggVorbis";
        case GodotTypeHint::FontFile:
            return "FontFile";
        case GodotTypeHint::MeshLibrary:
            return "MeshLibrary";
        case GodotTypeHint::GDExtension:
            return "GDExtension";
        case GodotTypeHint::ConfigFile:
            return "ConfigFile";
        case GodotTypeHint::ResourceUID:
            return "ResourceUID";
        case GodotTypeHint::ProjectSettings:
            return "ProjectSettings";
        case GodotTypeHint::NotGodotTyped:
        default:
            return "NGT";
    }
}

const char* to_string(TypeHintSource value)
{
    switch (value) {
        case TypeHintSource::Extension:
            return "extension";
        case TypeHintSource::Path:
            return "path";
        case TypeHintSource::ImportMetadata:
            return "import_metadata";
        case TypeHintSource::EditorFileSystem:
            return "editor_filesystem";
        case TypeHintSource::ExplicitInspection:
            return "explicit_inspection";
        case TypeHintSource::None:
        default:
            return "none";
    }
}

const char* to_string(DirtyState value)
{
    switch (value) {
        case DirtyState::Clean:
            return "clean";
        case DirtyState::Deleted:
            return "deleted";
        case DirtyState::Dirty:
        default:
            return "dirty";
    }
}

const char* to_string(DirtyReason value)
{
    switch (value) {
        case DirtyReason::None:
            return "none";
        case DirtyReason::NewPath:
            return "new_path";
        case DirtyReason::DeletedPath:
            return "deleted_path";
        case DirtyReason::KindChanged:
            return "kind_changed";
        case DirtyReason::SizeChanged:
            return "size_changed";
        case DirtyReason::ModifiedTimeChanged:
            return "modified_time_changed";
        case DirtyReason::FileIdentityChanged:
            return "file_identity_changed";
        case DirtyReason::ParserVersionChanged:
            return "parser_version_changed";
        case DirtyReason::DependencyParserVersionChanged:
            return "dependency_parser_version_changed";
        case DirtyReason::SceneParserVersionChanged:
            return "scene_parser_version_changed";
        case DirtyReason::ClassifierVersionChanged:
            return "classifier_version_changed";
        case DirtyReason::PriorParseFailedRetry:
            return "prior_parse_failed_retry";
        case DirtyReason::WatcherInvalidated:
            return "watcher_invalidated";
        case DirtyReason::ForceRescan:
            return "force_rescan";
        default:
            return "unknown";
    }
}

const char* to_string(ParseStatus value)
{
    switch (value) {
        case ParseStatus::ParsedClass:
            return "parsed_class";
        case ParseStatus::NoClass:
            return "no_class";
        case ParseStatus::IoError:
            return "io_error";
        case ParseStatus::UnsupportedLanguage:
            return "unsupported_language";
        case ParseStatus::Malformed:
            return "malformed";
        case ParseStatus::NotParsed:
        default:
            return "not_parsed";
    }
}

const char* to_string(ScriptLanguage value)
{
    switch (value) {
        case ScriptLanguage::GDScript:
            return "GDScript";
        case ScriptLanguage::CSharp:
            return "CSharp";
        case ScriptLanguage::Unknown:
        default:
            return "";
    }
}

const char* to_string(DependencyKind value)
{
    switch (value) {
        case DependencyKind::PreloadPath:
            return "preload_path";
        case DependencyKind::LoadPath:
            return "load_path";
        case DependencyKind::ResourceLoaderLoadPath:
            return "resource_loader_load_path";
        case DependencyKind::GDLoadPath:
            return "gd_load_path";
        case DependencyKind::ExtendsPath:
            return "extends_path";
        case DependencyKind::ExtendsClass:
            return "extends_class";
        case DependencyKind::ClassNameDeclaration:
            return "class_name_declaration";
        case DependencyKind::ConstPreloadAlias:
            return "const_preload_alias";
        case DependencyKind::TypedVarRef:
            return "typed_var_ref";
        case DependencyKind::TypedParamRef:
            return "typed_param_ref";
        case DependencyKind::TypedReturnRef:
            return "typed_return_ref";
        case DependencyKind::TypedArrayElementRef:
            return "typed_array_element_ref";
        case DependencyKind::TypedDictionaryRef:
            return "typed_dictionary_ref";
        case DependencyKind::ExportTypeRef:
            return "export_type_ref";
        case DependencyKind::SignalTypeRef:
            return "signal_type_ref";
        case DependencyKind::NewClassInstantiation:
            return "new_class_instantiation";
        case DependencyKind::SceneNodePath:
            return "scene_node_path";
        case DependencyKind::ResourceUIDRef:
            return "resource_uid_ref";
        case DependencyKind::DynamicLoad:
            return "dynamic_load";
        case DependencyKind::UnresolvedSymbol:
            return "unresolved_symbol";
        case DependencyKind::Unknown:
        default:
            return "unknown";
    }
}

// NOLINTEND(readability-static-definition-in-anonymous-namespace,readability-uppercase-literal-suffix,readability-use-anyofallof,modernize-use-starts-ends-with,modernize-use-ranges,modernize-use-auto,modernize-return-braced-init-list,modernize-use-emplace,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,readability-function-size)
} // namespace gotool::project_scanner
