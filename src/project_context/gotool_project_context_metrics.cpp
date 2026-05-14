#include "project_context/gotool_project_context_helpers.hpp"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

String from_utf8(const std::string &value) {
  return String::utf8(value.c_str());
}

} // namespace

Dictionary
metrics_to_dictionary(const gotool::project_scanner::ScanMetrics &metrics) {
  Dictionary result;
  result["total_wall_ms"] = metrics.total_wall_ms;
  result["traversal_ms"] = metrics.traversal_ms;
  result["metadata_ms"] = metrics.metadata_ms;
  result["existing_snapshot_load_ms"] = metrics.existing_snapshot_load_ms;
  result["reserve_setup_ms"] = metrics.reserve_setup_ms;
  result["dirty_check_ms"] = metrics.dirty_check_ms;
  result["script_candidate_ms"] = metrics.script_candidate_ms;
  result["classification_ms"] = metrics.classification_ms;
  result["script_parse_ms"] = metrics.script_parse_ms;
  result["dependency_parse_ms"] = metrics.dependency_parse_ms;
  result["full_symbol_parse_ms"] = metrics.full_symbol_parse_ms;
  result["doc_comment_parse_ms"] = metrics.doc_comment_parse_ms;
  result["scene_attachment_parse_ms"] = metrics.scene_attachment_parse_ms;
  result["tokenizer_ms"] = metrics.tokenizer_ms;
  result["sqlite_write_ms"] = metrics.sqlite_write_ms;
  result["sqlite_stage_insert_ms"] = metrics.sqlite_stage_insert_ms;
  result["sqlite_file_merge_ms"] = metrics.sqlite_file_merge_ms;
  result["sqlite_clean_refresh_ms"] = metrics.sqlite_clean_refresh_ms;
  result["sqlite_parent_resolve_ms"] = metrics.sqlite_parent_resolve_ms;
  result["sqlite_parse_status_ms"] = metrics.sqlite_parse_status_ms;
  result["sqlite_custom_class_ms"] = metrics.sqlite_custom_class_ms;
  result["dependency_sqlite_stage_ms"] = metrics.dependency_sqlite_stage_ms;
  result["dependency_resolution_ms"] = metrics.dependency_resolution_ms;
  result["sqlite_tombstone_ms"] = metrics.sqlite_tombstone_ms;
  result["sqlite_deleted_reconcile_ms"] = metrics.sqlite_deleted_reconcile_ms;
  result["sqlite_metrics_write_ms"] = metrics.sqlite_metrics_write_ms;
  result["godot_materialization_ms"] = metrics.godot_materialization_ms;
  result["files_seen"] = metrics.files_seen;
  result["dirs_seen"] = metrics.dirs_seen;
  result["dirs_skipped"] = metrics.dirs_skipped;
  result["entries_clean"] = metrics.entries_clean;
  result["entries_dirty"] = metrics.entries_dirty;
  result["entries_new"] = metrics.entries_new;
  result["entries_deleted"] = metrics.entries_deleted;
  result["rows_inserted"] = metrics.rows_inserted;
  result["rows_updated"] = metrics.rows_updated;
  result["rows_clean_refreshed"] = metrics.rows_clean_refreshed;
  result["rows_tombstoned"] = metrics.rows_tombstoned;
  result["scripts_candidates"] = metrics.scripts_candidates;
  result["scripts_parsed"] = metrics.scripts_parsed;
  result["scripts_skipped_clean"] = metrics.scripts_skipped_clean;
  result["symbols_skipped_clean"] = metrics.symbols_skipped_clean;
  result["scenes_skipped_clean"] = metrics.scenes_skipped_clean;
  result["scripts_dependency_parsed"] = metrics.scripts_dependency_parsed;
  result["scripts_dependency_skipped_clean"] =
      metrics.scripts_dependency_skipped_clean;
  result["script_lines_scanned"] = metrics.script_lines_scanned;
  result["parser_lines_scanned"] = metrics.parser_lines_scanned;
  result["bytes_read"] = metrics.bytes_read;
  result["parser_bytes_read"] = metrics.parser_bytes_read;
  result["parser_tokens_generated"] = metrics.parser_tokens_generated;
  result["parser_limit_exceeded_count"] = metrics.parser_limit_exceeded_count;
    result["parser_limit_exceeded_header_fast_count"] =
      metrics.parser_limit_exceeded_header_fast_count;
    result["parser_limit_exceeded_full_symbols_count"] =
      metrics.parser_limit_exceeded_full_symbols_count;
    result["parser_limit_exceeded_scene_attachments_count"] =
      metrics.parser_limit_exceeded_scene_attachments_count;
  result["symbol_rows_created"] = metrics.symbol_rows_created;
  result["reference_rows_created"] = metrics.reference_rows_created;
  result["doc_comment_rows_created"] = metrics.doc_comment_rows_created;
  result["scene_attachment_rows_created"] =
      metrics.scene_attachment_rows_created;
  result["dependency_records_created"] = metrics.dependency_records_created;
  result["unresolved_dependency_count"] = metrics.unresolved_dependency_count;
  result["dynamic_dependency_count"] = metrics.dynamic_dependency_count;
  result["entry_record_count"] = metrics.entry_record_count;
  result["path_arena_bytes"] = metrics.path_arena_bytes;
  result["existing_snapshot_count"] = metrics.existing_snapshot_count;
  result["parsed_script_count"] = metrics.parsed_script_count;
  result["sqlite_statement_steps"] = metrics.sqlite_statement_steps;
  result["sqlite_transactions"] = metrics.sqlite_transactions;
  result["ui_rows_materialized"] = metrics.ui_rows_materialized;
  result["cancellation_requested"] = metrics.cancellation_requested;
  result["scan_result_status"] = from_utf8(metrics.scan_result_status);
  return result;
}

Dictionary scan_summary_to_dictionary(
    const gotool::project_scanner::ScanResultSummary &summary) {
  Dictionary result;
  result["scan_id"] = summary.scan_run_id;
  result["scan_run_id"] = summary.scan_run_id;
  result["scan_generation"] = summary.scan_generation;
  result["status"] = from_utf8(summary.status);
  result["files_seen"] = summary.files_seen;
  result["dirs_seen"] = summary.dirs_seen;
  result["entries_clean"] = summary.entries_clean;
  result["entries_dirty"] = summary.entries_dirty;
  result["entries_new"] = summary.entries_new;
  result["entries_deleted"] = summary.entries_deleted;
  result["scripts_candidates"] = summary.scripts_candidates;
  result["scripts_parsed"] = summary.scripts_parsed;
  result["scripts_skipped_clean"] = summary.scripts_skipped_clean;
  result["total_wall_ms"] = summary.total_wall_ms;
  return result;
}

} // namespace godot