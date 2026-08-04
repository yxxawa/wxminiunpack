#include "wedecode/wechat_cache_scanner.h"

#include "wedecode/wxapkg.h"

#include <windows.h>
#include <psapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace wedecode {
namespace {

constexpr std::string_view wxid_prefix = "wx";

struct RootDescriptor {
  fs::path applet_root;
  std::string account_key;
};

struct PackageInspection {
  std::string app_name;
  int app_name_score{};
  std::string kind{"缓存包"};
  std::set<std::string> plugin_wxids;
  std::set<std::string> declared_subpackages;
  std::set<std::string> available_subpackages;
  bool wasmcode2_platform_conditional{};
  bool data_package_uses_runtime_or_external_source{};
};

struct CacheCandidate {
  fs::path applet_root;
  fs::path package_root;
  fs::path icon_path;
  std::string icon_data_uri;
  std::string wxid;
  std::string version;
  std::string account_key;
  std::vector<fs::path> package_files;
  fs::path runtime_cache_path;
  std::size_t package_count{};
  fs::file_time_type newest_write{};
};

struct PackageSetSelection {
  fs::path selected_root;
  std::string version;
  std::vector<fs::path> packages;
};

bool cancelled(const std::atomic_bool* flag) {
  return flag && flag->load();
}

std::size_t parallel_worker_count(const std::size_t item_count, const std::size_t maximum = 6) {
  if (item_count == 0) return 0;
  const auto hardware = std::thread::hardware_concurrency();
  const auto available = hardware == 0 ? std::size_t{4} : static_cast<std::size_t>(hardware);
  return std::min(item_count, std::max<std::size_t>(1, std::min(maximum, available)));
}

template <typename Function>
std::size_t parallel_for(const std::size_t item_count,
                         const std::atomic_bool* cancel_requested,
                         const std::size_t maximum_workers,
                         Function&& function) {
  const auto worker_count = parallel_worker_count(item_count, maximum_workers);
  if (worker_count == 0) return 0;
  if (worker_count == 1) {
    for (std::size_t index = 0; index < item_count && !cancelled(cancel_requested); ++index) function(index);
    return 1;
  }

  std::atomic_size_t next_index{};
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&] {
      while (!cancelled(cancel_requested)) {
        const auto index = next_index.fetch_add(1, std::memory_order_relaxed);
        if (index >= item_count) break;
        function(index);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  return worker_count;
}

bool is_wxid(const std::string_view value) {
  if (value.size() != 18 || value.substr(0, wxid_prefix.size()) != wxid_prefix) return false;
  return std::all_of(value.begin() + 2, value.end(), [](const unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

std::optional<fs::path> known_folder(const KNOWNFOLDERID& id) {
  PWSTR raw{};
  const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
  if (FAILED(result) || !raw) return std::nullopt;
  fs::path path(raw);
  CoTaskMemFree(raw);
  return path;
}

std::optional<fs::path> registry_wechat_file_save_path() {
  wchar_t buffer[32768]{};
  DWORD size = sizeof(buffer);
  const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
                                      L"SOFTWARE\\Tencent\\WeChat",
                                      L"FileSavePath",
                                      RRF_RT_REG_SZ,
                                      nullptr,
                                      buffer,
                                      &size);
  if (status != ERROR_SUCCESS || buffer[0] == L'\0') return std::nullopt;
  std::wstring value(buffer);
  if (value == L"MyDocument:") return known_folder(FOLDERID_Documents);
  return fs::path(value);
}

void add_unique_path(std::vector<fs::path>& paths, const fs::path& path) {
  if (path.empty()) return;
  const auto normalized = fs::absolute(path).lexically_normal();
  const auto key = to_lower_ascii(wide_to_utf8(normalized.wstring()));
  const bool duplicate = std::any_of(paths.begin(), paths.end(), [&key](const fs::path& existing) {
    return to_lower_ascii(wide_to_utf8(existing.wstring())) == key;
  });
  if (!duplicate) paths.push_back(normalized);
}

void add_root_descriptor(std::vector<RootDescriptor>& roots, const fs::path& applet_root, std::string account_key) {
  std::error_code error;
  if (!fs::is_directory(applet_root, error) || error) return;
  const auto normalized = fs::absolute(applet_root).lexically_normal();
  const auto key = to_lower_ascii(wide_to_utf8(normalized.wstring()));
  const bool duplicate = std::any_of(roots.begin(), roots.end(), [&key](const RootDescriptor& existing) {
    return to_lower_ascii(wide_to_utf8(existing.applet_root.wstring())) == key;
  });
  if (!duplicate) roots.push_back({normalized, std::move(account_key)});
}

std::vector<RootDescriptor> discover_applet_roots() {
  std::vector<RootDescriptor> roots;

  if (const auto roaming = known_folder(FOLDERID_RoamingAppData)) {
    const auto radium = *roaming / L"Tencent" / L"xwechat" / L"radium";
    const auto users = radium / L"users";
    std::error_code error;
    for (fs::directory_iterator iterator(users, fs::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
      const auto entry = *iterator;
      if (!entry.is_directory(error) || error) {
        error.clear();
        continue;
      }
      add_root_descriptor(roots, entry.path() / L"applet", wide_to_utf8(entry.path().filename().wstring()));
    }
    add_root_descriptor(roots, radium / L"Applet", "shared-v4");
    add_root_descriptor(roots, radium / L"applet", "shared-v4");
  }

  std::vector<fs::path> legacy_bases;
  if (const auto configured = registry_wechat_file_save_path()) add_unique_path(legacy_bases, *configured);
  if (const auto documents = known_folder(FOLDERID_Documents)) add_unique_path(legacy_bases, *documents);

  for (const auto& base : legacy_bases) {
    add_root_descriptor(roots, base / L"WeChat Files" / L"Applet", "legacy-v3");
    add_root_descriptor(roots, base / L"Applet", "legacy-v3");
  }

  return roots;
}

std::vector<fs::path> find_wxapkg_files(const fs::path& directory) {
  std::vector<fs::path> packages;
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, error), end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }
    const auto entry = *iterator;
    if (!entry.is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    if (to_lower_ascii(wide_to_utf8(entry.path().extension().wstring())) == ".wxapkg") packages.push_back(entry.path());
  }
  return packages;
}

std::optional<unsigned long long> numeric_version(const fs::path& path) {
  const auto text = wide_to_utf8(path.filename().wstring());
  if (text.empty() || !std::all_of(text.begin(), text.end(), [](const unsigned char c) { return std::isdigit(c) != 0; })) {
    return std::nullopt;
  }
  unsigned long long value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc()) return std::nullopt;
  return value;
}

std::string package_slot_key(const fs::path& package) {
  return to_lower_ascii(wide_to_utf8(package.filename().wstring()));
}

std::optional<std::string> subpackage_name_from_filename(const fs::path& package) {
  const auto name = package_slot_key(package);
  constexpr std::string_view extension = ".wxapkg";
  if (!name.ends_with(extension)) return std::nullopt;

  constexpr std::string_view multi_prefix = "_subpackages_";
  constexpr std::string_view wrapped_suffix = "_.wxapkg";
  if (name.starts_with(multi_prefix) && name.ends_with(wrapped_suffix)) {
    const auto length = name.size() - multi_prefix.size() - wrapped_suffix.size();
    if (length != 0) return name.substr(multi_prefix.size(), length);
  }
  if (name.starts_with('_') && name.ends_with(wrapped_suffix) &&
      name != "__app__.wxapkg" && name != "__full__.wxapkg" &&
      name != "__subpackage__.wxapkg" && name != "__plugincode__.wxapkg" &&
      name != "__without_multi_plugincode__.wxapkg") {
    const auto length = name.size() - 1U - wrapped_suffix.size();
    if (length != 0) return name.substr(1, length);
  }
  return std::nullopt;
}

void record_available_subpackage_path(PackageInspection& inspection, std::string_view path) {
  while (!path.empty() && path.front() == '/') path.remove_prefix(1);
  while (!path.empty()) {
    const auto slash = path.find('/');
    const auto segment = path.substr(0, slash);
    if (!segment.empty() && segment != "." && segment != ".." && segment.find('.') == std::string_view::npos) {
      inspection.available_subpackages.insert(std::string(segment));
    }
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
  }
}

PackageSetSelection collect_package_set(const fs::path& wxid_root) {
  struct VersionPackages {
    unsigned long long number{};
    fs::path root;
    std::vector<fs::path> packages;
  };

  std::vector<VersionPackages> versions;
  std::error_code error;
  for (fs::directory_iterator iterator(wxid_root, fs::directory_options::skip_permission_denied, error), end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }
    const auto entry = *iterator;
    if (!entry.is_directory(error) || error) {
      error.clear();
      continue;
    }
    const auto number = numeric_version(entry.path());
    if (!number) continue;
    auto packages = find_wxapkg_files(entry.path());
    if (packages.empty()) continue;
    std::stable_sort(packages.begin(), packages.end(), [](const fs::path& left, const fs::path& right) {
      return package_slot_key(left) < package_slot_key(right);
    });
    versions.push_back({*number, entry.path(), std::move(packages)});
  }

  if (versions.empty()) {
    auto packages = find_wxapkg_files(wxid_root);
    std::stable_sort(packages.begin(), packages.end(), [](const fs::path& left, const fs::path& right) {
      return package_slot_key(left) < package_slot_key(right);
    });
    return {wxid_root, {}, std::move(packages)};
  }

  std::stable_sort(versions.begin(), versions.end(), [](const VersionPackages& left, const VersionPackages& right) {
    return left.number > right.number;
  });

  PackageSetSelection selection;
  selection.selected_root = versions.front().root;
  selection.version = wide_to_utf8(versions.front().root.filename().wstring());
  std::size_t package_count{};
  for (const auto& version : versions) package_count += version.packages.size();
  selection.packages.reserve(package_count);
  for (auto& version : versions) {
    selection.packages.insert(selection.packages.end(),
                              std::make_move_iterator(version.packages.begin()),
                              std::make_move_iterator(version.packages.end()));
  }
  return selection;
}

std::map<std::string, fs::path> index_icons(const fs::path& applet_root) {
  std::map<std::string, std::pair<fs::file_time_type, fs::path>> newest_icons;
  const auto icon_root = applet_root / L"icon";
  std::error_code error;
  for (fs::directory_iterator iterator(icon_root, fs::directory_options::skip_permission_denied, error), end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }
    const auto entry = *iterator;
    if (!entry.is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    const auto filename = to_lower_ascii(wide_to_utf8(entry.path().filename().wstring()));
    const auto extension = to_lower_ascii(wide_to_utf8(entry.path().extension().wstring()));
    if (extension != ".png" || filename.size() <= 18 || filename[18] != '_') continue;
    const auto wxid = filename.substr(0, 18);
    if (!is_wxid(wxid)) continue;
    const auto write = entry.last_write_time(error);
    if (error) {
      error.clear();
      continue;
    }
    const auto current = newest_icons.find(wxid);
    if (current == newest_icons.end() || write > current->second.first) {
      newest_icons[wxid] = {write, entry.path()};
    }
  }

  std::map<std::string, fs::path> result;
  for (auto& [wxid, icon] : newest_icons) {
    result.emplace(std::move(wxid), std::move(icon.second));
  }
  return result;
}

std::vector<CacheCandidate> candidates_from_root(
    const RootDescriptor& root,
    const std::function<void(const CacheCandidate&)>& candidate_sink = {}) {
  std::vector<CacheCandidate> candidates;
  std::vector<fs::path> package_parents;
  const auto icons = index_icons(root.applet_root);
  std::error_code error;
  if (fs::is_directory(root.applet_root / L"packages", error) && !error) package_parents.push_back(root.applet_root / L"packages");
  error.clear();
  package_parents.push_back(root.applet_root);

  std::set<std::string> visited;
  for (const auto& package_parent : package_parents) {
    for (fs::directory_iterator iterator(package_parent, fs::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
      const auto entry = *iterator;
      if (!entry.is_directory(error) || error) {
        error.clear();
        continue;
      }
      const auto wxid = wide_to_utf8(entry.path().filename().wstring());
      if (!is_wxid(wxid) || !visited.insert(to_lower_ascii(wxid)).second) continue;
      auto package_set = collect_package_set(entry.path());
      if (package_set.packages.empty()) continue;

      fs::file_time_type newest{};
      for (const auto& package : package_set.packages) {
        const auto write = fs::last_write_time(package, error);
        if (error) {
          error.clear();
          continue;
        }
        if (write > newest) newest = write;
      }
      const auto runtime_cache = root.applet_root / L"local" / utf8_to_wide(wxid) / L"usr" / L"__GAME_FILE_CACHE";
      error.clear();
      const auto runtime_cache_path = fs::is_directory(runtime_cache, error) && !error ? runtime_cache : fs::path{};
      const auto package_count = package_set.packages.size();
      const auto icon = icons.find(to_lower_ascii(wxid));
      candidates.push_back({root.applet_root,
                            std::move(package_set.selected_root),
                            icon == icons.end() ? fs::path{} : icon->second,
                            icon == icons.end() ? std::string{} : png_data_uri(icon->second),
                            wxid,
                            std::move(package_set.version),
                            root.account_key,
                            std::move(package_set.packages),
                            runtime_cache_path,
                            package_count,
                            newest});
      if (candidate_sink) candidate_sink(candidates.back());
    }
    error.clear();
  }
  return candidates;
}

std::string json_unescape_string(const std::string_view text, std::size_t& position) {
  std::string result;
  if (position >= text.size() || text[position] != '"') return result;
  ++position;
  while (position < text.size()) {
    const char c = text[position++];
    if (c == '"') break;
    if (c != '\\') {
      result.push_back(c);
      continue;
    }
    if (position >= text.size()) break;
    const char escaped = text[position++];
    switch (escaped) {
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      case 'u': {
        if (position + 4 > text.size()) break;
        unsigned int codepoint{};
        const auto parsed = std::from_chars(text.data() + position, text.data() + position + 4, codepoint, 16);
        if (parsed.ec != std::errc()) break;
        position += 4;
        if (codepoint <= 0x7fU) {
          result.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
          result.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
          result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
          result.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
          result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
          result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
        break;
      }
      default: result.push_back(escaped); break;
    }
  }
  return result;
}

void append_utf8_codepoint(std::string& output, const unsigned int codepoint) {
  if (codepoint <= 0x7fU) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else if (codepoint <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else {
    output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  }
}

std::string javascript_unescape_string(const std::string_view text, std::size_t& position) {
  std::string result;
  if (position >= text.size() || (text[position] != '"' && text[position] != '\'')) return result;
  const char quote = text[position++];
  while (position < text.size()) {
    const char c = text[position++];
    if (c == quote) break;
    if (c != '\\') {
      result.push_back(c);
      continue;
    }
    if (position >= text.size()) break;
    const char escaped = text[position++];
    switch (escaped) {
      case '"': result.push_back('"'); break;
      case '\'': result.push_back('\''); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      case 'v': result.push_back('\v'); break;
      case 'x': {
        if (position + 2 > text.size()) break;
        unsigned int codepoint{};
        const auto parsed = std::from_chars(text.data() + position, text.data() + position + 2, codepoint, 16);
        if (parsed.ec != std::errc()) break;
        position += 2;
        append_utf8_codepoint(result, codepoint);
        break;
      }
      case 'u': {
        if (position + 4 > text.size()) break;
        unsigned int codepoint{};
        const auto parsed = std::from_chars(text.data() + position, text.data() + position + 4, codepoint, 16);
        if (parsed.ec != std::errc()) break;
        position += 4;
        if (codepoint >= 0xd800U && codepoint <= 0xdbffU && position + 6 <= text.size() &&
            text[position] == '\\' && text[position + 1] == 'u') {
          unsigned int low{};
          const auto low_parsed =
              std::from_chars(text.data() + position + 2, text.data() + position + 6, low, 16);
          if (low_parsed.ec == std::errc() && low >= 0xdc00U && low <= 0xdfffU) {
            position += 6;
            codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
          }
        }
        append_utf8_codepoint(result, codepoint);
        break;
      }
      case '\r':
        if (position < text.size() && text[position] == '\n') ++position;
        break;
      case '\n': break;
      default: result.push_back(escaped); break;
    }
  }
  return result;
}

struct ScriptStringValue {
  std::string value;
  std::size_t key_position{};
  std::size_t value_position{};
};

bool javascript_identifier_character(const char value) {
  const auto c = static_cast<unsigned char>(value);
  return std::isalnum(c) != 0 || value == '_' || value == '$';
}

std::vector<ScriptStringValue> script_string_values(const std::string_view text, const std::string_view key) {
  std::vector<ScriptStringValue> values;
  std::size_t search{};
  while (search < text.size()) {
    const auto found = text.find(key, search);
    if (found == std::string_view::npos) break;
    search = found + key.size();

    std::size_t position = found + key.size();
    const bool quoted_key =
        found > 0 && position < text.size() &&
        (text[found - 1] == '"' || text[found - 1] == '\'') && text[position] == text[found - 1];
    if (quoted_key) {
      ++position;
    } else {
      if ((found > 0 && javascript_identifier_character(text[found - 1])) ||
          (position < text.size() && javascript_identifier_character(text[position]))) {
        continue;
      }
    }

    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position >= text.size() || (text[position] != ':' && text[position] != '=')) continue;
    ++position;
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position >= text.size() || (text[position] != '"' && text[position] != '\'')) continue;

    const auto value_position = position;
    auto value = trim(javascript_unescape_string(text, position));
    if (!value.empty()) values.push_back({std::move(value), found, value_position});
  }
  return values;
}

std::optional<std::pair<std::size_t, std::size_t>> balanced_object_range(const std::string_view text,
                                                                         const std::size_t open) {
  if (open >= text.size() || text[open] != '{') return std::nullopt;
  std::size_t depth{};
  char quote{};
  bool escaped{};
  for (std::size_t position = open; position < text.size(); ++position) {
    const char c = text[position];
    if (quote != 0) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '"' || c == '\'' || c == '`') {
      quote = c;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (depth == 0) return std::nullopt;
      --depth;
      if (depth == 0) return std::pair{open, position};
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> json_object_value(const std::string_view text, const std::string_view key) {
  const std::string field = "\"" + std::string(key) + "\"";
  std::size_t search{};
  while (search < text.size()) {
    const auto found = text.find(field, search);
    if (found == std::string_view::npos) break;
    auto position = found + field.size();
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position >= text.size() || text[position] != ':') {
      search = found + field.size();
      continue;
    }
    ++position;
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position >= text.size() || text[position] != '{') {
      search = position;
      continue;
    }
    if (const auto range = balanced_object_range(text, position)) {
      return text.substr(range->first, range->second - range->first + 1);
    }
    search = position + 1;
  }
  return std::nullopt;
}

std::vector<std::string> json_string_values(const std::string_view text, const std::string_view key) {
  std::vector<std::string> values;
  const std::string field = "\"" + std::string(key) + "\"";
  std::size_t search{};
  while (search < text.size()) {
    const auto found = text.find(field, search);
    if (found == std::string_view::npos) break;
    auto position = found + field.size();
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position >= text.size() || text[position] != ':') {
      search = position;
      continue;
    }
    ++position;
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position < text.size() && text[position] == '"') {
      auto value = trim(json_unescape_string(text, position));
      if (!value.empty()) values.push_back(std::move(value));
    }
    search = std::max(position, found + field.size());
  }
  return values;
}

