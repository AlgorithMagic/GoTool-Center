#include "project_scanner/gotool_project_scanner.hpp"

#include "project_scanner/gotool_project_scanner_rules.hpp"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/scene_state.hpp>

namespace godot {

using gotool::project_scanner::FileFacts;

void ProjectScanner::_bind_methods() {
    ClassDB::bind_method(D_METHOD("scan_res"), &ProjectScanner::scan_res);
    ClassDB::bind_method(D_METHOD("scan_autoloads"), &ProjectScanner::scan_autoloads);
    ClassDB::bind_method(
        D_METHOD("scan_custom_script_classes", "file_entries"),
        &ProjectScanner::scan_custom_script_classes
    );
    ClassDB::bind_method(D_METHOD("scan_project_inventory"), &ProjectScanner::scan_project_inventory);
}

Array ProjectScanner::scan_res() {
    Array entries;
    scan_directory("res://", entries);
    return entries;
}

Dictionary ProjectScanner::scan_project_inventory() {
    const Array files = scan_res();

    Dictionary inventory;
    inventory["files"] = files;
    inventory["autoloads"] = scan_autoloads();
    inventory["custom_classes"] = scan_custom_script_classes(files);

    return inventory;
}

Array ProjectScanner::scan_autoloads() const {
    Array autoloads;

    ProjectSettings *project_settings = ProjectSettings::get_singleton();

    if (project_settings == nullptr) {
        return autoloads;
    }

    const Array properties = project_settings->get_property_list();

    for (int64_t i = 0; i < properties.size(); ++i) {
        const Dictionary property = properties[i];
        const String setting_name = property.get("name", "");

        if (!setting_name.begins_with("autoload/")) {
            continue;
        }

        const String autoload_name = setting_name.substr(String("autoload/").length());
        String target_path = String(project_settings->get_setting(setting_name));

        const bool is_singleton = target_path.begins_with("*");

        if (is_singleton) {
            target_path = target_path.substr(1);
        }

        Dictionary entry;
        entry["autoload_name"] = autoload_name;
        entry["target_path"] = target_path;
        entry["target_project_relative_path"] = gotool::project_scanner::to_project_relative_path(target_path);
        entry["is_singleton"] = is_singleton;

        autoloads.append(entry);
    }

    return autoloads;
}

Array ProjectScanner::scan_custom_script_classes(const Array &file_entries) const {
    Array custom_classes;

    for (int64_t i = 0; i < file_entries.size(); ++i) {
        const Dictionary file_entry = file_entries[i];

        if (bool(file_entry.get("is_directory", false))) {
            continue;
        }

        const String path = file_entry.get("path", "");
        const String extension = file_entry.get("extension", "");

        if (!gotool::project_scanner::is_script_extension(extension)) {
            continue;
        }

        if (path.is_empty() || path == "res://" || path == "res:///") {
            continue;
        }

        const Dictionary parsed = gotool::project_scanner::parse_custom_script_class(path, extension);

        if (parsed.is_empty()) {
            continue;
        }

        const String class_name = parsed.get("class_name", "");
        const String base_type = parsed.get("base_type", "");

        if (class_name.is_empty()) {
            continue;
        }

        Dictionary entry;
        entry["class_name"] = class_name;
        entry["script_path"] = path;
        entry["script_project_relative_path"] = gotool::project_scanner::to_project_relative_path(path);
        entry["language"] = gotool::project_scanner::language_from_script_extension(extension);
        entry["base_type"] = base_type;
        entry["is_resource_type"] = gotool::project_scanner::is_class_or_parent_class(base_type, "Resource");
        entry["is_node_type"] = gotool::project_scanner::is_class_or_parent_class(base_type, "Node");

        custom_classes.append(entry);
    }

    return custom_classes;
}

Dictionary ProjectScanner::make_entry(const String &full_path, const String &name, bool is_dir) const {
    Dictionary entry;
    const FileFacts facts = gotool::project_scanner::build_file_facts(full_path, name, is_dir);

    const ProjectSettings *project_settings = ProjectSettings::get_singleton();

    const String absolute_path =
        project_settings != nullptr
            ? project_settings->globalize_path(full_path)
            : full_path;

    const int64_t size_bytes =
        is_dir
            ? 0
            : FileAccess::get_size(absolute_path);

    const int64_t modified_time_unix =
        is_dir
            ? 0
            : static_cast<int64_t>(FileAccess::get_modified_time(absolute_path));

    const bool is_hidden =
        name.begins_with(".") ||
        FileAccess::get_hidden_attribute(absolute_path);

    const String file_type = classify_file_type(full_path, name, is_dir);
    String godot_type = detect_godot_type(full_path, name, is_dir, file_type, facts.extension);

    if (godot_type.is_empty()) {
        godot_type = gotool::project_scanner::NOT_GODOT_TYPED;
    }

    entry["path"] = full_path;
    entry["name"] = name;
    entry["type"] = is_dir ? "folder" : "file";
    entry["project_relative_path"] = gotool::project_scanner::to_project_relative_path(full_path);
    entry["absolute_path"] = absolute_path;
    entry["file_name"] = name;
    entry["extension"] = facts.extension;
    entry["file_type"] = file_type;
    entry["godot_type"] = godot_type;
    entry["size_bytes"] = size_bytes;
    entry["modified_time_unix"] = modified_time_unix;
    entry["is_directory"] = is_dir;
    entry["is_hidden"] = is_hidden;

    return entry;
}

String ProjectScanner::classify_file_type(
    const String &full_path,
    const String &name,
    bool is_dir
) const {
    const FileFacts facts = gotool::project_scanner::build_file_facts(full_path, name, is_dir);
    return gotool::project_scanner::classify_file_type_from_facts(facts);
}

String ProjectScanner::detect_godot_type(
    const String &full_path,
    const String &name,
    bool is_dir,
    const String &file_type,
    const String &extension
) const {
    FileFacts facts = gotool::project_scanner::build_file_facts(full_path, name, is_dir);

    if (!extension.is_empty()) {
        facts.extension = extension;
    }

    const String engine_type = detect_godot_type_from_engine(facts, file_type);

    if (!engine_type.is_empty()) {
        return engine_type;
    }

    return gotool::project_scanner::detect_godot_type_from_facts(facts, file_type);
}

String ProjectScanner::detect_godot_type_from_engine(
    const FileFacts &facts,
    const String &file_type
) const {
    if (facts.is_directory || facts.full_path.is_empty()) {
        return "";
    }

    String editor_file_type;
    const Engine *engine = Engine::get_singleton();
    const bool is_editor_context = engine != nullptr && engine->is_editor_hint();
    const EditorInterface *editor_interface =
        is_editor_context ? EditorInterface::get_singleton() : nullptr;

    if (editor_interface != nullptr) {
        const EditorFileSystem *resource_filesystem = editor_interface->get_resource_filesystem();

        if (resource_filesystem != nullptr) {
            editor_file_type = resource_filesystem->get_file_type(facts.full_path).strip_edges();
        }
    }

    if (is_loadable_godot_resource_candidate(facts, file_type, editor_file_type)) {
        const String loaded_type = detect_loaded_resource_type(facts.full_path, editor_file_type);

        if (!loaded_type.is_empty()) {
            return loaded_type;
        }
    }

    return editor_file_type;
}

String ProjectScanner::detect_loaded_resource_type(
    const String &path,
    const String &editor_file_type
) const {
    const Engine *engine = Engine::get_singleton();

    if (engine != nullptr) {
        MainLoop *main_loop = engine->get_main_loop();
        SceneTree *scene_tree = Object::cast_to<SceneTree>(main_loop);

        if (scene_tree != nullptr) {
            Node *current_scene = scene_tree->get_current_scene();

            if (current_scene != nullptr) {
                const String current_scene_path = current_scene->get_scene_file_path();

                if (!current_scene_path.is_empty() && current_scene_path == path) {
                    return "";
                }
            }
        }
    }

    ResourceLoader *resource_loader = ResourceLoader::get_singleton();

    if (resource_loader == nullptr || path.is_empty()) {
        return "";
    }

    const String type_hint = editor_file_type.strip_edges();
    const bool resource_exists =
        type_hint.is_empty()
            ? resource_loader->exists(path)
            : resource_loader->exists(path, type_hint) || resource_loader->exists(path);

    if (!resource_exists) {
        return "";
    }

    Ref<Resource> resource = resource_loader->load(
        path,
        type_hint,
        ResourceLoader::CACHE_MODE_IGNORE_DEEP
    );
    String detected_type;

    if (resource.is_valid()) {
        Ref<PackedScene> scene = resource;

        if (scene.is_valid()) {
            Ref<SceneState> scene_state = scene->get_state();

            if (scene_state.is_valid() && scene_state->get_node_count() > 0) {
                detected_type = String(scene_state->get_node_type(0)).strip_edges();
            }
        }

        if (detected_type.is_empty()) {
            detected_type = resource->get_class().strip_edges();
        }
    }

    return detected_type;
}

bool ProjectScanner::is_loadable_godot_resource_candidate(
    const FileFacts &facts,
    const String &file_type,
    const String &editor_file_type
) const {
    if (facts.is_directory || facts.full_path.is_empty()) {
        return false;
    }

    if (facts.extension == ".tscn" ||
        facts.extension == ".scn" ||
        facts.extension == ".tres" ||
        facts.extension == ".res" ||
        facts.extension == ".meshlib") {
        return true;
    }

    return file_type == "GodotScene" ||
           (file_type == "GodotResource" && !editor_file_type.is_empty());
}

void ProjectScanner::scan_directory(const String &path, Array &out_entries) {
    Ref<DirAccess> dir = DirAccess::open(path);

    if (dir.is_null()) {
        return;
    }

    dir->set_include_hidden(true);
    dir->set_include_navigational(false);

    dir->list_dir_begin();

    String name = dir->get_next();

    while (!name.is_empty()) {
        const bool is_dir = dir->current_is_dir();
        const String full_path = path.path_join(name);

        if (gotool::project_scanner::should_skip_scan_path(full_path)) {
            name = dir->get_next();
            continue;
        }

        out_entries.append(make_entry(full_path, name, is_dir));

        if (is_dir) {
            scan_directory(full_path, out_entries);
        }

        name = dir->get_next();
    }

    dir->list_dir_end();
}

} // namespace godot
