#include "gotool_project_context.hpp"

#include "database/gotool_project_inventory_repository.hpp"
#include "database/gotool_project_registry_repository.hpp"
#include "database/gotool_schema.hpp"
#include "project_scanner/gotool_project_scanner.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace godot {

namespace {

std::string to_utf8(const String &value) {
    const CharString utf8_value = value.utf8();
    return utf8_value.get_data() != nullptr ? utf8_value.get_data() : "";
}

String from_utf8(const std::string &value) {
    return String::utf8(value.c_str());
}

std::string to_utf8_from_u8(const std::u8string &value) {
    return std::string(reinterpret_cast<const char *>(value.c_str()), value.size());
}

std::filesystem::path godot_string_to_path(const String &value) {
    return std::filesystem::u8path(to_utf8(value));
}

String path_to_godot_string(const std::filesystem::path &value) {
    return from_utf8(to_utf8_from_u8(value.generic_u8string()));
}

std::filesystem::path canonicalize_existing_path(
    const std::filesystem::path &path,
    const std::string &label
) {
    std::error_code absolute_error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(path, absolute_error);

    if (absolute_error) {
        throw std::runtime_error(
            "Failed to build absolute path for " + label + ": " + absolute_error.message()
        );
    }

    std::error_code canonical_error;
    const std::filesystem::path canonical_path =
        std::filesystem::weakly_canonical(absolute_path, canonical_error);

    if (canonical_error) {
        throw std::runtime_error(
            "Failed to canonicalize " + label + ": " + canonical_error.message()
        );
    }

    return canonical_path.lexically_normal();
}

std::filesystem::path normalize_absolute_path(const std::filesystem::path &path) {
    std::error_code absolute_error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(path, absolute_error);

    if (absolute_error) {
        throw std::runtime_error(
            "Failed to build absolute path: " + absolute_error.message()
        );
    }

    return absolute_path.lexically_normal();
}

std::string read_text_file_first_line(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::in | std::ios::binary);

    if (!input.is_open()) {
        return "";
    }

    std::string line;
    std::getline(input, line);

    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }

    size_t non_space_index = 0;
    while (non_space_index < line.size() && (line[non_space_index] == ' ' || line[non_space_index] == '\t')) {
        ++non_space_index;
    }

    return line.substr(non_space_index);
}

std::string make_uid_from_seed(const std::string &seed_prefix, const std::string &seed) {
    const String seed_string = String::utf8(seed.c_str());
    const String hash = seed_string.sha256_text();
    return seed_prefix + to_utf8(hash.substr(0, 40));
}

std::string make_path_derived_uid(
    const std::filesystem::path &canonical_root,
    const std::filesystem::path &project_file_path
) {
    const std::string seed =
        to_utf8_from_u8(canonical_root.generic_u8string()) + "|" +
        to_utf8_from_u8(project_file_path.generic_u8string());

    return make_uid_from_seed("path-derived-", seed);
}

std::string make_generated_uid(
    const std::filesystem::path &canonical_root,
    const std::filesystem::path &project_file_path
) {
    const std::string seed =
        to_utf8_from_u8(canonical_root.generic_u8string()) + "|" +
        to_utf8_from_u8(project_file_path.generic_u8string()) + "|" +
        std::to_string(static_cast<int64_t>(std::time(nullptr)));

    return make_uid_from_seed("gotool-", seed);
}

String build_godot_version_string() {
    Engine *engine = Engine::get_singleton();

    if (engine == nullptr) {
        return "";
    }

    const Dictionary version_info = engine->get_version_info();

    const int64_t major = static_cast<int64_t>(version_info.get("major", 0));
    const int64_t minor = static_cast<int64_t>(version_info.get("minor", 0));
    const int64_t patch = static_cast<int64_t>(version_info.get("patch", 0));
    const String status = version_info.get("status", "");

    String version_text =
        String::num_int64(major) + "." +
        String::num_int64(minor) + "." +
        String::num_int64(patch);

    if (!status.is_empty()) {
        version_text += "-" + status;
    }

    return version_text;
}