std::optional<std::string> json_string_value(const std::string_view text, const std::string_view key) {
  auto values = json_string_values(text, key);
  if (values.empty()) return std::nullopt;
  return std::move(values.front());
}

std::optional<int> json_integer_value(const std::string_view text, const std::string_view key) {
  const std::string field = "\"" + std::string(key) + "\"";
  auto position = text.find(field);
  if (position == std::string_view::npos) return std::nullopt;
  position = text.find(':', position + field.size());
  if (position == std::string_view::npos) return std::nullopt;
  ++position;
  while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
  const auto begin = text.data() + position;
  const auto end = text.data() + text.size();
  int value{};
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc()) return std::nullopt;
  return value;
}

std::optional<bool> json_boolean_value(const std::string_view text, const std::string_view key) {
  const std::string field = "\"" + std::string(key) + "\"";
  auto position = text.find(field);
  if (position == std::string_view::npos) return std::nullopt;
  position = text.find(':', position + field.size());
  if (position == std::string_view::npos) return std::nullopt;
  ++position;
  while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
  if (text.substr(position, 4) == "true") return true;
  if (text.substr(position, 5) == "false") return false;
  return std::nullopt;
}

std::vector<std::string> json_string_array_values(const std::string_view text, const std::string_view key) {
  std::vector<std::string> values;
  const std::string field = "\"" + std::string(key) + "\"";
  auto position = text.find(field);
  if (position == std::string_view::npos) return values;
  position = text.find(':', position + field.size());
  if (position == std::string_view::npos) return values;
  position = text.find('[', position + 1);
  if (position == std::string_view::npos) return values;
  ++position;
  while (position < text.size()) {
    while (position < text.size() &&
           (std::isspace(static_cast<unsigned char>(text[position])) || text[position] == ',')) {
      ++position;
    }
    if (position >= text.size() || text[position] == ']') break;
    if (text[position] != '"') {
      ++position;
      continue;
    }
    auto value = json_unescape_string(text, position);
    if (!value.empty()) values.push_back(std::move(value));
  }
  return values;
}

void write_cache_text_atomically(const fs::path& path, const std::string_view text) {
  fs::create_directories(path.parent_path());
  auto temporary = path;
  temporary += L".tmp";
  write_text_utf8(temporary, text);
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const auto move_error = GetLastError();
    std::error_code ignored;
    fs::remove(temporary, ignored);
    throw std::system_error(static_cast<int>(move_error), std::system_category(),
                            "Could not replace cache file");
  }
}

struct PersistentNameMap {
  std::map<std::string, std::string> names;
  bool file_exists{};
};

PersistentNameMap load_persistent_name_map(const fs::path& cache_directory) {
  PersistentNameMap result;
  const auto path = cache_directory / L"app-name-map.json";
  std::error_code error;
  result.file_exists = fs::is_regular_file(path, error) && !error;
  if (!result.file_exists) return result;
  try {
    const auto text = read_text_utf8(path);
    if (json_integer_value(text, "version").value_or(0) != 1) {
      result.file_exists = false;
      return result;
    }
    const auto wxids = json_string_values(text, "wxid");
    const auto names = json_string_values(text, "name");
    const auto count = std::min(wxids.size(), names.size());
    for (std::size_t index = 0; index < count; ++index) {
      const auto wxid = to_lower_ascii(trim(wxids[index]));
      const auto name = trim(names[index]);
      if (is_wxid(wxid) && !name.empty()) result.names[wxid] = name;
    }
  } catch (...) {
    result.names.clear();
  }
  return result;
}

void save_persistent_name_map(const fs::path& cache_directory,
                              const std::map<std::string, std::string>& names) {
  std::ostringstream output;
  output << "{\n  \"version\": 1,\n  \"entries\": [";
  bool first = true;
  for (const auto& [wxid, name] : names) {
    if (!is_wxid(wxid) || name.empty()) continue;
    output << (first ? "\n" : ",\n")
           << "    {\"wxid\":\"" << json_escape(wxid)
           << "\",\"name\":\"" << json_escape(name) << "\"}";
    first = false;
  }
  if (!first) output << '\n';
  output << "  ]\n}\n";
  write_cache_text_atomically(cache_directory / L"app-name-map.json", output.str());
}

std::uint64_t fnv1a_append(std::uint64_t hash, const std::string_view value) {
  constexpr std::uint64_t prime = 1099511628211ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= prime;
  }
  hash ^= 0xffU;
  hash *= prime;
  return hash;
}

std::string hexadecimal_hash(const std::uint64_t value) {
  std::ostringstream output;
  output << std::hex << std::setw(16) << std::setfill('0') << value;
  return output.str();
}

std::string analysis_cache_key(const CacheCandidate& candidate) {
  return to_lower_ascii(wide_to_utf8(fs::absolute(candidate.applet_root).lexically_normal().wstring())) +
         "\n" + to_lower_ascii(candidate.account_key) + "\n" + to_lower_ascii(candidate.wxid);
}

std::string package_signature(const CacheCandidate& candidate) {
  constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
  std::uint64_t hash = offset_basis;
  hash = fnv1a_append(hash, to_lower_ascii(candidate.version));
  std::vector<fs::path> packages = candidate.package_files;
  std::stable_sort(packages.begin(), packages.end(), [](const fs::path& left, const fs::path& right) {
    return to_lower_ascii(wide_to_utf8(left.wstring())) < to_lower_ascii(wide_to_utf8(right.wstring()));
  });
  for (const auto& package : packages) {
    std::error_code error;
    auto relative = fs::relative(package, candidate.applet_root, error);
    if (error) {
      error.clear();
      relative = package;
    }
    hash = fnv1a_append(hash, to_lower_ascii(wide_to_utf8(relative.lexically_normal().wstring())));
    const auto size = fs::file_size(package, error);
    hash = fnv1a_append(hash, error ? "size-error" : std::to_string(size));
    error.clear();
    const auto write_time = fs::last_write_time(package, error);
    hash = fnv1a_append(hash, error ? "time-error" :
        std::to_string(write_time.time_since_epoch().count()));
  }
  hash = fnv1a_append(hash, std::to_string(packages.size()));
  return hexadecimal_hash(hash);
}

fs::path analysis_cache_path(const fs::path& cache_directory, const std::string_view key) {
  constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
  return cache_directory / L"package-analysis-v1" /
         utf8_to_wide(hexadecimal_hash(fnv1a_append(offset_basis, key)) + ".json");
}

std::optional<PackageInspection> load_cached_inspection(const fs::path& cache_directory,
                                                        const std::string_view key,
                                                        const std::string_view signature) {
  try {
    const auto text = read_text_utf8(analysis_cache_path(cache_directory, key));
    if (json_integer_value(text, "version").value_or(0) != 1) return std::nullopt;
    if (json_string_value(text, "key").value_or(std::string{}) != key) return std::nullopt;
    if (json_string_value(text, "signature").value_or(std::string{}) != signature) return std::nullopt;

    PackageInspection inspection;
    inspection.app_name = json_string_value(text, "appName").value_or(std::string{});
    inspection.app_name_score = json_integer_value(text, "appNameScore").value_or(0);
    inspection.kind = json_string_value(text, "kind").value_or("缓存包");
    const auto plugins = json_string_array_values(text, "pluginWxids");
    inspection.plugin_wxids.insert(plugins.begin(), plugins.end());
    const auto declared = json_string_array_values(text, "declaredSubpackages");
    inspection.declared_subpackages.insert(declared.begin(), declared.end());
    const auto available = json_string_array_values(text, "availableSubpackages");
    inspection.available_subpackages.insert(available.begin(), available.end());
    inspection.wasmcode2_platform_conditional =
        json_boolean_value(text, "wasmcode2PlatformConditional").value_or(false);
    inspection.data_package_uses_runtime_or_external_source =
        json_boolean_value(text, "dataPackageUsesRuntimeOrExternalSource").value_or(false);
    return inspection;
  } catch (...) {
    return std::nullopt;
  }
}

void append_json_array(std::ostringstream& output,
                       const std::string_view key,
                       const std::set<std::string>& values) {
  output << "  \"" << key << "\":[";
  bool first = true;
  for (const auto& value : values) {
    output << (first ? "" : ",") << "\"" << json_escape(value) << "\"";
    first = false;
  }
  output << ']';
}

void save_cached_inspection(const fs::path& cache_directory,
                            const std::string_view key,
                            const std::string_view signature,
                            const PackageInspection& inspection) {
  std::ostringstream output;
  output << "{\n"
         << "  \"version\":1,\n"
         << "  \"key\":\"" << json_escape(key) << "\",\n"
         << "  \"signature\":\"" << json_escape(signature) << "\",\n"
         << "  \"appName\":\"" << json_escape(inspection.app_name) << "\",\n"
         << "  \"appNameScore\":" << inspection.app_name_score << ",\n"
         << "  \"kind\":\"" << json_escape(inspection.kind) << "\",\n";
  append_json_array(output, "pluginWxids", inspection.plugin_wxids);
  output << ",\n";
  append_json_array(output, "declaredSubpackages", inspection.declared_subpackages);
  output << ",\n";
  append_json_array(output, "availableSubpackages", inspection.available_subpackages);
  output << ",\n"
         << "  \"wasmcode2PlatformConditional\":"
         << (inspection.wasmcode2_platform_conditional ? "true" : "false") << ",\n"
         << "  \"dataPackageUsesRuntimeOrExternalSource\":"
         << (inspection.data_package_uses_runtime_or_external_source ? "true" : "false") << "\n"
         << "}\n";
  write_cache_text_atomically(analysis_cache_path(cache_directory, key), output.str());
}

bool has_cjk(const std::string_view text) {
  for (std::size_t index = 0; index + 2 < text.size(); ++index) {
    const auto first = static_cast<unsigned char>(text[index]);
    const auto second = static_cast<unsigned char>(text[index + 1]);
    const auto third = static_cast<unsigned char>(text[index + 2]);
    if (first >= 0xe4U && first <= 0xe9U && (second & 0xc0U) == 0x80U && (third & 0xc0U) == 0x80U) return true;
  }
  return false;
}

std::size_t utf8_code_point_count(const std::string_view text) {
  return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](const unsigned char value) {
    return (value & 0xc0U) != 0x80U;
  }));
}

bool usable_display_name(const std::string_view candidate) {
  if (candidate.size() < 2 || candidate.size() > 96 || utf8_code_point_count(candidate) < 2 ||
      is_wxid(candidate)) {
    return false;
  }
  if (candidate.find("http://") != std::string_view::npos || candidate.find("https://") != std::string_view::npos) return false;
  if (candidate.find('/') != std::string_view::npos || candidate.find('\\') != std::string_view::npos) return false;
  if (candidate.find_first_of("{}[]<>=+`\"'") != std::string_view::npos) return false;
  if (candidate.find(".appName") != std::string_view::npos || candidate.find(".gameName") != std::string_view::npos) {
    return false;
  }
  if (candidate.find(".concat(") != std::string_view::npos ||
      candidate.find("=>") != std::string_view::npos ||
      candidate.find("function(") != std::string_view::npos ||
      candidate.find("return ") != std::string_view::npos) {
    return false;
  }
  if (!has_cjk(candidate) &&
      (candidate.starts_with('(') || candidate.starts_with(')') || candidate.starts_with('.') ||
       candidate.starts_with(',') || candidate.starts_with(';') || candidate.ends_with(',') ||
       candidate.ends_with(';'))) {
    return false;
  }
  if (candidate.ends_with('_') || candidate.starts_with('_')) return false;
  const auto lower = to_lower_ascii(std::string(candidate));
  constexpr std::array<std::string_view, 70> rejected{
      "main", "internal", "wasmcode", "data-package", "script-bundle", "minigame", "index", "home",
      "微信小程序", "微信小游戏", "weixin", "wechat", "default", "untitled", "app", "game", "plugin",
      "pages", "unknown", "首页", "主页", "我的", "设置", "详情", "登录", "授权", "分享", "加载中",
      "个人中心", "消息", "更多", "发现", "返回", "data", "dev", "effect", "font", "test", "demo",
      "sample", "common", "asset", "assets", "resource", "resources", "bundle", "loading", "login",
      "register", "profile", "user", "component", "service", "vendor", "config", "configuration",
      "launch", "runtime", "debug", "release", "player", "scene", "module", "package",
      "opendatacontext", "open-data-context", "project_wxgame", "wechatgame"};
  if (std::find(rejected.begin(), rejected.end(), lower) != rejected.end()) return false;
  if (candidate.starts_with("__") || candidate.ends_with("__")) return false;
  if (!has_cjk(candidate)) {
    const bool machine_like = std::all_of(candidate.begin(), candidate.end(), [](const unsigned char c) {
      return (c >= 'a' && c <= 'z') || std::isdigit(c) != 0 || c == '_' || c == '-';
    });
    if (machine_like) return false;
    if (candidate.find(' ') == std::string_view::npos && candidate.find('-') != std::string_view::npos) return false;
  }
  return std::none_of(candidate.begin(), candidate.end(), [](const unsigned char c) { return c < 0x20U; });
}

