// Copyright 2026 AlgorithMagic

#include "project_scanner/native_script_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path make_fuzz_input_path() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "gotool_fuzz";

  std::filesystem::create_directories(directory);

  return directory / "native_script_parser_input.gd";
}

void write_bytes_to_file(const std::filesystem::path &path, const uint8_t *data,
                         size_t size) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);

  if (!output.is_open()) {
    return;
  }

  output.write(reinterpret_cast<const char *>(data),
               static_cast<std::streamsize>(size));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (data == nullptr) {
    return 0;
  }

  if (size > 256 * 1024) {
    return 0;
  }

  const std::filesystem::path input_path = make_fuzz_input_path();
  write_bytes_to_file(input_path, data, size);

  const gotool::project_scanner::ScriptParseResult result =
      gotool::project_scanner::parse_script_intelligence(
          input_path, ".gd", gotool::project_scanner::ParseTier::FullSymbols,
          1024, 256 * 1024, 64 * 1024, 8 * 1024);

  static_cast<void>(result);

  std::error_code remove_error;
  std::filesystem::remove(input_path, remove_error);

  return 0;
}