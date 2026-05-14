// Copyright 2026 AlgorithMagic

#include "project_scanner/file_watcher.hpp"

namespace gotool::project_scanner {

namespace {

std::string backend_name() {
#if defined(_WIN32)
    return "windows_read_directory_changesw";
#elif defined(__APPLE__)
    return "macos_fsevents";
#elif defined(__linux__)
    return "linux_inotify";
#else
    return "unsupported";
#endif
}

bool backend_is_supported() {
    // Platform backends are scaffolded but intentionally not enabled yet.
    return false;
}

} // namespace

bool FileWatcher::start(const std::filesystem::path &root_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    root_path_ = root_path;
    backend_ = backend_name();
    supported_ = backend_is_supported();
    pending_events_.clear();
    last_error_.clear();

    if (!supported_) {
        running_ = false;
        requires_full_rescan_ = true;
        last_error_ = "Watcher backend is not enabled on this platform yet.";
        return false;
    }

    running_ = true;
    requires_full_rescan_ = false;
    return true;
}

void FileWatcher::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

std::vector<FileWatcherEvent> FileWatcher::drain_events() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FileWatcherEvent> drained;
    drained.swap(pending_events_);
    return drained;
}

bool FileWatcher::is_supported() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return supported_;
}

bool FileWatcher::requires_full_rescan() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requires_full_rescan_;
}

FileWatcherStatus FileWatcher::get_status() const {
    std::lock_guard<std::mutex> lock(mutex_);

    FileWatcherStatus status;
    status.running = running_;
    status.supported = supported_;
    status.requires_full_rescan = requires_full_rescan_;
    status.backend = backend_;
    status.last_error = last_error_;
    status.pending_events = static_cast<int64_t>(pending_events_.size());
    return status;
}

} // namespace gotool::project_scanner