std::optional<std::string> normalized_display_name(std::string candidate) {
  candidate = trim(candidate);
  if (candidate.find('%') != std::string::npos) {
    std::string decoded;
    decoded.reserve(candidate.size());
    for (std::size_t index = 0; index < candidate.size();) {
      if (candidate[index] == '%' && index + 2 < candidate.size()) {
        unsigned int value{};
        const auto parsed = std::from_chars(candidate.data() + index + 1,
                                            candidate.data() + index + 3,
                                            value,
                                            16);
        if (parsed.ec == std::errc() && parsed.ptr == candidate.data() + index + 3) {
          decoded.push_back(static_cast<char>(value));
          index += 3;
          continue;
        }
      }
      decoded.push_back(candidate[index++]);
    }
    candidate = trim(decoded);
  }
  constexpr std::array<std::string_view, 8> prefixes{
      "微信小程序：", "微信小游戏：", "小程序：", "小游戏：",
      "微信小程序:", "微信小游戏:", "小程序:", "小游戏:"};
  for (const auto prefix : prefixes) {
    if (candidate.starts_with(prefix)) {
      candidate = trim(candidate.substr(prefix.size()));
      break;
    }
  }
  if (!usable_display_name(candidate)) return std::nullopt;
  return candidate;
}

std::optional<std::string> best_name_from_text(const std::string_view text) {
  // Only fields that describe the product/application itself belong here.
  // Generic fields such as "name" and "title" commonly describe routes,
  // bundles or assets and previously produced false names such as Data/Dev/Font.
  constexpr std::array<std::string_view, 20> keys{
      "gameName", "appName", "productName", "projectName", "displayName", "miniProgramName", "brandName",
      "storeName", "mallName", "pluginName", "app_name", "game_name", "projectname", "h5_game_name",
      "gameTitle", "appTitle", "appNickName", "appNickname", "wxNickName", "shortName"};
  std::vector<std::string> candidates;
  for (const auto key : keys) {
    for (auto& value : json_string_values(text, key)) {
      if (auto normalized = normalized_display_name(std::move(value))) {
        candidates.push_back(std::move(*normalized));
      }
    }
  }
  if (candidates.empty()) return std::nullopt;
  std::stable_sort(candidates.begin(), candidates.end(), [](const std::string& left, const std::string& right) {
    if (has_cjk(left) != has_cjk(right)) return has_cjk(left);
    return left.size() < right.size();
  });
  return candidates.front();
}

std::optional<std::string> best_page_title_from_text(const std::string_view text) {
  std::vector<std::string> candidates;
  for (auto& value : json_string_values(text, "navigationBarTitleText")) {
    if (auto normalized = normalized_display_name(std::move(value))) {
      candidates.push_back(std::move(*normalized));
    }
  }
  if (candidates.empty()) return std::nullopt;
  std::stable_sort(candidates.begin(), candidates.end(), [](const std::string& left, const std::string& right) {
    if (has_cjk(left) != has_cjk(right)) return has_cjk(left);
    return left.size() < right.size();
  });
  return candidates.front();
}

std::optional<std::string> best_global_title_from_app_config(const std::string_view text) {
  const auto global = json_object_value(text, "global");
  if (!global) return std::nullopt;
  const auto window = json_object_value(*global, "window");
  if (!window) return std::nullopt;
  return best_page_title_from_text(*window);
}

std::optional<std::string> best_entry_title_from_app_config(const std::string_view text) {
  const auto entry_paths = json_string_values(text, "entryPagePath");
  const auto pages = json_object_value(text, "page");
  if (entry_paths.empty() || !pages) return std::nullopt;

  std::vector<std::string> path_variants;
  auto entry_path = trim(entry_paths.front());
  while (!entry_path.empty() && entry_path.front() == '/') entry_path.erase(entry_path.begin());
  if (entry_path.empty()) return std::nullopt;
  path_variants.push_back(entry_path);
  if (entry_path.ends_with(".html")) {
    path_variants.push_back(entry_path.substr(0, entry_path.size() - 5));
  } else {
    path_variants.push_back(entry_path + ".html");
  }

  for (const auto& path : path_variants) {
    const auto page = json_object_value(*pages, path);
    if (!page) continue;
    const auto window = json_object_value(*page, "window");
    if (!window) continue;
    if (auto title = best_page_title_from_text(*window)) return title;
  }
  return std::nullopt;
}

std::optional<std::string> best_serialized_ui_product_name(const std::string_view text) {
  // Cocos Creator frequently serializes a loading-scene node named AppName
  // separately from its label text, for example:
  //   [0,"AppName",3,[[3,"贪吃蛇",...]]]
  // Exact, case-sensitive node markers plus one nearby CJK label keep this
  // rule separate from cross-promotion records using a normal appName field.
  constexpr std::array<std::string_view, 3> markers{"AppName", "GameName", "ProductName"};
  for (const auto marker : markers) {
    const auto literal = "\"" + std::string(marker) + "\"";
    std::size_t search{};
    while (search < text.size()) {
      const auto marker_position = text.find(literal, search);
      if (marker_position == std::string_view::npos) break;
      const auto context_end = std::min(text.size(), marker_position + literal.size() + 384U);
      auto position = marker_position + literal.size();
      while (position < context_end) {
        const auto quote = text.find_first_of("\"'", position);
        if (quote == std::string_view::npos || quote >= context_end) break;
        auto value_end = quote;
        auto value = trim(javascript_unescape_string(text, value_end));
        if (value_end <= quote) {
          position = quote + 1;
          continue;
        }
        position = value_end;
        if (!has_cjk(value)) continue;
        if (auto normalized = normalized_display_name(std::move(value))) {
          return normalized;
        }
      }
      search = marker_position + literal.size();
    }
  }
  return std::nullopt;
}

std::optional<std::string> known_plugin_name(const std::string_view wxid) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 12> names{{
      {"wx0446ba2621dda60a", "Cocos Creator 3.x 引擎插件"},
      {"wx2b03c6e691cd7370", "微信直播播放器插件"},
      {"wx2ea687f4258401a9", "微信小游戏社交中心插件"},
      {"wx7095f7fa398a2f30", "Cocos Creator 2.x 引擎插件"},
      {"wx709bed2b09b9df79", "Cocos Creator 引擎插件"},
      {"wx70d8aa25ec591f7a", "Laya 引擎插件"},
      {"wx722e01aaedff584c", "微信小游戏玩家资料插件"},
      {"wx7a727ff7d940bb3f", "Layout 排版引擎插件"},
      {"wxaed5ace05d92b218", "MiniGameCommon 游戏圈插件"},
      {"wxbd990766293b9dc4", "MinigameLoading 加载插件"},
      {"wxe5a48f1ed5f544b7", "Unity Loader 插件"},
      {"wxf3f436ba9bd4be7b", "微信商家优惠券插件"},
  }};
  const auto normalized = to_lower_ascii(std::string(wxid));
  for (const auto& [id, name] : names) {
    if (normalized == id) return std::string(name);
  }
  return std::nullopt;
}

class WinHandle {
public:
  explicit WinHandle(HANDLE value = nullptr) : value_(value) {}
  ~WinHandle() {
    if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
  }
  WinHandle(const WinHandle&) = delete;
  WinHandle& operator=(const WinHandle&) = delete;
  WinHandle(WinHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  WinHandle& operator=(WinHandle&& other) noexcept {
    if (this == &other) return *this;
    if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    value_ = std::exchange(other.value_, nullptr);
    return *this;
  }
  [[nodiscard]] HANDLE get() const { return value_; }
  explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }

private:
  HANDLE value_{};
};

bool ascii_identifier_character(const unsigned char value) {
  return std::isalnum(value) != 0 || value == '_';
}

bool ascii_equal_case_insensitive(const char left, const char right) {
  return std::tolower(static_cast<unsigned char>(left)) ==
         std::tolower(static_cast<unsigned char>(right));
}

std::size_t find_ascii_case_insensitive(const std::string_view text,
                                        const std::string_view needle,
                                        const std::size_t start = 0) {
  if (needle.empty() || start >= text.size() || needle.size() > text.size() - start) {
    return std::string_view::npos;
  }
  const auto match = std::search(text.begin() + static_cast<std::ptrdiff_t>(start),
                                 text.end(),
                                 needle.begin(),
                                 needle.end(),
                                 ascii_equal_case_insensitive);
  return match == text.end()
             ? std::string_view::npos
             : static_cast<std::size_t>(std::distance(text.begin(), match));
}

bool bounded_ascii_key(const std::string_view text,
                       const std::size_t position,
                       const std::size_t length) {
  if (position > 0 && ascii_identifier_character(static_cast<unsigned char>(text[position - 1]))) {
    return false;
  }
  const auto end = position + length;
  return end >= text.size() ||
         !ascii_identifier_character(static_cast<unsigned char>(text[end]));
}

bool runtime_separator_character(const unsigned char value) {
  return value == '\\' || value == '"' || value == '\'' || value == ':' || value == '=' ||
         std::isspace(value) != 0;
}

std::optional<std::pair<std::string, std::size_t>> runtime_wxid_after_key(
    const std::string_view text,
    const std::size_t key_end) {
  const auto limit = std::min(text.size(), key_end + 96U);
  auto position = key_end;
  while (position < limit && runtime_separator_character(static_cast<unsigned char>(text[position]))) {
    ++position;
  }
  if (position + 18U > limit) return std::nullopt;
  auto wxid = std::string(text.substr(position, 18));
  if (!is_wxid(to_lower_ascii(wxid))) return std::nullopt;
  return std::pair{to_lower_ascii(std::move(wxid)), position + 18U};
}

std::string peel_runtime_escapes(std::string value) {
  for (std::size_t round = 0; round < 8; ++round) {
    std::string next;
    next.reserve(value.size());
    bool changed{};
    for (std::size_t index = 0; index < value.size();) {
      if (value[index] == '\\' && index + 1 < value.size() &&
          (value[index + 1] == '\\' || value[index + 1] == '"' || value[index + 1] == '\'')) {
        next.push_back(value[index + 1]);
        index += 2;
        changed = true;
      } else {
        next.push_back(value[index++]);
      }
    }
    value = std::move(next);
    if (!changed) break;
  }
  return value;
}

std::optional<std::string> runtime_name_after_key(const std::string_view text,
                                                  const std::size_t key_end,
                                                  const std::size_t record_end) {
  auto position = key_end;
  const auto syntax_end = std::min(record_end, key_end + 96U);
  while (position < syntax_end) {
    const auto current = static_cast<unsigned char>(text[position]);
    if (current == '\\') {
      auto slash_end = position;
      while (slash_end < syntax_end && text[slash_end] == '\\') ++slash_end;
      if (slash_end < syntax_end && (text[slash_end] == '"' || text[slash_end] == '\'')) {
        position = slash_end + 1;
        continue;
      }
      break;
    }
    if (!runtime_separator_character(current)) break;
    ++position;
  }
  if (position >= syntax_end) return std::nullopt;

  const auto value_start = position;
  const auto value_limit = std::min(record_end, value_start + 384U);
  while (position < value_limit) {
    const auto current = text[position];
    if (current == '\0' || current == '\r' || current == '\n' || current == '"' || current == '\'') break;
    if (current == '\\') {
      auto slash_end = position;
      while (slash_end < value_limit && text[slash_end] == '\\') ++slash_end;
      if (slash_end < value_limit && (text[slash_end] == '"' || text[slash_end] == '\'')) break;
      position = slash_end;
      continue;
    }
    ++position;
  }
  if (position <= value_start || position - value_start > 320U) return std::nullopt;

  auto raw = peel_runtime_escapes(std::string(text.substr(value_start, position - value_start)));
  std::string quoted;
  quoted.reserve(raw.size() + 2);
  quoted.push_back('"');
  quoted.append(raw);
  quoted.push_back('"');
  std::size_t unescape_position{};
  return normalized_display_name(javascript_unescape_string(quoted, unescape_position));
}

using RuntimeNameVotes = std::map<std::string, std::map<std::string, std::size_t>>;

constexpr std::array<std::string_view, 9> runtime_identity_keys{
    "hostappid", "hostapp_id", "wxappid", "wx_appid", "mini_program_appid",
    "miniappappid", "app_id", "appid", "wxid"};

constexpr std::array<std::string_view, 17> runtime_name_keys{
    "app_nick_name", "appnickname", "wxnickname", "miniprogramname", "gamename",
    "game_name", "mini_program_name", "game_nick_name", "appname", "productname",
    "projectname", "displayname", "app_display_name", "brandname", "app_title",
    "nickname", "nick_name"};

struct RuntimeIdentityOccurrence {
  std::size_t position{};
  std::size_t key_end{};
  std::string wxid;
};

struct RuntimeNameCandidate {
  std::string name;
  std::size_t priority{};
  std::size_t position{};
};

bool runtime_key_token(const std::string_view text,
                       const std::size_t position,
                       const std::size_t length,
                       std::size_t& token_start) {
  if (!bounded_ascii_key(text, position, length)) return false;
  token_start = position;
  if (position > 0 && position + length < text.size() &&
      (text[position - 1] == '"' || text[position - 1] == '\'') &&
      text[position + length] == text[position - 1]) {
    token_start = position - 1;
  }
  return true;
}

bool direct_runtime_object_key(const std::string_view object,
                               const std::size_t position,
                               const std::size_t length) {
  std::size_t token_start{};
  if (!runtime_key_token(object, position, length, token_start)) return false;

  std::size_t object_depth{};
  std::size_t array_depth{};
  char quote{};
  bool escaped{};
  for (std::size_t index = 0; index < token_start; ++index) {
    const char current = object[index];
    if (quote != 0) {
      if (escaped) {
        escaped = false;
      } else if (current == '\\') {
        escaped = true;
      } else if (current == quote) {
        quote = 0;
      }
      continue;
    }
    if (current == '"' || current == '\'' || current == '`') {
      quote = current;
    } else if (current == '{') {
      ++object_depth;
    } else if (current == '}') {
      if (object_depth == 0) return false;
      --object_depth;
    } else if (current == '[') {
      ++array_depth;
    } else if (current == ']') {
      if (array_depth == 0) return false;
      --array_depth;
    }
  }
  return quote == 0 && object_depth == 1 && array_depth == 0;
}

