#include "database/gotool_database.hpp"
#include "database/gotool_schema.hpp"
#include "project_scanner/native_directory_enumerator.hpp"
#include "project_scanner/native_scan_pipeline.hpp"
#include "project_scanner/native_scan_rules.hpp"
#include "project_scanner/native_script_parser.hpp"

#include "doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace {

using gotool::database::Database;
using gotool::database::Statement;
using gotool::project_scanner::DirtyReason;
using gotool::project_scanner::DirtyState;
using gotool::project_scanner::DependencyKind;
using gotool::project_scanner::EntryKind;
using gotool::project_scanner::EnumerationOptions;
using gotool::project_scanner::EnumerationResult;
using gotool::project_scanner::ExistingEntrySnapshot;
using gotool::project_scanner::FileTypeId;
using gotool::project_scanner::GodotTypeHint;
using gotool::project_scanner::NativeDirectoryEnumerator;
using gotool::project_scanner::NativeScanPipeline;
using gotool::project_scanner::ParseStatus;
using gotool::project_scanner::PathArena;
using gotool::project_scanner::ScanMetrics;
using gotool::project_scanner::ScanOptions;
using gotool::project_scanner::ScanRepository;
using gotool::project_scanner::ScriptLanguage;

std::filesystem::path make_temp_root(const std::string &test_name) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "gotool_center_scanner_tests" /
        (test_name + "_" + std::to_string(now));
    std::filesystem::create_directories(root);
    return root;
}

std::filesystem::path make_temp_database_path(const std::string &test_name) {
    const std::filesystem::path root = make_temp_root(test_name);
    return root / "test.sqlite3";
}

struct TemporaryRoot {
    explicit TemporaryRoot(std::filesystem::path value) :
        path(std::move(value)) {}

