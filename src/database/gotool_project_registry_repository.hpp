#pragma once

#include "database/gotool_database.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gotool::database {

struct ProjectRegistrationInput {
  std::string project_uid;
  std::string display_name;
  std::filesystem::path root_absolute_path;
  std::filesystem::path root_canonical_path;
  std::filesystem::path project_file_absolute_path;
  std::string godot_version;
  std::string identity_source;
  std::string identity_warning;
  int64_t observed_at_unix = 0;
};

struct RegisteredProject {
  int64_t project_id = 0;
  std::string identity_source;
  std::string identity_warning;
};

struct ProjectListItem {
  int64_t project_id = 0;
  std::string project_uid;
  std::string display_name;
  std::string root_absolute_path;
  std::string root_canonical_path;
  std::string project_file_absolute_path;
  std::string godot_version;
  std::string identity_source;
  std::string identity_warning;
  int64_t first_seen_unix = 0;
  int64_t last_seen_unix = 0;
  int64_t created_at_unix = 0;
  int64_t updated_at_unix = 0;
};

struct ProjectSummary {
  ProjectListItem project;
  int64_t latest_scan_run_id = 0;
  std::string latest_scan_status;
  int64_t files_count = 0;
  int64_t autoloads_count = 0;
  int64_t custom_classes_count = 0;
  int64_t unknowns_count = 0;
};

class ProjectRegistryRepository {
public:
  explicit ProjectRegistryRepository(Database &database);

  RegisteredProject register_project(const ProjectRegistrationInput &input);
  std::vector<ProjectListItem> list_projects() const;
  std::optional<ProjectSummary> get_project_summary(int64_t project_id) const;

private:
  static std::string normalize_path_string(const std::filesystem::path &path);

  ProjectListItem read_project_row(Statement &statement) const;

  Database *database_ = nullptr;
};

} // namespace gotool::database