std::vector<RuntimeIdentityOccurrence> runtime_identity_occurrences(
    const std::string_view text,
    const std::set<std::string>& relevant_wxids) {
  std::vector<RuntimeIdentityOccurrence> occurrences;
  for (const auto key : runtime_identity_keys) {
    std::size_t search{};
    while (search < text.size()) {
      const auto position = find_ascii_case_insensitive(text, key, search);
      if (position == std::string_view::npos) break;
      search = position + key.size();
      if (!bounded_ascii_key(text, position, key.size())) continue;
      const auto identity = runtime_wxid_after_key(text, position + key.size());
      if (!identity || !relevant_wxids.contains(identity->first)) continue;
      occurrences.push_back({position, position + key.size(), identity->first});
    }
  }
  std::stable_sort(occurrences.begin(), occurrences.end(), [](const auto& left, const auto& right) {
    if (left.position != right.position) return left.position < right.position;
    return left.key_end < right.key_end;
  });
  occurrences.erase(
      std::unique(occurrences.begin(), occurrences.end(), [](const auto& left, const auto& right) {
        return left.position == right.position && left.wxid == right.wxid;
      }),
      occurrences.end());
  return occurrences;
}

std::optional<std::pair<std::size_t, std::size_t>> smallest_runtime_object(
    const std::string_view text,
    const std::size_t identity_position) {
  constexpr std::size_t maximum_object_bytes = 12U * 1024U;
  const auto minimum =
      identity_position > maximum_object_bytes ? identity_position - maximum_object_bytes : 0U;
  std::optional<std::pair<std::size_t, std::size_t>> best;
  auto open = identity_position;
  std::size_t examined{};
  while (open > minimum && examined < 96U) {
    open = text.rfind('{', open - 1U);
    if (open == std::string_view::npos || open < minimum) break;
    ++examined;
    const auto range = balanced_object_range(text, open);
    if (!range || range->second < identity_position ||
        range->second - range->first + 1U > maximum_object_bytes) {
      continue;
    }
    if (!best || range->second - range->first < best->second - best->first) best = range;
  }
  return best;
}

std::set<std::string> direct_runtime_identities(const std::string_view object) {
  std::set<std::string> identities;
  for (const auto key : runtime_identity_keys) {
    std::size_t search{};
    while (search < object.size()) {
      const auto position = find_ascii_case_insensitive(object, key, search);
      if (position == std::string_view::npos) break;
      search = position + key.size();
      if (!direct_runtime_object_key(object, position, key.size())) continue;
      if (const auto identity = runtime_wxid_after_key(object, position + key.size())) {
        identities.insert(identity->first);
      }
    }
  }
  return identities;
}

std::vector<RuntimeNameCandidate> direct_runtime_names(const std::string_view object) {
  std::vector<RuntimeNameCandidate> candidates;
  for (std::size_t priority = 0; priority < runtime_name_keys.size(); ++priority) {
    const auto key = runtime_name_keys[priority];
    std::size_t search{};
    while (search < object.size()) {
      const auto position = find_ascii_case_insensitive(object, key, search);
      if (position == std::string_view::npos) break;
      search = position + key.size();
      if (!direct_runtime_object_key(object, position, key.size())) continue;
      if (auto name = runtime_name_after_key(object, position + key.size(), object.size())) {
        candidates.push_back({std::move(*name), priority, position});
      }
    }
  }
  return candidates;
}

std::optional<RuntimeNameCandidate> best_runtime_name(
    std::vector<RuntimeNameCandidate> candidates) {
  if (candidates.empty()) return std::nullopt;
  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
    if (left.priority != right.priority) return left.priority < right.priority;
    return left.position < right.position;
  });
  return std::move(candidates.front());
}

std::size_t next_runtime_identity_position(const std::string_view text,
                                           const std::size_t start,
                                           const std::size_t limit) {
  auto next = limit;
  for (const auto key : runtime_identity_keys) {
    auto search = start;
    while (search < limit) {
      const auto position = find_ascii_case_insensitive(text, key, search);
      if (position == std::string_view::npos || position >= limit) break;
      search = position + key.size();
      if (!bounded_ascii_key(text, position, key.size())) continue;
      if (runtime_wxid_after_key(text, position + key.size())) {
        next = std::min(next, position);
        break;
      }
    }
  }
  return next;
}

std::optional<RuntimeNameCandidate> forward_runtime_name(
    const std::string_view text,
    const RuntimeIdentityOccurrence& identity) {
  constexpr std::size_t maximum_record_bytes = 4096U;
  auto record_end = std::min(text.size(), identity.key_end + maximum_record_bytes);
  record_end = next_runtime_identity_position(text, identity.key_end, record_end);
  std::vector<RuntimeNameCandidate> candidates;
  for (std::size_t priority = 0; priority < runtime_name_keys.size(); ++priority) {
    const auto key = runtime_name_keys[priority];
    auto search = identity.key_end;
    while (search < record_end) {
      const auto position = find_ascii_case_insensitive(text, key, search);
      if (position == std::string_view::npos || position >= record_end) break;
      search = position + key.size();
      if (!bounded_ascii_key(text, position, key.size())) continue;
      if (auto name = runtime_name_after_key(text, position + key.size(), record_end)) {
        candidates.push_back({std::move(*name), priority, position});
        break;
      }
    }
  }
  return best_runtime_name(std::move(candidates));
}

void collect_runtime_name_pairs(const std::string_view text,
                                const std::set<std::string>& relevant_wxids,
                                RuntimeNameVotes& votes) {
  std::set<std::pair<std::size_t, std::size_t>> processed_objects;
  for (const auto& occurrence : runtime_identity_occurrences(text, relevant_wxids)) {
    const auto range = smallest_runtime_object(text, occurrence.position);
    if (range) {
      const auto object = text.substr(range->first, range->second - range->first + 1U);
      const auto relative_position = occurrence.position - range->first;
      const auto key_length = occurrence.key_end - occurrence.position;
      if (direct_runtime_object_key(object, relative_position, key_length)) {
        if (!processed_objects.insert(*range).second) continue;
        const auto identities = direct_runtime_identities(object);
        if (identities.size() != 1U || !identities.contains(occurrence.wxid)) continue;
        if (auto name = best_runtime_name(direct_runtime_names(object))) {
          ++votes[occurrence.wxid][name->name];
        }
        continue;
      }
    }

    // Escaped JSON fragments often have visible key/value text but no
    // structurally balanced object. Keep the fallback bounded by the next
    // application identity so adjacent projects are never merged.
    if (auto name = forward_runtime_name(text, occurrence)) {
      ++votes[occurrence.wxid][name->name];
    }
  }
}

std::string utf16le_window_to_utf8(const std::string_view bytes) {
  if (bytes.size() < 2) return {};
  std::wstring wide;
  wide.reserve(bytes.size() / 2U);
  for (std::size_t index = 0; index + 1U < bytes.size(); index += 2U) {
    const auto low = static_cast<unsigned char>(bytes[index]);
    const auto high = static_cast<unsigned char>(bytes[index + 1U]);
    wide.push_back(static_cast<wchar_t>(low | (static_cast<unsigned int>(high) << 8U)));
  }
  if (wide.empty()) return {};

  const auto required = WideCharToMultiByte(
      CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (required <= 0) return {};
  std::string utf8(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8,
                          0,
                          wide.data(),
                          static_cast<int>(wide.size()),
                          utf8.data(),
                          required,
                          nullptr,
                          nullptr) <= 0) {
    return {};
  }
  return utf8;
}

void collect_utf16le_runtime_name_pairs(const std::string_view bytes,
                                        const std::set<std::string>& relevant_wxids,
                                        RuntimeNameVotes& votes) {
  struct Window {
    std::size_t begin{};
    std::size_t end{};
    std::size_t alignment{};
  };

  constexpr std::size_t wxid_units = 18U;
  constexpr std::size_t wxid_bytes = wxid_units * 2U;
  constexpr std::size_t radius = 32U * 1024U;
  std::vector<Window> windows;
  for (std::size_t alignment = 0; alignment < 2U; ++alignment) {
    for (std::size_t position = alignment; position + wxid_bytes <= bytes.size(); position += 2U) {
      const auto prefix_w = static_cast<unsigned char>(bytes[position]);
      const auto prefix_x = static_cast<unsigned char>(bytes[position + 2U]);
      if ((prefix_w != 'w' && prefix_w != 'W') ||
          (prefix_x != 'x' && prefix_x != 'X') ||
          bytes[position + 1U] != '\0' ||
          bytes[position + 3U] != '\0') {
        continue;
      }

      std::string wxid;
      wxid.reserve(wxid_units);
      bool valid = true;
      for (std::size_t unit = 0; unit < wxid_units; ++unit) {
        const auto byte_offset = position + unit * 2U;
        const auto current = static_cast<unsigned char>(bytes[byte_offset]);
        if (bytes[byte_offset + 1U] != '\0' ||
            (unit >= 2U && std::isxdigit(current) == 0)) {
          valid = false;
          break;
        }
        wxid.push_back(static_cast<char>(std::tolower(current)));
      }
      if (!valid || !relevant_wxids.contains(wxid)) continue;

      auto begin = position > radius ? position - radius : alignment;
      if ((begin & 1U) != alignment) ++begin;
      auto end = std::min(bytes.size(), position + wxid_bytes + radius);
      if ((end - begin) % 2U != 0) --end;
      if (end > begin) windows.push_back({begin, end, alignment});
      position += wxid_bytes - 2U;
    }
  }

  std::stable_sort(windows.begin(), windows.end(), [](const Window& left, const Window& right) {
    if (left.alignment != right.alignment) return left.alignment < right.alignment;
    if (left.begin != right.begin) return left.begin < right.begin;
    return left.end < right.end;
  });
  std::vector<Window> merged;
  for (const auto& window : windows) {
    if (!merged.empty() && merged.back().alignment == window.alignment &&
        window.begin <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, window.end);
    } else {
      merged.push_back(window);
    }
  }

  for (const auto& window : merged) {
    const auto decoded = utf16le_window_to_utf8(
        bytes.substr(window.begin, window.end - window.begin));
    if (!decoded.empty()) collect_runtime_name_pairs(decoded, relevant_wxids, votes);
  }
}

bool contains_runtime_key(const std::string_view text,
                          const std::span<const std::string_view> keys) {
  return std::any_of(keys.begin(), keys.end(), [&](const std::string_view key) {
    return find_ascii_case_insensitive(text, key) != std::string_view::npos;
  });
}

std::map<std::string, std::string> resolve_runtime_name_votes(RuntimeNameVotes votes) {
  std::map<std::string, std::string> resolved;
  for (auto& [wxid, names] : votes) {
    std::vector<std::pair<std::string, std::size_t>> ranked(names.begin(), names.end());
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
      if (left.second != right.second) return left.second > right.second;
      if (has_cjk(left.first) != has_cjk(right.first)) return has_cjk(left.first);
      return left.first.size() < right.first.size();
    });
    if (!ranked.empty()) resolved.emplace(std::move(wxid), std::move(ranked.front().first));
  }
  return resolved;
}

std::vector<DWORD> running_wechat_processes() {
  struct ProcessWithWorkingSet {
    DWORD id{};
    SIZE_T working_set{};
  };
  std::vector<ProcessWithWorkingSet> main_processes;
  std::vector<ProcessWithWorkingSet> renderer_processes;
  WinHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (!snapshot) return {};

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.get(), &entry)) return {};
  do {
    const bool is_main = _wcsicmp(entry.szExeFile, L"Weixin.exe") == 0;
    const bool is_renderer = _wcsicmp(entry.szExeFile, L"WeChatAppEx.exe") == 0;
    if (!is_main && !is_renderer) continue;
    WinHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID));
    if (!process) continue;
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(process.get(), &counters, sizeof(counters))) continue;
    (is_main ? main_processes : renderer_processes)
        .push_back({entry.th32ProcessID, counters.WorkingSetSize});
  } while (Process32NextW(snapshot.get(), &entry));

  const auto by_working_set = [](const ProcessWithWorkingSet& left,
                                 const ProcessWithWorkingSet& right) {
    return left.working_set > right.working_set;
  };
  std::stable_sort(main_processes.begin(), main_processes.end(), by_working_set);
  std::stable_sort(renderer_processes.begin(), renderer_processes.end(), by_working_set);
  std::vector<DWORD> result;
  // Weixin.exe holds shared account metadata. WeChatAppEx.exe is the actual
  // mini-program/game renderer and contains most live app identity records.
  for (std::size_t index = 0; index < std::min<std::size_t>(2, main_processes.size()); ++index) {
    result.push_back(main_processes[index].id);
  }
  for (std::size_t index = 0; index < std::min<std::size_t>(20, renderer_processes.size()); ++index) {
    result.push_back(renderer_processes[index].id);
  }
  return result;
}

bool readable_memory_protection(const DWORD protection) {
  if ((protection & PAGE_GUARD) != 0 || (protection & PAGE_NOACCESS) != 0) return false;
  switch (protection & 0xffU) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

std::map<std::string, std::string> runtime_app_names(
    const std::set<std::string>& relevant_wxids,
    const std::atomic_bool* cancel_requested,
    const RuntimeNameUpdateSink& update_sink) {
  if (relevant_wxids.empty()) return {};

  SYSTEM_INFO system_info{};
  GetNativeSystemInfo(&system_info);
  const auto minimum_address = reinterpret_cast<std::uintptr_t>(system_info.lpMinimumApplicationAddress);
  const auto maximum_address = reinterpret_cast<std::uintptr_t>(system_info.lpMaximumApplicationAddress);
  constexpr SIZE_T chunk_bytes = 4U * 1024U * 1024U;
  constexpr std::size_t overlap_bytes = 16U * 1024U;
  const auto process_ids = running_wechat_processes();
  RuntimeNameVotes merged_votes;
  std::map<std::string, std::string> published_names;
  std::mutex votes_mutex;

  const auto publish_votes = [&](RuntimeNameVotes votes) {
    if (votes.empty()) return;
    std::vector<std::pair<std::string, std::string>> updates;
    {
      std::lock_guard lock(votes_mutex);
      for (auto& [wxid, names] : votes) {
        for (auto& [name, count] : names) merged_votes[wxid][name] += count;
      }
      if (update_sink) {
        const auto resolved = resolve_runtime_name_votes(merged_votes);
        for (const auto& [wxid, name] : resolved) {
          const auto published = published_names.find(wxid);
          if (published != published_names.end() && published->second == name) continue;
          published_names[wxid] = name;
          updates.emplace_back(wxid, name);
        }
      }
    }
    for (auto& [wxid, name] : updates) update_sink(std::move(wxid), std::move(name));
  };

  parallel_for(process_ids.size(), cancel_requested, 6, [&](const std::size_t process_index) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto process_id = process_ids[process_index];
    WinHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, process_id));
    if (!process) return;
    std::vector<char> buffer(chunk_bytes);

    auto address = minimum_address;
    while (address < maximum_address && !cancelled(cancel_requested)) {
      MEMORY_BASIC_INFORMATION memory{};
      if (VirtualQueryEx(process.get(),
                         reinterpret_cast<const void*>(address),
                         &memory,
                         sizeof(memory)) == 0) {
        break;
      }
      const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
      const auto region_size = static_cast<SIZE_T>(memory.RegionSize);
      const auto next_address = base + std::max<SIZE_T>(region_size, 0x1000U);
      if (memory.State == MEM_COMMIT && region_size != 0 && readable_memory_protection(memory.Protect)) {
        SIZE_T offset{};
        std::string tail;
        while (offset < region_size && !cancelled(cancel_requested)) {
          const auto requested = std::min<SIZE_T>(chunk_bytes, region_size - offset);
          SIZE_T bytes_read{};
          const auto source = reinterpret_cast<const void*>(base + offset);
          const BOOL read = ReadProcessMemory(process.get(), source, buffer.data(), requested, &bytes_read);
          if ((read || bytes_read != 0) && bytes_read != 0) {
            std::string chunk;
            chunk.reserve(tail.size() + bytes_read);
            chunk.append(tail);
            chunk.append(buffer.data(), bytes_read);
            RuntimeNameVotes chunk_votes;
            if (contains_runtime_key(chunk, runtime_identity_keys) &&
                contains_runtime_key(chunk, runtime_name_keys)) {
              collect_runtime_name_pairs(chunk, relevant_wxids, chunk_votes);
            }
            collect_utf16le_runtime_name_pairs(chunk, relevant_wxids, chunk_votes);
            publish_votes(std::move(chunk_votes));
            const auto overlap = std::min(overlap_bytes, chunk.size());
            tail.assign(chunk.end() - static_cast<std::ptrdiff_t>(overlap), chunk.end());
          } else {
            tail.clear();
          }
          offset += requested;
        }
      }
      if (next_address <= address) break;
      address = next_address;
    }
  });

  return resolve_runtime_name_votes(std::move(merged_votes));
}