    ~TemporaryRoot() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

void write_text(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

int64_t query_int64(Database &database, const std::string &sql) {
    Statement statement = database.prepare(sql);
    REQUIRE(statement.step() == Statement::StepResult::Row);
    return statement.column_int64(0);
}

int64_t query_int64_with_bind(Database &database, const std::string &sql, int64_t value) {
    Statement statement = database.prepare(sql);
    statement.bind_int64(1, value);
    REQUIRE(statement.step() == Statement::StepResult::Row);
    return statement.column_int64(0);
}

int64_t count_dependencies_of_kind(
    const gotool::project_scanner::ScriptParseResult &result,
    DependencyKind kind
) {
    int64_t count = 0;
    for (const gotool::project_scanner::ScriptDependencyRecord &dependency : result.dependencies) {
        if (dependency.dependency_kind == kind) {
            ++count;
        }
    }
    return count;
}

bool has_dependency_target_path(
    const gotool::project_scanner::ScriptParseResult &result,
    DependencyKind kind,
    const std::string &target_path
) {
    for (const gotool::project_scanner::ScriptDependencyRecord &dependency : result.dependencies) {
        if (dependency.dependency_kind != kind || !dependency.target_project_relative_path.has_value()) {
            continue;
        }

        if (dependency.target_project_relative_path.value() == target_path) {
            return true;
        }
    }
    return false;
}

ExistingEntrySnapshot snapshot_for(
    EntryKind kind,
    int64_t size_bytes,
    int64_t modified_time_ns,
    const std::string &platform_file_id = ""
) {
    ExistingEntrySnapshot snapshot;
    snapshot.entry_kind = kind;
    snapshot.size_bytes = size_bytes;
    snapshot.modified_time_ns = modified_time_ns;
    snapshot.platform_file_id = platform_file_id;
    snapshot.parser_version = gotool::project_scanner::PARSER_VERSION;
    snapshot.classifier_version = gotool::project_scanner::CLASSIFIER_VERSION;
    return snapshot;
}

} // namespace

TEST_CASE("path_normalization_uses_project_relative_forward_slashes") {
    CHECK(gotool::project_scanner::normalize_project_path("res://folder\\player.gd") == "folder/player.gd");
    CHECK(gotool::project_scanner::normalize_project_path("./folder//nested/../scene.tscn") == "folder/scene.tscn");
    CHECK(gotool::project_scanner::normalize_project_path("folder/./asset.png") == "folder/asset.png");
}

TEST_CASE("skip_policy_excludes_gotool_addon_and_cache_paths") {
    gotool::project_scanner::SkipPolicy policy;

    CHECK(policy.should_skip("addons/GoToolCenter"));
    CHECK(policy.should_skip("addons/GoToolCenter/bin/libgotool_center.dll"));
    CHECK(policy.should_skip(".godot/gotool_center/project.uid"));
    CHECK_FALSE(policy.should_skip("scripts/player.gd"));
}

TEST_CASE("skip_policy_supports_external_and_normalized_path_modes") {
    gotool::project_scanner::SkipPolicy policy;

    CHECK(policy.should_skip_external("res://addons\\GoToolCenter\\bin\\libgotool_center.dll"));
    CHECK(policy.should_skip_normalized(".godot/imported/scene.md5"));
    CHECK_FALSE(policy.should_skip_normalized("res://addons/GoToolCenter"));
    CHECK_FALSE(policy.should_skip_external("res://scripts/player.gd"));
}

TEST_CASE("extension_extraction_and_classification_are_cheap") {
    CHECK(gotool::project_scanner::extension_from_path("Scripts/Player.GD") == ".gd");
    CHECK(gotool::project_scanner::extension_from_path("README") == "");
    CHECK(gotool::project_scanner::classify_entry("levels/main.tscn", EntryKind::File) == FileTypeId::GodotScene);
    CHECK(gotool::project_scanner::classify_entry("scripts/player.gd", EntryKind::File) == FileTypeId::Script);
    CHECK(gotool::project_scanner::detect_godot_type_hint("icon.png", FileTypeId::Asset) == GodotTypeHint::Texture2D);
}

TEST_CASE("extension_classification_handles_metadata_and_source_artifacts") {
    CHECK(gotool::project_scanner::classify_entry("tests/fixtures/type_probe_extensions/palette.ase", EntryKind::File) == FileTypeId::ColorPalette);
    CHECK(gotool::project_scanner::classify_entry("tests/fixtures/type_probe_extensions/sprite.aseprite", EntryKind::File) == FileTypeId::SourceArt);
    CHECK(gotool::project_scanner::classify_entry("tests/fixtures/type_probe_extensions/backup.blend1", EntryKind::File) == FileTypeId::ModelBackup);
    CHECK(gotool::project_scanner::classify_entry("tests/fixtures/type_probe_extensions/blob.bin", EntryKind::File) == FileTypeId::BinaryData);
    CHECK(gotool::project_scanner::classify_entry("tests/fixtures/type_probe_extensions/image.exr", EntryKind::File) == FileTypeId::Image);
    CHECK(gotool::project_scanner::classify_entry("tests/fixtures/type_probe_extensions/mesh.assbin", EntryKind::File) == FileTypeId::ModelCache);
    CHECK(gotool::project_scanner::classify_entry("tests/fixtures/type_probe_extensions/material.spp", EntryKind::File) == FileTypeId::MaterialSource);
    CHECK(gotool::project_scanner::classify_entry("tests/fixtures/type_probe_extensions/editor.node", EntryKind::File) == FileTypeId::GodotEditorMetadata);
    CHECK(gotool::project_scanner::classify_entry("tests/fixtures/type_probe_extensions/meta.import", EntryKind::File) == FileTypeId::GodotImportMetadata);

    CHECK(gotool::project_scanner::classify_entry(".godot/imported/type_probe.md5", EntryKind::File) == FileTypeId::GodotImportHash);
    CHECK(gotool::project_scanner::classify_entry(".godot/shader_cache/type_probe.cache", EntryKind::File) == FileTypeId::GodotShaderCache);

    CHECK(gotool::project_scanner::detect_godot_type_hint("tests/fixtures/type_probe_extensions/meta.import", FileTypeId::GodotImportMetadata) == GodotTypeHint::NotGodotTyped);
    CHECK(gotool::project_scanner::detect_godot_type_hint("tests/fixtures/type_probe_extensions/editor.node", FileTypeId::GodotEditorMetadata) == GodotTypeHint::NotGodotTyped);
    CHECK(gotool::project_scanner::detect_godot_type_hint("tests/fixtures/type_probe_extensions/image.exr", FileTypeId::Image) == GodotTypeHint::NotGodotTyped);
}

TEST_CASE("extension_dispatch_from_facts_matches_wrapper_classification") {
    struct Case {
        const char *path;
        EntryKind kind;
        FileTypeId expected_type;
        GodotTypeHint expected_hint;
    };

    const std::vector<Case> cases = {
        { "levels/main.tscn", EntryKind::File, FileTypeId::GodotScene, GodotTypeHint::PackedScene },
        { "scripts/player.gd", EntryKind::File, FileTypeId::Script, GodotTypeHint::GDScript },
        { "audio/theme.ogg", EntryKind::File, FileTypeId::Audio, GodotTypeHint::AudioStreamOggVorbis },
        { "textures/icon.png", EntryKind::File, FileTypeId::Asset, GodotTypeHint::Texture2D },
        { ".godot/imported/cache.md5", EntryKind::File, FileTypeId::GodotImportHash, GodotTypeHint::NotGodotTyped },
        { "plugins", EntryKind::Directory, FileTypeId::Folder, GodotTypeHint::NotGodotTyped },
    };

    for (const Case &item : cases) {
        const std::string normalized = gotool::project_scanner::normalize_project_path(item.path);
        const std::string lowered = gotool::project_scanner::lower_ascii(normalized);
        const std::string file_name = gotool::project_scanner::file_name_from_path(normalized);
        const std::string extension = gotool::project_scanner::extension_from_path(normalized);

        gotool::project_scanner::EntryFacts facts;
        facts.project_relative_path = normalized;
        facts.project_relative_path_lower = lowered;
        facts.file_name = file_name;
        facts.extension = extension;
        facts.entry_kind = item.kind;
        facts.extension_id = gotool::project_scanner::extension_id_from_extension(extension);

        const FileTypeId fact_type = gotool::project_scanner::classify_entry_from_facts(facts);
        CHECK(fact_type == item.expected_type);
        CHECK(gotool::project_scanner::classify_entry(item.path, item.kind) == item.expected_type);

        const GodotTypeHint fact_hint =
            gotool::project_scanner::detect_godot_type_hint_from_facts(facts, fact_type);
        CHECK(fact_hint == item.expected_hint);
        CHECK(gotool::project_scanner::detect_godot_type_hint(item.path, fact_type) == item.expected_hint);
    }
}

TEST_CASE("dirty_detector_reports_clean_and_metadata_changes") {
    CHECK(
        gotool::project_scanner::detect_dirty_state(
            EntryKind::File,
            10,
            20,
            "42",
            snapshot_for(EntryKind::File, 10, 20, "42"),
            false
        ).state == DirtyState::Clean
    );

    CHECK(
        gotool::project_scanner::detect_dirty_state(
            EntryKind::File,
            11,
            20,
            "42",
            snapshot_for(EntryKind::File, 10, 20, "42"),
            false
        ).reason == DirtyReason::SizeChanged
    );

    CHECK(
        gotool::project_scanner::detect_dirty_state(
            EntryKind::File,
            10,
            21,
            "42",
            snapshot_for(EntryKind::File, 10, 20, "42"),
            false
        ).reason == DirtyReason::ModifiedTimeChanged
    );

    CHECK(
        gotool::project_scanner::detect_dirty_state(
            EntryKind::Directory,
            0,
            20,
            "42",
            snapshot_for(EntryKind::File, 0, 20, "42"),
            false
        ).reason == DirtyReason::KindChanged
    );

    CHECK(
        gotool::project_scanner::detect_dirty_state(
            EntryKind::Directory,
            0,
            999,
            "",
            snapshot_for(EntryKind::Directory, 0, 20, ""),
            false
        ).state == DirtyState::Clean
    );

    CHECK(
        gotool::project_scanner::detect_dirty_state(
            EntryKind::File,
            10,
            20,
            "43",
            snapshot_for(EntryKind::File, 10, 20, "42"),
            false
        ).reason == DirtyReason::FileIdentityChanged
    );
}

TEST_CASE("dirty_detector_honors_parser_and_classifier_versions") {
    ExistingEntrySnapshot parser_snapshot = snapshot_for(EntryKind::File, 10, 20);
    parser_snapshot.parser_version = gotool::project_scanner::PARSER_VERSION - 1;

    CHECK(
        gotool::project_scanner::detect_dirty_state(
            EntryKind::File,
            10,
            20,
            "",
            parser_snapshot,
            false
        ).reason == DirtyReason::ParserVersionChanged
    );

    ExistingEntrySnapshot classifier_snapshot = snapshot_for(EntryKind::File, 10, 20);
    classifier_snapshot.classifier_version = gotool::project_scanner::CLASSIFIER_VERSION - 1;

    CHECK(
        gotool::project_scanner::detect_dirty_state(
            EntryKind::File,
            10,
            20,
            "",
            classifier_snapshot,
            false
        ).reason == DirtyReason::ClassifierVersionChanged
    );

    ExistingEntrySnapshot dependency_parser_snapshot = snapshot_for(EntryKind::File, 10, 20);
    dependency_parser_snapshot.dependency_parser_version =
        gotool::project_scanner::DEPENDENCY_PARSER_VERSION - 1;

    CHECK(
        gotool::project_scanner::detect_dirty_state(
            EntryKind::File,
            10,
            20,
            "",
            dependency_parser_snapshot,
            false
        ).reason == DirtyReason::DependencyParserVersionChanged
    );
}

TEST_CASE("gdscript_parser_detects_class_name_and_extends") {
    TemporaryRoot root(make_temp_root("gdscript_parser"));
    const std::filesystem::path script = root.path / "player.gd";
    write_text(
        script,
        "# header\n"
        "class_name PlayerController # exported name\n"
        "extends CharacterBody2D\n"
    );

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_header(script, ".gd");

    CHECK(result.status == ParseStatus::ParsedClass);
    CHECK(result.language == ScriptLanguage::GDScript);
    CHECK(result.class_name == "PlayerController");
    CHECK(result.direct_base_type == "CharacterBody2D");
}

TEST_CASE("csharp_parser_detects_global_class_and_base_type") {
    TemporaryRoot root(make_temp_root("csharp_parser"));
    const std::filesystem::path script = root.path / "Enemy.cs";
    write_text(
        script,
        "using Godot;\n"
        "[GlobalClass]\n"
        "public partial class Enemy : CharacterBody3D\n"
        "{\n"
    );

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_header(script, ".cs");

    CHECK(result.status == ParseStatus::ParsedClass);
    CHECK(result.language == ScriptLanguage::CSharp);
    CHECK(result.class_name == "Enemy");
    CHECK(result.direct_base_type == "CharacterBody3D");
}

TEST_CASE("gdscript_parser_extracts_dependency_patterns") {
    TemporaryRoot root(make_temp_root("gdscript_dependency_patterns"));
    const std::filesystem::path script = root.path / "player.gd";
    write_text(
        script,
        "class_name Player\n"
        "extends \"res://actors/base_player.gd\"\n"
        "const EnemyScene = preload(\"res://scenes/enemy.tscn\")\n"
        "var hero: Hero\n"
        "var squad: Array[Enemy]\n"
        "signal damaged(target: Enemy)\n"
        "@export var scene: PackedScene\n"
        "func spawn(target: Enemy) -> Enemy:\n"
        "    var packed = load(\"res://scenes/enemy.tscn\")\n"
        "    var dynamic_scene = load(target)\n"
        "    Enemy.new()\n"
        "    var uid = \"uid://111122223333\"\n"
        "    var node_path = NodePath(\"Player/Camera\")\n"
    );

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_header(script, ".gd");

    CHECK(result.status == ParseStatus::ParsedClass);
    CHECK(result.class_name == "Player");
    CHECK(result.tokens_generated > 0);
    CHECK(result.dependency_parse_ms >= 0);
    CHECK(count_dependencies_of_kind(result, DependencyKind::ClassNameDeclaration) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::ExtendsPath) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::ConstPreloadAlias) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::TypedVarRef) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::TypedArrayElementRef) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::TypedParamRef) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::TypedReturnRef) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::SignalTypeRef) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::ExportTypeRef) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::NewClassInstantiation) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::SceneNodePath) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::ResourceUIDRef) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::DynamicLoad) >= 1);
    CHECK(has_dependency_target_path(result, DependencyKind::LoadPath, "scenes/enemy.tscn"));
    CHECK(has_dependency_target_path(result, DependencyKind::PreloadPath, "scenes/enemy.tscn"));
}

