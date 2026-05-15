// Copyright 2026 AlgorithMagic

#include "project_scanner/native_directory_enumerator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <dirent.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

namespace gotool::project_scanner {

namespace {

bool is_cancelled(const EnumerationOptions& options)
{
    return options.cancel_requested != nullptr &&
           options.cancel_requested->load(std::memory_order_relaxed);
}

std::string_view file_name_view_from_path(std::string_view path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string extension_from_lower_name(std::string_view name_lower)
{
    const size_t dot = name_lower.rfind('.');

    if (dot == std::string::npos || dot == 0 || dot + 1 >= name_lower.size()) {
        return "";
    }

    return std::string(name_lower.substr(dot));
}

int64_t finish_record(EntryRecord& record, PathArena& arena, std::string_view project_path,
                      std::string_view name, int64_t parent_record_index, EntryKind kind,
                      bool hidden)
{
    const auto classification_start = std::chrono::steady_clock::now();

    const std::string project_path_lower = lower_ascii(project_path);
    const std::string_view lower_name_view = file_name_view_from_path(project_path_lower);

    const std::string extension =
        kind == EntryKind::Directory ? std::string() : extension_from_lower_name(lower_name_view);

    record.path_offset = arena.append(project_path);
    record.path_length = static_cast<uint32_t>(project_path.size());
    record.lower_path_offset = arena.append(project_path_lower);
    record.lower_path_length = static_cast<uint32_t>(project_path_lower.size());
    record.name_offset = arena.append(name);
    record.name_length = static_cast<uint32_t>(name.size());
    record.extension_offset = arena.append(extension);
    record.extension_length = static_cast<uint32_t>(extension.size());

    record.parent_record_index = parent_record_index;
    record.entry_kind = kind;
    record.extension_id = extension_id_from_extension(extension);
    record.set_hidden(hidden);

    EntryFacts facts;
    facts.project_relative_path = project_path;
    facts.project_relative_path_lower = project_path_lower;
    facts.file_name = name;
    facts.extension = extension;
    facts.entry_kind = kind;
    facts.extension_id = record.extension_id;
    facts.hidden = hidden;

    record.file_type_id = classify_entry_from_facts(facts);
    record.godot_type_hint = detect_godot_type_hint_from_facts(facts, record.file_type_id);
    record.type_hint_source = type_hint_source_for(record.godot_type_hint);

    const auto classification_end = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(classification_end -
                                                                classification_start)
        .count();
}

void copy_record_into_arena(const EntryRecord& source, const PathArena& source_arena,
                            PathArena& target_arena, std::vector<EntryRecord>& target_records)
{
    EntryRecord copied = source;
    copied.path_offset =
        target_arena.append(source_arena.view(source.path_offset, source.path_length));
    copied.lower_path_offset =
        target_arena.append(source_arena.view(source.lower_path_offset, source.lower_path_length));
    copied.name_offset =
        target_arena.append(source_arena.view(source.name_offset, source.name_length));
    copied.extension_offset =
        target_arena.append(source_arena.view(source.extension_offset, source.extension_length));

    // Parent IDs are resolved during persistence via parent path lookups.
    copied.parent_record_index = -1;
    target_records.push_back(copied);
}

void sort_records_if_requested(const EnumerationOptions& options, PathArena& arena,
                               std::vector<EntryRecord>& records)
{
    if (!options.deterministic_record_order || records.empty()) {
        return;
    }

    std::sort(
        records.begin(), records.end(),
        [&arena](const EntryRecord& left, const EntryRecord& right) {
            const std::string_view left_path = arena.view(left.path_offset, left.path_length);
            const std::string_view right_path = arena.view(right.path_offset, right.path_length);

            if (left_path == right_path) {
                if (left.entry_kind != right.entry_kind) {
                    return static_cast<int>(left.entry_kind) < static_cast<int>(right.entry_kind);
                }

                return left.extension_id < right.extension_id;
            }

            return left_path < right_path;
        });

    for (EntryRecord& record : records) {
        record.parent_record_index = -1;
    }
}

#ifdef _WIN32

std::string wide_to_utf8(const std::wstring& value)
{
    if (value.empty()) {
        return "";
    }

    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);

    if (required <= 0) {
        return "";
    }

    std::string output(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(),
                        required, nullptr, nullptr);
    return output;
}

int64_t filetime_to_unix_ns(const FILETIME& filetime)
{
    ULARGE_INTEGER value;
    value.LowPart = filetime.dwLowDateTime;
    value.HighPart = filetime.dwHighDateTime;

    static constexpr uint64_t WINDOWS_TO_UNIX_100NS = 116444736000000000ULL;
    if (value.QuadPart <= WINDOWS_TO_UNIX_100NS) {
        return 0;
    }

    return static_cast<int64_t>((value.QuadPart - WINDOWS_TO_UNIX_100NS) * 100ULL);
}

void enumerate_windows_directory(const EnumerationOptions& options,
                                 const std::filesystem::path& absolute_dir,
                                 const std::string& relative_dir, int64_t parent_record_index,
                                 PathArena& arena, std::vector<EntryRecord>& records,
                                 EnumerationResult& result, bool recurse_children,
                                 int64_t& classification_ns_total)
{
    if (is_cancelled(options)) {
        result.completed = false;
        return;
    }

    std::filesystem::path search_path = absolute_dir / L"*";
    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileExW(search_path.c_str(), FindExInfoBasic, &data,
                                     FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);

    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            result.errors.push_back("failed_to_enumerate:" + relative_dir);
        }
        return;
    }