struct CodeNameMatch {
  std::string name;
  bool identity_matched{};
};

std::optional<std::pair<std::size_t, std::size_t>> nearest_identity_object(
    const std::string_view text,
    const std::size_t wxid_position,
    const std::string_view expected_wxid) {
  constexpr std::size_t maximum_object_bytes = 12U * 1024U;
  const auto lower_expected = to_lower_ascii(std::string(expected_wxid));
  constexpr std::array<std::string_view, 17> identity_keys{
      "hostAppId", "hostappid", "hostapp_id", "appId", "appid", "wxAppId",
      "wxAppID", "wxappid", "wx_appid", "miniProgramAppId", "miniappappid",
      "partnerAppId", "customer_appid", "APP_ID", "APPID", "app_id", "wxid"};

  std::optional<std::pair<std::size_t, std::size_t>> best;
  const auto minimum = wxid_position > maximum_object_bytes ? wxid_position - maximum_object_bytes : 0;
  std::size_t open = wxid_position;
  std::size_t examined{};
  while (open > minimum && examined < 96) {
    open = text.rfind('{', open - 1);
    if (open == std::string_view::npos || open < minimum) break;
    ++examined;
    const auto range = balanced_object_range(text, open);
    if (!range || range->second < wxid_position || range->second - range->first + 1 > maximum_object_bytes) continue;
    const auto object = text.substr(range->first, range->second - range->first + 1);
    bool has_matching_identity{};
    for (const auto key : identity_keys) {
      const auto values = script_string_values(object, key);
      if (std::any_of(values.begin(), values.end(), [&](const ScriptStringValue& value) {
            return to_lower_ascii(value.value) == lower_expected;
          })) {
        has_matching_identity = true;
        break;
      }
    }
    if (!has_matching_identity) continue;
    if (!best || range->second - range->first < best->second - best->first) best = range;
  }
  return best;
}

std::optional<CodeNameMatch> best_name_from_code(const std::string_view text,
                                                 const std::string_view expected_wxid) {
  struct RankedName {
    std::string name;
    int priority{};
    std::size_t distance{};
    bool identity_matched{};
  };
  std::vector<RankedName> candidates;
  constexpr std::array<std::pair<std::string_view, int>, 19> identity_name_keys{{
      {"h5_game_name", 0},
      {"GAME_NAME", 1},
      {"gameTitle", 2},
      {"appName", 3},
      {"appNickName", 4},
      {"appNickname", 5},
      {"app_nick_name", 6},
      {"wxNickName", 7},
      {"gameName", 8},
      {"miniProgramName", 9},
      {"productName", 10},
      {"projectName", 11},
      {"displayName", 12},
      {"brandName", 13},
      {"APPNAME", 14},
      {"app_name", 15},
      {"game_name", 16},
      {"projectname", 17},
      {"name", 24},
  }};

  const auto lower_text = to_lower_ascii(std::string(text));
  const auto lower_wxid = to_lower_ascii(std::string(expected_wxid));
  std::size_t wxid_search{};
  while (wxid_search < lower_text.size()) {
    const auto wxid_position = lower_text.find(lower_wxid, wxid_search);
    if (wxid_position == std::string::npos) break;
    wxid_search = wxid_position + lower_wxid.size();

    if (const auto range = nearest_identity_object(text, wxid_position, expected_wxid)) {
      const auto object = text.substr(range->first, range->second - range->first + 1);
      for (const auto& [key, priority] : identity_name_keys) {
        for (const auto& value : script_string_values(object, key)) {
          if (auto normalized = normalized_display_name(value.value)) {
            const auto absolute_position = range->first + value.value_position;
            const auto distance = absolute_position > wxid_position
                                      ? absolute_position - wxid_position
                                      : wxid_position - absolute_position;
            candidates.push_back({std::move(*normalized), priority, distance, true});
          }
        }
      }
    }

    // Some generated bundles compare the current WXID and assign the title
    // outside an object literal. Only product-specific keys in a bounded
    // context are accepted here.
    constexpr std::size_t radius = 16U * 1024U;
    const auto context_begin = wxid_position > radius ? wxid_position - radius : 0;
    const auto context_end = std::min(text.size(), wxid_position + lower_wxid.size() + radius);
    const auto context = text.substr(context_begin, context_end - context_begin);
    constexpr std::array<std::pair<std::string_view, int>, 4> contextual_keys{{
        {"h5_game_name", 30},
        {"GAME_NAME", 31},
        {"gameTitle", 32},
        {"APPNAME", 33},
    }};
    for (const auto& [key, priority] : contextual_keys) {
      for (const auto& value : script_string_values(context, key)) {
        if (auto normalized = normalized_display_name(value.value)) {
          const auto absolute_position = context_begin + value.value_position;
          const auto distance = absolute_position > wxid_position
                                    ? absolute_position - wxid_position
                                    : wxid_position - absolute_position;
          candidates.push_back({std::move(*normalized), priority, distance, true});
        }
      }
    }
  }

  // Unity/HTML5 launchers sometimes place the human-facing game name in a
  // generated configuration block tens of kilobytes away from APPID. When the
  // whole file contains the expected WXID and exactly one usable
  // h5_game_name, that value is the product identity rather than a route or
  // cross-promotion title.
  if (lower_text.find(lower_wxid) != std::string::npos) {
    std::set<std::string> unique_h5_names;
    for (const auto& value : script_string_values(text, "h5_game_name")) {
      if (auto normalized = normalized_display_name(value.value); normalized && has_cjk(*normalized)) {
        unique_h5_names.insert(std::move(*normalized));
      }
    }
    if (unique_h5_names.size() == 1) {
      candidates.push_back({*unique_h5_names.begin(), 34, text.size(), true});
    }
  }

  if (!candidates.empty()) {
    std::stable_sort(candidates.begin(), candidates.end(), [](const RankedName& left, const RankedName& right) {
      if (left.priority != right.priority) return left.priority < right.priority;
      if (left.distance != right.distance) return left.distance < right.distance;
      if (has_cjk(left.name) != has_cjk(right.name)) return has_cjk(left.name);
      return left.name.size() < right.name.size();
    });
    return CodeNameMatch{std::move(candidates.front().name), true};
  }

  using Regex = std::regex;
  static const Regex product_build_pattern(
      R"rx(\bappName\s*:\s*(["'])((?:\\.|[^\\\r\n])*?)\1\s*,\s*version\s*:\s*[^,}\r\n]{1,256}\s*,\s*buildNumber\s*:\s*[^,}\r\n]{1,256})rx",
      std::regex_constants::icase | std::regex_constants::optimize);
  for (std::regex_iterator<std::string_view::const_iterator> match(text.begin(), text.end(), product_build_pattern), end;
       match != end; ++match) {
    if (match->size() < 3) continue;
    auto raw = std::string((*match)[2].first, (*match)[2].second);
    std::string quoted;
    quoted.reserve(raw.size() + 2);
    quoted.push_back((*match)[1].str().empty() ? '"' : (*match)[1].str().front());
    quoted.append(raw);
    quoted.push_back(quoted.front());
    std::size_t position{};
    if (auto candidate = normalized_display_name(javascript_unescape_string(quoted, position))) {
      return CodeNameMatch{std::move(*candidate), lower_text.find(lower_wxid) != std::string::npos};
    }
  }

  static const std::array<Regex, 7> patterns{
      Regex(R"rx(appId\s*:\s*["'][^"']+["']\s*,\s*appName\s*:\s*(["'])((?:\\.|[^\\\r\n])*?)\1\s*,\s*appVersion)rx",
            std::regex_constants::icase | std::regex_constants::optimize),
      Regex(R"rx(appName\s*:\s*(["'])((?:\\.|[^\\\r\n])*?)\1\s*,\s*appVersion\s*:\s*["'][^"']+["']\s*,\s*appVersionCode)rx",
            std::regex_constants::icase | std::regex_constants::optimize),
      Regex(R"rx(authFloatInfo\s*:\s*\{[\s\S]{0,2000}?\bname\s*:\s*(["'])((?:\\.|[^\\\r\n])*?)\1)rx",
            std::regex_constants::icase | std::regex_constants::optimize),
      Regex(R"rx(\bAPPNAME\s*:\s*(["'])((?:\\.|[^\\\r\n])*?)\1)rx",
            std::regex_constants::optimize),
      Regex(R"rx(\bAPPNAME\s*=\s*(["'])((?:\\.|[^\\\r\n])*?)\1)rx",
            std::regex_constants::optimize),
      Regex(R"rx(\b(?:gameName|productName|projectName|miniProgramName|brandName)\s*[:=]\s*(["'])((?:\\.|[^\\\r\n])*?)\1)rx",
            std::regex_constants::icase | std::regex_constants::optimize),
      Regex(R"rx(["'](?:gameName|productName|projectName|miniProgramName|brandName|app_name|game_name)["']\s*:\s*(["'])((?:\\.|[^\\\r\n])*?)\1)rx",
            std::regex_constants::icase | std::regex_constants::optimize)};

  for (const auto& pattern : patterns) {
    for (std::regex_iterator<std::string_view::const_iterator> match(text.begin(), text.end(), pattern), end;
         match != end; ++match) {
      if (match->size() < 3) continue;
      auto raw = std::string((*match)[2].first, (*match)[2].second);
      std::string quoted;
      quoted.reserve(raw.size() + 2);
      quoted.push_back((*match)[1].str().empty() ? '"' : (*match)[1].str().front());
      quoted.append(raw);
      quoted.push_back(quoted.front());
      std::size_t position{};
      if (auto candidate = normalized_display_name(javascript_unescape_string(quoted, position))) {
        return CodeNameMatch{std::move(*candidate), false};
      }
    }
  }
  return std::nullopt;
}

std::vector<std::string> names_from_local_metadata(const CacheCandidate& candidate,
                                                   const std::atomic_bool* cancel_requested) {
  std::vector<std::string> names;
  const auto local_root = candidate.applet_root / L"local" / utf8_to_wide(candidate.wxid);
  std::error_code error;
  if (!fs::is_directory(local_root, error) || error) return names;

  constexpr std::array<std::string_view, 14> metadata_names{
      "appinfo.json", "appinfo.config", "app-config.json", "app.json", "game.json", "manifest.json",
      "ext.json", "extconfig.json", "project.config.json", "project.private.config.json", "launch.config",
      "config.json", "info.json", "metadata.json"};
  std::size_t examined{};
  for (fs::recursive_directory_iterator iterator(local_root, fs::directory_options::skip_permission_denied, error), end;
       !error && iterator != end && examined < 80; iterator.increment(error)) {
    if (cancelled(cancel_requested)) break;
    if (error) {
      error.clear();
      continue;
    }
    if (iterator.depth() > 2) {
      iterator.disable_recursion_pending();
      continue;
    }
    const auto entry = *iterator;
    if (!entry.is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    const auto filename = to_lower_ascii(wide_to_utf8(entry.path().filename().wstring()));
    if (std::find(metadata_names.begin(), metadata_names.end(), filename) == metadata_names.end()) continue;
    const auto size = entry.file_size(error);
    if (error || size > 2U * 1024U * 1024U) {
      error.clear();
      continue;
    }
    ++examined;
    try {
      const auto text = read_text_utf8(entry.path());
      if (auto name = best_name_from_text(text)) names.push_back(std::move(*name));
      if (filename == "app-config.json") {
        if (auto name = best_global_title_from_app_config(text)) names.push_back(std::move(*name));
      }
    } catch (...) {
    }
  }
  return names;
}

void inspect_unity_loader_hints(const std::string_view text, PackageInspection& inspection) {
  if (text.empty()) return;

  const bool mentions_data_package_setting =
      text.find("loadDataPackageFromSubpackage") != std::string_view::npos;
  if (mentions_data_package_setting) {
    static const std::regex disabled_data_package_pattern(
        R"re(loadDataPackageFromSubpackage["']?\s*:\s*(?:false|!1)(?:\b|[,}]))re",
        std::regex::icase);
    const std::string source(text);
    if (std::regex_search(source, disabled_data_package_pattern)) {
      inspection.data_package_uses_runtime_or_external_source = true;
    }
  }

  const bool mentions_wasmcode2 = text.find("wasmcode2") != std::string_view::npos;
  const bool has_ios_renderer_guard =
      text.find("canUseH5Renderer") != std::string_view::npos ||
      text.find("isIOSHighPerformanceMode") != std::string_view::npos ||
      text.find("iOS_hp") != std::string_view::npos;
  const bool has_delayed_function_split_loader =
      text.find("kDelayLoadWasmcode2Time") != std::string_view::npos ||
      text.find("delayFuncInitTime") != std::string_view::npos ||
      text.find("wasm split") != std::string_view::npos;
  if (mentions_wasmcode2 && has_ios_renderer_guard && has_delayed_function_split_loader) {
    inspection.wasmcode2_platform_conditional = true;
  }
}

struct LocalRuntimeNameEvidence {
  std::vector<std::string> identity_names;
  std::vector<std::string> product_config_names;
};

std::vector<std::string> product_config_names_from_text(const std::string_view text) {
  constexpr std::string_view suffix = "产品配置";
  std::vector<std::string> names;
  std::size_t search{};
  while (search < text.size()) {
    const auto suffix_position = text.find(suffix, search);
    if (suffix_position == std::string_view::npos) break;
    search = suffix_position + suffix.size();

    const auto minimum = suffix_position > 96U ? suffix_position - 96U : 0U;
    auto begin = suffix_position;
    while (begin > minimum) {
      const auto current = static_cast<unsigned char>(text[begin - 1U]);
      if (current < 0x80U &&
          (std::isspace(current) != 0 ||
           std::string_view{"/\\?&=:'\"<>,;|[](){}"}.find(static_cast<char>(current)) !=
               std::string_view::npos)) {
        break;
      }
      --begin;
    }
    if (begin >= suffix_position) continue;
    auto raw = std::string(text.substr(begin, suffix_position - begin));
    if (auto normalized = normalized_display_name(std::move(raw));
        normalized && has_cjk(*normalized)) {
      names.push_back(std::move(*normalized));
    }
  }
  return names;
}

LocalRuntimeNameEvidence inspect_local_runtime_records(
    const CacheCandidate& candidate,
    PackageInspection& inspection,
    const std::atomic_bool* cancel_requested) {
  LocalRuntimeNameEvidence evidence;
  const auto local_root = candidate.applet_root / L"local" / utf8_to_wide(candidate.wxid);
  std::error_code error;
  if (!fs::is_directory(local_root, error) || error) return evidence;

  struct LocalRecordFile {
    fs::path path;
    std::uintmax_t size{};
    int priority{};
  };
  std::vector<LocalRecordFile> files;
  files.reserve(192);
  constexpr std::uintmax_t max_file_bytes = 12U * 1024U * 1024U;
  for (fs::recursive_directory_iterator iterator(
           local_root, fs::directory_options::skip_permission_denied, error),
       end;
       iterator != end;
       iterator.increment(error)) {
    if (cancelled(cancel_requested)) break;
    if (error) {
      error.clear();
      continue;
    }
    if (iterator.depth() > 5) {
      iterator.disable_recursion_pending();
      continue;
    }

    const auto entry = *iterator;
    const auto path_key = to_lower_ascii(wide_to_utf8(entry.path().wstring()));
    if (path_key.find("__game_file_cache") != std::string::npos) {
      if (entry.is_directory(error) && !error) iterator.disable_recursion_pending();
      error.clear();
      continue;
    }
    if (!entry.is_regular_file(error) || error) {
      error.clear();
      continue;
    }

    const auto filename = to_lower_ascii(wide_to_utf8(entry.path().filename().wstring()));
    const auto extension = to_lower_ascii(wide_to_utf8(entry.path().extension().wstring()));
    int priority = 4;
    if (path_key.find("miniprogramlog") != std::string::npos ||
        path_key.find("\\log\\") != std::string::npos ||
        filename.starts_with("log")) {
      priority = 0;
    } else if (filename.find("app-config") != std::string::npos ||
               filename.find("appinfo") != std::string::npos ||
               filename.find("metadata") != std::string::npos) {
      priority = 1;
    } else if (extension == ".json" || extension == ".config" ||
               extension == ".cfg" || extension == ".dbt") {
      priority = 2;
    } else if (extension == ".log" || extension == ".txt" ||
               extension == ".bin" ||
               (extension.empty() &&
                (path_key.find("\\temp\\") != std::string::npos ||
                 path_key.find("\\cfg\\") != std::string::npos))) {
      priority = 3;
    } else {
      continue;
    }

    const auto size = entry.file_size(error);
    if (error || size == 0 || size > max_file_bytes) {
      error.clear();
      continue;
    }
    files.push_back({entry.path(), size, priority});
  }

  std::stable_sort(files.begin(), files.end(), [](const LocalRecordFile& left,
                                                   const LocalRecordFile& right) {
    if (left.priority != right.priority) return left.priority < right.priority;
    if (left.size != right.size) return left.size < right.size;
    return left.path.native() < right.path.native();
  });

  std::size_t examined{};
  std::uintmax_t examined_bytes{};
  constexpr std::size_t max_files = 160;
  constexpr std::uintmax_t max_total_bytes = 64U * 1024U * 1024U;
  RuntimeNameVotes local_votes;
  const std::set<std::string> relevant_wxids{to_lower_ascii(candidate.wxid)};
  for (const auto& file : files) {
    if (examined >= max_files || examined_bytes >= max_total_bytes) break;
    if (cancelled(cancel_requested)) break;
    if (examined_bytes + file.size > max_total_bytes) continue;
    ++examined;
    examined_bytes += file.size;
    try {
      const auto text = read_text_utf8(file.path);
      inspect_unity_loader_hints(text, inspection);
      collect_runtime_name_pairs(text, relevant_wxids, local_votes);
      collect_utf16le_runtime_name_pairs(text, relevant_wxids, local_votes);
      auto product_names = product_config_names_from_text(text);
      evidence.product_config_names.insert(
          evidence.product_config_names.end(),
          std::make_move_iterator(product_names.begin()),
          std::make_move_iterator(product_names.end()));
    } catch (...) {
    }
  }
  if (auto resolved = resolve_runtime_name_votes(std::move(local_votes));
      !resolved.empty()) {
    if (const auto found = resolved.find(to_lower_ascii(candidate.wxid)); found != resolved.end()) {
      evidence.identity_names.push_back(found->second);
    }
  }
  return evidence;
}

class WinHttpHandle {
public:
  explicit WinHttpHandle(HINTERNET value = nullptr) : value_(value) {}
  ~WinHttpHandle() { if (value_) WinHttpCloseHandle(value_); }
  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;
  [[nodiscard]] HINTERNET get() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }
private:
  HINTERNET value_{};
};

std::string html_unescape(std::string_view text) {
  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] != '&') {
      decoded.push_back(text[index++]);
      continue;
    }

    const auto semicolon = text.find(';', index + 1U);
    if (semicolon == std::string_view::npos || semicolon - index > 12U) {
      decoded.push_back(text[index++]);
      continue;
    }
    const auto entity = text.substr(index, semicolon - index + 1U);
    if (entity == "&amp;") {
      decoded.push_back('&');
    } else if (entity == "&quot;") {
      decoded.push_back('"');
    } else if (entity == "&#39;" || entity == "&apos;") {
      decoded.push_back('\'');
    } else if (entity == "&lt;") {
      decoded.push_back('<');
    } else if (entity == "&gt;") {
      decoded.push_back('>');
    } else if (entity == "&nbsp;") {
      decoded.push_back(' ');
    } else {
      decoded.append(entity);
    }
    index = semicolon + 1U;
  }
  return decoded;
}