TEST_CASE("gdscript_parser_ignores_comments_and_handles_escaped_and_multiline_strings") {
    TemporaryRoot root(make_temp_root("gdscript_comments_strings"));
    const std::filesystem::path script = root.path / "comment_guard.gd";
    write_text(
        script,
        "class_name CommentGuard\n"
        "extends Node\n"
        "var text = \"literal # not comment and escaped quote \\\"ok\\\"\"\n"
        "var blob = \"\"\"\n"
        "load(\"res://ignored_from_multiline.tscn\")\n"
        "\"\"\"\n"
        "# preload(\"res://ignored_from_comment.tscn\")\n"
        "var keep = preload(\"res://kept.tscn\") # preload(\"res://ignored_trailing.tscn\")\n"
    );

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_header(script, ".gd");

    CHECK(result.status == ParseStatus::ParsedClass);
    CHECK(count_dependencies_of_kind(result, DependencyKind::PreloadPath) == 1);
    CHECK(has_dependency_target_path(result, DependencyKind::PreloadPath, "kept.tscn"));
    CHECK(count_dependencies_of_kind(result, DependencyKind::LoadPath) == 0);
}

TEST_CASE("csharp_parser_extracts_globalclass_load_and_export_dependencies") {
    TemporaryRoot root(make_temp_root("csharp_dependency_patterns"));
    const std::filesystem::path script = root.path / "EnemyFactory.cs";
    write_text(
        script,
        "using Godot;\n"
        "[GlobalClass]\n"
        "public partial class EnemyFactory : Node\n"
        "{\n"
        "    [Export]\n"
        "    public PackedScene EnemyScene { get; set; }\n"
        "\n"
        "    public void Spawn()\n"
        "    {\n"
        "        var enemyScene = GD.Load<PackedScene>(\"res://scenes/enemy.tscn\");\n"
        "        var enemyScript = ResourceLoader.Load<CSharpScript>(\"res://scripts/enemy.cs\");\n"
        "        PackedScene.Instantiate<Enemy>();\n"
        "    }\n"
        "}\n"
    );

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_header(script, ".cs");

    CHECK(result.status == ParseStatus::ParsedClass);
    CHECK(result.class_name == "EnemyFactory");
    CHECK(count_dependencies_of_kind(result, DependencyKind::ClassNameDeclaration) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::ExtendsClass) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::ExportTypeRef) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::GDLoadPath) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::ResourceLoaderLoadPath) >= 1);
    CHECK(count_dependencies_of_kind(result, DependencyKind::NewClassInstantiation) >= 1);
    CHECK(has_dependency_target_path(result, DependencyKind::GDLoadPath, "scenes/enemy.tscn"));
    CHECK(has_dependency_target_path(result, DependencyKind::ResourceLoaderLoadPath, "scripts/enemy.cs"));
}

TEST_CASE("script_parser_limits_are_enforced_without_throwing") {
    TemporaryRoot root(make_temp_root("parser_limit_enforcement"));
    const std::filesystem::path script = root.path / "limit.gd";

    std::string text;
    text += "class_name LimitCase\n";
    text += "extends Node\n";
    for (int32_t i = 0; i < 20; ++i) {
        text += "var dep" + std::to_string(i) + " = preload(\"res://dep" + std::to_string(i) + ".tscn\")\n";
    }
    write_text(script, text);

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_header(script, ".gd", 1024, 1024 * 128, 1024 * 8, 3);

    CHECK(result.status == ParseStatus::ParsedClass);
    CHECK(result.limit_exceeded);
    CHECK(static_cast<int64_t>(result.dependencies.size()) == 3);
}

TEST_CASE("script_parser_handles_large_synthetic_script_throughput") {
    TemporaryRoot root(make_temp_root("parser_large_synthetic"));
    const std::filesystem::path script = root.path / "large.gd";

    std::string text;
    text += "class_name LargeSynthetic\n";
    text += "extends Node\n";
    for (int32_t i = 0; i < 1500; ++i) {
        text += "var enemy_" + std::to_string(i) + ": Enemy\n";
        text += "var scene_" + std::to_string(i) + " = preload(\"res://scenes/enemy.tscn\")\n";
    }
    write_text(script, text);

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_header(script, ".gd", 5000, 1024 * 1024, 1024 * 256, 1024 * 64);

    CHECK(result.status == ParseStatus::ParsedClass);
    CHECK(result.tokens_generated > 0);
    CHECK(result.lines_scanned > 1000);
    CHECK(result.bytes_read > 0);
    CHECK(result.dependency_parse_ms >= 0);
    CHECK(result.tokenizer_ms >= 0);
    CHECK_FALSE(result.limit_exceeded);
    CHECK(static_cast<int64_t>(result.dependencies.size()) >= 500);
}

TEST_CASE("malformed_script_reports_no_class_without_throwing") {
    TemporaryRoot root(make_temp_root("malformed_parser"));
    const std::filesystem::path script = root.path / "broken.gd";
    write_text(script, "extends Node\nfunc _ready(:\n");

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_header(script, ".gd");

    CHECK(result.status == ParseStatus::NoClass);
    CHECK(result.class_name.empty());
}

