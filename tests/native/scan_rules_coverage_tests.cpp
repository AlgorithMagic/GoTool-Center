// Copyright 2026 AlgorithMagic

#include <optional>
#include <string>
#include <vector>

#include "doctest.h"
#include "project_scanner/native_scan_rules.hpp"

namespace {

using gotool::project_scanner::DependencyKind;
using gotool::project_scanner::DirtyReason;
using gotool::project_scanner::DirtyState;
using gotool::project_scanner::EntryFacts;
using gotool::project_scanner::EntryKind;
using gotool::project_scanner::EntryRecord;
using gotool::project_scanner::ExistingEntrySnapshot;
using gotool::project_scanner::ExtensionId;
using gotool::project_scanner::FileTypeId;
using gotool::project_scanner::GodotTypeHint;
using gotool::project_scanner::ParseStatus;
using gotool::project_scanner::ScriptLanguage;
using gotool::project_scanner::TypeHintSource;

} // namespace

// These table-driven coverage tests intentionally keep broad scenario checks in one case.
// NOLINTBEGIN(readability-function-size)
TEST_CASE("scan_rules_to_string_covers_non_default_enum_paths")
{
    struct ExtensionCase
    {
        ExtensionId id;
        const char* expected;

        constexpr ExtensionCase(ExtensionId id_value, const char* expected_value) noexcept
            : id(id_value), expected(expected_value)
        {}
    };

    const std::vector<ExtensionCase> extension_cases = {
        {ExtensionId::TSCN, ".tscn"},
        {ExtensionId::SCN, ".scn"},
        {ExtensionId::TRES, ".tres"},
        {ExtensionId::RES, ".res"},
        {ExtensionId::GD, ".gd"},
        {ExtensionId::CS, ".cs"},
        {ExtensionId::SH, ".sh"},
        {ExtensionId::GDSHADER, ".gdshader"},
        {ExtensionId::GDSHADERINC, ".gdshaderinc"},
        {ExtensionId::SHADER, ".shader"},
        {ExtensionId::IMPORT, ".import"},
        {ExtensionId::UID, ".uid"},
        {ExtensionId::PNG, ".png"},
        {ExtensionId::JPG, ".jpg"},
        {ExtensionId::JPEG, ".jpeg"},
        {ExtensionId::WEBP, ".webp"},
        {ExtensionId::SVG, ".svg"},
        {ExtensionId::WAV, ".wav"},
        {ExtensionId::OGG, ".ogg"},
        {ExtensionId::MP3, ".mp3"},
        {ExtensionId::GLB, ".glb"},
        {ExtensionId::GLTF, ".gltf"},
        {ExtensionId::FBX, ".fbx"},
        {ExtensionId::DAE, ".dae"},
        {ExtensionId::BLEND, ".blend"},
        {ExtensionId::MESHLIB, ".meshlib"},
        {ExtensionId::GODOT, ".godot"},
        {ExtensionId::CFG, ".cfg"},
        {ExtensionId::JSON, ".json"},
        {ExtensionId::CSV, ".csv"},
        {ExtensionId::MD, ".md"},
        {ExtensionId::DLL, ".dll"},
        {ExtensionId::SO, ".so"},
        {ExtensionId::DYLIB, ".dylib"},
        {ExtensionId::BIN, ".bin"},
        {ExtensionId::Unknown, ""},
    };

    for (const ExtensionCase& item : extension_cases) {
        CHECK(std::string(gotool::project_scanner::to_string(item.id)) == item.expected);
    }

    struct FileTypeCase
    {
        FileTypeId value;
        const char* expected;

        constexpr FileTypeCase(FileTypeId type_value, const char* expected_value) noexcept
            : value(type_value), expected(expected_value)
        {}
    };

    const std::vector<FileTypeCase> file_type_cases = {
        {FileTypeId::Folder, "Folder"},
        {FileTypeId::GodotScene, "GodotScene"},
        {FileTypeId::GodotResource, "GodotResource"},
        {FileTypeId::Script, "Script"},
        {FileTypeId::Shader, "Shader"},
        {FileTypeId::Asset, "Asset"},
        {FileTypeId::Config, "Config"},
        {FileTypeId::GodotImportMetadata, "GodotImportMetadata"},
        {FileTypeId::GodotImportHash, "GodotImportHash"},
        {FileTypeId::GodotShaderCache, "GodotShaderCache"},
        {FileTypeId::GodotEditorMetadata, "GodotEditorMetadata"},
        {FileTypeId::Data, "Data"},
        {FileTypeId::GeneratedArtifact, "GeneratedArtifact"},
        {FileTypeId::SourceArt, "SourceArt"},
        {FileTypeId::ColorPalette, "ColorPalette"},
        {FileTypeId::SourceCode, "SourceCode"},
        {FileTypeId::Documentation, "Documentation"},
        {FileTypeId::Image, "Image"},
        {FileTypeId::Texture, "Texture"},
        {FileTypeId::Audio, "Audio"},
        {FileTypeId::Video, "Video"},
        {FileTypeId::Font, "Font"},
        {FileTypeId::Model, "Model"},
        {FileTypeId::ModelBackup, "ModelBackup"},
        {FileTypeId::ModelCache, "ModelCache"},
        {FileTypeId::MaterialSource, "MaterialSource"},
        {FileTypeId::BinaryData, "BinaryData"},
        {FileTypeId::Archive, "Archive"},
        {FileTypeId::Database, "Database"},
        {FileTypeId::BuildArtifact, "BuildArtifact"},
        {FileTypeId::Unknown, "Unknown"},
    };

    for (const FileTypeCase& item : file_type_cases) {
        CHECK(std::string(gotool::project_scanner::to_string(item.value)) == item.expected);
    }

    struct HintCase
    {
        GodotTypeHint value;
        const char* expected;

        constexpr HintCase(GodotTypeHint hint_value, const char* expected_value) noexcept
            : value(hint_value), expected(expected_value)
        {}
    };

    const std::vector<HintCase> hint_cases = {
        {GodotTypeHint::PackedScene, "PackedScene"},
        {GodotTypeHint::Resource, "Resource"},
        {GodotTypeHint::GDScript, "GDScript"},
        {GodotTypeHint::CSharpScript, "CSharpScript"},
        {GodotTypeHint::Shader, "Shader"},
        {GodotTypeHint::ShaderInclude, "ShaderInclude"},
        {GodotTypeHint::Texture2D, "Texture2D"},
        {GodotTypeHint::AudioStreamWAV, "AudioStreamWAV"},
        {GodotTypeHint::AudioStreamMP3, "AudioStreamMP3"},
        {GodotTypeHint::AudioStreamOggVorbis, "AudioStreamOggVorbis"},
        {GodotTypeHint::FontFile, "FontFile"},
        {GodotTypeHint::MeshLibrary, "MeshLibrary"},
        {GodotTypeHint::GDExtension, "GDExtension"},
        {GodotTypeHint::ConfigFile, "ConfigFile"},
        {GodotTypeHint::ResourceUID, "ResourceUID"},
        {GodotTypeHint::ProjectSettings, "ProjectSettings"},
        {GodotTypeHint::NotGodotTyped, "NGT"},
    };

    for (const HintCase& item : hint_cases) {
        CHECK(std::string(gotool::project_scanner::to_string(item.value)) == item.expected);
    }

    CHECK(std::string(gotool::project_scanner::to_string(TypeHintSource::Extension)) ==
          "extension");
    CHECK(std::string(gotool::project_scanner::to_string(TypeHintSource::Path)) == "path");
    CHECK(std::string(gotool::project_scanner::to_string(TypeHintSource::ImportMetadata)) ==
          "import_metadata");
    CHECK(std::string(gotool::project_scanner::to_string(TypeHintSource::EditorFileSystem)) ==
          "editor_filesystem");
    CHECK(std::string(gotool::project_scanner::to_string(TypeHintSource::ExplicitInspection)) ==
          "explicit_inspection");
    CHECK(std::string(gotool::project_scanner::to_string(TypeHintSource::None)) == "none");

    CHECK(std::string(gotool::project_scanner::to_string(DirtyState::Clean)) == "clean");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyState::Deleted)) == "deleted");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyState::Dirty)) == "dirty");

    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::None)) == "none");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::NewPath)) == "new_path");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::DeletedPath)) ==
          "deleted_path");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::KindChanged)) ==
          "kind_changed");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::SizeChanged)) ==
          "size_changed");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::ModifiedTimeChanged)) ==
          "modified_time_changed");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::FileIdentityChanged)) ==
          "file_identity_changed");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::ParserVersionChanged)) ==
          "parser_version_changed");
    CHECK(std::string(gotool::project_scanner::to_string(
              DirtyReason::DependencyParserVersionChanged)) == "dependency_parser_version_changed");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::SceneParserVersionChanged)) ==
          "scene_parser_version_changed");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::ClassifierVersionChanged)) ==
          "classifier_version_changed");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::PriorParseFailedRetry)) ==
          "prior_parse_failed_retry");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::WatcherInvalidated)) ==
          "watcher_invalidated");
    CHECK(std::string(gotool::project_scanner::to_string(DirtyReason::ForceRescan)) ==
          "force_rescan");

    CHECK(std::string(gotool::project_scanner::to_string(ParseStatus::ParsedClass)) ==
          "parsed_class");
    CHECK(std::string(gotool::project_scanner::to_string(ParseStatus::NoClass)) == "no_class");
    CHECK(std::string(gotool::project_scanner::to_string(ParseStatus::IoError)) == "io_error");
    CHECK(std::string(gotool::project_scanner::to_string(ParseStatus::UnsupportedLanguage)) ==
          "unsupported_language");
    CHECK(std::string(gotool::project_scanner::to_string(ParseStatus::Malformed)) == "malformed");
    CHECK(std::string(gotool::project_scanner::to_string(ParseStatus::NotParsed)) == "not_parsed");

    CHECK(std::string(gotool::project_scanner::to_string(ScriptLanguage::GDScript)) == "GDScript");
    CHECK(std::string(gotool::project_scanner::to_string(ScriptLanguage::CSharp)) == "CSharp");
    CHECK(std::string(gotool::project_scanner::to_string(ScriptLanguage::Unknown)).empty());

    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::PreloadPath)) ==
          "preload_path");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::LoadPath)) == "load_path");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::ResourceLoaderLoadPath)) ==
          "resource_loader_load_path");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::GDLoadPath)) ==
          "gd_load_path");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::ExtendsPath)) ==
          "extends_path");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::ExtendsClass)) ==
          "extends_class");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::ClassNameDeclaration)) ==
          "class_name_declaration");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::ConstPreloadAlias)) ==
          "const_preload_alias");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::TypedVarRef)) ==
          "typed_var_ref");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::TypedParamRef)) ==
          "typed_param_ref");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::TypedReturnRef)) ==
          "typed_return_ref");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::TypedArrayElementRef)) ==
          "typed_array_element_ref");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::TypedDictionaryRef)) ==
          "typed_dictionary_ref");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::ExportTypeRef)) ==
          "export_type_ref");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::SignalTypeRef)) ==
          "signal_type_ref");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::NewClassInstantiation)) ==
          "new_class_instantiation");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::SceneNodePath)) ==
          "scene_node_path");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::ResourceUIDRef)) ==
          "resource_uid_ref");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::DynamicLoad)) ==
          "dynamic_load");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::UnresolvedSymbol)) ==
          "unresolved_symbol");
    CHECK(std::string(gotool::project_scanner::to_string(DependencyKind::Unknown)) == "unknown");
}
// NOLINTEND(readability-function-size)

