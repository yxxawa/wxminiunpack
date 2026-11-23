#include "wedecode/common.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace wedecode {
namespace {

std::string base64_encode(const std::vector<std::byte>& bytes) {
  static constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t offset = 0; offset < bytes.size(); offset += 3U) {
    const auto first = std::to_integer<unsigned int>(bytes[offset]);
    const auto second = offset + 1U < bytes.size() ? std::to_integer<unsigned int>(bytes[offset + 1U]) : 0U;
    const auto third = offset + 2U < bytes.size() ? std::to_integer<unsigned int>(bytes[offset + 2U]) : 0U;
    const auto block = (first << 16U) | (second << 8U) | third;
    encoded.push_back(alphabet[(block >> 18U) & 0x3fU]);
    encoded.push_back(alphabet[(block >> 12U) & 0x3fU]);
    encoded.push_back(offset + 1U < bytes.size() ? alphabet[(block >> 6U) & 0x3fU] : '=');
    encoded.push_back(offset + 2U < bytes.size() ? alphabet[block & 0x3fU] : '=');
  }
  return encoded;
}

}  // namespace

std::wstring utf8_to_wide(std::string_view value) {
  if (value.empty()) return {};
  const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
  return result;
}

std::string wide_to_utf8(std::wstring_view value) {
  if (value.empty()) return {};
  const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
  return result;
}

std::string json_escape(std::string_view value) {
  std::ostringstream out;
  for (const unsigned char c : value) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
        } else {
          out << static_cast<char>(c);
        }
    }
  }
  return out.str();
}

fs::path path_from_utf8(const std::string_view value) {
  return fs::path(utf8_to_wide(value));
}

fs::path executable_directory() {
  std::array<wchar_t, 32768> buffer{};
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) throw std::runtime_error("Could not resolve executable path");
  return fs::path(std::wstring_view(buffer.data(), length)).parent_path();
}

fs::path application_data_directory() {
  std::array<wchar_t, 32768> buffer{};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
  fs::path base;
  if (length != 0 && length < buffer.size()) {
    base = fs::path(std::wstring_view(buffer.data(), length));
  } else {
    base = fs::temp_directory_path();
  }
  return base / L"微信小程序一键反编译";
}

std::string timestamp_for_directory() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_s(&local, &time);
  std::ostringstream out;
  out << std::put_time(&local, "%Y%m%d-%H%M%S");
  return out.str();
}

fs::path make_unique_directory(const fs::path& parent, std::string_view base_name) {
  fs::create_directories(parent);
  const std::string base(base_name);
  for (std::size_t index = 1;; ++index) {
    const auto candidate = parent / path_from_utf8(index == 1 ? base : base + "-" + std::to_string(index));
    std::error_code ec;
    if (fs::create_directory(candidate, ec)) return candidate;
    if (ec && ec != std::errc::file_exists) throw fs::filesystem_error("Could not create output directory", candidate, ec);
  }
}

std::string read_text_utf8(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Could not read " + wide_to_utf8(path.wstring()));
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

std::vector<std::byte> read_binary(const fs::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("Could not read " + wide_to_utf8(path.wstring()));
  const auto size = input.tellg();
  if (size < 0) throw std::runtime_error("Could not determine size of " + wide_to_utf8(path.wstring()));
  std::vector<std::byte> data(static_cast<std::size_t>(size));
  input.seekg(0);
  if (!data.empty() && !input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
    throw std::runtime_error("Could not read all bytes from " + wide_to_utf8(path.wstring()));
  }
  return data;
}

std::string png_data_uri(const fs::path& path, const std::size_t max_bytes) {
  if (path.empty() || max_bytes == 0) return {};
  try {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > max_bytes) return {};
    return "data:image/png;base64," + base64_encode(read_binary(path));
  } catch (...) {
    return {};
  }
}

void write_text_utf8(const fs::path& path, std::string_view text, bool overwrite) {
  fs::create_directories(path.parent_path());
  if (!overwrite && fs::exists(path)) return;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("Could not write " + wide_to_utf8(path.wstring()));
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!output) throw std::runtime_error("Could not finish writing " + wide_to_utf8(path.wstring()));
}

void write_binary(const fs::path& path, const std::vector<std::byte>& bytes, bool overwrite) {
  fs::create_directories(path.parent_path());
  if (!overwrite && fs::exists(path)) return;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("Could not write " + wide_to_utf8(path.wstring()));
  if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("Could not finish writing " + wide_to_utf8(path.wstring()));
}

std::string normalise_relative_path(std::string_view packed_path) {
  std::string path(packed_path);
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  return path;
}

bool is_safe_relative_path(std::string_view packed_path) {
  const std::string path = normalise_relative_path(packed_path);
  if (path.empty() || path.find_first_of(":<>\"|?*") != std::string::npos ||
      std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 0x20; })) return false;
  fs::path relative = path_from_utf8(path).lexically_normal();
  if (relative.empty() || relative.is_absolute()) return false;
  for (const auto& part : relative) {
    const auto text = to_lower_ascii(wide_to_utf8(part.wstring()));
    if (text.empty() || text == "." || text == ".." || text.back() == '.' || text.back() == ' ') return false;
    const auto stem = text.substr(0, text.find('.'));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
         stem[3] >= '1' && stem[3] <= '9')) return false;
  }
  return true;
}

std::string trim(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
  return std::string(value.substr(begin, end - begin));
}

std::string to_lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

Logger::Logger(Sink sink) : sink_(std::move(sink)) {}

void Logger::write(std::string_view level, std::string_view message) const {
  if (!sink_) return;
  sink_("[" + std::string(level) + "] " + std::string(message));
}

void Logger::info(std::string_view message) const { write("INFO", message); }
void Logger::success(std::string_view message) const { write("OK", message); }
void Logger::warning(std::string_view message) const { write("WARN", message); }
void Logger::error(std::string_view message) const { write("ERROR", message); }

}  // namespace wedecode