TEST_CASE("script_intelligence_parser_extracts_symbols_docs_and_references") {
    TemporaryRoot root(make_temp_root("script_intelligence_parser"));
    const std::filesystem::path script = root.path / "player.gd";
    write_text(
        script,
        "## Player script doc\n"
        "class_name Player\n"
        "extends Node\n"
        "\n"
        "## Current health\n"
        "var health: int = 100\n"
        "\n"
        "## Apply damage\n"
        "func apply_damage(amount: int) -> void:\n"
        "    health -= amount\n"
        "\n"
        "const EnemyScene = preload(\"res://scenes/enemy.tscn\")\n"
    );

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_intelligence(
            script,
            ".gd",
            gotool::project_scanner::ParseTier::FullSymbols
        );

    CHECK(result.status == ParseStatus::ParsedClass);
    CHECK(result.class_name == "Player");
    CHECK_FALSE(result.symbols.empty());
    CHECK_FALSE(result.doc_comments.empty());
    CHECK_FALSE(result.references.empty());
    CHECK(result.full_symbol_parse_ms >= 0);
    CHECK(result.doc_comment_parse_ms >= 0);
    CHECK((result.script_flags & gotool::project_scanner::SCRIPT_FLAG_HAS_CLASS_NAME) != 0);
    CHECK((result.script_flags & gotool::project_scanner::SCRIPT_FLAG_EXTENDS_NODE) != 0);
    CHECK((result.script_flags & gotool::project_scanner::SCRIPT_FLAG_HAS_DOC_COMMENT) != 0);

    bool has_class_symbol = false;
    bool has_function_symbol = false;
    bool has_property_symbol = false;
    bool has_parameter_symbol = false;
    bool class_symbol_has_doc_state = false;
    bool function_symbol_has_return_type = false;
    bool property_symbol_has_type_and_default = false;
    for (const auto &symbol : result.symbols) {
        has_class_symbol = has_class_symbol || symbol.symbol_kind == gotool::project_scanner::SymbolKind::Class;
        has_function_symbol = has_function_symbol || symbol.symbol_kind == gotool::project_scanner::SymbolKind::Function;
        has_property_symbol = has_property_symbol || symbol.symbol_kind == gotool::project_scanner::SymbolKind::Property;
        has_parameter_symbol = has_parameter_symbol || symbol.symbol_kind == gotool::project_scanner::SymbolKind::Parameter;
        if (symbol.symbol_kind == gotool::project_scanner::SymbolKind::Class &&
            symbol.name == "Player") {
            CHECK(symbol.qualified_name.find("Player") != std::string::npos);
            class_symbol_has_doc_state = symbol.doc_comment_state == "present";
        }
        if (symbol.symbol_kind == gotool::project_scanner::SymbolKind::Function &&
            symbol.name == "apply_damage") {
            function_symbol_has_return_type = symbol.return_type == "void";
            CHECK(symbol.parent_local_symbol_id.has_value());
            CHECK(symbol.doc_comment_state == "present");
        }
        if (symbol.symbol_kind == gotool::project_scanner::SymbolKind::Property &&
            symbol.name == "health") {
            property_symbol_has_type_and_default =
                symbol.declared_type == "int" &&
                symbol.default_value_excerpt == "100";
            CHECK(symbol.doc_comment_state == "present");
        }
        if (symbol.symbol_kind == gotool::project_scanner::SymbolKind::Parameter &&
            symbol.name == "amount") {
            CHECK(symbol.declared_type == "int");
            CHECK(symbol.parent_local_symbol_id.has_value());
        }
    }
    CHECK(has_class_symbol);
    CHECK(has_function_symbol);
    CHECK(has_property_symbol);
    CHECK(has_parameter_symbol);
    CHECK(class_symbol_has_doc_state);
    CHECK(function_symbol_has_return_type);
    CHECK(property_symbol_has_type_and_default);

    bool has_preload_reference = false;
    for (const auto &reference : result.references) {
        if ((reference.reference_kind == "preload_path" || reference.reference_kind == "const_preload_alias") &&
            reference.target_project_relative_path.has_value() &&
            reference.target_project_relative_path.value() == "scenes/enemy.tscn") {
            has_preload_reference = true;
            CHECK(reference.source_symbol_local_id.has_value());
            CHECK(reference.is_dynamic == false);
            CHECK(reference.is_unresolved == false);
        }
    }
    CHECK(has_preload_reference);
}

TEST_CASE("scene_attachment_parser_extracts_external_resources_and_script_bindings") {
    TemporaryRoot root(make_temp_root("scene_attachment_parser"));
    const std::filesystem::path scene = root.path / "scenes" / "main.tscn";
    write_text(
        scene,
        "[gd_scene load_steps=2 format=3]\n"
        "\n"
        "[ext_resource type=\"Script\" uid=\"uid://player-script\" path=\"res://scripts/player.gd\" id=\"1_player\"]\n"
        "\n"
        "[node name=\"Root\" type=\"Node\"]\n"
        "script = ExtResource(\"1_player\")\n"
    );

    const gotool::project_scanner::SceneParseResult result =
        gotool::project_scanner::parse_scene_attachments(scene);

    CHECK(result.status == ParseStatus::NoClass);
    CHECK_FALSE(result.external_resources.empty());
    CHECK_FALSE(result.script_attachments.empty());
    CHECK(result.parse_ms >= 0);
    const auto &resource = result.external_resources.front();
    CHECK(resource.is_script_resource);
    CHECK(resource.ext_resource_id == "1_player");
    CHECK(resource.resource_path == "scripts/player.gd");
    CHECK(resource.resource_uid == "uid://player-script");
    CHECK(resource.is_resolved);

    const auto &attachment = result.script_attachments.front();
    CHECK(attachment.script_project_relative_path == "scripts/player.gd");
    CHECK(attachment.script_resource_path == "scripts/player.gd");
    CHECK(attachment.script_uid == "uid://player-script");
    CHECK(attachment.ext_resource_id == "1_player");
    CHECK(attachment.attachment_kind == "ext_resource");
    CHECK(attachment.node_path == "Root");
    CHECK(attachment.node_name == "Root");
    CHECK(attachment.node_type == "Node");
    CHECK(attachment.is_resolved);
}

TEST_CASE("path_arena_stores_offsets_without_godot_strings") {
    PathArena arena;
    const uint32_t offset = arena.append("scripts/player.gd");

    CHECK(arena.view(offset, 17) == "scripts/player.gd");
}

TEST_CASE("path_arena_reserve_is_stable_for_hot_path_appends") {
    PathArena arena;
    arena.reserve(128);

    const size_t before_capacity = arena.capacity();
    const uint32_t first = arena.append("scripts");
    const uint32_t second = arena.append("/player.gd");

    CHECK(before_capacity >= 128);
    CHECK(arena.capacity() >= before_capacity);
    CHECK(arena.view(first, 7) == "scripts");
    CHECK(arena.view(second, 10) == "/player.gd");
}

TEST_CASE("platform_file_identity_serialization_is_stable") {
    gotool::project_scanner::EntryRecord record;
    record.platform_file_id_high = 0x12;
    record.platform_file_id_low = 0x34;
    record.flags |= 1u << 1u;

    CHECK(gotool::project_scanner::platform_file_id_to_string(record) == "12:34");

    record.clear_platform_file_id();
    CHECK(gotool::project_scanner::platform_file_id_to_string(record).empty());
}

