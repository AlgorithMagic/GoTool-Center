#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace gotool::project_scanner {

struct FileWatcherEvent {
    std::string project_relative_path;
    bool removed = false;
    bool is_directory = false;
};

struct FileWatcherStatus {
    bool running = false;
    bool supported = false;
    bool requires_full_rescan = false;
    std::string backend;
    std::string last_error;
    int64_t pending_events = 0;
};

class FileWatcher {
public:
    bool start(const std::filesystem::path &root_path);
    void stop();
    std::vector<FileWatcherEvent> drain_events();

    bool is_supported() const;
    bool requires_full_rescan() const;
    FileWatcherStatus get_status() const;

private:
    mutable std::mutex mutex_;
    std::filesystem::path root_path_;
    std::vector<FileWatcherEvent> pending_events_;
    std::string backend_;
    std::string last_error_;
    bool running_ = false;
    bool supported_ = false;
    bool requires_full_rescan_ = false;
};

} // namespace gotool::project_scanner
