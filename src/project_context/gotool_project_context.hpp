#pragma once

#include "database/gotool_database.hpp"
#include "project_scanner/file_watcher.hpp"
#include "project_scanner/native_scan_rules.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace godot {

class GodotProjectContext : public RefCounted {
  GDCLASS(GodotProjectContext, RefCounted)

protected:
  static void _bind_methods();

public:
  ~GodotProjectContext() override;

  bool initialize_database();
  int64_t register_current_project();

  String get_database_virtual_path() const;
  String get_database_absolute_path() const;

  bool scan_current_project();
  bool scan_project();

  Dictionary start_scan(const Dictionary &options);
  bool cancel_scan(int64_t scan_id);
  Dictionary get_scan_status(int64_t scan_id) const;
  Dictionary get_scan_metrics(int64_t scan_id) const;

  Dictionary scan_project_inventory_fast(const Dictionary &options);
  Dictionary scan_current_project_fast(const Dictionary &options);
  Dictionary benchmark_native_scan(const Dictionary &options);

  bool start_watcher();
  void stop_watcher();
  Dictionary get_watcher_status() const;
  Array consume_watcher_changes();
  Array get_dirty_paths();

  int64_t get_file_count(const Dictionary &filter) const;
  Array get_files_page(int64_t offset, int64_t limit, const String &sort,
                       const Dictionary &filter) const;
  Dictionary get_file_details(int64_t file_id) const;
  Array get_directory_children(int64_t directory_id, int64_t offset,
                               int64_t limit, const String &sort,
                               const Dictionary &filter) const;
  int64_t get_custom_class_count(const Dictionary &filter) const;
  Array get_custom_classes_page(int64_t offset, int64_t limit,
                                const String &sort,
                                const Dictionary &filter) const;
  Array list_dependencies_for_script(int64_t script_file_id) const;
  Array list_dependents_of_file(int64_t target_file_id) const;
  Array list_dependents_of_class(const String &class_name) const;
  Array list_unresolved_dependencies() const;
  Array list_dynamic_dependencies() const;
  Array list_symbols_for_script(int64_t script_file_id,
                                const Dictionary &filter) const;
  Array list_functions_for_script(int64_t script_file_id) const;
  Array list_properties_for_script(int64_t script_file_id) const;
  Array list_parameters_for_function(int64_t function_symbol_id) const;
  Array list_doc_comment_gaps(const Dictionary &filter) const;
  Array list_references_for_script(int64_t script_file_id) const;
  Array list_references_from_symbol(int64_t symbol_id) const;
  Array list_unresolved_references(const Dictionary &filter) const;
  Array list_dynamic_references(const Dictionary &filter) const;
  Array list_scene_script_attachments(const Dictionary &filter) const;
  Array list_scenes_using_script(int64_t script_file_id) const;
  Array list_scripts_attached_to_scene(int64_t scene_file_id) const;
  Dictionary get_symbol_details(int64_t symbol_id) const;
  Dictionary get_script_intelligence_summary(int64_t script_file_id) const;
  Array list_dependency_cycles() const;
  Array get_dependency_graph_slice(int64_t root_script_file_id,
                                   int64_t depth) const;
  Dictionary export_full_inventory_for_debug() const;

  Array list_projects() const;
  Dictionary get_project_summary(int64_t project_id) const;

  Dictionary get_last_scan_results() const;
  String get_last_error() const;

private:
  struct ActiveScanState {
    int64_t scan_id = 0;
    int64_t project_id = 0;
    int64_t scan_generation = 0;
    int64_t started_at_unix = 0;
    int64_t finished_at_unix = 0;
    std::string status = "queued";
    std::string last_error;
    gotool::project_scanner::ScanMetrics metrics;
    std::atomic_bool cancellation_requested{false};
  };

  static int64_t current_unix_time();
  std::filesystem::path get_current_project_root_path() const;
  void stop_scan_worker();
  void join_finished_scan_worker_if_idle();
  void sync_last_scan_results_from_active_state() const;
  Dictionary
  active_state_to_summary_dictionary(const ActiveScanState &state) const;

  std::unique_ptr<gotool::database::Database> database_;
  mutable std::mutex database_mutex_;

  mutable std::mutex scan_mutex_;
  mutable std::condition_variable scan_cv_;
  std::shared_ptr<ActiveScanState> active_scan_state_;
  std::thread scan_worker_;

  std::unique_ptr<gotool::project_scanner::FileWatcher> file_watcher_;
  mutable std::mutex watcher_mutex_;

  int64_t current_project_id_ = 0;
  String current_identity_source_;
  String current_identity_warning_;

  String database_virtual_path_;
  String database_absolute_path_;
  mutable String last_error_;

  mutable Dictionary last_scan_results_;
};

} // namespace godot