TEST_CASE("scanner_schema_v3_supports_batched_writes_tombstones_and_paging") {
    TemporaryRoot root(make_temp_root("scanner_schema"));
    TemporaryRoot db_root(make_temp_root("scanner_schema_db"));
    write_text(root.path / "project.godot", "[application]\n");
    write_text(root.path / "scripts" / "player.gd", "class_name Player\nextends Node\n");
    write_text(root.path / "levels" / "main.tscn", "[gd_scene format=3]\n");

    // Keep the database out of the scanned project tree to avoid self-induced
    // dirty entries when SQLite file size changes between scans.
    Database database((db_root.path / "scanner.sqlite3").string());
    gotool::database::create_schema(database, 0);
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'scanner-test', 'scanner-test', '" + root.path.generic_string() + "', '" +
        root.path.generic_string() + "', '" + (root.path / "project.godot").generic_string() +
        "', '4.6.0-stable', 'test', 1, 1, 1, 1"
        ");"
    );

    NativeScanPipeline pipeline(database);
    ScanOptions options;
    options.project_id = 1;
    options.project_root = root.path;
    options.include_hidden = true;

    const gotool::project_scanner::ScanResultSummary cold = pipeline.run(options);
    CHECK(cold.scan_run_id > 0);
    CHECK(cold.files_seen >= 3);
    CHECK(cold.scripts_parsed == 1);
    CHECK(query_int64(database, "SELECT COUNT(*) FROM project_files WHERE project_id = 1 AND is_deleted = 0;") >= 3);
    CHECK(query_int64(database, "SELECT COUNT(*) FROM project_custom_classes WHERE project_id = 1;") == 1);

    ScanRepository repository(database);
    CHECK(repository.count_files(1, gotool::project_scanner::FileQuery()) >= 3);
    CHECK(repository.list_files(1, gotool::project_scanner::FileQuery(), 0, 2, "path").size() == 2);
    CHECK(repository.count_custom_classes(1, gotool::project_scanner::CustomClassQuery()) == 1);

    const gotool::project_scanner::ScanResultSummary clean = pipeline.run(options);
    CHECK(clean.scripts_parsed == 0);
    CHECK(clean.scripts_skipped_clean == 1);

    const ScanMetrics clean_metrics = repository.get_scan_metrics(1, clean.scan_run_id);
    std::string unexpected_path;
    std::string unexpected_reason;
    if (clean_metrics.rows_updated != 0) {
        Statement unexpected = database.prepare(R"sql(
            SELECT project_relative_path, dirty_reason
            FROM project_files
            WHERE project_id = ?1
              AND scan_generation = ?2
              AND is_deleted = 0
              AND dirty_state <> 'clean'
            ORDER BY project_relative_path ASC
            LIMIT 1;
        )sql");
        unexpected.bind_int64(1, 1);
        unexpected.bind_int64(2, clean.scan_generation);
        if (unexpected.step() == Statement::StepResult::Row) {
            unexpected_path = unexpected.column_text(0);
            unexpected_reason = unexpected.column_text(1);
        }
    }

    CHECK_MESSAGE(
        clean_metrics.rows_updated == 0,
        "rows_updated=", clean_metrics.rows_updated,
        " path='", unexpected_path,
        "' reason='", unexpected_reason, "'"
    );
    CHECK(clean_metrics.rows_clean_refreshed >= 3);
    CHECK(clean_metrics.sqlite_stage_insert_ms >= 0);
    CHECK(clean_metrics.scripts_dependency_parsed == 0);
    CHECK(clean_metrics.scripts_dependency_skipped_clean >= 1);

    write_text(root.path / "scripts" / "player.gd", "class_name Player\nextends Resource\n");
    const gotool::project_scanner::ScanResultSummary one_script = pipeline.run(options);
    CHECK(one_script.scripts_parsed == 1);

    std::filesystem::remove(root.path / "levels" / "main.tscn");
    const gotool::project_scanner::ScanResultSummary deleted = pipeline.run(options);
    CHECK(deleted.entries_deleted >= 1);
    CHECK(query_int64(database, "SELECT COUNT(*) FROM deleted_entries WHERE project_id = 1;") >= 1);
}

TEST_CASE("native_scan_pipeline_reparses_when_dependency_parser_version_changes") {
    TemporaryRoot root(make_temp_root("dependency_parser_version_invalidation"));
    TemporaryRoot db_root(make_temp_root("dependency_parser_version_invalidation_db"));

    write_text(root.path / "project.godot", "[application]\n");
    write_text(
        root.path / "scripts" / "player.gd",
        "class_name Player\n"
        "extends Node\n"
        "var enemy_scene = preload(\"res://scenes/enemy.tscn\")\n"
    );
    write_text(root.path / "scenes" / "enemy.tscn", "[gd_scene format=3]\n");

    Database database((db_root.path / "scanner.sqlite3").string());
    gotool::database::create_schema(database, 0);
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'scanner-dependency-parser-version', 'scanner-dependency-parser-version', '" + root.path.generic_string() + "', '" +
        root.path.generic_string() + "', '" + (root.path / "project.godot").generic_string() +
        "', '4.6.0-stable', 'test', 1, 1, 1, 1"
        ");"
    );

    NativeScanPipeline pipeline(database);
    ScanOptions options;
    options.project_id = 1;
    options.project_root = root.path;
    options.include_hidden = true;
    options.persist_to_database = true;
    options.collect_custom_classes = true;
    options.collect_script_dependencies = true;

    const gotool::project_scanner::ScanResultSummary cold = pipeline.run(options);
    CHECK(cold.scripts_parsed == 1);

    const int64_t player_file_id = query_int64(
        database,
        "SELECT id FROM project_files WHERE project_id = 1 AND project_relative_path = 'scripts/player.gd' LIMIT 1;"
    );

    Statement force_old_dependency_parser_version = database.prepare(R"sql(
        UPDATE project_files
        SET dependency_parser_version = 0
        WHERE project_id = ?1 AND id = ?2;
    )sql");
    force_old_dependency_parser_version.bind_int64(1, 1);
    force_old_dependency_parser_version.bind_int64(2, player_file_id);
    force_old_dependency_parser_version.step_done();

    const gotool::project_scanner::ScanResultSummary rescanned = pipeline.run(options);
    CHECK(rescanned.scripts_parsed == 1);

    CHECK(
        query_int64(
            database,
            "SELECT dependency_parser_version FROM project_files "
            "WHERE project_id = 1 AND id = " + std::to_string(player_file_id) + ";"
        ) == gotool::project_scanner::DEPENDENCY_PARSER_VERSION
    );

    Statement reason = database.prepare(R"sql(
        SELECT dirty_reason
        FROM project_files
        WHERE project_id = ?1
          AND id = ?2
          AND scan_generation = ?3
        LIMIT 1;
    )sql");
    reason.bind_int64(1, 1);
    reason.bind_int64(2, player_file_id);
    reason.bind_int64(3, rescanned.scan_generation);
    REQUIRE(reason.step() == Statement::StepResult::Row);
    CHECK(reason.column_text(0) == "dependency_parser_version_changed");

    ScanRepository repository(database);
    const ScanMetrics metrics = repository.get_scan_metrics(1, rescanned.scan_run_id);
    CHECK(metrics.scripts_dependency_parsed == 1);
}