// NOLINTNEXTLINE(readability-function-size)
TEST_CASE(
    "scan_rules_classification_and_hint_detection_cover_special_paths") // NOLINT(readability-function-size)
{
    CHECK(gotool::project_scanner::classify_entry("addons/plugin.cfg", EntryKind::File) ==
          FileTypeId::Config);
    CHECK(gotool::project_scanner::classify_entry("project.godot", EntryKind::File) ==
          FileTypeId::Config);
    CHECK(gotool::project_scanner::classify_entry(
              "build/libgotool_center.x86_64", EntryKind::File) == FileTypeId::GeneratedArtifact);
    CHECK(gotool::project_scanner::classify_entry("archives/build.tar", EntryKind::File) ==
          FileTypeId::Archive);
    CHECK(gotool::project_scanner::classify_entry("data/cache.sqlite3", EntryKind::File) ==
          FileTypeId::Database);
    CHECK(gotool::project_scanner::classify_entry("src/main.cpp", EntryKind::File) ==
          FileTypeId::SourceCode);
    CHECK(gotool::project_scanner::classify_entry("unknown/custom.zzz", EntryKind::File) ==
          FileTypeId::Unknown);
    CHECK(gotool::project_scanner::classify_entry("scripts", EntryKind::Directory) ==
          FileTypeId::Folder);

    CHECK(gotool::project_scanner::detect_godot_type_hint(
              "addons/plugin.cfg", FileTypeId::Config) == GodotTypeHint::ConfigFile);
    CHECK(gotool::project_scanner::detect_godot_type_hint("project.godot", FileTypeId::Config) ==
          GodotTypeHint::ProjectSettings);
    CHECK(gotool::project_scanner::detect_godot_type_hint(
              "addons/plugin.remap", FileTypeId::Config) == GodotTypeHint::ConfigFile);
    CHECK(gotool::project_scanner::detect_godot_type_hint(
              "notes/readme.md", FileTypeId::Documentation) == GodotTypeHint::NotGodotTyped);

    EntryFacts plugin_facts;
    plugin_facts.project_relative_path = "addons/plugin.cfg";
    plugin_facts.project_relative_path_lower = "addons/plugin.cfg";
    plugin_facts.file_name = "plugin.cfg";
    plugin_facts.extension = ".cfg";
    plugin_facts.entry_kind = EntryKind::File;
    plugin_facts.extension_id = ExtensionId::CFG;

    CHECK(gotool::project_scanner::classify_entry_from_facts(plugin_facts) == FileTypeId::Config);
    CHECK(gotool::project_scanner::detect_godot_type_hint_from_facts(
              plugin_facts, FileTypeId::Config) == GodotTypeHint::ConfigFile);

    CHECK(gotool::project_scanner::type_hint_source_for(GodotTypeHint::NotGodotTyped) ==
          TypeHintSource::None);
    CHECK(gotool::project_scanner::type_hint_source_for(GodotTypeHint::PackedScene) ==
          TypeHintSource::Extension);
}

