#pragma once

#include "project_scanner/native_scan_rules.hpp"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace gotool::project_scanner {

struct EnumerationOptions {
    std::filesystem::path root;
    bool include_hidden = true;
    const SkipPolicy *skip_policy = nullptr;
    const std::atomic_bool *cancel_requested = nullptr;
};

struct EnumerationResult {
    bool completed = true;
    int64_t files_seen = 0;
    int64_t dirs_seen = 0;
    int64_t dirs_skipped = 0;
    std::vector<std::string> errors;
};

class NativeDirectoryEnumerator {
public:
    EnumerationResult enumerate(
        const EnumerationOptions &options,
        PathArena &arena,
        std::vector<EntryRecord> &records
    ) const;
};

} // namespace gotool::project_scanner