TEST_CASE("native_scan_pipeline_reparses_when_scene_parser_version_changes") {
    TemporaryRoot root(make_temp_root("scene_parser_version_invalidation"));
    TemporaryRoot db_root(make_temp_root("scene_parser_version_invalidation_db"));

    write_text(root.path / "project.godot", "[application]\n");
    write_text(root.path / "scripts" / "player.gd", "class_name Player\nextends Node\n");
    write_text(
        root.path / "scenes" / "main.tscn",
        "[gd_scene load_steps=2 format=3]\n"
        "\n"
        "[ext_resource type=\"Script\" uid=\"uid://player-script\" path=\"res://scripts/player.gd\" id=\"1_player\"]\n"
        "\n"
        "[node name=\"Root\" type=\"Node\"]\n"
        "script = ExtResource(\"1_player\")\n"
    );

    Database database((db_root.path / "scanner.sqlite3").string());
    gotool::database::create_schema(database, 0);
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'scanner-scene-parser-version', 'scanner-scene-parser-version', '" + root.path.generic_string() + "', '" +
        root.path.generic_string() + "', '" + (root.path / "project.godot").generic_string() +
        "', '4.6.0-stable', 'test', 1, 1, 1, 1"
        ");"
    );

    NativeScanPipeline pipeline(database);
    ScanOptions options;
    options.project_id = 1;
    options.project_root = root.path;
    options.include_hidden = true;
    options.persist_to_database = true;
    options.collect_custom_classes = true;
    options.collect_script_dependencies = true;

    const gotool::project_scanner::ScanResultSummary cold = pipeline.run(options);
    CHECK(cold.scan_run_id > 0);

    const int64_t scene_file_id = query_int64(
        database,
        "SELECT id FROM project_files WHERE project_id = 1 AND project_relative_path = 'scenes/main.tscn' LIMIT 1;"
    );
    CHECK(
        query_int64(
            database,
            "SELECT COUNT(*) FROM scene_script_attachments WHERE project_id = 1 AND scene_file_id = " +
                std::to_string(scene_file_id) + ";"
        ) >= 1
    );

    Statement force_old_scene_parser_version = database.prepare(R"sql(
        UPDATE project_files
        SET scene_parser_version = 0
        WHERE project_id = ?1 AND id = ?2;
    )sql");
    force_old_scene_parser_version.bind_int64(1, 1);
    force_old_scene_parser_version.bind_int64(2, scene_file_id);
    force_old_scene_parser_version.step_done();

    const gotool::project_scanner::ScanResultSummary rescanned = pipeline.run(options);
    CHECK(rescanned.scan_run_id > 0);

    CHECK(
        query_int64(
            database,
            "SELECT scene_parser_version FROM project_files "
            "WHERE project_id = 1 AND id = " + std::to_string(scene_file_id) + ";"
        ) == gotool::project_scanner::SCENE_PARSER_VERSION
    );

    Statement reason = database.prepare(R"sql(
        SELECT dirty_reason
        FROM project_files
        WHERE project_id = ?1
          AND id = ?2
          AND scan_generation = ?3
        LIMIT 1;
    )sql");
    reason.bind_int64(1, 1);
    reason.bind_int64(2, scene_file_id);
    reason.bind_int64(3, rescanned.scan_generation);
    REQUIRE(reason.step() == Statement::StepResult::Row);
    CHECK(reason.column_text(0) == "scene_parser_version_changed");

    ScanRepository repository(database);
    const ScanMetrics metrics = repository.get_scan_metrics(1, rescanned.scan_run_id);
    CHECK(metrics.scene_attachment_parse_ms >= 0);
    CHECK(metrics.scene_attachment_rows_created >= 1);
}

TEST_CASE("scan_repository_exposes_script_intelligence_and_scene_attachment_queries") {
    TemporaryRoot root(make_temp_root("repository_script_intelligence_queries"));
    TemporaryRoot db_root(make_temp_root("repository_script_intelligence_queries_db"));

    write_text(root.path / "project.godot", "[application]\n");
    write_text(
        root.path / "scripts" / "player.gd",
        "## Player docs\n"
        "class_name Player\n"
        "extends Node\n"
        "\n"
        "## Current health\n"
        "var health: int = 100\n"
        "\n"
        "## Take damage\n"
        "func take_damage(amount: int) -> void:\n"
        "    health -= amount\n"
        "\n"
        "const EnemyScene = preload(\"res://scenes/enemy.tscn\")\n"
    );
    write_text(root.path / "scenes" / "enemy.tscn", "[gd_scene format=3]\n");
    write_text(
        root.path / "scenes" / "main.tscn",
        "[gd_scene load_steps=2 format=3]\n"
        "\n"
        "[ext_resource type=\"Script\" uid=\"uid://player-script\" path=\"res://scripts/player.gd\" id=\"1_player\"]\n"
        "\n"
        "[node name=\"Root\" type=\"Node\"]\n"
        "script = ExtResource(\"1_player\")\n"
    );

    Database database((db_root.path / "scanner.sqlite3").string());
    gotool::database::create_schema(database, 0);
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'repository-script-intelligence', 'repository-script-intelligence', '" + root.path.generic_string() + "', '" +
        root.path.generic_string() + "', '" + (root.path / "project.godot").generic_string() +
        "', '4.6.0-stable', 'test', 1, 1, 1, 1"
        ");"
    );

    NativeScanPipeline pipeline(database);
    ScanOptions options;
    options.project_id = 1;
    options.project_root = root.path;
    options.include_hidden = true;
    options.persist_to_database = true;
    options.collect_custom_classes = true;
    options.collect_script_dependencies = true;

    pipeline.run(options);

    const int64_t script_file_id = query_int64(
        database,
        "SELECT id FROM project_files WHERE project_id = 1 AND project_relative_path = 'scripts/player.gd' LIMIT 1;"
    );
    const int64_t scene_file_id = query_int64(
        database,
        "SELECT id FROM project_files WHERE project_id = 1 AND project_relative_path = 'scenes/main.tscn' LIMIT 1;"
    );

    ScanRepository repository(database);

    gotool::project_scanner::SymbolQueryFilter symbol_filter;
    const std::vector<gotool::project_scanner::ScriptSymbolRow> symbols =
        repository.list_symbols_for_script(1, script_file_id, symbol_filter);
    CHECK_FALSE(symbols.empty());

    const std::vector<gotool::project_scanner::ScriptSymbolRow> functions =
        repository.list_functions_for_script(1, script_file_id);
    CHECK_FALSE(functions.empty());

    const std::vector<gotool::project_scanner::ScriptSymbolRow> properties =
        repository.list_properties_for_script(1, script_file_id);
    CHECK_FALSE(properties.empty());

    const std::vector<gotool::project_scanner::ScriptSymbolRow> parameters =
        repository.list_parameters_for_function(1, functions.front().id);
    CHECK_FALSE(parameters.empty());

    gotool::project_scanner::DocCommentGapFilter gap_filter;
    const std::vector<gotool::project_scanner::ScriptSymbolRow> doc_gaps =
        repository.list_doc_comment_gaps(1, gap_filter);
    CHECK(doc_gaps.size() >= 1);

    const std::vector<gotool::project_scanner::ScriptReferenceRow> references =
        repository.list_references_for_script(1, script_file_id);
    CHECK_FALSE(references.empty());

    const std::vector<gotool::project_scanner::ScriptReferenceRow> refs_from_symbol =
        repository.list_references_from_symbol(1, functions.front().id);
    CHECK(refs_from_symbol.size() <= references.size());

    gotool::project_scanner::ReferenceQueryFilter reference_filter;
    reference_filter.script_file_id = script_file_id;
    const std::vector<gotool::project_scanner::ScriptReferenceRow> unresolved_references =
        repository.list_unresolved_references(1, reference_filter);
    const std::vector<gotool::project_scanner::ScriptReferenceRow> dynamic_references =
        repository.list_dynamic_references(1, reference_filter);
    for (const auto &reference : unresolved_references) {
        CHECK(reference.source_script_file_id == script_file_id);
    }
    for (const auto &reference : dynamic_references) {
        CHECK(reference.source_script_file_id == script_file_id);
    }

    gotool::project_scanner::SceneAttachmentQueryFilter attachment_filter;
    attachment_filter.scene_file_id = scene_file_id;
    const std::vector<gotool::project_scanner::SceneScriptAttachmentRow> attachments =
        repository.list_scene_script_attachments(1, attachment_filter);
    CHECK_FALSE(attachments.empty());
    CHECK(attachments.front().is_resolved);

    CHECK_FALSE(repository.list_scenes_using_script(1, script_file_id).empty());
    CHECK_FALSE(repository.list_scripts_attached_to_scene(1, scene_file_id).empty());

    const std::optional<gotool::project_scanner::ScriptSymbolRow> symbol_details =
        repository.get_symbol_details(1, functions.front().id);
    CHECK(symbol_details.has_value());

    const gotool::project_scanner::ScriptIntelligenceSummaryRow summary =
        repository.get_script_intelligence_summary(1, script_file_id);
    CHECK(summary.script_file_id == script_file_id);
    CHECK(summary.symbol_count >= 1);
    CHECK(summary.function_count >= 1);
    CHECK(summary.property_count >= 1);
    CHECK(summary.parameter_count >= 1);
    CHECK(summary.reference_count >= 1);
}