std::optional<std::string> marketplace_app_name_from_html(const std::string_view html) {
  // Tencent application detail pages expose the exact product name as an
  // encoded appname query parameter. This is more stable than deriving a name
  // from page copy and supports names containing punctuation.
  std::size_t search{};
  while (search < html.size()) {
    const auto position = find_ascii_case_insensitive(html, "appname=", search);
    if (position == std::string_view::npos) break;
    const auto value_begin = position + std::string_view("appname=").size();
    auto value_end = value_begin;
    while (value_end < html.size()) {
      const auto current = html[value_end];
      if (current == '&' || current == '"' || current == '\'' || current == '<' ||
          current == '>' || std::isspace(static_cast<unsigned char>(current)) != 0) {
        break;
      }
      ++value_end;
    }
    if (value_end > value_begin) {
      if (auto normalized =
              normalized_display_name(html_unescape(html.substr(value_begin, value_end - value_begin)))) {
        return normalized;
      }
    }
    search = value_begin;
  }

  const auto title_open = find_ascii_case_insensitive(html, "<title");
  if (title_open == std::string_view::npos) return std::nullopt;
  const auto title_begin = html.find('>', title_open);
  if (title_begin == std::string_view::npos) return std::nullopt;
  const auto title_end = find_ascii_case_insensitive(html, "</title>", title_begin + 1U);
  if (title_end == std::string_view::npos) return std::nullopt;

  const auto title = html_unescape(html.substr(title_begin + 1U, title_end - title_begin - 1U));
  constexpr std::array<std::string_view, 7> product_markers{
      "游戏下载-", "软件下载-", "应用下载-", "微信小游戏-", "微信小程序-",
      "官方小程序入口-", "官方小游戏入口-"};
  auto product_end = std::string::npos;
  for (const auto marker : product_markers) {
    const auto marker_position = title.find(marker);
    if (marker_position != std::string::npos) product_end = std::min(product_end, marker_position);
  }
  if (product_end == std::string::npos) return std::nullopt;
  return normalized_display_name(title.substr(0, product_end));
}

std::optional<std::string> https_request(const std::wstring_view host,
                                         const std::wstring_view path,
                                         const std::wstring_view method = L"GET",
                                         const std::string_view body = {}) {
  WinHttpHandle session(WinHttpOpen(L"WeChatMiniProgramDecompiler/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0));
  if (!session) return std::nullopt;
  WinHttpSetTimeouts(session.get(), 1200, 1200, 1600, 1600);

  WinHttpHandle connection(WinHttpConnect(session.get(), std::wstring(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) return std::nullopt;
  WinHttpHandle request(WinHttpOpenRequest(connection.get(),
                                           std::wstring(method).c_str(),
                                           std::wstring(path).c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE));
  if (!request) return std::nullopt;

  DWORD decompression =
      WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
  WinHttpSetOption(request.get(),
                   WINHTTP_OPTION_DECOMPRESSION,
                   &decompression,
                   sizeof(decompression));

  const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
  const DWORD header_length = body.empty() ? 0 : static_cast<DWORD>(-1L);
  void* request_data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
  const DWORD request_size = static_cast<DWORD>(body.size());
  if (!WinHttpSendRequest(request.get(), headers, header_length, request_data, request_size, request_size, 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    return std::nullopt;
  }

  DWORD status{};
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(),
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX,
                           &status,
                           &status_size,
                           WINHTTP_NO_HEADER_INDEX) ||
      status < 200 || status >= 300) {
    return std::nullopt;
  }

  std::string response;
  while (response.size() < 2U * 1024U * 1024U) {
    DWORD available{};
    if (!WinHttpQueryDataAvailable(request.get(), &available)) return std::nullopt;
    if (available == 0) break;
    const auto remaining = 2U * 1024U * 1024U - response.size();
    const DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(available, remaining));
    const auto old_size = response.size();
    response.resize(old_size + to_read);
    DWORD read{};
    if (!WinHttpReadData(request.get(), response.data() + old_size, to_read, &read)) return std::nullopt;
    response.resize(old_size + read);
    if (read == 0) break;
  }
  return response;
}

std::optional<std::string> remote_app_name(const std::string_view wxid) {
  const auto id = to_lower_ascii(std::string(wxid));
  std::optional<std::string> resolved;

  if (const auto response =
          https_request(L"sj.qq.com", utf8_to_wide("/appdetail/" + id))) {
    resolved = marketplace_app_name_from_html(*response);
  }

  if (!resolved) {
    const auto response = https_request(
        L"mp.weixin.qq.com",
        utf8_to_wide("/wxawap/waverifyinfo?action=get&appid=" + id));
    if (response) {
    constexpr std::array<std::string_view, 5> keys{"nickname", "nickName", "appName", "name", "title"};
    for (const auto key : keys) {
      for (auto value : json_string_values(*response, key)) {
        if (auto normalized = normalized_display_name(std::move(value))) {
          resolved = std::move(*normalized);
          break;
        }
      }
      if (resolved) break;
    }
    }
  }

  if (!resolved) {
    const auto payload = "{\"appId\":\"" + id + "\",\"restType\":1}";
    if (const auto response = https_request(L"pos.meituan.com",
                                            L"/api/v1/crm/mini/app/tenant/query",
                                            L"POST",
                                            payload)) {
      for (auto value : json_string_values(*response, "nickname")) {
        if (auto normalized = normalized_display_name(std::move(value))) {
          resolved = std::move(*normalized);
          break;
        }
      }
    }
  }

  return resolved;
}

std::string entry_text(const std::vector<std::byte>& package, const PackFileEntry& entry) {
  if (entry.offset > package.size() || package.size() - entry.offset < entry.size) return {};
  const auto* begin = reinterpret_cast<const char*>(package.data() + entry.offset);
  return std::string(begin, begin + entry.size);
}

std::set<std::string> plugin_providers_from_text(const std::string_view text) {
  std::set<std::string> providers;
  if (text.empty()) return providers;
  const std::string source(text);
  static const std::regex provider_pattern(
      R"re(["']provider["']\s*:\s*["'](wx[0-9a-fA-F]{16})["'])re",
      std::regex::icase);
  static const std::regex plugin_url_pattern(
      R"re(plugin://(wx[0-9a-fA-F]{16}))re",
      std::regex::icase);
  for (std::sregex_iterator match(source.begin(), source.end(), provider_pattern), end; match != end; ++match) {
    providers.insert(to_lower_ascii((*match)[1].str()));
  }
  for (std::sregex_iterator match(source.begin(), source.end(), plugin_url_pattern), end; match != end; ++match) {
    providers.insert(to_lower_ascii((*match)[1].str()));
  }
  return providers;
}

PackageInspection inspect_packages(const CacheCandidate& candidate, const std::atomic_bool* cancel_requested) {
  PackageInspection inspection;
  auto packages = candidate.package_files.empty() ? find_wxapkg_files(candidate.package_root) : candidate.package_files;
  std::stable_sort(packages.begin(), packages.end(), [](const fs::path& left, const fs::path& right) {
    const auto left_name = to_lower_ascii(wide_to_utf8(left.filename().wstring()));
    const auto right_name = to_lower_ascii(wide_to_utf8(right.filename().wstring()));
    const auto rank = [](const std::string& name) {
      if (name == "__app__.wxapkg" || name == "__full__.wxapkg") return 0;
      if (name == "__without_multi_plugincode__.wxapkg" || name == "__subpackage__.wxapkg") return 1;
      if (name.find("wasmcode") != std::string::npos) return 2;
      if (name == "__plugincode__.wxapkg") return 4;
      return 3;
    };
    if (rank(left_name) != rank(right_name)) return rank(left_name) < rank(right_name);
    return left_name < right_name;
  });

  bool saw_game{};
  bool saw_program{};
  bool saw_non_plugin{};
  auto metadata_candidates = names_from_local_metadata(candidate, cancel_requested);
  std::vector<std::string> entry_title_candidates;
  std::vector<std::string> global_title_candidates;
  std::vector<std::string> serialized_ui_candidates;
  std::vector<std::string> page_title_candidates;
  std::vector<std::string> precise_code_candidates;
  std::vector<std::string> auxiliary_code_candidates;
  std::vector<std::string> local_identity_candidates;
  std::vector<std::string> local_product_config_candidates;
  std::uintmax_t auxiliary_code_bytes{};
  constexpr std::uintmax_t max_auxiliary_code_bytes = 48U * 1024U * 1024U;
  constexpr std::array<std::string_view, 8> preferred_code_files{
      "common/vendor.js", "app-service.js", "app.js", "game.js",
      "common/main.js", "main.js", "vendor.js", "manifest.js"};

  for (const auto& package_path : packages) {
    if (cancelled(cancel_requested)) break;
    try {
      auto bytes = read_binary(package_path);
      if (!Wxapkg::has_valid_header(bytes)) bytes = Wxapkg::decrypt(candidate.wxid, bytes);
      if (!Wxapkg::has_valid_header(bytes)) continue;
      const auto entries = Wxapkg::read_file_index(bytes);
      const auto package_name = to_lower_ascii(wide_to_utf8(package_path.filename().wstring()));
      if (const auto subpackage = subpackage_name_from_filename(package_path)) {
        inspection.available_subpackages.insert(*subpackage);
      }
      if (package_name != "__plugincode__.wxapkg") saw_non_plugin = true;
      if (package_name.find("wasmcode") != std::string::npos || package_name == "__without_multi_plugincode__.wxapkg") {
        saw_game = true;
      }
      for (const auto& entry : entries) {
        auto path = to_lower_ascii(entry.packed_path);
        while (!path.empty() && path.front() == '/') path.erase(path.begin());
        record_available_subpackage_path(inspection, path);
        const auto filename_position = path.find_last_of('/');
        const auto filename = filename_position == std::string::npos ? path : path.substr(filename_position + 1);
        if (filename == "game.js" || filename == "game.json" || path.find("open-data") != std::string::npos) saw_game = true;
        if (filename == "app-service.js" || filename == "app.json") saw_program = true;

        const bool metadata_file = filename == "app-config.json" || filename == "app.json" || filename == "game.json" ||
                                   filename == "appinfo.json" || filename == "plugin.json" ||
                                   filename == "project.config.json" || filename == "project.private.config.json" ||
                                   filename == "ext.json" || filename == "extconfig.json" ||
                                   path.find("manifest") != std::string::npos;
        if (metadata_file && entry.size <= 4U * 1024U * 1024U) {
          const auto text = entry_text(bytes, entry);
          inspect_unity_loader_hints(text, inspection);
          if (auto name = best_name_from_text(text)) metadata_candidates.push_back(std::move(*name));
          // A page-level title commonly describes Login, Backpack or Settings.
          // Only the root app/game configuration is allowed to provide this
          // low-confidence fallback.
          if (path == "app.json" || path == "game.json") {
            if (auto name = best_page_title_from_text(text)) page_title_candidates.push_back(std::move(*name));
          }
          if (filename == "app-config.json") {
            if (auto name = best_entry_title_from_app_config(text)) {
              entry_title_candidates.push_back(std::move(*name));
            }
            if (auto name = best_global_title_from_app_config(text)) {
              global_title_candidates.push_back(std::move(*name));
            }
            static const std::regex subpackage_root_pattern(
                R"re(["']root["']\s*:\s*["']/?([^"'/]+?)/?["'])re",
                std::regex::icase);
            for (std::sregex_iterator match(text.begin(), text.end(), subpackage_root_pattern), end;
                 match != end; ++match) {
              inspection.declared_subpackages.insert(to_lower_ascii((*match)[1].str()));
            }
          }
          const auto providers = plugin_providers_from_text(text);
          inspection.plugin_wxids.insert(providers.begin(), providers.end());
        }

        // Cocos/Unity projects frequently keep the actual product identity in
        // imported JSON assets instead of app.json/game.json. Only JSON files
        // that literally contain this project's WXID are considered, and the
        // name extractor still requires the name and WXID to belong to the
        // same bounded object.
        if (!metadata_file && filename.ends_with(".json") && entry.size <= 4U * 1024U * 1024U) {
          const auto text = entry_text(bytes, entry);
          const bool loading_identity_asset =
              path.find("loading") != std::string::npos ||
              path.find("splash") != std::string::npos ||
              path.find("launch") != std::string::npos ||
              path.find("startup") != std::string::npos;
          if (loading_identity_asset) {
            if (auto name = best_serialized_ui_product_name(text)) {
              serialized_ui_candidates.push_back(std::move(*name));
            }
          }
          if (text.find(candidate.wxid) != std::string::npos) {
            if (auto name = best_name_from_code(text, candidate.wxid); name && name->identity_matched) {
              precise_code_candidates.push_back(std::move(name->name));
            }
          }
        }

        const bool precise_code_file =
            std::find(preferred_code_files.begin(), preferred_code_files.end(), path) != preferred_code_files.end();
        const bool javascript_file = filename.ends_with(".js");
        const bool bounded_auxiliary_file =
            javascript_file && entry.size <= 6U * 1024U * 1024U &&
            auxiliary_code_bytes + entry.size <= max_auxiliary_code_bytes;
        if ((precise_code_file || bounded_auxiliary_file) && entry.size <= 24U * 1024U * 1024U) {
          if (!precise_code_file) auxiliary_code_bytes += entry.size;
          const auto text = entry_text(bytes, entry);
          inspect_unity_loader_hints(text, inspection);
          if (auto name = best_name_from_code(text, candidate.wxid)) {
            if (precise_code_file || name->identity_matched) {
              precise_code_candidates.push_back(std::move(name->name));
            } else {
              auxiliary_code_candidates.push_back(std::move(name->name));
            }
          }
          const auto providers = plugin_providers_from_text(text);
          inspection.plugin_wxids.insert(providers.begin(), providers.end());
        }
      }
    } catch (...) {
      // A single partially-downloaded cache package should not prevent discovery of the app.
    }
  }

  auto local_runtime_evidence =
      inspect_local_runtime_records(candidate, inspection, cancel_requested);
  local_identity_candidates = std::move(local_runtime_evidence.identity_names);
  local_product_config_candidates =
      std::move(local_runtime_evidence.product_config_names);

  auto choose_best = [](std::vector<std::string>& candidates) -> std::string {
    if (candidates.empty()) return {};
    std::stable_sort(candidates.begin(), candidates.end(), [](const std::string& left, const std::string& right) {
      if (has_cjk(left) != has_cjk(right)) return has_cjk(left);
      return left.size() < right.size();
    });
    return candidates.front();
  };
  auto choose_repeated = [](const std::vector<std::string>& candidates) -> std::string {
    struct NameFrequency {
      std::size_t count{};
      std::string display;
    };
    std::map<std::string, NameFrequency> frequencies;
    for (const auto& candidate_name : candidates) {
      auto& frequency = frequencies[to_lower_ascii(candidate_name)];
      ++frequency.count;
      if (frequency.display.empty()) frequency.display = candidate_name;
    }

    std::vector<NameFrequency> repeated;
    repeated.reserve(frequencies.size());
    for (auto& [key, frequency] : frequencies) {
      (void)key;
      if (frequency.count >= 2) repeated.push_back(std::move(frequency));
    }
    if (repeated.empty()) return {};
    std::stable_sort(repeated.begin(), repeated.end(), [](const NameFrequency& left, const NameFrequency& right) {
      if (left.count != right.count) return left.count > right.count;
      if (has_cjk(left.display) != has_cjk(right.display)) return has_cjk(left.display);
      return left.display.size() < right.display.size();
    });
    return repeated.front().display;
  };
  inspection.app_name = choose_best(precise_code_candidates);
  if (!inspection.app_name.empty()) {
    inspection.app_name_score = 95;
  }
  if (inspection.app_name.empty()) {
    inspection.app_name = choose_best(local_identity_candidates);
    if (!inspection.app_name.empty()) inspection.app_name_score = 94;
  }
  if (inspection.app_name.empty()) {
    inspection.app_name = choose_best(entry_title_candidates);
    if (!inspection.app_name.empty()) inspection.app_name_score = 91;
  }
  if (inspection.app_name.empty()) {
    inspection.app_name = choose_best(global_title_candidates);
    if (!inspection.app_name.empty()) inspection.app_name_score = 90;
  }
  if (inspection.app_name.empty()) {
    inspection.app_name = choose_repeated(local_product_config_candidates);
    if (!inspection.app_name.empty()) inspection.app_name_score = 88;
  }
  if (inspection.app_name.empty()) {
    inspection.app_name = choose_best(metadata_candidates);
    if (!inspection.app_name.empty()) inspection.app_name_score = 85;
  }
  if (inspection.app_name.empty()) {
    // Loading/splash text can be a scene title or slogan, so it is below
    // package identity and local product configuration evidence.
    inspection.app_name = choose_best(serialized_ui_candidates);
    if (!inspection.app_name.empty()) inspection.app_name_score = 82;
  }
  if (inspection.app_name.empty()) {
    // Auxiliary bundles contain many embedded modules. Requiring the same
    // candidate in multiple files filters isolated component/internal names
    // while still recovering product names copied into split bundles.
    inspection.app_name = choose_repeated(auxiliary_code_candidates);
    if (!inspection.app_name.empty()) inspection.app_name_score = 65;
  }
  if (inspection.app_name.empty()) {
    inspection.app_name = choose_best(page_title_candidates);
    if (!inspection.app_name.empty()) inspection.app_name_score = 40;
  }
  // An explicit mini-program entry point is stronger than auxiliary game-like
  // filenames that can appear in shared/runtime bundles.
  if (saw_program) inspection.kind = "小程序";
  else if (saw_game) inspection.kind = "小游戏";
  else if (!saw_non_plugin) inspection.kind = "插件包";
  if (inspection.kind == "插件包") {
    if (auto name = known_plugin_name(candidate.wxid)) {
      inspection.app_name = std::move(*name);
      inspection.app_name_score = 100;
    }
  }
  return inspection;
}

