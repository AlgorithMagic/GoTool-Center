// Copyright 2026 AlgorithMagic

#include "project_context/gotool_project_context_helpers.hpp"

#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

std::string to_utf8(const String &value) {
  const CharString utf8_value = value.utf8();
  return utf8_value.get_data() != nullptr ? utf8_value.get_data() : "";
}

String from_utf8(const std::string &value) {
  return String::utf8(value.c_str());
}

} // namespace

Dictionary to_project_dictionary(const gotool::database::ProjectListItem &item) {
  Dictionary project;
  project["project_id"] = item.project_id;
  project["project_uid"] = from_utf8(item.project_uid);
  project["display_name"] = from_utf8(item.display_name);
  project["root_absolute_path"] = from_utf8(item.root_absolute_path);
  project["root_canonical_path"] = from_utf8(item.root_canonical_path);
  project["project_file_absolute_path"] =
      from_utf8(item.project_file_absolute_path);
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

gotool::project_scanner::FileQuery
file_query_from_dictionary(const Dictionary &filter) {
  gotool::project_scanner::FileQuery query;
  query.include_deleted =
      static_cast<bool>(filter.get("include_deleted", false));

  if (filter.has("parent_id")) {
    query.parent_id = static_cast<int64_t>(filter.get("parent_id", 0));
  }

  if (filter.has("is_directory")) {
    query.is_directory = static_cast<bool>(filter.get("is_directory", false));
  }

  query.extension = to_utf8(String(filter.get("extension", "")));
  query.file_type = to_utf8(String(filter.get("file_type", "")));
  query.godot_type = to_utf8(String(filter.get("godot_type", "")));
  query.search = to_utf8(String(filter.get("search", "")));
  return query;
}

gotool::project_scanner::CustomClassQuery
custom_class_query_from_dictionary(const Dictionary &filter) {
  gotool::project_scanner::CustomClassQuery query;
  query.language = to_utf8(String(filter.get("language", "")));
  query.base_type = to_utf8(String(filter.get("base_type", "")));
  query.search = to_utf8(String(filter.get("search", "")));
  return query;
}

gotool::project_scanner::SymbolQueryFilter
symbol_query_filter_from_dictionary(const Dictionary &filter) {
  gotool::project_scanner::SymbolQueryFilter query;
  query.symbol_kind = to_utf8(String(filter.get("symbol_kind", "")));
  query.search = to_utf8(String(filter.get("search", "")));
  return query;
}

gotool::project_scanner::ReferenceQueryFilter
reference_query_filter_from_dictionary(const Dictionary &filter) {
  gotool::project_scanner::ReferenceQueryFilter query;
  if (filter.has("script_file_id")) {
    const int64_t script_file_id =
        static_cast<int64_t>(filter.get("script_file_id", 0));
    if (script_file_id > 0) {
      query.script_file_id = script_file_id;
    }
  }
  query.reference_kind = to_utf8(String(filter.get("reference_kind", "")));
  return query;
}

gotool::project_scanner::SceneAttachmentQueryFilter
scene_attachment_filter_from_dictionary(const Dictionary &filter) {
  gotool::project_scanner::SceneAttachmentQueryFilter query;
  if (filter.has("scene_file_id")) {
    const int64_t scene_file_id =
        static_cast<int64_t>(filter.get("scene_file_id", 0));
    if (scene_file_id > 0) {
      query.scene_file_id = scene_file_id;
    }
  }
  if (filter.has("script_file_id")) {
    const int64_t script_file_id =
        static_cast<int64_t>(filter.get("script_file_id", 0));
    if (script_file_id > 0) {
      query.script_file_id = script_file_id;
    }
  }
  return query;
}

gotool::project_scanner::DocCommentGapFilter
doc_comment_gap_filter_from_dictionary(const Dictionary &filter) {
  gotool::project_scanner::DocCommentGapFilter query;
  query.symbol_kind = to_utf8(String(filter.get("symbol_kind", "")));
  query.search = to_utf8(String(filter.get("search", "")));
  return query;
}

Dictionary file_row_to_dictionary(const gotool::project_scanner::FileRow &row) {
  Dictionary entry;
  entry["id"] = row.id;
  entry["file_id"] = row.id;
  entry["parent_id"] = row.parent_id;
  entry["path"] = from_utf8("res://" + row.project_relative_path);
  entry["project_relative_path"] = from_utf8(row.project_relative_path);
  entry["file_name"] = from_utf8(row.file_name);
  entry["name"] = from_utf8(row.file_name);
  entry["extension"] = from_utf8(row.extension);
  entry["file_type"] = from_utf8(row.file_type);
  entry["type"] = row.is_directory ? "folder" : "file";
  entry["godot_type"] = from_utf8(row.godot_type);
  entry["godot_type_hint"] = from_utf8(row.godot_type);
  entry["type_hint_source"] = from_utf8(row.type_hint_source);
  entry["size_bytes"] = row.size_bytes;
  entry["modified_time_ns"] = row.modified_time_ns;
  entry["modified_time_unix"] =
      static_cast<int64_t>(row.modified_time_ns / 1'000'000'000LL);
  entry["is_directory"] = row.is_directory;
  entry["is_hidden"] = row.is_hidden;
  entry["is_deleted"] = row.is_deleted;
  entry["scan_generation"] = row.scan_generation;
  entry["last_seen_generation"] = row.last_seen_generation;
  entry["dirty_state"] = from_utf8(row.dirty_state);
  entry["dirty_reason"] = from_utf8(row.dirty_reason);
  return entry;
}

Dictionary custom_class_row_to_dictionary(
    const gotool::project_scanner::CustomClassRow &row) {
  Dictionary entry;
  entry["id"] = row.id;
  entry["class_name"] = from_utf8(row.class_name);
  entry["script_path"] = from_utf8(row.script_path);
  entry["script_project_relative_path"] =
      from_utf8(row.script_project_relative_path);
  entry["language"] = from_utf8(row.language);
  entry["base_type"] = from_utf8(row.direct_base_type);
  entry["direct_base_type"] = from_utf8(row.direct_base_type);
  entry["is_resource_type"] = row.is_resource_type;
  entry["is_node_type"] = row.is_node_type;
  entry["script_file_id"] = row.script_file_id;
  entry["parser_version"] = row.parser_version;
  entry["parse_status"] = from_utf8(row.parse_status);
  entry["parse_error"] = from_utf8(row.parse_error);
  entry["last_parsed_generation"] = row.last_parsed_generation;
  return entry;
}

Dictionary dependency_row_to_dictionary(
    const gotool::project_scanner::ScriptDependencyRow &row) {
  Dictionary entry;
  entry["id"] = row.id;
  entry["project_id"] = row.project_id;
  entry["source_script_file_id"] = row.source_script_file_id;
  if (row.source_symbol_id.has_value()) {
    entry["source_symbol_id"] = row.source_symbol_id.value();
  } else {
    entry["source_symbol_id"] = Variant();
  }
  if (row.target_file_id.has_value()) {
    entry["target_file_id"] = row.target_file_id.value();
  } else {
    entry["target_file_id"] = Variant();
  }
  entry["target_project_relative_path"] =
      from_utf8(row.target_project_relative_path);
  entry["target_path"] =
      from_utf8(row.target_project_relative_path.empty()
                    ? ""
                    : "res://" + row.target_project_relative_path);
  entry["target_class_name"] = from_utf8(row.target_class_name);
  entry["target_resource_uid"] = from_utf8(row.target_resource_uid);
  entry["dependency_kind"] = from_utf8(row.dependency_kind);
  entry["reference_text"] = from_utf8(row.reference_text);
  entry["source_line"] = row.source_line;
  entry["source_column"] = row.source_column;
  entry["confidence"] = row.confidence;
  entry["is_dynamic"] = row.is_dynamic;
  entry["is_resolved"] = row.is_resolved;
  entry["parser_version"] = row.parser_version;
  entry["scan_generation"] = row.scan_generation;
  entry["created_at_unix"] = row.created_at_unix;
  return entry;
}

Dictionary dependency_cycle_row_to_dictionary(
    const gotool::project_scanner::DependencyCycleRow &row) {
  Dictionary entry;
  entry["source_script_file_id"] = row.source_script_file_id;
  entry["cycle_to_script_file_id"] = row.cycle_to_script_file_id;
  entry["cycle_path"] = from_utf8(row.cycle_path);
  entry["hop_count"] = row.hop_count;
  return entry;
}

Dictionary
symbol_row_to_dictionary(const gotool::project_scanner::ScriptSymbolRow &row) {
  Dictionary entry;
  entry["id"] = row.id;
  entry["symbol_id"] = row.id;
  entry["project_id"] = row.project_id;
  entry["script_file_id"] = row.script_file_id;
  entry["symbol_slot"] = row.symbol_slot;
  if (row.parent_symbol_slot.has_value()) {
    entry["parent_symbol_slot"] = row.parent_symbol_slot.value();
  } else {
    entry["parent_symbol_slot"] = Variant();
  }
  if (row.parent_symbol_id.has_value()) {
    entry["parent_symbol_id"] = row.parent_symbol_id.value();
  } else {
    entry["parent_symbol_id"] = Variant();
  }
  entry["symbol_kind"] = from_utf8(row.symbol_kind);
  entry["name"] = from_utf8(row.name);
  entry["qualified_name"] = from_utf8(row.qualified_name);
  entry["declared_type"] = from_utf8(row.declared_type);
  entry["return_type"] = from_utf8(row.return_type);
  entry["default_value_excerpt"] = from_utf8(row.default_value_excerpt);
  entry["visibility"] = from_utf8(row.visibility);
  entry["flags"] = row.flags;
  entry["doc_comment_state"] = from_utf8(row.doc_comment_state);
  entry["symbol_name"] = from_utf8(row.symbol_name);
  entry["class_name"] = from_utf8(row.class_name);
  entry["language"] = from_utf8(row.language);
  entry["signature_text"] = from_utf8(row.signature_text);
  entry["symbol_flags"] = row.symbol_flags;
  entry["line_start"] = row.line_start;
  entry["column_start"] = row.column_start;
  entry["line_end"] = row.line_end;
  entry["column_end"] = row.column_end;
  entry["parser_version"] = row.parser_version;
  entry["last_parsed_generation"] = row.last_parsed_generation;
  entry["last_seen_scan_run_id"] = row.last_seen_scan_run_id;
  entry["created_at_unix"] = row.created_at_unix;
  entry["updated_at_unix"] = row.updated_at_unix;
  return entry;
}

Dictionary reference_row_to_dictionary(
    const gotool::project_scanner::ScriptReferenceRow &row) {
  Dictionary entry;
  entry["id"] = row.id;
  entry["reference_id"] = row.id;
  entry["project_id"] = row.project_id;
  entry["script_file_id"] = row.script_file_id;
  entry["source_script_file_id"] = row.source_script_file_id;
  if (row.source_symbol_id.has_value()) {
    entry["source_symbol_id"] = row.source_symbol_id.value();
  } else {
    entry["source_symbol_id"] = Variant();
  }
  if (row.target_file_id.has_value()) {
    entry["target_file_id"] = row.target_file_id.value();
  } else {
    entry["target_file_id"] = Variant();
  }
  if (row.target_symbol_id.has_value()) {
    entry["target_symbol_id"] = row.target_symbol_id.value();
  } else {
    entry["target_symbol_id"] = Variant();
  }
  entry["target_project_relative_path"] =
      from_utf8(row.target_project_relative_path);
  entry["target_path"] =
      from_utf8(row.target_project_relative_path.empty()
                    ? ""
                    : "res://" + row.target_project_relative_path);
  entry["target_class_name"] = from_utf8(row.target_class_name);
  entry["target_symbol_name"] = from_utf8(row.target_symbol_name);
  entry["target_resource_uid"] = from_utf8(row.target_resource_uid);
  entry["reference_kind"] = from_utf8(row.reference_kind);
  entry["reference_text"] = from_utf8(row.reference_text);
  entry["source_line"] = row.source_line;
  entry["source_column"] = row.source_column;
  entry["source_line_end"] = row.source_line_end;
  entry["source_column_end"] = row.source_column_end;
  entry["confidence"] = row.confidence;
  entry["is_dynamic"] = row.is_dynamic;
  entry["is_resolved"] = row.is_resolved;
  entry["is_unresolved"] = row.is_unresolved;
  entry["parser_version"] = row.parser_version;
  entry["scan_generation"] = row.scan_generation;
  entry["created_at_unix"] = row.created_at_unix;
  return entry;
}

Dictionary scene_attachment_row_to_dictionary(
    const gotool::project_scanner::SceneScriptAttachmentRow &row) {
  Dictionary entry;
  entry["id"] = row.id;
  entry["attachment_id"] = row.id;
  entry["project_id"] = row.project_id;
  entry["scene_file_id"] = row.scene_file_id;
  entry["node_path"] = from_utf8(row.node_path);
  entry["node_name"] = from_utf8(row.node_name);
  entry["node_type"] = from_utf8(row.node_type);
  entry["attachment_kind"] = from_utf8(row.attachment_kind);
  entry["ext_resource_id"] = from_utf8(row.ext_resource_id);
  entry["ext_resource_slot"] = from_utf8(row.ext_resource_slot);
  entry["script_resource_path"] = from_utf8(row.script_resource_path);
  entry["script_uid"] = from_utf8(row.script_uid);
  entry["script_project_relative_path"] =
      from_utf8(row.script_project_relative_path);
  entry["script_path"] =
      from_utf8(row.script_project_relative_path.empty()
                    ? ""
                    : "res://" + row.script_project_relative_path);
  entry["script_resource_uid"] = from_utf8(row.script_resource_uid);
  if (row.script_file_id.has_value()) {
    entry["script_file_id"] = row.script_file_id.value();
  } else {
    entry["script_file_id"] = Variant();
  }
  if (row.script_symbol_id.has_value()) {
    entry["script_symbol_id"] = row.script_symbol_id.value();
  } else {
    entry["script_symbol_id"] = Variant();
  }
  entry["is_dynamic"] = row.is_dynamic;
  entry["is_resolved"] = row.is_resolved;
  entry["source_line"] = row.source_line;
  entry["source_column"] = row.source_column;
  entry["parser_version"] = row.parser_version;
  entry["scan_generation"] = row.scan_generation;
  entry["created_at_unix"] = row.created_at_unix;
  return entry;
}

Dictionary script_intelligence_summary_row_to_dictionary(
    const gotool::project_scanner::ScriptIntelligenceSummaryRow &row) {
  Dictionary entry;
  entry["script_file_id"] = row.script_file_id;
  entry["symbol_count"] = row.symbol_count;
  entry["function_count"] = row.function_count;
  entry["property_count"] = row.property_count;
  entry["parameter_count"] = row.parameter_count;
  entry["doc_comment_count"] = row.doc_comment_count;
  entry["reference_count"] = row.reference_count;
  entry["unresolved_reference_count"] = row.unresolved_reference_count;
  entry["dynamic_reference_count"] = row.dynamic_reference_count;
  return entry;
}

} // namespace godot