TEST_CASE("scanner_dependency_graph_delete_replace_and_resolution_behaviour") {
    TemporaryRoot root(make_temp_root("scanner_dependency_graph"));
    TemporaryRoot db_root(make_temp_root("scanner_dependency_graph_db"));

    write_text(root.path / "project.godot", "[application]\n");
    write_text(root.path / "scenes" / "enemy.tscn", "[gd_scene format=3]\n");
    write_text(root.path / "scripts" / "enemy.gd", "class_name Enemy\nextends Node\n");
    write_text(
        root.path / "scripts" / "spawner.gd",
        "class_name Spawner\n"
        "extends Node\n"
        "const EnemyScene = preload(\"res://scenes/enemy.tscn\")\n"
        "var target: Enemy\n"
        "func spawn() -> Enemy:\n"
        "    return Enemy.new()\n"
    );

    Database database((db_root.path / "scanner.sqlite3").string());
    gotool::database::create_schema(database, 0);
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'scanner-dependency-graph', 'scanner-dependency-graph', '" + root.path.generic_string() + "', '" +
        root.path.generic_string() + "', '" + (root.path / "project.godot").generic_string() +
        "', '4.6.0-stable', 'test', 1, 1, 1, 1"
        ");"
    );

    NativeScanPipeline pipeline(database);
    ScanOptions options;
    options.project_id = 1;
    options.project_root = root.path;
    options.include_hidden = true;
    options.persist_to_database = true;
    options.collect_custom_classes = true;
    options.collect_script_dependencies = true;

    pipeline.run(options);

    const int64_t spawner_id = query_int64(
        database,
        "SELECT id FROM project_files WHERE project_id = 1 AND project_relative_path = 'scripts/spawner.gd' LIMIT 1;"
    );
    const int64_t enemy_id = query_int64(
        database,
        "SELECT id FROM project_files WHERE project_id = 1 AND project_relative_path = 'scripts/enemy.gd' LIMIT 1;"
    );

    CHECK(
        query_int64(
            database,
            "SELECT COUNT(*) FROM script_dependencies "
            "WHERE project_id = 1 AND source_script_file_id = " + std::to_string(spawner_id) +
            " AND dependency_kind = 'const_preload_alias';"
        ) == 1
    );

    CHECK(
        query_int64(
            database,
            "SELECT COUNT(*) FROM script_dependencies "
            "WHERE project_id = 1 AND source_script_file_id = " + std::to_string(spawner_id) +
            " AND target_class_name = 'Enemy' "
            "AND target_file_id = " + std::to_string(enemy_id) +
            " AND is_resolved = 1;"
        ) >= 1
    );

    CHECK(
        query_int64(
            database,
            "SELECT COUNT(*) FROM script_dependencies "
            "WHERE project_id = 1 AND source_script_file_id = " + std::to_string(spawner_id) +
            " AND parser_version = " + std::to_string(gotool::project_scanner::DEPENDENCY_PARSER_VERSION) + ";"
        ) >= 1
    );

    ScanRepository repository(database);
    CHECK_FALSE(repository.list_dependencies_for_script(1, spawner_id).empty());
    CHECK_FALSE(repository.list_dependents_of_file(1, enemy_id).empty());
    CHECK_FALSE(repository.list_dependents_of_class(1, "Enemy").empty());
    CHECK(repository.list_dynamic_dependencies(1).size() >= 0);
    CHECK(repository.list_dependency_cycles(1).size() >= 0);
    CHECK(repository.get_dependency_graph_slice(1, spawner_id, 2).size() >= 1);

    write_text(
        root.path / "scripts" / "spawner.gd",
        "class_name Spawner\n"
        "extends Node\n"
        "func spawn() -> void:\n"
        "    return\n"
    );
    pipeline.run(options);
    CHECK(
        query_int64(
            database,
            "SELECT COUNT(*) FROM script_dependencies "
            "WHERE project_id = 1 AND source_script_file_id = " + std::to_string(spawner_id) +
            " AND dependency_kind = 'const_preload_alias';"
        ) == 0
    );

    write_text(
        root.path / "scripts" / "spawner.gd",
        "class_name Spawner\n"
        "extends Node\n"
        "var target: MissingEnemy\n"
        "func spawn(target: MissingEnemy) -> MissingEnemy:\n"
        "    return MissingEnemy.new()\n"
    );
    pipeline.run(options);
    CHECK(
        query_int64(
            database,
            "SELECT COUNT(*) FROM script_dependencies "
            "WHERE project_id = 1 AND source_script_file_id = " + std::to_string(spawner_id) +
            " AND target_class_name = 'MissingEnemy' AND is_resolved = 0;"
        ) >= 1
    );
    CHECK_FALSE(repository.list_unresolved_dependencies(1).empty());

    write_text(root.path / "scripts" / "missing_enemy.gd", "class_name MissingEnemy\nextends Node\n");
    pipeline.run(options);
    CHECK(
        query_int64(
            database,
            "SELECT COUNT(*) FROM script_dependencies "
            "WHERE project_id = 1 AND source_script_file_id = " + std::to_string(spawner_id) +
            " AND target_class_name = 'MissingEnemy' AND is_resolved = 0;"
        ) >= 1
    );

    const int64_t missing_enemy_id = query_int64(
        database,
        "SELECT id FROM project_files WHERE project_id = 1 AND project_relative_path = 'scripts/missing_enemy.gd' LIMIT 1;"
    );

    // Reparse the source script so class-based targets can resolve to the new file.
    write_text(
        root.path / "scripts" / "spawner.gd",
        "class_name Spawner\n"
        "extends Node\n"
        "var target: MissingEnemy\n"
        "func spawn(target: MissingEnemy) -> MissingEnemy:\n"
        "    return MissingEnemy.new()\n"
        "# touch for reparse\n"
    );
    pipeline.run(options);

    CHECK(
        query_int64(
            database,
            "SELECT COUNT(*) FROM script_dependencies "
            "WHERE project_id = 1 AND source_script_file_id = " + std::to_string(spawner_id) +
            " AND target_class_name = 'MissingEnemy' "
            "AND target_file_id = " + std::to_string(missing_enemy_id) +
            " AND is_resolved = 1;"
        ) >= 1
    );
}

TEST_CASE("native_directory_enumerator_stops_when_cancel_already_requested") {
    TemporaryRoot root(make_temp_root("enumerator_cancel_before_start"));
    write_text(root.path / "project.godot", "[application]\n");
    write_text(root.path / "scripts" / "player.gd", "class_name Player\nextends Node\n");

    std::atomic_bool cancel_requested = true;
    EnumerationOptions options;
    options.root = root.path;
    options.include_hidden = true;
    options.cancel_requested = &cancel_requested;

    PathArena arena;
    std::vector<gotool::project_scanner::EntryRecord> records;
    NativeDirectoryEnumerator enumerator;
    const EnumerationResult result = enumerator.enumerate(options, arena, records);

    CHECK_FALSE(result.completed);
}

TEST_CASE("native_scan_pipeline_reports_cancelled_before_enumeration_and_keeps_db_clean") {
    TemporaryRoot root(make_temp_root("pipeline_cancel_before_enumeration"));
    write_text(root.path / "project.godot", "[application]\n");
    write_text(root.path / "scripts" / "player.gd", "class_name Player\nextends Node\n");

    Database database((root.path / "scanner.sqlite3").string());
    gotool::database::create_schema(database, 0);
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'scanner-cancelled', 'scanner-cancelled', '" + root.path.generic_string() + "', '" +
        root.path.generic_string() + "', '" + (root.path / "project.godot").generic_string() +
        "', '4.6.0-stable', 'test', 1, 1, 1, 1"
        ");"
    );

    std::atomic_bool cancel_requested = true;
    NativeScanPipeline pipeline(database);
    ScanOptions options;
    options.project_id = 1;
    options.project_root = root.path;
    options.include_hidden = true;
    options.persist_to_database = true;
    options.cancel_requested = &cancel_requested;

    const gotool::project_scanner::ScanResultSummary cancelled = pipeline.run(options);
    CHECK(cancelled.status == "cancelled");
    CHECK(cancelled.scan_run_id > 0);
    CHECK(query_int64(database, "SELECT COUNT(*) FROM project_files WHERE project_id = 1;") == 0);
    CHECK(query_int64(database, "SELECT COUNT(*) FROM project_scan_runs WHERE project_id = 1 AND status = 'cancelled';") == 1);
}

