// Copyright 2026 AlgorithMagic

#include "project_context/gotool_project_context.hpp"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <exception>
#include <vector>

namespace godot {

namespace {

String from_utf8(const std::string &value) {
  return String::utf8(value.c_str());
}

} // namespace

bool GodotProjectContext::start_watcher() {
  last_error_ = "";

  try {
    const std::filesystem::path project_root = get_current_project_root_path();
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    if (file_watcher_ == nullptr) {
      file_watcher_ = std::make_unique<gotool::project_scanner::FileWatcher>();
    }

    const bool started = file_watcher_->start(project_root);
    if (!started) {
      const gotool::project_scanner::FileWatcherStatus status =
          file_watcher_->get_status();
      if (!status.last_error.empty()) {
        last_error_ = from_utf8(status.last_error);
      }
    }
    return started;
  } catch (const std::exception &error) {
    last_error_ = error.what();
    return false;
  }
}

void GodotProjectContext::stop_watcher() {
  std::lock_guard<std::mutex> lock(watcher_mutex_);
  if (file_watcher_ != nullptr) {
    file_watcher_->stop();
  }
}

Dictionary GodotProjectContext::get_watcher_status() const {
  Dictionary status;

  std::lock_guard<std::mutex> lock(watcher_mutex_);
  if (file_watcher_ == nullptr) {
    status["running"] = false;
    status["supported"] = false;
    status["requires_full_rescan"] = true;
    status["backend"] = "none";
    status["pending_events"] = 0;
    return status;
  }

  const gotool::project_scanner::FileWatcherStatus watcher_status =
      file_watcher_->get_status();
  status["running"] = watcher_status.running;
  status["supported"] = watcher_status.supported;
  status["requires_full_rescan"] = watcher_status.requires_full_rescan;
  status["backend"] = from_utf8(watcher_status.backend);
  status["last_error"] = from_utf8(watcher_status.last_error);
  status["pending_events"] = watcher_status.pending_events;
  return status;
}

Array GodotProjectContext::consume_watcher_changes() {
  Array events;

  std::lock_guard<std::mutex> lock(watcher_mutex_);
  if (file_watcher_ == nullptr) {
    return events;
  }

  const std::vector<gotool::project_scanner::FileWatcherEvent> drained =
      file_watcher_->drain_events();
  for (const gotool::project_scanner::FileWatcherEvent &event : drained) {
    Dictionary row;
    row["project_relative_path"] = from_utf8(event.project_relative_path);
    row["path"] = from_utf8("res://" + event.project_relative_path);
    row["removed"] = event.removed;
    row["is_directory"] = event.is_directory;
    events.append(row);
  }

  return events;
}

Array GodotProjectContext::get_dirty_paths() {
  Array dirty_paths;
  const Array changes = consume_watcher_changes();
  for (int64_t i = 0; i < changes.size(); ++i) {
    const Dictionary change = changes[i];
    dirty_paths.append(change.get("path", ""));
  }
  return dirty_paths;
}

} // namespace godot