bool candidate_is_better(const CacheCandidate& candidate, const CacheCandidate& current) {
  const auto candidate_version = numeric_version(candidate.package_root);
  const auto current_version = numeric_version(current.package_root);
  if (candidate_version && current_version && *candidate_version != *current_version) return *candidate_version > *current_version;
  if (candidate.package_count != current.package_count) return candidate.package_count > current.package_count;
  return candidate.newest_write > current.newest_write;
}

bool candidate_is_plugin_only(const CacheCandidate& candidate) {
  return !candidate.package_files.empty() &&
         std::all_of(candidate.package_files.begin(), candidate.package_files.end(), [](const fs::path& package) {
           return to_lower_ascii(wide_to_utf8(package.filename().wstring())) == "__plugincode__.wxapkg";
         });
}

InputFolder provisional_input(const CacheCandidate& candidate,
                              const std::string_view account_name) {
  InputFolder input;
  input.path = fs::absolute(candidate.package_root).lexically_normal();
  input.wxid = candidate.wxid;
  input.display_name = candidate.wxid;
  input.kind = "正在整理";
  input.version = candidate.version;
  input.account_key = candidate.account_key;
  input.account_name = std::string(account_name);
  input.icon_path = candidate.icon_path;
  input.icon_data_uri = candidate.icon_data_uri;
  input.runtime_cache_path = candidate.runtime_cache_path;
  input.package_files = candidate.package_files;
  input.package_count = candidate.package_count;
  input.account_count = 1;
  input.app_name_score = 0;
  input.scan_ready = false;
  input.auto_discovered = true;
  return input;
}

void apply_persisted_name(const PersistentNameMap& persisted_names,
                          const CacheCandidate& candidate,
                          PackageInspection& inspection,
                          const bool inspection_cache_hit) {
  if (inspection.kind == "插件包") return;
  const auto cached_name = persisted_names.names.find(to_lower_ascii(candidate.wxid));
  if (cached_name == persisted_names.names.end()) return;
  if (inspection_cache_hit || inspection.app_name_score < 95) {
    inspection.app_name = cached_name->second;
    inspection.app_name_score = std::max(inspection.app_name_score, 98);
  }
}

InputFolder inspection_progress_input(const CacheCandidate& candidate,
                                      const PackageInspection& inspection,
                                      const std::string_view account_name) {
  auto input = provisional_input(candidate, account_name);
  input.app_name = inspection.app_name;
  input.display_name = inspection.app_name.empty() ? candidate.wxid : inspection.app_name;
  input.kind = inspection.kind;
  input.app_name_score = inspection.app_name_score;
  return input;
}

void emit_inspection_progress(const CacheCandidate& candidate,
                              const PackageInspection& inspection,
                              const std::string_view account_name,
                              const CacheScanUpdateSink& update_sink) {
  if (!update_sink || inspection.kind == "插件包") return;
  CacheScanUpdate progress;
  progress.inputs.push_back(inspection_progress_input(candidate, inspection, account_name));
  update_sink(std::move(progress));
}