// NOLINTNEXTLINE(readability-function-size)
TEST_CASE(
    "scan_rules_dirty_state_optional_snapshot_and_retry_paths") // NOLINT(readability-function-size)
{
    ExistingEntrySnapshot existing;
    existing.entry_kind = EntryKind::File;
    existing.size_bytes = 10;
    existing.modified_time_ns = 20;
    existing.platform_file_id = "id-1";
    existing.parser_version = gotool::project_scanner::PARSER_VERSION;
    existing.dependency_parser_version = gotool::project_scanner::DEPENDENCY_PARSER_VERSION;
    existing.classifier_version = gotool::project_scanner::CLASSIFIER_VERSION;
    existing.parse_status = ParseStatus::NoClass;

    CHECK(gotool::project_scanner::detect_dirty_state(EntryKind::File, 10, 20, "id-1", existing,
                                                      false)
              .state == DirtyState::Clean);

    CHECK(gotool::project_scanner::detect_dirty_state(EntryKind::File, 10, 20, "id-2", existing,
                                                      false)
              .reason == DirtyReason::FileIdentityChanged);

    ExistingEntrySnapshot empty_platform_existing = existing;
    empty_platform_existing.platform_file_id.clear();
    CHECK(gotool::project_scanner::detect_dirty_state(EntryKind::File, 10, 20, "id-2",
                                                      empty_platform_existing, false)
              .state == DirtyState::Clean);

    ExistingEntrySnapshot io_error_existing = existing;
    io_error_existing.parse_status = ParseStatus::IoError;
    CHECK(gotool::project_scanner::detect_dirty_state(EntryKind::File, 10, 20, "id-1",
                                                      io_error_existing, false)
              .reason == DirtyReason::PriorParseFailedRetry);

    ExistingEntrySnapshot malformed_existing = existing;
    malformed_existing.parse_status = ParseStatus::Malformed;
    CHECK(gotool::project_scanner::detect_dirty_state(EntryKind::File, 10, 20, "id-1",
                                                      malformed_existing, false)
              .reason == DirtyReason::PriorParseFailedRetry);

    CHECK(
        gotool::project_scanner::detect_dirty_state(EntryKind::File, 10, 20, "id-1", existing, true)
            .reason == DirtyReason::ForceRescan);

    const std::optional<ExistingEntrySnapshot> no_existing = std::nullopt;
    CHECK(gotool::project_scanner::detect_dirty_state(EntryKind::File, 1, 2, "", no_existing, false)
              .reason == DirtyReason::NewPath);
}

