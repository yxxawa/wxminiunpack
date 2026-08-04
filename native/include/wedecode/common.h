#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wedecode {

std::wstring utf8_to_wide(std::string_view value);
std::string wide_to_utf8(std::wstring_view value);
std::string json_escape(std::string_view value);
std::filesystem::path path_from_utf8(std::string_view value);
std::filesystem::path executable_directory();
std::filesystem::path application_data_directory();
std::string timestamp_for_directory();
std::filesystem::path make_unique_directory(const std::filesystem::path& parent, std::string_view base_name);
std::string read_text_utf8(const std::filesystem::path& path);
std::vector<std::byte> read_binary(const std::filesystem::path& path);
std::string png_data_uri(const std::filesystem::path& path,
                         std::size_t max_bytes = 2U * 1024U * 1024U);
void write_text_utf8(const std::filesystem::path& path, std::string_view text, bool overwrite = true);
void write_binary(const std::filesystem::path& path, const std::vector<std::byte>& bytes, bool overwrite = true);
std::string normalise_relative_path(std::string_view packed_path);
bool is_safe_relative_path(std::string_view packed_path);
std::string trim(std::string_view value);
std::string to_lower_ascii(std::string value);

class Logger {
public:
  using Sink = std::function<void(std::string_view)>;

  explicit Logger(Sink sink = {});
  void info(std::string_view message) const;
  void success(std::string_view message) const;
  void warning(std::string_view message) const;
  void error(std::string_view message) const;

private:
  Sink sink_;
  void write(std::string_view level, std::string_view message) const;
};

}  // namespace wedecode