Dictionary to_project_dictionary(const gotool::database::ProjectListItem &item) {
    Dictionary project;
    project["project_id"] = item.project_id;
    project["project_uid"] = from_utf8(item.project_uid);
    project["display_name"] = from_utf8(item.display_name);
    project["root_absolute_path"] = from_utf8(item.root_absolute_path);
    project["root_canonical_path"] = from_utf8(item.root_canonical_path);
    project["project_file_absolute_path"] = from_utf8(item.project_file_absolute_path);
    project["godot_version"] = from_utf8(item.godot_version);
    project["identity_source"] = from_utf8(item.identity_source);
    project["identity_warning"] = from_utf8(item.identity_warning);
    project["is_path_derived_identity"] = item.identity_source == "path_derived";
    project["first_seen_unix"] = item.first_seen_unix;
    project["last_seen_unix"] = item.last_seen_unix;
    project["created_at_unix"] = item.created_at_unix;
    project["updated_at_unix"] = item.updated_at_unix;
    return project;
}

} // namespace

void GodotProjectContext::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("initialize_database"),
        &GodotProjectContext::initialize_database
    );

    ClassDB::bind_method(
        D_METHOD("register_current_project"),
        &GodotProjectContext::register_current_project
    );

    ClassDB::bind_method(
        D_METHOD("get_database_virtual_path"),
        &GodotProjectContext::get_database_virtual_path
    );

    ClassDB::bind_method(
        D_METHOD("get_database_absolute_path"),
        &GodotProjectContext::get_database_absolute_path
    );

    ClassDB::bind_method(
        D_METHOD("scan_current_project"),
        &GodotProjectContext::scan_current_project
    );

    ClassDB::bind_method(
        D_METHOD("scan_project"),
        &GodotProjectContext::scan_project
    );

    ClassDB::bind_method(
        D_METHOD("list_projects"),
        &GodotProjectContext::list_projects
    );

    ClassDB::bind_method(
        D_METHOD("get_project_summary", "project_id"),
        &GodotProjectContext::get_project_summary
    );

    ClassDB::bind_method(
        D_METHOD("get_last_scan_results"),
        &GodotProjectContext::get_last_scan_results
    );

    ClassDB::bind_method(
        D_METHOD("get_last_error"),
        &GodotProjectContext::get_last_error
    );
}