// NOLINTNEXTLINE(readability-function-size)
TEST_CASE(
    "scan_rules_entry_record_flags_and_type_hint_helpers_behave_consistently") // NOLINT(readability-function-size)
{
    EntryRecord record;
    CHECK_FALSE(record.is_hidden());

    record.set_hidden(true);
    CHECK(record.is_hidden());

    record.set_hidden(false);
    CHECK_FALSE(record.is_hidden());

    CHECK_FALSE(record.has_platform_file_id());
    CHECK(gotool::project_scanner::platform_file_id_to_string(record).empty());

    record.flags = 2;
    record.platform_file_id_high = 0xA;
    record.platform_file_id_low = 0xB;
    CHECK(record.has_platform_file_id());
    CHECK(gotool::project_scanner::platform_file_id_to_string(record) == "a:b");

    record.clear_platform_file_id();
    CHECK_FALSE(record.has_platform_file_id());
    CHECK(gotool::project_scanner::platform_file_id_to_string(record).empty());

    CHECK(gotool::project_scanner::is_builtin_node_type_hint("Node3D"));
    CHECK(gotool::project_scanner::is_builtin_node_type_hint("EditorPlugin"));
    CHECK_FALSE(gotool::project_scanner::is_builtin_node_type_hint("CustomNodeType"));

    CHECK(gotool::project_scanner::is_builtin_resource_type_hint("Resource"));
    CHECK(gotool::project_scanner::is_builtin_resource_type_hint("MeshLibrary"));
    CHECK_FALSE(gotool::project_scanner::is_builtin_resource_type_hint("CustomResourceType"));

    CHECK(gotool::project_scanner::is_script_extension(".gd"));
    CHECK(gotool::project_scanner::is_script_extension(".cs"));
    CHECK_FALSE(gotool::project_scanner::is_script_extension(".txt"));

    CHECK(gotool::project_scanner::language_from_extension(".gd") == ScriptLanguage::GDScript);
    CHECK(gotool::project_scanner::language_from_extension(".cs") == ScriptLanguage::CSharp);
    CHECK(gotool::project_scanner::language_from_extension(".txt") == ScriptLanguage::Unknown);
}
