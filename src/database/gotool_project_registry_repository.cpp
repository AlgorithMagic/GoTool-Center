// Copyright 2026 AlgorithMagic

#include "database/gotool_project_registry_repository.hpp"

#include <ctime>
#include <stdexcept>
#include <utility>

namespace gotool::database {

namespace {

int64_t current_unix_time() { return static_cast<int64_t>(std::time(nullptr)); }

struct ExistingProjectKey {
  int64_t project_id = 0;
  std::string project_uid;
};

std::optional<ExistingProjectKey>
find_project_by_uid(Database &database, const std::string &project_uid) {
  Statement statement = database.prepare(
      "SELECT id, project_uid FROM projects WHERE project_uid = ?1 LIMIT 1;");
  statement.bind_text(1, project_uid);

  if (statement.step() != Statement::StepResult::Row) {
    return std::nullopt;
  }

  ExistingProjectKey result;
  result.project_id = statement.column_int64(0);
  result.project_uid = statement.column_text(1);
  return result;
}

std::optional<ExistingProjectKey>
find_project_by_canonical_root(Database &database,
                               const std::string &root_canonical_path) {
  Statement statement =
      database.prepare("SELECT id, project_uid FROM projects WHERE "
                       "root_canonical_path = ?1 LIMIT 1;");
  statement.bind_text(1, root_canonical_path);

  if (statement.step() != Statement::StepResult::Row) {
    return std::nullopt;
  }

  ExistingProjectKey result;
  result.project_id = statement.column_int64(0);
  result.project_uid = statement.column_text(1);
  return result;
}

} // namespace

ProjectRegistryRepository::ProjectRegistryRepository(Database &database)
    : database_(&database) {
  if (database_ == nullptr) {
    throw std::runtime_error(
        "ProjectRegistryRepository requires a valid Database instance.");
  }
}

RegisteredProject ProjectRegistryRepository::register_project(
    const ProjectRegistrationInput &input) {
  if (input.project_uid.empty()) {
    throw std::runtime_error("Cannot register project: project_uid was empty.");
  }

  const std::string root_absolute_path =
      normalize_path_string(input.root_absolute_path);
  const std::string root_canonical_path =
      normalize_path_string(input.root_canonical_path);
  const std::string project_file_absolute_path =
      normalize_path_string(input.project_file_absolute_path);

  if (root_canonical_path.empty()) {
    throw std::runtime_error(
        "Cannot register project: canonical root path was empty.");
  }

  if (project_file_absolute_path.empty()) {
    throw std::runtime_error(
        "Cannot register project: project.godot absolute path was empty.");
  }

  const int64_t observed_at_unix =
      input.observed_at_unix > 0 ? input.observed_at_unix : current_unix_time();

  const std::optional<ExistingProjectKey> existing_by_uid =
      find_project_by_uid(*database_, input.project_uid);

  const std::optional<ExistingProjectKey> existing_by_canonical_root =
      find_project_by_canonical_root(*database_, root_canonical_path);

  if (existing_by_uid.has_value() && existing_by_canonical_root.has_value() &&
      existing_by_uid->project_id != existing_by_canonical_root->project_id) {
    throw std::runtime_error("Project identity conflict: project_uid '" +
                             input.project_uid +
                             "' maps to a different row than canonical root '" +
                             root_canonical_path + "'.");
  }

  if (!existing_by_uid.has_value() && existing_by_canonical_root.has_value()) {
    throw std::runtime_error("Project identity conflict: canonical root '" +
                             root_canonical_path +
                             "' is already associated with project_uid '" +
                             existing_by_canonical_root->project_uid +
                             "', not '" + input.project_uid + "'.");
  }

  if (existing_by_uid.has_value()) {
    Statement statement = database_->prepare(R"sql(
            UPDATE projects
            SET
                display_name = ?1,
                root_absolute_path = ?2,
                root_canonical_path = ?3,
                project_file_absolute_path = ?4,
                godot_version = ?5,
                identity_source = ?6,
                identity_warning = ?7,
                last_seen_unix = ?8,
                updated_at_unix = ?8
            WHERE id = ?9;
        )sql");
    statement.bind_text(1, input.display_name);
    statement.bind_text(2, root_absolute_path);
    statement.bind_text(3, root_canonical_path);
    statement.bind_text(4, project_file_absolute_path);
    statement.bind_text(5, input.godot_version);
    statement.bind_text(6, input.identity_source);
    statement.bind_text(7, input.identity_warning);
    statement.bind_int64(8, observed_at_unix);
    statement.bind_int64(9, existing_by_uid->project_id);
    statement.step_done();

    RegisteredProject result;
    result.project_id = existing_by_uid->project_id;
    result.identity_source = input.identity_source;
    result.identity_warning = input.identity_warning;
    return result;
  }

  Statement statement = database_->prepare(R"sql(
        INSERT INTO projects (
            project_uid,
            display_name,
            root_absolute_path,
            root_canonical_path,
            project_file_absolute_path,
            godot_version,
            identity_source,
            identity_warning,
            first_seen_unix,
            last_seen_unix,
            created_at_unix,
            updated_at_unix
        ) VALUES (
            ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?9, ?9, ?9
        );
    )sql");
  statement.bind_text(1, input.project_uid);
  statement.bind_text(2, input.display_name);
  statement.bind_text(3, root_absolute_path);
  statement.bind_text(4, root_canonical_path);
  statement.bind_text(5, project_file_absolute_path);
  statement.bind_text(6, input.godot_version);
  statement.bind_text(7, input.identity_source);
  statement.bind_text(8, input.identity_warning);
  statement.bind_int64(9, observed_at_unix);
  statement.step_done();

  RegisteredProject result;
  result.project_id = database_->last_insert_row_id();
  result.identity_source = input.identity_source;
  result.identity_warning = input.identity_warning;
  return result;
}

std::vector<ProjectListItem> ProjectRegistryRepository::list_projects() const {
  std::vector<ProjectListItem> projects;

  Statement statement = database_->prepare(R"sql(
        SELECT
            id,
            project_uid,
            display_name,
            root_absolute_path,
            root_canonical_path,
            project_file_absolute_path,
            COALESCE(godot_version, ''),
            identity_source,
            identity_warning,
            first_seen_unix,
            last_seen_unix,
            created_at_unix,
            updated_at_unix
        FROM projects
        ORDER BY last_seen_unix DESC, id DESC;
    )sql");

  while (statement.step() == Statement::StepResult::Row) {
    projects.push_back(read_project_row(statement));
  }

  return projects;
}

std::optional<ProjectSummary>
ProjectRegistryRepository::get_project_summary(int64_t project_id) const {
  Statement project_statement = database_->prepare(R"sql(
        SELECT
            id,
            project_uid,
            display_name,
            root_absolute_path,
            root_canonical_path,
            project_file_absolute_path,
            COALESCE(godot_version, ''),
            identity_source,
            identity_warning,
            first_seen_unix,
            last_seen_unix,
            created_at_unix,
            updated_at_unix
        FROM projects
        WHERE id = ?1
        LIMIT 1;
    )sql");
  project_statement.bind_int64(1, project_id);

  if (project_statement.step() != Statement::StepResult::Row) {
    return std::nullopt;
  }

  ProjectSummary summary;
  summary.project = read_project_row(project_statement);

  Statement scan_run_statement = database_->prepare(R"sql(
        SELECT id, status
        FROM project_scan_runs
        WHERE project_id = ?1
        ORDER BY started_at_unix DESC, id DESC
        LIMIT 1;
    )sql");
  scan_run_statement.bind_int64(1, project_id);

  if (scan_run_statement.step() == Statement::StepResult::Row) {
    summary.latest_scan_run_id = scan_run_statement.column_int64(0);
    summary.latest_scan_status = scan_run_statement.column_text(1);
  }

  Statement count_files_statement = database_->prepare(
      "SELECT COUNT(*) FROM project_files WHERE project_id = ?1;");
  count_files_statement.bind_int64(1, project_id);
  if (count_files_statement.step() == Statement::StepResult::Row) {
    summary.files_count = count_files_statement.column_int64(0);
  }

  Statement count_autoloads_statement = database_->prepare(
      "SELECT COUNT(*) FROM project_autoloads WHERE project_id = ?1;");
  count_autoloads_statement.bind_int64(1, project_id);
  if (count_autoloads_statement.step() == Statement::StepResult::Row) {
    summary.autoloads_count = count_autoloads_statement.column_int64(0);
  }

  Statement count_custom_classes_statement = database_->prepare(
      "SELECT COUNT(*) FROM project_custom_classes WHERE project_id = ?1;");
  count_custom_classes_statement.bind_int64(1, project_id);
  if (count_custom_classes_statement.step() == Statement::StepResult::Row) {
    summary.custom_classes_count =
        count_custom_classes_statement.column_int64(0);
  }

  Statement count_unknowns_statement = database_->prepare(
      "SELECT COUNT(*) FROM project_scan_unknowns WHERE project_id = ?1;");
  count_unknowns_statement.bind_int64(1, project_id);
  if (count_unknowns_statement.step() == Statement::StepResult::Row) {
    summary.unknowns_count = count_unknowns_statement.column_int64(0);
  }

  return summary;
}

std::string ProjectRegistryRepository::normalize_path_string(
    const std::filesystem::path &path) {
  std::filesystem::path normalized = path.lexically_normal();
  return normalized.make_preferred().string();
}

ProjectListItem
ProjectRegistryRepository::read_project_row(Statement &statement) const {
  ProjectListItem item;
  item.project_id = statement.column_int64(0);
  item.project_uid = statement.column_text(1);
  item.display_name = statement.column_text(2);
  item.root_absolute_path = statement.column_text(3);
  item.root_canonical_path = statement.column_text(4);
  item.project_file_absolute_path = statement.column_text(5);
  item.godot_version = statement.column_text(6);
  item.identity_source = statement.column_text(7);
  item.identity_warning = statement.column_text(8);
  item.first_seen_unix = statement.column_int64(9);
  item.last_seen_unix = statement.column_int64(10);
  item.created_at_unix = statement.column_int64(11);
  item.updated_at_unix = statement.column_int64(12);
  return item;
}

} // namespace gotool::database
