#include "project_context/gotool_project_context_helpers.hpp"

#include <cctype>
#include <fstream>
#include <string_view>
#include <utility>

namespace godot {

namespace {

std::string trim_ascii(std::string_view value) {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return std::string(value.substr(begin, end - begin));
}

std::string trim_inline_comment(const std::string &value) {
  const size_t comment = value.find(';');
  if (comment == std::string::npos) {
    return trim_ascii(value);
  }
  return trim_ascii(std::string_view(value).substr(0, comment));
}

std::string unquote(std::string value) {
  if (value.size() >= 2) {
    const char first = value.front();
    const char last = value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      return value.substr(1, value.size() - 2);
    }
  }
  return value;
}

} // namespace

std::vector<ParsedAutoload>
parse_project_autoloads(const std::filesystem::path &project_root) {
  std::vector<ParsedAutoload> autoloads;
  const std::filesystem::path project_file = project_root / "project.godot";

  std::ifstream input(project_file, std::ios::in | std::ios::binary);
  if (!input.is_open()) {
    return autoloads;
  }

  bool in_autoload_section = false;
  std::string line;
  while (std::getline(input, line)) {
    const std::string trimmed = trim_ascii(line);
    if (trimmed.empty()) {
      continue;
    }

    if (trimmed.front() == '[' && trimmed.back() == ']') {
      in_autoload_section = (trimmed == "[autoload]");
      continue;
    }

    if (!in_autoload_section || trimmed.front() == ';' ||
        trimmed.front() == '#') {
      continue;
    }

    const size_t separator = trimmed.find('=');
    if (separator == std::string::npos) {
      continue;
    }

    const std::string key =
        trim_ascii(std::string_view(trimmed).substr(0, separator));
    std::string value = trim_inline_comment(trimmed.substr(separator + 1));
    if (key.empty() || value.empty()) {
      continue;
    }

    value = unquote(value);

    ParsedAutoload entry;
    entry.autoload_name = key;
    entry.is_singleton = !value.empty() && value.front() == '*';
    if (entry.is_singleton) {
      value.erase(value.begin());
    }

    entry.target_path = trim_ascii(value);
    if (!entry.target_path.empty()) {
      autoloads.push_back(std::move(entry));
    }
  }

  return autoloads;
}

} // namespace godot