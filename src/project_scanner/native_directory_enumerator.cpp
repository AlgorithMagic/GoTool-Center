#include "project_scanner/native_directory_enumerator.hpp"

#include <chrono>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace gotool::project_scanner {

namespace {

bool is_cancelled(const EnumerationOptions &options) {
    return options.cancel_requested != nullptr && options.cancel_requested->load();
}

void finish_record(
    EntryRecord &record,
    PathArena &arena,
    std::string_view project_path,
    std::string_view name,
    int64_t parent_record_index,
    EntryKind kind,
    bool hidden
) {
    record.path_offset = arena.append(project_path);
    record.path_length = static_cast<uint32_t>(project_path.size());
    record.name_offset = arena.append(name);
    record.name_length = static_cast<uint32_t>(name.size());

    const std::string extension =
        kind == EntryKind::Directory
            ? std::string()
            : extension_from_path(project_path);
    record.extension_offset = arena.append(extension);
    record.extension_length = static_cast<uint32_t>(extension.size());

    record.parent_record_index = parent_record_index;
    record.entry_kind = kind;
    record.set_hidden(hidden);
    record.file_type_id = classify_entry(project_path, kind);
    record.godot_type_hint = detect_godot_type_hint(project_path, record.file_type_id);
    record.type_hint_source = type_hint_source_for(record.godot_type_hint);
}

#if defined(_WIN32)

std::string wide_to_utf8(const std::wstring &value) {
    if (value.empty()) {
        return "";
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (required <= 0) {
        return "";
    }

    std::string output(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        output.data(),
        required,
        nullptr,
        nullptr
    );
    return output;
}

int64_t filetime_to_unix_ns(const FILETIME &filetime) {
    ULARGE_INTEGER value;
    value.LowPart = filetime.dwLowDateTime;
    value.HighPart = filetime.dwHighDateTime;

    static constexpr uint64_t WINDOWS_TO_UNIX_100NS = 116444736000000000ULL;
    if (value.QuadPart <= WINDOWS_TO_UNIX_100NS) {
        return 0;
    }

    return static_cast<int64_t>((value.QuadPart - WINDOWS_TO_UNIX_100NS) * 100ULL);
}

void enumerate_windows_directory(
    const EnumerationOptions &options,
    const std::filesystem::path &absolute_dir,
    const std::string &relative_dir,
    int64_t parent_record_index,
    PathArena &arena,
    std::vector<EntryRecord> &records,
    EnumerationResult &result
) {
    if (is_cancelled(options)) {
        result.completed = false;
        return;
    }

    std::filesystem::path search_path = absolute_dir / L"*";
    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileExW(
        search_path.c_str(),
        FindExInfoBasic,
        &data,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH
    );

    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            result.errors.push_back("failed_to_enumerate:" + relative_dir);
        }
        return;
    }

    do {
        const std::wstring wide_name(data.cFileName);
        if (wide_name == L"." || wide_name == L"..") {
            continue;
        }

        const std::string name = wide_to_utf8(wide_name);
        if (name.empty()) {
            continue;
        }

        const bool hidden = (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 || name.front() == '.';
        if (hidden && !options.include_hidden) {
            continue;
        }

        const std::string project_path = relative_dir.empty() ? name : relative_dir + "/" + name;

        if (options.skip_policy != nullptr && options.skip_policy->should_skip(project_path)) {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                ++result.dirs_skipped;
            }
            continue;
        }

        const bool is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        EntryRecord record;
        finish_record(
            record,
            arena,
            project_path,
            name,
            parent_record_index,
            is_directory ? EntryKind::Directory : EntryKind::File,
            hidden
        );
        record.modified_time_ns = filetime_to_unix_ns(data.ftLastWriteTime);
        if (!is_directory) {
            LARGE_INTEGER size;
            size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
            size.LowPart = data.nFileSizeLow;
            record.size_bytes = size.QuadPart;
        }

        const int64_t record_index = static_cast<int64_t>(records.size());
        records.push_back(record);

        if (is_directory) {
            ++result.dirs_seen;
            const bool is_reparse_point = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (!is_reparse_point) {
                enumerate_windows_directory(
                    options,
                    absolute_dir / wide_name,
                    project_path,
                    record_index,
                    arena,
                    records,
                    result
                );
            }
        } else {
            ++result.files_seen;
        }
    } while (FindNextFileW(handle, &data));

    FindClose(handle);
}

#else

int64_t timespec_to_ns(const timespec &value) {
    return static_cast<int64_t>(value.tv_sec) * 1'000'000'000LL + static_cast<int64_t>(value.tv_nsec);
}

void enumerate_posix_directory(
    const EnumerationOptions &options,
    const std::filesystem::path &absolute_dir,
    const std::string &relative_dir,
    int64_t parent_record_index,
    PathArena &arena,
    std::vector<EntryRecord> &records,
    EnumerationResult &result
) {
    if (is_cancelled(options)) {
        result.completed = false;
        return;
    }

    DIR *dir = opendir(absolute_dir.c_str());
    if (dir == nullptr) {
        result.errors.push_back("failed_to_enumerate:" + relative_dir);
        return;
    }

    while (dirent *entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }

        const bool hidden = !name.empty() && name.front() == '.';
        if (hidden && !options.include_hidden) {
            continue;
        }

        const std::string project_path = relative_dir.empty() ? name : relative_dir + "/" + name;

        if (options.skip_policy != nullptr && options.skip_policy->should_skip(project_path)) {
            ++result.dirs_skipped;
            continue;
        }

        const std::filesystem::path absolute_path = absolute_dir / name;
        struct stat metadata {};
        if (lstat(absolute_path.c_str(), &metadata) != 0) {
            result.errors.push_back("failed_to_stat:" + project_path);
            continue;
        }

        const bool is_directory = S_ISDIR(metadata.st_mode);
        EntryRecord record;
        finish_record(
            record,
            arena,
            project_path,
            name,
            parent_record_index,
            is_directory ? EntryKind::Directory : EntryKind::File,
            hidden
        );
        record.size_bytes = is_directory ? 0 : static_cast<int64_t>(metadata.st_size);
#if defined(__APPLE__)
        record.modified_time_ns = timespec_to_ns(metadata.st_mtimespec);
#else
        record.modified_time_ns = timespec_to_ns(metadata.st_mtim);
#endif
        record.platform_file_id_high = static_cast<uint64_t>(metadata.st_dev);
        record.platform_file_id_low = static_cast<uint64_t>(metadata.st_ino);
        record.flags |= 1u << 1u;

        const int64_t record_index = static_cast<int64_t>(records.size());
        records.push_back(record);

        if (is_directory) {
            ++result.dirs_seen;
            enumerate_posix_directory(options, absolute_path, project_path, record_index, arena, records, result);
        } else {
            ++result.files_seen;
        }
    }

    closedir(dir);
}

#endif

} // namespace

EnumerationResult NativeDirectoryEnumerator::enumerate(
    const EnumerationOptions &options,
    PathArena &arena,
    std::vector<EntryRecord> &records
) const {
    EnumerationResult result;

    if (options.root.empty()) {
        result.completed = false;
        result.errors.push_back("empty_root");
        return result;
    }

#if defined(_WIN32)
    enumerate_windows_directory(options, options.root, "", -1, arena, records, result);
#else
    enumerate_posix_directory(options, options.root, "", -1, arena, records, result);
#endif

    return result;
}

} // namespace gotool::project_scanner