bool GodotProjectContext::initialize_database() {
    last_error_ = "";
    current_project_id_ = 0;
    current_identity_source_ = "";
    current_identity_warning_ = "";

    OS *os = OS::get_singleton();

    if (os == nullptr) {
        last_error_ = "OS singleton was not available.";
        database_.reset();
        return false;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();

    if (project_settings == nullptr) {
        last_error_ = "ProjectSettings singleton was not available.";
        database_.reset();
        return false;
    }

    try {
        std::filesystem::path os_data_root = godot_string_to_path(os->get_data_dir());

        if (os_data_root.empty()) {
            os_data_root = godot_string_to_path(os->get_user_data_dir());
        }

        if (os_data_root.empty()) {
            throw std::runtime_error("OS data directory was empty.");
        }

        const std::filesystem::path storage_root =
            normalize_absolute_path(os_data_root / "GoToolCenter");

        std::error_code mkdir_error;
        std::filesystem::create_directories(storage_root, mkdir_error);

        if (mkdir_error) {
            throw std::runtime_error(
                "Failed to create GoTool storage directory '" +
                to_utf8_from_u8(storage_root.generic_u8string()) +
                "': " + mkdir_error.message()
            );
        }

        const std::filesystem::path database_path = storage_root / "gotool_center.sqlite3";
        database_ = std::make_unique<gotool::database::Database>(
            to_utf8_from_u8(database_path.generic_u8string())
        );

        // First pass creates/ensures the v2 schema and projects table. Legacy migration
        // is deferred until register_current_project() supplies a concrete project_id.
        gotool::database::create_schema(*database_, 0);

        const int64_t project_id = register_current_project();

        if (project_id <= 0) {
            database_.reset();
            return false;
        }

        database_absolute_path_ = path_to_godot_string(database_path.lexically_normal());

        const String localized_database_path =
            project_settings->localize_path(database_absolute_path_);

        if (localized_database_path.begins_with("res://") ||
            localized_database_path.begins_with("user://")) {
            database_virtual_path_ = localized_database_path;
        } else {
            database_virtual_path_ = database_absolute_path_;
        }

        last_scan_results_.clear();
        last_error_ = "";
        return true;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        database_.reset();
        return false;
    }
}

int64_t GodotProjectContext::register_current_project() {
    last_error_ = "";

    if (database_ == nullptr) {
        last_error_ = "Database is not initialized. Call initialize_database() first.";
        current_project_id_ = 0;
        return 0;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();

    if (project_settings == nullptr) {
        last_error_ = "ProjectSettings singleton was not available.";
        current_project_id_ = 0;
        return 0;
    }

    try {
        const std::filesystem::path project_root_absolute =
            normalize_absolute_path(godot_string_to_path(project_settings->globalize_path("res://")));

        const std::filesystem::path project_file_absolute =
            normalize_absolute_path(godot_string_to_path(project_settings->globalize_path("res://project.godot")));

        if (!std::filesystem::exists(project_file_absolute)) {
            throw std::runtime_error(
                "Project file was not found at '" +
                to_utf8_from_u8(project_file_absolute.generic_u8string()) +
                "'."
            );
        }

        const std::filesystem::path project_root_canonical =
            canonicalize_existing_path(project_root_absolute, "project root");

        const std::filesystem::path project_file_canonical =
            canonicalize_existing_path(project_file_absolute, "project.godot file");

        const std::filesystem::path uid_file_path =
            project_root_canonical / ".godot" / "gotool_center" / "project.uid";

        std::string project_uid = read_text_file_first_line(uid_file_path);
        std::string identity_source = "uid_file";
        std::string identity_warning;

        if (project_uid.empty()) {
            std::error_code mkdir_error;
            std::filesystem::create_directories(uid_file_path.parent_path(), mkdir_error);

            if (!mkdir_error) {
                const std::string generated_uid =
                    make_generated_uid(project_root_canonical, project_file_canonical);

                std::ofstream uid_output(uid_file_path, std::ios::out | std::ios::trunc);

                if (uid_output.is_open()) {
                    uid_output << generated_uid << '\n';
                    uid_output.flush();

                    if (uid_output.good()) {
                        project_uid = generated_uid;
                    } else {
                        identity_source = "path_derived";
                        identity_warning =
                            "Could not persist project UID to '" +
                            to_utf8_from_u8(uid_file_path.generic_u8string()) +
                            "'. Falling back to path-derived identity.";
                    }
                } else {
                    identity_source = "path_derived";
                    identity_warning =
                        "Could not open project UID file for writing at '" +
                        to_utf8_from_u8(uid_file_path.generic_u8string()) +
                        "'. Falling back to path-derived identity.";
                }
            } else {
                identity_source = "path_derived";
                identity_warning =
                    "Could not create UID directory '" +
                    to_utf8_from_u8(uid_file_path.parent_path().generic_u8string()) +
                    "': " + mkdir_error.message() +
                    ". Falling back to path-derived identity.";
            }
        }

        if (project_uid.empty()) {
            identity_source = "path_derived";
            if (identity_warning.empty()) {
                identity_warning = "Falling back to path-derived identity because project UID was unavailable.";
            }
            project_uid = make_path_derived_uid(project_root_canonical, project_file_canonical);
        }

        String display_name = project_settings->get_setting("application/config/name", "");

        if (display_name.is_empty()) {
            display_name = path_to_godot_string(project_root_canonical.filename());
        }

        gotool::database::ProjectRegistrationInput registration_input;
        registration_input.project_uid = project_uid;
        registration_input.display_name = to_utf8(display_name);
        registration_input.root_absolute_path = project_root_absolute;
        registration_input.root_canonical_path = project_root_canonical;
        registration_input.project_file_absolute_path = project_file_canonical;
        registration_input.godot_version = to_utf8(build_godot_version_string());
        registration_input.identity_source = identity_source;
        registration_input.identity_warning = identity_warning;
        registration_input.observed_at_unix = static_cast<int64_t>(std::time(nullptr));

        gotool::database::ProjectRegistryRepository registry(*database_);
        const gotool::database::RegisteredProject registered_project =
            registry.register_project(registration_input);

        gotool::database::create_schema(*database_, registered_project.project_id);

        current_project_id_ = registered_project.project_id;
        current_identity_source_ = from_utf8(registered_project.identity_source);
        current_identity_warning_ = from_utf8(registered_project.identity_warning);
        return current_project_id_;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        current_project_id_ = 0;
        current_identity_source_ = "";
        current_identity_warning_ = "";
        return 0;
    }
}

bool GodotProjectContext::scan_current_project() {
    last_error_ = "";

    if (database_ == nullptr) {
        last_error_ = "Database is not initialized. Call initialize_database() first.";
        last_scan_results_.clear();
        return false;
    }

    const int64_t project_id = register_current_project();

    if (project_id <= 0) {
        last_scan_results_.clear();
        return false;
    }

    Ref<ProjectScanner> scanner;
    scanner.instantiate();

    if (scanner.is_null()) {
        last_error_ = "Failed to instantiate ProjectScanner.";
        last_scan_results_.clear();
        return false;
    }

    try {
        const Dictionary inventory = scanner->scan_project_inventory();

        if (!inventory.has("files") ||
            !inventory.has("autoloads") ||
            !inventory.has("custom_classes")) {
            last_error_ = "ProjectScanner returned an invalid inventory payload.";
            last_scan_results_.clear();
            return false;
        }

        const Variant files_variant = inventory.get("files", Variant());
        const Variant autoloads_variant = inventory.get("autoloads", Variant());
        const Variant custom_classes_variant = inventory.get("custom_classes", Variant());

        if (files_variant.get_type() != Variant::ARRAY ||
            autoloads_variant.get_type() != Variant::ARRAY ||
            custom_classes_variant.get_type() != Variant::ARRAY) {
            last_error_ = "ProjectScanner inventory payload must contain arrays.";
            last_scan_results_.clear();
            return false;
        }

        gotool::database::ProjectInventoryRepository repository(*database_);
        repository.persist_inventory(project_id, inventory);

        last_scan_results_ = inventory;
        last_error_ = "";
        return true;
    } catch (const std::exception &error) {
        last_error_ = error.what();
        last_scan_results_.clear();
        return false;
    }
}

bool GodotProjectContext::scan_project() {
    return scan_current_project();
}

Array GodotProjectContext::list_projects() const {
    Array projects;

    if (database_ == nullptr) {
        return projects;
    }

    try {
        gotool::database::ProjectRegistryRepository registry(*database_);
        const std::vector<gotool::database::ProjectListItem> items = registry.list_projects();

        for (const gotool::database::ProjectListItem &item : items) {
            projects.append(to_project_dictionary(item));
        }
    } catch (...) {
        return projects;
    }

    return projects;
}

Dictionary GodotProjectContext::get_project_summary(int64_t project_id) const {
    Dictionary summary;

    if (database_ == nullptr || project_id <= 0) {
        return summary;
    }

    try {
        gotool::database::ProjectRegistryRepository registry(*database_);
        const std::optional<gotool::database::ProjectSummary> project_summary =
            registry.get_project_summary(project_id);

        if (!project_summary.has_value()) {
            return summary;
        }

        summary["project"] = to_project_dictionary(project_summary->project);
        summary["latest_scan_run_id"] = project_summary->latest_scan_run_id;
        summary["latest_scan_status"] = from_utf8(project_summary->latest_scan_status);
        summary["files_count"] = project_summary->files_count;
        summary["autoloads_count"] = project_summary->autoloads_count;
        summary["custom_classes_count"] = project_summary->custom_classes_count;
        summary["unknowns_count"] = project_summary->unknowns_count;
    } catch (...) {
        return Dictionary();
    }

    return summary;
}

String GodotProjectContext::get_database_virtual_path() const {
    return database_virtual_path_;
}

String GodotProjectContext::get_database_absolute_path() const {
    return database_absolute_path_;
}

Dictionary GodotProjectContext::get_last_scan_results() const {
    return last_scan_results_;
}

String GodotProjectContext::get_last_error() const {
    return last_error_;
}

} // namespace godot