TEST_CASE("native_scan_pipeline_no_db_mode_returns_records_without_db_writes") {
    TemporaryRoot root(make_temp_root("pipeline_no_db_mode"));
    write_text(root.path / "project.godot", "[application]\n");
    write_text(root.path / "scripts" / "player.gd", "class_name Player\nextends Node\n");

    Database database((root.path / "scanner.sqlite3").string());
    gotool::database::create_schema(database, 0);
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'scanner-no-db', 'scanner-no-db', '" + root.path.generic_string() + "', '" +
        root.path.generic_string() + "', '" + (root.path / "project.godot").generic_string() +
        "', '4.6.0-stable', 'test', 1, 1, 1, 1"
        ");"
    );

    NativeScanPipeline pipeline(database);
    ScanOptions options;
    options.project_id = 1;
    options.project_root = root.path;
    options.include_hidden = true;
    options.persist_to_database = false;
    options.collect_custom_classes = true;

    const gotool::project_scanner::NativeScanResult result = pipeline.run_detailed(options);
    CHECK(result.summary.scan_run_id == 0);
    CHECK(result.summary.status == "completed");
    CHECK_FALSE(result.records.empty());
    CHECK(query_int64(database, "SELECT COUNT(*) FROM project_scan_runs WHERE project_id = 1;") == 0);
    CHECK(query_int64(database, "SELECT COUNT(*) FROM project_files WHERE project_id = 1;") == 0);
}

TEST_CASE("native_scan_pipeline_can_skip_existing_snapshot_load_for_benchmark_paths") {
    TemporaryRoot root(make_temp_root("pipeline_skip_snapshot"));
    write_text(root.path / "project.godot", "[application]\n");
    write_text(root.path / "scripts" / "player.gd", "class_name Player\nextends Node\n");

    Database database((root.path / "scanner.sqlite3").string());
    gotool::database::create_schema(database, 0);
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'scanner-skip-snapshot', 'scanner-skip-snapshot', '" + root.path.generic_string() + "', '" +
        root.path.generic_string() + "', '" + (root.path / "project.godot").generic_string() +
        "', '4.6.0-stable', 'test', 1, 1, 1, 1"
        ");"
    );

    NativeScanPipeline pipeline(database);

    ScanOptions persisted;
    persisted.project_id = 1;
    persisted.project_root = root.path;
    persisted.include_hidden = true;
    persisted.persist_to_database = true;
    pipeline.run(persisted);

    ScanOptions benchmark_like;
    benchmark_like.project_id = 1;
    benchmark_like.project_root = root.path;
    benchmark_like.include_hidden = true;
    benchmark_like.persist_to_database = false;
    benchmark_like.load_existing_snapshot = false;

    const gotool::project_scanner::NativeScanResult benchmark_like_result = pipeline.run_detailed(benchmark_like);
    CHECK(benchmark_like_result.metrics.existing_snapshot_count == 0);
}

TEST_CASE("native_scan_pipeline_dirty_path_filter_limits_script_reparse_scope") {
    TemporaryRoot root(make_temp_root("pipeline_dirty_path_filter"));
    write_text(root.path / "project.godot", "[application]\n");
    write_text(root.path / "scripts" / "player.gd", "class_name Player\nextends Node\n");
    write_text(root.path / "scripts" / "enemy.gd", "class_name Enemy\nextends Node\n");

    Database database((root.path / "scanner.sqlite3").string());
    gotool::database::create_schema(database, 0);
    database.exec(
        "INSERT INTO projects ("
        "id, project_uid, display_name, root_absolute_path, root_canonical_path, "
        "project_file_absolute_path, godot_version, identity_source, first_seen_unix, last_seen_unix, "
        "created_at_unix, updated_at_unix"
        ") VALUES ("
        "1, 'scanner-dirty-filter', 'scanner-dirty-filter', '" + root.path.generic_string() + "', '" +
        root.path.generic_string() + "', '" + (root.path / "project.godot").generic_string() +
        "', '4.6.0-stable', 'test', 1, 1, 1, 1"
        ");"
    );

    NativeScanPipeline pipeline(database);

    ScanOptions cold;
    cold.project_id = 1;
    cold.project_root = root.path;
    cold.include_hidden = true;
    cold.persist_to_database = true;
    pipeline.run(cold);

    write_text(root.path / "scripts" / "player.gd", "class_name Player\nextends Resource\n");

    ScanOptions incremental;
    incremental.project_id = 1;
    incremental.project_root = root.path;
    incremental.include_hidden = true;
    incremental.persist_to_database = false;
    incremental.collect_custom_classes = true;
    incremental.load_existing_snapshot = true;
    incremental.use_dirty_path_filter = true;
    incremental.dirty_paths = { "scripts/player.gd" };

    const gotool::project_scanner::NativeScanResult incremental_result = pipeline.run_detailed(incremental);
    CHECK(incremental_result.metrics.scripts_parsed == 1);
    CHECK(incremental_result.metrics.scripts_skipped_clean >= 1);
}

TEST_CASE("native_directory_enumerator_parallel_mode_preserves_deterministic_path_set") {
    TemporaryRoot root(make_temp_root("parallel_enumerator"));
    write_text(root.path / "project.godot", "[application]\n");

    for (int32_t i = 0; i < 120; ++i) {
        write_text(
            root.path / "assets" / ("bucket_" + std::to_string(i % 12)) /
                ("asset_" + std::to_string(i) + ".json"),
            "{}\n"
        );
    }

    gotool::project_scanner::SkipPolicy skip_policy;
    NativeDirectoryEnumerator enumerator;

    EnumerationOptions serial;
    serial.root = root.path;
    serial.include_hidden = true;
    serial.skip_policy = &skip_policy;
    serial.enable_parallel_traversal = false;
    serial.deterministic_record_order = true;

    EnumerationOptions parallel = serial;
    parallel.enable_parallel_traversal = true;
    parallel.max_parallel_workers = 4;

    PathArena serial_arena;
    std::vector<gotool::project_scanner::EntryRecord> serial_records;
    const EnumerationResult serial_result = enumerator.enumerate(serial, serial_arena, serial_records);

    PathArena parallel_arena;
    std::vector<gotool::project_scanner::EntryRecord> parallel_records;
    const EnumerationResult parallel_result = enumerator.enumerate(parallel, parallel_arena, parallel_records);

    CHECK(serial_result.completed);
    CHECK(parallel_result.completed);
    CHECK(serial_result.files_seen == parallel_result.files_seen);
    CHECK(serial_result.dirs_seen == parallel_result.dirs_seen);

    std::multiset<std::string> serial_paths;
    for (const auto &record : serial_records) {
        serial_paths.insert(serial_arena.string_at(record.path_offset, record.path_length));
    }

    std::multiset<std::string> parallel_paths;
    for (const auto &record : parallel_records) {
        parallel_paths.insert(parallel_arena.string_at(record.path_offset, record.path_length));
    }

    CHECK(serial_paths == parallel_paths);
}