    for (bool has_entry = true; has_entry; has_entry = FindNextFileW(handle, &data) != 0) {
        const std::wstring wide_name(data.cFileName);
        if (wide_name == L"." || wide_name == L"..") {
            continue;
        }

        const std::string name = wide_to_utf8(wide_name);
        if (name.empty()) {
            continue;
        }

        const bool hidden =
            (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 || name.front() == '.';
        if (hidden && !options.include_hidden) {
            continue;
        }

        const bool is_directory{(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0};
        std::string project_path;
        if (relative_dir.empty()) {
            project_path = name;
        } else {
            project_path.reserve(relative_dir.size() + 1 + name.size());
            project_path.assign(relative_dir).append("/").append(name);
        }

        if (options.skip_policy != nullptr &&
            options.skip_policy->should_skip_normalized(project_path)) {
            if (is_directory) {
                ++result.dirs_skipped;
            }
            continue;
        }

        EntryRecord record;
        classification_ns_total +=
            finish_record(record, arena, project_path, name, parent_record_index,
                          is_directory ? EntryKind::Directory : EntryKind::File, hidden);
        record.modified_time_ns = filetime_to_unix_ns(data.ftLastWriteTime);
        if (!is_directory) {
            LARGE_INTEGER size;
            size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
            size.LowPart = data.nFileSizeLow;
            record.size_bytes = size.QuadPart;
        }

        const auto record_index = static_cast<int64_t>(records.size());
        records.push_back(record);

        if (is_directory) {
            ++result.dirs_seen;

            if (!recurse_children) {
                continue;
            }

            const bool is_reparse_point =
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (!is_reparse_point) {
                enumerate_windows_directory(options, absolute_dir / wide_name, project_path,
                                            record_index, arena, records, result, true,
                                            classification_ns_total);
            }
        } else {
            ++result.files_seen;
        }
    }

    FindClose(handle);
}

#else

int64_t timespec_to_ns(const timespec& value)
{
    return static_cast<int64_t>(value.tv_sec) * 1'000'000'000LL +
           static_cast<int64_t>(value.tv_nsec);
}

void enumerate_posix_directory(const EnumerationOptions& options,
                               const std::filesystem::path& absolute_dir,
                               const std::string& relative_dir, int64_t parent_record_index,
                               PathArena& arena, std::vector<EntryRecord>& records,
                               EnumerationResult& result, bool recurse_children,
                               int64_t& classification_ns_total)
{
    if (is_cancelled(options)) {
        result.completed = false;
        return;
    }

    DIR* dir = opendir(absolute_dir.c_str());
    if (dir == nullptr) {
        result.errors.push_back("failed_to_enumerate:" + relative_dir);
        return;
    }

    while (dirent* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }

        const bool hidden = !name.empty() && name.front() == '.';
        if (hidden && !options.include_hidden) {
            continue;
        }

        std::string project_path;
        if (relative_dir.empty()) {
            project_path = name;
        } else {
            project_path.reserve(relative_dir.size() + 1 + name.size());
            project_path.assign(relative_dir).append("/").append(name);
        }
        const std::filesystem::path absolute_path = absolute_dir / name;

        struct stat metadata{};
        if (lstat(absolute_path.c_str(), &metadata) != 0) {
            result.errors.push_back("failed_to_stat:" + project_path);
            continue;
        }

        const bool is_directory = S_ISDIR(metadata.st_mode);
        if (options.skip_policy != nullptr &&
            options.skip_policy->should_skip_normalized(project_path)) {
            if (is_directory) {
                ++result.dirs_skipped;
            }
            continue;
        }

        EntryRecord record;
        classification_ns_total +=
            finish_record(record, arena, project_path, name, parent_record_index,
                          is_directory ? EntryKind::Directory : EntryKind::File, hidden);
        record.size_bytes = is_directory ? 0 : static_cast<int64_t>(metadata.st_size);
#    ifdef __APPLE__
        record.modified_time_ns = timespec_to_ns(metadata.st_mtimespec);
#    else
        record.modified_time_ns = timespec_to_ns(metadata.st_mtim);
#    endif
        record.platform_file_id_high = static_cast<uint64_t>(metadata.st_dev);
        record.platform_file_id_low = static_cast<uint64_t>(metadata.st_ino);
        record.flags |= 1u << 1u;

        const auto record_index = static_cast<int64_t>(records.size());
        records.push_back(record);

        if (is_directory) {
            ++result.dirs_seen;

            if (!recurse_children) {
                continue;
            }

            enumerate_posix_directory(options, absolute_path, project_path, record_index, arena,
                                      records, result, true, classification_ns_total);
        } else {
            ++result.files_seen;
        }
    }

    closedir(dir);
}

#endif

void merge_enumeration_result(EnumerationResult& into, const EnumerationResult& from)
{
    into.completed = into.completed && from.completed;
    into.classification_ms += from.classification_ms;
    into.files_seen += from.files_seen;
    into.dirs_seen += from.dirs_seen;
    into.dirs_skipped += from.dirs_skipped;
    into.errors.insert(into.errors.end(), from.errors.begin(), from.errors.end());
}

EnumerationResult enumerate_serial(const EnumerationOptions& options, PathArena& arena,
                                   std::vector<EntryRecord>& records)
{
    EnumerationResult result;
    int64_t classification_ns_total = 0;

#ifdef _WIN32
    enumerate_windows_directory(options, options.root, "", -1, arena, records, result, true,
                                classification_ns_total);
#else
    enumerate_posix_directory(options, options.root, "", -1, arena, records, result, true,
                              classification_ns_total);
#endif

    result.classification_ms = classification_ns_total / 1'000'000LL;

    sort_records_if_requested(options, arena, records);
    return result;
}

EnumerationResult enumerate_parallel(const EnumerationOptions& options, PathArena& arena,
                                     std::vector<EntryRecord>& records)
{
    EnumerationResult result;

    PathArena root_arena;
    std::vector<EntryRecord> root_records;
    int64_t root_classification_ns = 0;

#ifdef _WIN32
    enumerate_windows_directory(options, options.root, "", -1, root_arena, root_records, result,
                                false, root_classification_ns);
#else
    enumerate_posix_directory(options, options.root, "", -1, root_arena, root_records, result,
                              false, root_classification_ns);
#endif

    result.classification_ms = root_classification_ns / 1'000'000LL;

    if (!result.completed || is_cancelled(options)) {
        result.completed = false;
        return result;
    }

    struct SubtreeTask
    {
        std::filesystem::path absolute_dir;
        std::string relative_dir;
    };

    std::vector<SubtreeTask> tasks;
    tasks.reserve(root_records.size());

    for (const EntryRecord& record : root_records) {
        copy_record_into_arena(record, root_arena, arena, records);

        if (record.entry_kind != EntryKind::Directory) {
            continue;
        }

        const std::string_view project_path =
            root_arena.view(record.path_offset, record.path_length);
        tasks.push_back({options.root / std::filesystem::u8path(std::string(project_path)),
                         std::string(project_path)});
    }

    if (tasks.empty()) {
        sort_records_if_requested(options, arena, records);
        return result;
    }

    int64_t max_workers = options.max_parallel_workers;
    if (max_workers <= 0) {
        max_workers = 4;
    }

    const int64_t hardware =
        std::max<int64_t>(2, static_cast<int64_t>(std::thread::hardware_concurrency()));
    const int64_t worker_count = std::max<int64_t>(1, std::min<int64_t>(hardware - 1, max_workers));

    if (worker_count <= 1) {
        for (const SubtreeTask& task : tasks) {
            if (is_cancelled(options)) {
                result.completed = false;
                return result;
            }

            PathArena local_arena;
            std::vector<EntryRecord> local_records;
            EnumerationResult local_result;
            int64_t local_classification_ns = 0;

#ifdef _WIN32
            enumerate_windows_directory(options, task.absolute_dir, task.relative_dir, -1,
                                        local_arena, local_records, local_result, true,
                                        local_classification_ns);
#else
            enumerate_posix_directory(options, task.absolute_dir, task.relative_dir, -1,
                                      local_arena, local_records, local_result, true,
                                      local_classification_ns);
#endif

            local_result.classification_ms = local_classification_ns / 1'000'000LL;

            merge_enumeration_result(result, local_result);

            for (const EntryRecord& record : local_records) {
                copy_record_into_arena(record, local_arena, arena, records);
            }
        }

        sort_records_if_requested(options, arena, records);
        return result;
    }

    std::atomic_size_t next_task{0};
    std::mutex merge_mutex;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(worker_count));

