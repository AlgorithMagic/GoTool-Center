// Copyright 2026 AlgorithMagic

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "project_scanner/native_script_parser.hpp"

namespace {

constexpr std::string_view FUZZ_EXTENSION = ".gd";
constexpr std::size_t MAX_FUZZ_INPUT_BYTES = static_cast<const std::size_t>(256U) * 1024U;

constexpr int64_t MAX_PARSE_LINES = static_cast<const int64_t>(1024);
constexpr int64_t MAX_PARSE_BYTES = static_cast<const int64_t>(256) * 1024;
constexpr int64_t MAX_PARSE_TOKENS = static_cast<const int64_t>(64) * 1024;
constexpr int64_t MAX_PARSE_DEPENDENCIES = static_cast<const int64_t>(8) * 1024;

std::optional<std::filesystem::path> make_fuzz_input_path()
{
    std::error_code error;

    const std::filesystem::path temp_directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return std::nullopt;
    }

    const std::filesystem::path directory = temp_directory / "gotool_fuzz_native_script_parser";
    std::filesystem::create_directories(directory, error);
    if (error) {
        return std::nullopt;
    }

    return directory / "input.gd";
}

bool write_bytes_to_file(const std::filesystem::path& path, const uint8_t* data, std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));

    output.flush();

    return output.good();
}

void remove_file_noexcept(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    if (data == nullptr || size == 0 || size > MAX_FUZZ_INPUT_BYTES) {
        return 0;
    }

    const std::optional<std::filesystem::path> input_path = make_fuzz_input_path();
    if (!input_path.has_value()) {
        return 0;
    }

    if (!write_bytes_to_file(*input_path, data, size)) {
        remove_file_noexcept(*input_path);
        return 0;
    }

    const gotool::project_scanner::ScriptParseResult result =
        gotool::project_scanner::parse_script_intelligence(
            *input_path, FUZZ_EXTENSION, gotool::project_scanner::ParseTier::FullSymbols,
            MAX_PARSE_LINES, MAX_PARSE_BYTES, MAX_PARSE_TOKENS, MAX_PARSE_DEPENDENCIES);

    static_cast<void>(result);

    remove_file_noexcept(*input_path);

    return 0;
}