CacheScanResult scan_descriptors(const std::vector<RootDescriptor>& roots,
                                 const Logger& logger,
                                 const std::atomic_bool* cancel_requested,
                                 const fs::path& cache_directory,
                                 const CacheScanUpdateSink& update_sink) {
  CacheScanResult result;
  result.cache_root_count = roots.size();
  const auto persisted_names = load_persistent_name_map(cache_directory);
  std::set<std::string> accounts;
  for (const auto& root : roots) accounts.insert(root.account_key);

  std::map<std::string, std::string> account_names;
  std::size_t account_number{};
  for (const auto& account : accounts) {
    if (account == "shared-v4") {
      account_names[account] = "微信公共缓存";
      continue;
    }
    if (account == "legacy-v3") {
      account_names[account] = "微信旧版公共缓存";
      continue;
    }
    ++account_number;
    const auto short_key = account.substr(0, std::min<std::size_t>(8, account.size()));
    account_names[account] = "微信账号 " + std::to_string(account_number) +
                             (short_key.empty() ? std::string{} : " · " + short_key);
  }

  std::map<std::string, CacheCandidate> best_by_account_wxid;
  std::map<std::string, fs::path> icon_by_account_wxid;
  std::map<std::string, fs::path> icon_by_wxid;

  const auto scoped_key = [](const std::string_view account_key, const std::string_view wxid) {
    return to_lower_ascii(std::string(account_key)) + "\n" + to_lower_ascii(std::string(wxid));
  };

  std::vector<std::vector<CacheCandidate>> candidates_by_root(roots.size());
  const auto root_workers = parallel_for(roots.size(), cancel_requested, 4, [&](const std::size_t index) {
    try {
      candidates_by_root[index] = candidates_from_root(
          roots[index],
          [&](const CacheCandidate& candidate) {
            if (!update_sink || candidate_is_plugin_only(candidate)) return;
            CacheScanUpdate progress;
            progress.inputs.push_back(
                provisional_input(candidate, account_names.at(roots[index].account_key)));
            update_sink(std::move(progress));
          });
    } catch (...) {
      // One damaged or inaccessible account root should not block the others.
    }
  });
  if (root_workers > 1) {
    logger.info("已启用 " + std::to_string(root_workers) + " 路并发目录扫描。");
  }

  for (std::size_t root_index = 0; root_index < roots.size(); ++root_index) {
    if (cancelled(cancel_requested)) break;
    for (auto& candidate : candidates_by_root[root_index]) {
      const auto wxid_key = to_lower_ascii(candidate.wxid);
      const auto key = scoped_key(candidate.account_key, candidate.wxid);
      if (!candidate.icon_path.empty()) {
        icon_by_account_wxid[key] = candidate.icon_path;
        icon_by_wxid[wxid_key] = candidate.icon_path;
      }
      const auto existing = best_by_account_wxid.find(key);
      if (existing == best_by_account_wxid.end() || candidate_is_better(candidate, existing->second)) {
        best_by_account_wxid[key] = std::move(candidate);
      }
    }
  }

  result.account_count = accounts.size();

  // Each account keeps its own newest copy of a WXID. A dependency may fall
  // back to a shared WeChat cache, but never to another user's private cache.
  std::map<std::string, std::string> fallback_candidate_by_wxid;
  for (const auto& [key, candidate] : best_by_account_wxid) {
    if (candidate.account_key != "shared-v4" && candidate.account_key != "legacy-v3") continue;
    const auto wxid_key = to_lower_ascii(candidate.wxid);
    const auto fallback = fallback_candidate_by_wxid.find(wxid_key);
    if (fallback == fallback_candidate_by_wxid.end() ||
        candidate_is_better(candidate, best_by_account_wxid.at(fallback->second))) {
      fallback_candidate_by_wxid[wxid_key] = key;
    }
  }

  std::map<std::string, InputFolder> all_inputs;
  std::map<std::string, std::set<std::string>> dependencies_by_input;
  struct CandidateWork {
    std::string key;
    CacheCandidate* candidate{};
    PackageInspection inspection;
    std::string icon_data_uri;
    std::string analysis_key;
    std::string signature;
    std::string account_name;
    bool completed{};
    bool inspection_cache_hit{};
  };
  std::vector<CandidateWork> work_items;
  work_items.reserve(best_by_account_wxid.size());

  for (auto& [key, candidate] : best_by_account_wxid) {
    const auto wxid_key = to_lower_ascii(candidate.wxid);
    if (candidate.icon_path.empty()) {
      const auto account_icon = icon_by_account_wxid.find(key);
      if (account_icon != icon_by_account_wxid.end()) candidate.icon_path = account_icon->second;
      else if (const auto icon = icon_by_wxid.find(wxid_key); icon != icon_by_wxid.end()) candidate.icon_path = icon->second;
    }
    if (candidate.icon_data_uri.empty() && !candidate.icon_path.empty()) {
      candidate.icon_data_uri = png_data_uri(candidate.icon_path);
    }
    CandidateWork work;
    work.key = key;
    work.candidate = &candidate;
    work.icon_data_uri = candidate.icon_data_uri;
    work.analysis_key = analysis_cache_key(candidate);
    work.signature = package_signature(candidate);
    work.account_name = account_names.at(candidate.account_key);
    if (auto cached = load_cached_inspection(cache_directory, work.analysis_key, work.signature)) {
      work.inspection = std::move(*cached);
      work.completed = true;
      work.inspection_cache_hit = true;
      apply_persisted_name(persisted_names, candidate, work.inspection, true);
      ++result.inspection_cache_hits;
    } else {
      ++result.inspection_cache_misses;
    }
    work_items.push_back(std::move(work));
    if (work_items.back().completed) {
      emit_inspection_progress(candidate,
                               work_items.back().inspection,
                               work_items.back().account_name,
                               update_sink);
    }
  }

  result.runtime_name_scan_skipped = true;

  std::vector<std::size_t> inspection_misses;
  inspection_misses.reserve(result.inspection_cache_misses);
  for (std::size_t index = 0; index < work_items.size(); ++index) {
    if (!work_items[index].inspection_cache_hit) inspection_misses.push_back(index);
  }
  const auto inspection_workers = parallel_for(inspection_misses.size(), cancel_requested, 8, [&](const std::size_t miss_index) {
    const auto index = inspection_misses[miss_index];
    try {
      work_items[index].inspection = inspect_packages(*work_items[index].candidate, cancel_requested);
      apply_persisted_name(persisted_names,
                           *work_items[index].candidate,
                           work_items[index].inspection,
                           false);
      work_items[index].completed = true;
      emit_inspection_progress(*work_items[index].candidate,
                               work_items[index].inspection,
                               work_items[index].account_name,
                               update_sink);
    } catch (...) {
      // Keep the remaining projects available when one cache entry is damaged.
    }
  });
  if (result.inspection_cache_hits != 0) {
    logger.info("包分析缓存命中 " + std::to_string(result.inspection_cache_hits) +
                " 个账号内项目，已跳过重复解密与内容分析。");
  }
  if (inspection_workers > 1) {
    logger.info("已启用 " + std::to_string(inspection_workers) + " 路并发包分析，共 " +
                std::to_string(inspection_misses.size()) + " 个新增或发生变化的账号内项目。");
  } else if (!inspection_misses.empty()) {
    logger.info("正在分析 " + std::to_string(inspection_misses.size()) + " 个新增或发生变化的账号内项目。");
  }

  std::set<std::string> reused_name_wxids;
  for (auto& work : work_items) {
    if (!work.completed || work.inspection.kind == "插件包") continue;
    const auto wxid = to_lower_ascii(work.candidate->wxid);
    const auto cached_name = persisted_names.names.find(wxid);
    if (cached_name == persisted_names.names.end()) continue;
    reused_name_wxids.insert(wxid);
  }
  result.name_cache_hits = reused_name_wxids.size();
  if (!reused_name_wxids.empty()) {
    logger.info("已从本地名称映射复用 " + std::to_string(reused_name_wxids.size()) +
                " 个项目名称，不再重复获取。");
  }

  std::map<std::string, std::pair<int, std::string>> resolved_names_by_wxid;
  for (auto& work : work_items) {
    if (!work.completed) continue;
    auto& inspection = work.inspection;
    const auto wxid_key = to_lower_ascii(work.candidate->wxid);
    if (!inspection.app_name.empty()) {
      const auto existing = resolved_names_by_wxid.find(wxid_key);
      if (existing == resolved_names_by_wxid.end() || inspection.app_name_score > existing->second.first) {
        resolved_names_by_wxid[wxid_key] = {inspection.app_name_score, inspection.app_name};
      }
    }
  }

  for (auto& work : work_items) {
    if (!work.completed) continue;
    const auto wxid_key = to_lower_ascii(work.candidate->wxid);
    if (const auto resolved = resolved_names_by_wxid.find(wxid_key); resolved != resolved_names_by_wxid.end()) {
      if (resolved->second.first > work.inspection.app_name_score) {
        work.inspection.app_name_score = resolved->second.first;
        work.inspection.app_name = resolved->second.second;
      }
    }
  }

  auto updated_names = persisted_names.names;
  for (const auto& work : work_items) {
    if (!work.completed || work.inspection.kind == "插件包" ||
        work.inspection.app_name.empty() || work.inspection.app_name_score < 70) {
      continue;
    }
    updated_names[to_lower_ascii(work.candidate->wxid)] = work.inspection.app_name;
  }
  try {
    if (!persisted_names.file_exists || updated_names != persisted_names.names) {
      save_persistent_name_map(cache_directory, updated_names);
      logger.info("项目名称映射已保存到本地，下次启动直接复用。");
    }
  } catch (const std::exception& error) {
    logger.warning("保存本地名称映射失败：" + std::string(error.what()));
  }

  std::size_t analysis_cache_saved{};
  for (const auto index : inspection_misses) {
    const auto& work = work_items[index];
    if (!work.completed) continue;
    try {
      save_cached_inspection(cache_directory, work.analysis_key, work.signature, work.inspection);
      ++analysis_cache_saved;
    } catch (...) {
      // A cache write failure should not discard a successfully scanned project.
    }
  }
  if (analysis_cache_saved != 0) {
    logger.info("已保存 " + std::to_string(analysis_cache_saved) +
                " 个包分析结果；文件未变化时将直接命中。");
  }

  for (auto& work : work_items) {
    if (!work.completed) continue;
    auto& candidate = *work.candidate;
    auto& inspection = work.inspection;
    InputFolder input;
    input.path = fs::absolute(candidate.package_root).lexically_normal();
    input.wxid = candidate.wxid;
    input.app_name = inspection.app_name;
    input.display_name = inspection.app_name.empty() ? candidate.wxid : inspection.app_name;
    input.kind = inspection.kind;
    input.version = candidate.version;
    input.account_key = candidate.account_key;
    input.account_name = account_names[candidate.account_key];
    input.icon_path = candidate.icon_path;
    input.icon_data_uri = std::move(work.icon_data_uri);
    input.runtime_cache_path = candidate.runtime_cache_path;
    input.package_files = candidate.package_files;
    input.package_count = candidate.package_count;
    input.app_name_score = inspection.app_name_score;
    input.scan_ready = true;
    input.declared_subpackages.assign(inspection.declared_subpackages.begin(), inspection.declared_subpackages.end());
    for (const auto& subpackage : inspection.declared_subpackages) {
      if (inspection.available_subpackages.contains(subpackage)) continue;
      if (subpackage == "wasmcode2" && inspection.wasmcode2_platform_conditional) {
        input.platform_conditional_subpackages.push_back(subpackage);
      } else if (subpackage == "data-package" && inspection.data_package_uses_runtime_or_external_source) {
        input.runtime_or_external_subpackages.push_back(subpackage);
      } else {
        input.missing_subpackages.push_back(subpackage);
      }
    }
    input.account_count = 1;
    input.auto_discovered = true;
    result.package_count += candidate.package_count;
    if (input.kind == "插件包") ++result.plugin_cache_count;
    dependencies_by_input[work.key] = std::move(inspection.plugin_wxids);
    all_inputs.emplace(work.key, std::move(input));
  }

  for (auto& [key, input] : all_inputs) {
    if (input.kind == "插件包") continue;
    const auto dependency_ids = dependencies_by_input.find(key);
    if (dependency_ids != dependencies_by_input.end()) {
      for (const auto& dependency_wxid : dependency_ids->second) {
        const auto dependency_wxid_key = to_lower_ascii(dependency_wxid);
        if (dependency_wxid_key == to_lower_ascii(input.wxid)) continue;

        auto dependency = all_inputs.find(scoped_key(input.account_key, dependency_wxid_key));
        if (dependency == all_inputs.end()) {
          const auto fallback = fallback_candidate_by_wxid.find(dependency_wxid_key);
          if (fallback != fallback_candidate_by_wxid.end()) dependency = all_inputs.find(fallback->second);
        }
        if (dependency == all_inputs.end()) {
          input.missing_dependency_wxids.push_back(dependency_wxid);
          continue;
        }

        PackageDependency package_dependency;
        package_dependency.path = dependency->second.path;
        package_dependency.wxid = dependency->second.wxid;
        package_dependency.display_name = dependency->second.display_name;
        package_dependency.version = dependency->second.version;
        package_dependency.package_count = dependency->second.package_count;
        package_dependency.package_files = dependency->second.package_files;
        result.dependency_package_count += package_dependency.package_count;
        input.dependencies.push_back(std::move(package_dependency));
      }
    }
    std::stable_sort(input.dependencies.begin(), input.dependencies.end(), [](const PackageDependency& left,
                                                                             const PackageDependency& right) {
      return to_lower_ascii(left.display_name) < to_lower_ascii(right.display_name);
    });
    result.inputs.push_back(input);
  }

  std::stable_sort(result.inputs.begin(), result.inputs.end(), [](const InputFolder& left, const InputFolder& right) {
    if (left.account_key != right.account_key) {
      return to_lower_ascii(left.account_key) < to_lower_ascii(right.account_key);
    }
    return to_lower_ascii(left.wxid) < to_lower_ascii(right.wxid);
  });

  const auto named_count = std::count_if(result.inputs.begin(), result.inputs.end(), [](const InputFolder& input) {
    return !input.app_name.empty();
  });
  logger.info("本机微信缓存扫描完成：" + std::to_string(result.account_count) + " 个账号目录，" +
              std::to_string(result.inputs.size()) + " 个账号内可选项目，识别到 " +
              std::to_string(named_count) + " 个项目名称、" +
              std::to_string(result.plugin_cache_count) + " 个账号内依赖插件缓存，共 " +
              std::to_string(result.package_count) + " 个可用缓存包（包含当前版本与本机仍保留的全部历史版本包）。");
  return result;
}

}  // namespace

std::map<std::string, std::string> extract_runtime_app_names(
    const std::string_view text,
    const std::set<std::string>& relevant_wxids) {
  std::set<std::string> normalized_wxids;
  for (const auto& wxid : relevant_wxids) normalized_wxids.insert(to_lower_ascii(wxid));
  RuntimeNameVotes votes;
  collect_runtime_name_pairs(text, normalized_wxids, votes);
  collect_utf16le_runtime_name_pairs(text, normalized_wxids, votes);
  return resolve_runtime_name_votes(std::move(votes));
}

std::optional<std::string> extract_marketplace_app_name(const std::string_view html) {
  return marketplace_app_name_from_html(html);
}

WechatCacheScanner::WechatCacheScanner(Logger logger, fs::path cache_directory)
    : logger_(std::move(logger)),
      cache_directory_(cache_directory.empty()
                           ? application_data_directory()
                           : fs::absolute(std::move(cache_directory)).lexically_normal()) {}

CacheScanResult WechatCacheScanner::scan(const std::atomic_bool* cancel_requested,
                                         CacheScanUpdateSink update_sink) const {
  const auto roots = discover_applet_roots();
  if (roots.empty()) {
    logger_.warning("未发现微信小程序缓存目录，可继续使用拖入或手动选择。");
    return {};
  }
  logger_.info("正在读取本机全部微信账号的小程序缓存目录...");
  return scan_descriptors(roots, logger_, cancel_requested, cache_directory_,
                          update_sink);
}

CacheScanResult WechatCacheScanner::scan_applet_roots(const std::vector<fs::path>& applet_roots,
                                                       const std::atomic_bool* cancel_requested,
                                                       CacheScanUpdateSink update_sink) const {
  std::vector<RootDescriptor> roots;
  std::size_t index{};
  for (const auto& root : applet_roots) add_root_descriptor(roots, root, "test-" + std::to_string(index++));
  return scan_descriptors(roots, logger_, cancel_requested, cache_directory_,
                          update_sink);
}

std::map<std::string, std::string> WechatCacheScanner::resolve_runtime_names(
    const std::vector<InputFolder>& inputs,
    const std::atomic_bool* cancel_requested,
    RuntimeNameUpdateSink update_sink) const {
  const auto persisted_names = load_persistent_name_map(cache_directory_);
  std::map<std::string, std::string> resolved;
  std::set<std::string> relevant_wxids;

  for (const auto& input : inputs) {
    if (input.kind == "插件包" || input.app_name_score >= 95) continue;
    const auto wxid = to_lower_ascii(input.wxid);
    if (const auto cached = persisted_names.names.find(wxid);
        cached != persisted_names.names.end()) {
      resolved[wxid] = cached->second;
      if (update_sink) update_sink(wxid, cached->second);
    } else if (!known_plugin_name(wxid)) {
      relevant_wxids.insert(wxid);
    }
  }

  if (!relevant_wxids.empty() && !cancelled(cancel_requested)) {
    logger_.info("后台正在补全 " + std::to_string(relevant_wxids.size()) +
                 " 个尚未命名项目；项目列表与反编译操作可继续使用。");
    const std::vector<std::string> lookup_ids(relevant_wxids.begin(), relevant_wxids.end());
    std::map<std::string, std::string> remote_names;
    std::set<std::string> live_resolved;
    std::mutex publication_mutex;

    std::jthread remote_worker([&] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      parallel_for(lookup_ids.size(), cancel_requested, 4, [&](const std::size_t index) {
        try {
          const auto name = remote_app_name(lookup_ids[index]);
          if (!name || cancelled(cancel_requested)) return;
          std::lock_guard lock(publication_mutex);
          remote_names[lookup_ids[index]] = *name;
        } catch (...) {
          // Keep the remaining background lookups available when one endpoint
          // or response is malformed.
        }
      });
    });

    const auto live_sink = [&](std::string wxid, std::string name) {
      std::lock_guard lock(publication_mutex);
      live_resolved.insert(wxid);
      if (update_sink) update_sink(std::move(wxid), std::move(name));
    };
    auto live_names = runtime_app_names(relevant_wxids, cancel_requested, live_sink);
    remote_worker.join();

    // Local WeChat runtime identity is published first. Online indexes run in
    // parallel for speed, but are shown only for WXIDs that local records did
    // not resolve, avoiding a lower-confidence title flashing before the
    // machine's own project name.
    for (const auto& [wxid, name] : remote_names) {
      if (!live_resolved.contains(wxid) && update_sink) update_sink(wxid, name);
    }

    resolved.insert(remote_names.begin(), remote_names.end());
    for (const auto& [wxid, name] : live_names) resolved[wxid] = name;

    if (!remote_names.empty()) {
      logger_.info("后台在线项目索引补全了 " + std::to_string(remote_names.size()) +
                   " 个项目名称。");
    }
    if (!live_names.empty()) {
      logger_.info("后台微信运行记录补全了 " + std::to_string(live_names.size()) +
                   " 个项目名称。");
    }

    if (!remote_names.empty() || !live_names.empty()) {
      auto updated_names = persisted_names.names;
      for (const auto& [wxid, name] : remote_names) updated_names[wxid] = name;
      for (const auto& [wxid, name] : live_names) updated_names[wxid] = name;
      try {
        save_persistent_name_map(cache_directory_, updated_names);
        logger_.info("项目名称映射已保存到本地，下次启动直接复用。");
      } catch (const std::exception& error) {
        logger_.warning("保存后台名称映射失败：" + std::string(error.what()));
      }
    }
  }
  return resolved;
}

}  // namespace wedecode