    for (int64_t i = 0; i < worker_count; ++i) {
        workers.emplace_back([&]() {
            EnumerationResult local_totals;
            std::vector<std::pair<PathArena, std::vector<EntryRecord>>> local_outputs;

            while (true) {
                if (is_cancelled(options)) {
                    local_totals.completed = false;
                    break;
                }

                const size_t index = next_task.fetch_add(1, std::memory_order_relaxed);
                if (index >= tasks.size()) {
                    break;
                }

                const SubtreeTask& task = tasks[index];

                PathArena local_arena;
                std::vector<EntryRecord> local_records;
                EnumerationResult local_result;
                int64_t local_classification_ns = 0;

#ifdef _WIN32
                enumerate_windows_directory(options, task.absolute_dir, task.relative_dir, -1,
                                            local_arena, local_records, local_result, true,
                                            local_classification_ns);
#else
                enumerate_posix_directory(options, task.absolute_dir, task.relative_dir, -1,
                                          local_arena, local_records, local_result, true,
                                          local_classification_ns);
#endif

                local_result.classification_ms = local_classification_ns / 1'000'000LL;

                merge_enumeration_result(local_totals, local_result);
                local_outputs.emplace_back(std::move(local_arena), std::move(local_records));
            }

            std::scoped_lock lock(merge_mutex);
            merge_enumeration_result(result, local_totals);
            for (auto& output : local_outputs) {
                for (const EntryRecord& record : output.second) {
                    copy_record_into_arena(record, output.first, arena, records);
                }
            }
        });
    }

    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    if (is_cancelled(options)) {
        result.completed = false;
        return result;
    }

    sort_records_if_requested(options, arena, records);
    return result;
}

} // namespace

EnumerationResult NativeDirectoryEnumerator::enumerate(const EnumerationOptions& options,
                                                       PathArena& arena,
                                                       std::vector<EntryRecord>& records) const
{
    EnumerationResult result;

    if (options.root.empty()) {
        result.completed = false;
        result.errors.emplace_back("empty_root");
        return result;
    }

    if (options.enable_parallel_traversal) {
        return enumerate_parallel(options, arena, records);
    }

    return enumerate_serial(options, arena, records);
}

} // namespace gotool::project_scanner
