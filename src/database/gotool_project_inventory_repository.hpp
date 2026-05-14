#pragma once

#include "database/gotool_database.hpp"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>

namespace gotool::database {

struct PersistedInventorySummary {
  int64_t scan_run_id = 0;
  int64_t files_found = 0;
  int64_t folders_found = 0;
  int64_t unknown_entries = 0;
  int64_t sqlite_prepare_count = 0;
  int64_t sqlite_step_count = 0;
};

class ProjectInventoryRepository {
public:
  explicit ProjectInventoryRepository(Database &database);

  PersistedInventorySummary
  persist_inventory(int64_t project_id, const godot::Dictionary &inventory);

private:
  static int64_t current_unix_time();
  static std::string to_utf8(const godot::String &value);
  static bool variant_to_bool(const godot::Variant &value);
  static int64_t variant_to_int64(const godot::Variant &value);
  static godot::String variant_to_string(const godot::Variant &value);

  int64_t create_scan_run(int64_t project_id, int64_t started_at_unix);
  void complete_scan_run(int64_t project_id, int64_t scan_run_id,
                         int64_t finished_at_unix, int64_t files_found,
                         int64_t folders_found);

  void upsert_project_file(Statement &statement, int64_t project_id,
                           const godot::Dictionary &entry, int64_t scan_run_id,
                           int64_t observed_at_unix);
  void delete_missing_project_files(Statement &statement, int64_t project_id,
                                    int64_t scan_run_id);

  void upsert_autoload(Statement &statement, int64_t project_id,
                       const godot::Dictionary &entry, int64_t scan_run_id,
                       int64_t observed_at_unix);
  void delete_missing_autoloads(Statement &statement, int64_t project_id,
                                int64_t scan_run_id);

  void upsert_custom_class(Statement &statement, int64_t project_id,
                           const godot::Dictionary &entry, int64_t scan_run_id,
                           int64_t observed_at_unix);
  void delete_missing_custom_classes(Statement &statement, int64_t project_id,
                                     int64_t scan_run_id);

  void upsert_unknown(Statement &statement, int64_t project_id,
                      const godot::Dictionary &entry, int64_t scan_run_id,
                      int64_t observed_at_unix);
  void delete_missing_unknowns(Statement &statement, int64_t project_id,
                               int64_t scan_run_id);

  Database *database_ = nullptr;
};

} // namespace gotool::database
