#include "project_scanner/gotool_project_scanner_rules.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <initializer_list>

namespace gotool::project_scanner {

namespace {

static constexpr const char *GOTOOL_ADDON_PATH = "res://addons/GoToolCenter";
static constexpr const char *GOTOOL_ADDON_PATH_PREFIX = "res://addons/GoToolCenter/";

bool matches_exact_any(const godot::String &value, std::initializer_list<const char *> candidates) {
    for (const char *candidate : candidates) {
        if (value == candidate) {
            return true;
        }
    }

    return false;
}

bool matches_suffix_any(const godot::String &value, std::initializer_list<const char *> candidates) {
    for (const char *candidate : candidates) {
        if (value.ends_with(candidate)) {
            return true;
        }
    }

    return false;
}

bool is_config_file_extension(const godot::String &extension) {
    return matches_exact_any(extension, {
        ".cfg", ".ini", ".conf", ".config"
    });
}

bool is_godot_editor_path(const godot::String &lower_path) {
    return lower_path.begins_with("res://.godot/editor/");
}

bool is_godot_imported_path(const godot::String &lower_path) {
    return lower_path.begins_with("res://.godot/imported/");
}

bool is_godot_exported_path(const godot::String &lower_path) {
    return lower_path.begins_with("res://.godot/exported/");
}

godot::String strip_inline_gdscript_comment(const godot::String &line) {
    const int64_t comment_index = line.find("#");

    if (comment_index < 0) {
        return line;
    }

    return line.substr(0, comment_index);
}

godot::String parse_gdscript_class_name_from_line(const godot::String &line) {
    godot::String cleaned = strip_inline_gdscript_comment(line).strip_edges();

    if (!cleaned.begins_with("class_name")) {
        return "";
    }

    cleaned = cleaned.substr(godot::String("class_name").length()).strip_edges();

    if (cleaned.is_empty()) {
        return "";
    }

    const godot::PackedStringArray parts = cleaned.split(" ", false);

    if (parts.is_empty()) {
        return "";
    }

    return godot::String(parts[0]).strip_edges();
}

godot::String parse_gdscript_extends_from_line(const godot::String &line) {
    godot::String cleaned = strip_inline_gdscript_comment(line).strip_edges();

    if (!cleaned.begins_with("extends")) {
        return "";
    }

    cleaned = cleaned.substr(godot::String("extends").length()).strip_edges();

    if (cleaned.is_empty()) {
        return "";
    }

    const godot::PackedStringArray parts = cleaned.split(" ", false);

    if (parts.is_empty()) {
        return "";
    }

    const godot::String base = godot::String(parts[0]).strip_edges();

    if ((base.begins_with("\"") && base.ends_with("\"")) ||
        (base.begins_with("'") && base.ends_with("'"))) {
        return "ScriptPath";
    }

    return base;
}

godot::Dictionary parse_gdscript_custom_class(const godot::String &path) {
    godot::Dictionary result;

    if (path.is_empty() || path == "res://" || path == "res:///") {
        return result;
    }

    godot::Ref<godot::FileAccess> file = godot::FileAccess::open(path, godot::FileAccess::READ);

    if (file.is_null()) {
        return result;
    }

    godot::String class_name;
    godot::String base_type;

    int64_t lines_scanned = 0;
    static constexpr int64_t MAX_HEADER_LINES_TO_SCAN = 120;

    while (!file->eof_reached() && lines_scanned < MAX_HEADER_LINES_TO_SCAN) {
        const godot::String line = file->get_line();
        ++lines_scanned;

        if (class_name.is_empty()) {
            class_name = parse_gdscript_class_name_from_line(line);
        }

        if (base_type.is_empty()) {
            base_type = parse_gdscript_extends_from_line(line);
        }

        if (!class_name.is_empty() && !base_type.is_empty()) {
            break;
        }
    }

    if (class_name.is_empty()) {
        return result;
    }

    result["class_name"] = class_name;
    result["base_type"] = base_type;

    return result;
}

bool line_contains_csharp_global_class_attribute(const godot::String &line) {
    const godot::String cleaned = line.strip_edges();

    return cleaned.contains("[GlobalClass]") ||
           cleaned.contains("[GlobalClassAttribute]");
}

godot::String strip_csharp_line_comment(const godot::String &line) {
    const int64_t comment_index = line.find("//");

    if (comment_index < 0) {
        return line;
    }

    return line.substr(0, comment_index);
}

godot::Dictionary parse_csharp_custom_class(const godot::String &path) {
    godot::Dictionary result;

    if (path.is_empty() || path == "res://" || path == "res:///") {
        return result;
    }

    godot::Ref<godot::FileAccess> file = godot::FileAccess::open(path, godot::FileAccess::READ);

    if (file.is_null()) {
        return result;
    }

    bool saw_global_class = false;

    int64_t lines_scanned = 0;
    static constexpr int64_t MAX_HEADER_LINES_TO_SCAN = 240;

    while (!file->eof_reached() && lines_scanned < MAX_HEADER_LINES_TO_SCAN) {
        godot::String line = strip_csharp_line_comment(file->get_line()).strip_edges();
        ++lines_scanned;

        if (line.is_empty()) {
            continue;
        }

        if (line_contains_csharp_global_class_attribute(line)) {
            saw_global_class = true;
            continue;
        }

        if (!saw_global_class) {
            continue;
        }

        if (!line.contains(" class ")) {
            continue;
        }

        line = line.replace("{", " ");
        line = line.replace(":", " : ");
        line = line.replace(",", " ");

        const godot::PackedStringArray parts = line.split(" ", false);

        godot::String class_name;
        godot::String base_type;

        for (int64_t i = 0; i < parts.size(); ++i) {
            const godot::String part = godot::String(parts[i]).strip_edges();

            if (part == "class" && i + 1 < parts.size()) {
                class_name = godot::String(parts[i + 1]).strip_edges();
            }

            if (part == ":" && i + 1 < parts.size()) {
                base_type = godot::String(parts[i + 1]).strip_edges();
            }
        }

        if (!class_name.is_empty()) {
            result["class_name"] = class_name;
            result["base_type"] = base_type;
            return result;
        }
    }

    return result;
}

} // namespace

FileFacts build_file_facts(
    const godot::String &full_path,
    const godot::String &name,
    bool is_directory
) {
    FileFacts facts;
    facts.full_path = full_path;
    facts.name = name;
    facts.lower_path = full_path.to_lower();
    facts.lower_name = name.to_lower();
    facts.is_directory = is_directory;

    if (!is_directory) {
        const godot::String extension = name.get_extension().to_lower();
        facts.extension = extension.is_empty() ? "" : "." + extension;
    }

    return facts;
}

bool should_skip_scan_path(const godot::String &full_path) {
    return full_path == GOTOOL_ADDON_PATH || full_path.begins_with(GOTOOL_ADDON_PATH_PREFIX);
}

godot::String to_project_relative_path(const godot::String &full_path) {
    if (full_path.begins_with("res://")) {
        return full_path.substr(6);
    }

    return full_path;
}

bool is_script_extension(const godot::String &extension) {
    return extension == ".gd" || extension == ".cs";
}

godot::String language_from_script_extension(const godot::String &extension) {
    if (extension == ".cs") {
        return "CSharp";
    }

    if (extension == ".gd") {
        return "GDScript";
    }

    return "";
}

godot::Dictionary parse_custom_script_class(
    const godot::String &path,
    const godot::String &extension
) {
    if (extension == ".gd") {
        return parse_gdscript_custom_class(path);
    }

    if (extension == ".cs") {
        return parse_csharp_custom_class(path);
    }

    return godot::Dictionary();
}

bool is_class_or_parent_class(const godot::String &class_name, const char *expected_parent) {
    if (class_name.is_empty()) {
        return false;
    }

    if (class_name == expected_parent) {
        return true;
    }

    return godot::ClassDB::is_parent_class(godot::StringName(class_name), godot::StringName(expected_parent));
}

godot::String classify_file_type_from_facts(const FileFacts &facts) {
    if (facts.is_directory) {
        return "Folder";
    }

    const godot::String &lower_name = facts.lower_name;
    const godot::String &lower_path = facts.lower_path;

    if (is_godot_editor_path(lower_path)) {
        if (lower_path.begins_with("res://.godot/editor/filesystem_cache")) {
            return "GodotEditorFilesystemCache";
        }

        if (lower_path.begins_with("res://.godot/editor/filesystem_update")) {
            return "GodotEditorFilesystemUpdate";
        }

        if (lower_path == "res://.godot/editor/recent_dirs") {
            return "GodotEditorRecentDirs";
        }

        if (lower_path == "res://.godot/editor/favorites" ||
            lower_path.begins_with("res://.godot/editor/favorites.")) {
            return "GodotEditorFavoritesMetadata";
        }

        if (lower_path == "res://.godot/editor/create_recent" ||
            lower_path.begins_with("res://.godot/editor/create_recent.")) {
            return "GodotEditorRecentCreateMetadata";
        }
    }

    if (is_godot_exported_path(lower_path)) {
        if (lower_name == "file_cache" || lower_path.ends_with("/file_cache")) {
            return "GodotExportFileCache";
        }
    }

    if (is_godot_imported_path(lower_path)) {
        if (lower_name.ends_with(".ctex")) {
            return "GodotImportedTexture";
        }

        if (lower_name.ends_with(".fontdata")) {
            return "GodotImportedFontData";
        }

        if (lower_name.ends_with(".md5")) {
            return "GodotImportHash";
        }
    }

    if (lower_path.begins_with("res://.godot/shader_cache/")) {
        return "GodotShaderCache";
    }

    if (lower_name == "project.godot") {
        return "GodotProjectConfig";
    }

    if (lower_name == "plugin.cfg") {
        return "GodotPluginConfig";
    }

    if (lower_name == ".gdignore") {
        return "GodotIgnore";
    }

    if (lower_name == ".gitignore") {
        return "Git Ignore";
    }

    if (lower_name == ".gitattributes") {
        return "Git Attributes";
    }

    if (lower_name == ".gitmodules") {
        return "Git Modules";
    }

    if (lower_name == ".mailmap") {
        return "Git Mailmap";
    }

    if (lower_name == ".gitkeep") {
        return "Git Keep";
    }

    if (lower_name == ".editorconfig") {
        return "EditorConfig";
    }

    if (lower_name == ".dockerignore") {
        return "DockerIgnore";
    }

    if (lower_name == ".hgignore") {
        return "MercurialIgnore";
    }

    if (lower_name == ".svnignore") {
        return "SvnIgnore";
    }

    if (matches_suffix_any(lower_name, { ".gd" })) {
        return "GDScript";
    }

    if (matches_suffix_any(lower_name, { ".cs" })) {
        return "CSharpScript";
    }

    if (matches_suffix_any(lower_name, { ".tscn", ".scn" })) {
        return "Scene";
    }

    if (matches_suffix_any(lower_name, { ".tres", ".res" })) {
        return "GodotResource";
    }

    if (matches_suffix_any(lower_name, { ".gdshader", ".shader" })) {
        return "ShaderSource";
    }

    if (matches_suffix_any(lower_name, { ".gdshaderinc" })) {
        return "ShaderInclude";
    }

    if (matches_suffix_any(lower_name, { ".uid" })) {
        return "GodotResourceUID";
    }

    if (matches_suffix_any(lower_name, { ".import" })) {
        return "GodotImportMetadata";
    }

    if (matches_suffix_any(lower_name, { ".ctex" })) {
        return "GodotImportedTexture";
    }

    if (matches_suffix_any(lower_name, { ".fontdata" })) {
        return "GodotImportedFontData";
    }

    if (matches_suffix_any(lower_name, { ".gdextension" })) {
        return "GodotExtensionConfig";
    }

    if (matches_suffix_any(lower_name, { ".remap" })) {
        return "GodotExportRemap";
    }

    if (matches_suffix_any(lower_name, { ".node" })) {
        return "GodotEditorMetadata";
    }

    if (matches_suffix_any(lower_name, { ".object" })) {
        return "GodotEditorMetadata";
    }

    if (matches_suffix_any(lower_name, { ".resource" })) {
        return "GodotEditorMetadata";
    }

    if (matches_suffix_any(lower_name, { ".cfg", ".ini", ".conf", ".config" })) {
        if (lower_path.begins_with("res://.godot/editor/")) {
            return "GodotEditorConfig";
        }

        if (lower_path.begins_with("res://.godot/")) {
            return "GodotCacheConfig";
        }

        return "Config";
    }

    if (matches_exact_any(lower_name, {
            "dockerfile", "makefile", "cmakelists.txt", "justfile", "procfile", "jenkinsfile",
            "pipfile", "pipfile.lock", "gemfile", "gemfile.lock", "cargo.toml", "cargo.lock",
            "go.mod", "go.sum", "package.json", "package-lock.json", "npm-shrinkwrap.json",
            "yarn.lock", "pnpm-lock.yaml", "bun.lock", "bun.lockb", "deno.json", "deno.lock",
            "composer.json", "composer.lock", "requirements.txt", "pyproject.toml", "poetry.lock",
            "pubspec.yaml", "pubspec.lock", "build.gradle", "settings.gradle", "taskfile.yml",
            "taskfile.yaml", "docker-compose.yml", "docker-compose.yaml", "thumbs.db",
            "desktop.ini", "ehthumbs.db", ".ds_store"
        })) {
        if (matches_exact_any(lower_name, {
                "dockerfile", "makefile", "cmakelists.txt", "justfile", "taskfile.yml", "taskfile.yaml"
            })) {
            return "BuildScript";
        }

        if (matches_exact_any(lower_name, {
                "jenkinsfile", "procfile", "docker-compose.yml", "docker-compose.yaml", "build.gradle", "settings.gradle"
            })) {
            return "BuildConfig";
        }

        if (matches_exact_any(lower_name, {
                "pipfile.lock", "gemfile.lock", "cargo.lock", "go.sum", "package-lock.json",
                "npm-shrinkwrap.json", "yarn.lock", "pnpm-lock.yaml", "bun.lock", "bun.lockb",
                "deno.lock", "composer.lock", "poetry.lock", "pubspec.lock"
            })) {
            return "PackageLock";
        }

        if (matches_exact_any(lower_name, {
                "pipfile", "gemfile", "cargo.toml", "go.mod", "package.json", "deno.json",
                "composer.json", "requirements.txt", "pyproject.toml", "pubspec.yaml"
            })) {
            return "PackageManifest";
        }

        if (matches_exact_any(lower_name, {
                ".ds_store", "thumbs.db", "desktop.ini", "ehthumbs.db"
            })) {
            return "OSMetadata";
        }
    }

    if (matches_suffix_any(lower_name, {
            ".txt", ".text", ".md", ".markdown", ".mdown", ".mkd", ".rst", ".adoc", ".asciidoc",
            ".org", ".tex", ".log", ".out", ".err", ".trace", ".todo", ".notes", ".readme",
            ".changelog", ".license", ".notice", ".authors", ".contributors", ".copying",
            ".install", ".news"
        })) {
        return "Text";
    }

    if (matches_suffix_any(lower_name, {
            "package.json", "deno.json", "requirements.txt", "pipfile", "pyproject.toml",
            "cargo.toml", "go.mod", "gemfile", "pubspec.yaml", "composer.json"
        })) {
        return "PackageManifest";
    }

    if (matches_suffix_any(lower_name, {
            "package-lock.json", "npm-shrinkwrap.json", "yarn.lock", "pnpm-lock.yaml",
            "bun.lock", "bun.lockb", "deno.lock", "composer.lock", "pipfile.lock", "poetry.lock",
            "cargo.lock", "go.sum", "gemfile.lock", "pubspec.lock"
        })) {
        return "PackageLock";
    }

    if (matches_suffix_any(lower_name, {
            ".json", ".jsonc", ".json5", ".jsonl", ".ndjson", ".yaml", ".yml", ".toml", ".xml",
            ".csv", ".tsv", ".psv", ".properties", ".props",
            ".env", ".env.local", ".env.example", ".env.template", ".lock", ".plist", ".wxs",
            ".wxi", ".wxl", ".rdf", ".rss", ".atom", ".geojson", ".topojson", ".ndb", ".bytes",
            ".dat", ".data", ".asset", ".meta", ".edn"
        })) {
        return "Data";
    }

    if (matches_suffix_any(lower_name, { ".bin", ".blob" })) {
        return "BinaryData";
    }

    if (matches_suffix_any(lower_name, {
            ".db", ".db3", ".sqlite", ".sqlite3", ".s3db", ".sl3", ".duckdb", ".realm",
            ".mdb", ".accdb", ".dbf", ".fdb", ".gdb", ".kdbx", ".sqlitedb"
        })) {
        return "Database";
    }

    if (matches_suffix_any(lower_name, { ".sql" })) {
        return "DatabaseScript";
    }

    if (matches_suffix_any(lower_name, { ".xls", ".xlsx", ".xlsm", ".ods", ".numbers" })) {
        return "Spreadsheet";
    }

    if (matches_suffix_any(lower_name, { ".doc", ".docx", ".odt", ".rtf", ".pdf", ".pages", ".epub" })) {
        return "Document";
    }

    if (matches_suffix_any(lower_name, { ".ppt", ".pptx", ".odp", ".key" })) {
        return "Presentation";
    }

    if (matches_suffix_any(lower_name, {
            ".bmp", ".dib", ".gif", ".jpg", ".jpeg", ".jpe", ".jfif", ".png", ".apng", ".webp",
            ".avif", ".tga", ".targa", ".tif", ".tiff", ".ico", ".icns", ".heic", ".heif", ".qoi",
            ".exr", ".hdr", ".pbm", ".pgm", ".ppm", ".pnm"
        })) {
        return "Image";
    }

    if (matches_suffix_any(lower_name, { ".dds", ".ktx", ".ktx2", ".pvr", ".astc" })) {
        return "Texture";
    }

    if (matches_suffix_any(lower_name, {
            ".ase", ".aseprite", ".psd", ".psb", ".kra", ".ora", ".xcf", ".clip", ".clipstudiopaint",
            ".sai", ".sai2", ".mdp", ".pdn", ".afphoto", ".afdesign", ".cpt", ".procreate",
            ".sketch", ".figma"
        })) {
        return "SourceArt";
    }

    if (matches_suffix_any(lower_name, { ".svg", ".svgz" })) {
        return "VectorImage";
    }

    if (matches_suffix_any(lower_name, { ".ai", ".eps", ".cdr", ".sk1", ".dxf", ".dwg" })) {
        return "VectorSource";
    }

    if (matches_suffix_any(lower_name, {
            ".tiled-project", ".tiled-session", ".tmx", ".tsx", ".tilemap"
        })) {
        return "TilemapSource";
    }

    if (matches_suffix_any(lower_name, { ".world", ".ldtk", ".ldtkl", ".ogmo", ".ogmoproject" })) {
        return "MapSource";
    }

    if (matches_suffix_any(lower_name, { ".pyxel", ".piskel" })) {
        return "PixelArtSource";
    }

    if (matches_suffix_any(lower_name, {
            ".wav", ".mp3", ".ogg", ".oga", ".flac", ".aiff", ".aif", ".aifc", ".m4a",
            ".aac", ".opus", ".wma"
        })) {
        return "Audio";
    }

    if (matches_suffix_any(lower_name, { ".mid", ".midi" })) {
        return "Midi";
    }

    if (matches_suffix_any(lower_name, { ".xm", ".mod", ".it", ".s3m" })) {
        return "TrackerModule";
    }

    if (matches_suffix_any(lower_name, {
            ".flp", ".als", ".logicx", ".band", ".rpp", ".cpr", ".ptx", ".song", ".xrns", ".mmpz"
        })) {
        return "AudioProject";
    }

    if (matches_suffix_any(lower_name, { ".mscz", ".musicxml", ".mxl" })) {
        return "MusicScore";
    }

    if (matches_suffix_any(lower_name, {
            ".mp4", ".m4v", ".mov", ".webm", ".ogv", ".avi", ".mkv", ".wmv", ".flv", ".mpg", ".mpeg", ".3gp"
        })) {
        return "Video";
    }

    if (matches_suffix_any(lower_name, { ".prproj", ".aep", ".drp", ".kdenlive" })) {
        return "VideoProject";
    }

    if (matches_suffix_any(lower_name, { ".ttf", ".otf", ".woff", ".woff2", ".eot" })) {
        return "Font";
    }

    if (matches_suffix_any(lower_name, { ".fnt", ".bdf", ".pcf" })) {
        return "BitmapFont";
    }

    if (matches_suffix_any(lower_name, { ".sfd", ".glyphs", ".ufo", ".spritefont", ".spritefont2" })) {
        return "FontSource";
    }

    if (matches_suffix_any(lower_name, { ".blend", ".vroid", ".magica" })) {
        if (lower_name.ends_with(".vroid")) {
            return "AvatarSource";
        }

        if (lower_name.ends_with(".magica")) {
            return "VoxelSource";
        }

        return "ModelSource";
    }

    if (matches_suffix_any(lower_name, { ".blend1", ".blend2" })) {
        return "ModelBackup";
    }

    if (matches_suffix_any(lower_name, {
            ".fbx", ".obj", ".dae", ".glb", ".gltf", ".stl", ".ply", ".3ds",
            ".usd", ".usda", ".usdc", ".usdz", ".iqm", ".iqe"
        })) {
        return "Model";
    }

    if (matches_suffix_any(lower_name, { ".mtl" })) {
        return "Material";
    }

    if (matches_suffix_any(lower_name, { ".abc" })) {
        return "ModelCache";
    }

    if (matches_suffix_any(lower_name, { ".bvh" })) {
        return "Animation";
    }

    if (matches_suffix_any(lower_name, { ".vox" })) {
        return "VoxelModel";
    }

    if (matches_suffix_any(lower_name, { ".vrm" })) {
        return "AvatarModel";
    }

    if (matches_suffix_any(lower_name, { ".mat", ".sbs", ".sbsar" })) {
        return "MaterialSource";
    }

    if (matches_suffix_any(lower_name, {
            ".slang", ".glsl", ".vert", ".frag", ".geom", ".comp", ".tesc", ".tese",
            ".hlsl", ".fx", ".cg", ".shadergraph"
        })) {
        return "ShaderSource";
    }

    if (matches_suffix_any(lower_name, {
            ".zip", ".7z", ".rar", ".tar", ".gz", ".tgz", ".bz2", ".tbz2", ".xz", ".txz",
            ".zst", ".br", ".lz", ".lzma", ".cab"
        })) {
        return "Archive";
    }

    if (matches_suffix_any(lower_name, { ".iso", ".dmg", ".img" })) {
        return "DiskImage";
    }

    if (matches_suffix_any(lower_name, { ".exe" })) {
        return "Executable";
    }

    if (matches_suffix_any(lower_name, { ".dll", ".so", ".dylib" })) {
        return "Library";
    }

    if (matches_suffix_any(lower_name, { ".a", ".lib" })) {
        return "StaticLibrary";
    }

    if (matches_suffix_any(lower_name, { ".o", ".obj" })) {
        return "ObjectFile";
    }

    if (matches_suffix_any(lower_name, { ".pdb" })) {
        return "DebugSymbols";
    }

    if (matches_suffix_any(lower_name, { ".exp", ".ilk" })) {
        return "BuildArtifact";
    }

    if (matches_suffix_any(lower_name, { ".app" })) {
        return "ApplicationBundle";
    }

    if (matches_suffix_any(lower_name, { ".msi" })) {
        return "Installer";
    }

    if (matches_suffix_any(lower_name, { ".apk", ".aab" })) {
        return "AndroidPackage";
    }

    if (matches_suffix_any(lower_name, { ".ipa" })) {
        return "IOSPackage";
    }

    if (matches_suffix_any(lower_name, {
            ".c", ".h", ".cpp", ".cxx", ".cc", ".hpp", ".hxx", ".hh", ".inl", ".ipp", ".tpp",
            ".ixx", ".fs", ".vb", ".java", ".kt", ".kts", ".py", ".pyw", ".pyi", ".js",
            ".jsx", ".ts", ".tsx", ".mjs", ".cjs", ".vue", ".svelte", ".astro", ".rs", ".go",
            ".lua", ".rb", ".php", ".swift", ".m", ".mm", ".dart", ".ex", ".exs", ".erl", ".hrl",
            ".hs", ".lhs", ".ml", ".mli", ".clj", ".cljs", ".cljc", ".r", ".pl", ".pm"
        })) {
        return "SourceCode";
    }

    if (matches_suffix_any(lower_name, {
            ".cmake", ".sconstruct", ".sconscript", ".make", ".mk", ".mak", ".gradle"
        })) {
        return "BuildScript";
    }

    if (matches_suffix_any(lower_name, { ".csproj", ".sln", ".fsproj", ".vbproj" })) {
        return "ProjectFile";
    }

    if (matches_suffix_any(lower_name, { ".targets", ".nuget", ".props" })) {
        return "BuildConfig";
    }

    if (matches_suffix_any(lower_name, { ".jar" })) {
        return "JavaArchive";
    }

    if (matches_suffix_any(lower_name, { ".aar" })) {
        return "AndroidArchive";
    }

    if (matches_suffix_any(lower_name, { ".class", ".dex", ".pyc", ".pyo" })) {
        return "CompiledCode";
    }

    if (matches_suffix_any(lower_name, { ".jks", ".keystore" })) {
        return "Keystore";
    }

    if (matches_suffix_any(lower_name, { ".ipynb" })) {
        return "Notebook";
    }

    if (matches_suffix_any(lower_name, { ".requirements" })) {
        return "DependencyList";
    }

    if (matches_suffix_any(lower_name, { ".html", ".htm", ".xhtml" })) {
        return "WebDocument";
    }

    if (matches_suffix_any(lower_name, { ".css", ".scss", ".sass", ".less", ".postcss" })) {
        return "Stylesheet";
    }

    if (matches_suffix_any(lower_name, { ".map" })) {
        return "SourceMap";
    }

    if (matches_suffix_any(lower_name, { ".wasm" })) {
        return "WebAssembly";
    }

    if (matches_suffix_any(lower_name, {
            ".clang-format", ".clang-tidy", ".clangd", ".ccls", ".gdbinit", ".lldbinit",
            ".eslintrc", ".eslintrc.json", ".eslintrc.js", ".prettierrc", ".prettierrc.json",
            ".prettierrc.yaml", ".prettierignore", ".stylelintrc", ".stylelintrc.json",
            ".markdownlint.json", ".markdownlint.yaml", ".markdownlint.yml", ".flake8", ".pylintrc",
            ".mypy.ini", ".pyre_configuration", ".ruff.toml", ".taplo.toml"
        })) {
        return "ToolConfig";
    }

    if (matches_suffix_any(lower_name, { ".po", ".pot", ".mo", ".xlf", ".xliff", ".resx", ".lang", ".locale" })) {
        return "Localization";
    }

    if (matches_suffix_any(lower_name, { ".pem", ".pfx", ".p12" })) {
        return "CertificateOrKey";
    }

    if (matches_suffix_any(lower_name, { ".crt", ".cer", ".der" })) {
        return "Certificate";
    }

    if (matches_suffix_any(lower_name, { ".key" })) {
        return "Key";
    }

    if (matches_suffix_any(lower_name, { ".pub" })) {
        return "PublicKey";
    }

    if (matches_suffix_any(lower_name, { ".asc" })) {
        return "SignatureOrText";
    }

    if (matches_suffix_any(lower_name, { ".sig" })) {
        return "Signature";
    }

    if (matches_suffix_any(lower_name, { ".gpg" })) {
        return "EncryptedOrKeyData";
    }

    if (matches_suffix_any(lower_name, { ".bak", ".backup", ".old", ".orig" })) {
        return "Backup";
    }

    if (matches_suffix_any(lower_name, {
            ".tmp", ".temp", ".swp", ".swo", ".part", ".crdownload", ".download"
        })) {
        return "Temporary";
    }

    if (matches_suffix_any(lower_name, { ".cache" })) {
        return "Cache";
    }

    if (matches_suffix_any(lower_name, { ".md5", ".sha1", ".sha256", ".sha512" })) {
        return "Hash";
    }

    if (matches_suffix_any(lower_name, { ".dmp", ".dump", ".core" })) {
        return "Dump";
    }

    if (matches_suffix_any(lower_name, { ".fseventsd", ".spotlight-v100", ".trashes" })) {
        return "OSMetadata";
    }

    if (matches_suffix_any(lower_name, {
            ".unity", ".prefab", ".asset", ".physicsmaterial2d", ".physicsmaterial", ".uasset", ".umap"
        })) {
        return "OtherEngineAsset";
    }

    if (matches_suffix_any(lower_name, { ".meta" })) {
        return "OtherEngineMetadata";
    }

    if (matches_suffix_any(lower_name, { ".uproject" })) {
        return "OtherEngineProject";
    }

    if (matches_suffix_any(lower_name, { ".anim", ".controller" })) {
        return "AnimationSource";
    }

    if (matches_suffix_any(lower_name, { ".drawio", ".dio", ".puml", ".plantuml", ".mmd", ".graphml", ".dot", ".gv" })) {
        return "Diagram";
    }

    if (matches_suffix_any(lower_name, { ".mind", ".xmind", ".mm" })) {
        return "MindMap";
    }

    if (matches_suffix_any(lower_name, { ".kanban" })) {
        return "Planning";
    }

    return "Unknown";
}

godot::String detect_godot_type_from_facts(const FileFacts &facts, const godot::String &file_type) {
    (void)file_type;

    if (facts.is_directory) {
        return NOT_GODOT_TYPED;
    }

    const godot::String &lower_name = facts.lower_name;
    const godot::String &lower_path = facts.lower_path;
    const godot::String &extension = facts.extension;

    if (lower_name == "project.godot") {
        return "ProjectSettings";
    }

    if (lower_name == "plugin.cfg") {
        return "ConfigFile";
    }

    if (is_config_file_extension(extension)) {
        return "ConfigFile";
    }

    if (extension == ".uid") {
        return "ResourceUID";
    }

    if (extension == ".import") {
        return "ConfigFile";
    }

    if (extension == ".ctex") {
        return "CompressedTexture2D";
    }

    if (extension == ".fontdata") {
        return "FontFile";
    }

    if (extension == ".gd") {
        return "GDScript";
    }

    if (extension == ".cs") {
        return "CSharpScript";
    }

    if (extension == ".gdextension") {
        return "GDExtension";
    }

    if (extension == ".remap") {
        return "ConfigFile";
    }

    if (is_godot_editor_path(lower_path)) {
        if (extension == ".node") {
            return "Node";
        }

        if (extension == ".object") {
            return "Object";
        }

        if (extension == ".resource") {
            return "Resource";
        }

        if (lower_path.begins_with("res://.godot/editor/filesystem_cache")) {
            return "EditorFileSystem";
        }

        if (lower_path.begins_with("res://.godot/editor/filesystem_update")) {
            return "EditorFileSystem";
        }

        return NOT_GODOT_TYPED;
    }

    if (extension == ".tscn" || extension == ".scn") {
        return "PackedScene";
    }

    if (extension == ".tres" || extension == ".res") {
        return "Resource";
    }

    if (extension == ".png" ||
        extension == ".apng" ||
        extension == ".jpg" ||
        extension == ".jpeg" ||
        extension == ".webp" ||
        extension == ".bmp" ||
        extension == ".gif" ||
        extension == ".svg" ||
        extension == ".exr" ||
        extension == ".hdr") {
        return "Texture2D";
    }

    if (extension == ".wav") {
        return "AudioStreamWAV";
    }

    if (extension == ".ogg" || extension == ".opus") {
        return "AudioStreamOggVorbis";
    }

    if (extension == ".mp3") {
        return "AudioStreamMP3";
    }

    if (extension == ".ttf" || extension == ".otf" || extension == ".fnt") {
        return "FontFile";
    }

    if (extension == ".shader" || extension == ".gdshader") {
        return "Shader";
    }

    if (extension == ".gltf" ||
        extension == ".glb" ||
        extension == ".fbx" ||
        extension == ".dae") {
        return "PackedScene";
    }

    return NOT_GODOT_TYPED;
}

} // namespace gotool::project_scanner

