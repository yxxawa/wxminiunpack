#include "wedecode/batch_runner.h"

#include "wedecode/reconstructor.h"
#include "wedecode/wxapkg.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cwctype>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace wedecode {
namespace {

std::wstring identity_for_path(const fs::path& path) {
  std::wstring result = path.wstring();
  std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  return result;
}

std::string filename_utf8(const fs::path& path) {
  return wide_to_utf8(path.filename().wstring());
}

bool is_main_package(const fs::path& path) {
  const auto name = to_lower_ascii(filename_utf8(path));
  return name == "__app__.wxapkg" ||
         name == "app.wxapkg" ||
         name == "__full__.wxapkg" ||
         name == "__without_multi_plugincode__.wxapkg" ||
         name == "__subpackage__.wxapkg";
}

std::string safe_directory_name(std::string value, const std::string_view fallback) {
  for (char& character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20U || character == '<' || character == '>' || character == ':' || character == '"' ||
        character == '/' || character == '\\' || character == '|' || character == '?' || character == '*') {
      character = '_';
    }
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
  if (value.empty() || value == "." || value == "..") value = std::string(fallback);
  return value;
}

std::string package_slot_key(const fs::path& package) {
  return to_lower_ascii(filename_utf8(package));
}

std::optional<unsigned long long> numeric_version(const fs::path& path) {
  const auto text = wide_to_utf8(path.filename().wstring());
  if (text.empty() || !std::all_of(text.begin(), text.end(), [](const unsigned char character) {
        return std::isdigit(character) != 0;
      })) {
    return std::nullopt;
  }
  unsigned long long value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc()) return std::nullopt;
  return value;
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
    if (to_lower_ascii(wide_to_utf8(entry.path().extension().wstring())) == ".wxapkg") {
      packages.push_back(entry.path());
    }
  }
  return packages;
}

void append_unique_packages(std::vector<fs::path>& destination,
                            std::set<std::wstring>& seen,
                            std::vector<fs::path> packages) {
  std::stable_sort(packages.begin(), packages.end(), [](const fs::path& left, const fs::path& right) {
    return package_slot_key(left) < package_slot_key(right);
  });
  for (auto& package : packages) {
    std::error_code error;
    const auto absolute = fs::absolute(package, error).lexically_normal();
    if (error) continue;
    if (seen.insert(identity_for_path(absolute)).second) destination.push_back(std::move(absolute));
  }
}

std::vector<fs::path> refreshed_packages_for_input(const InputFolder& input) {
  if (!input.auto_discovered) {
    return input.package_files.empty() ? find_wxapkg_files(input.path) : input.package_files;
  }

  std::vector<fs::path> result;
  std::set<std::wstring> seen;
  append_unique_packages(result, seen, find_wxapkg_files(input.path));

  const auto wxid_root = input.path.parent_path();
  if (to_lower_ascii(filename_utf8(wxid_root)) == to_lower_ascii(input.wxid)) {
    std::vector<std::pair<unsigned long long, fs::path>> other_versions;
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
      if (identity_for_path(entry.path().lexically_normal()) == identity_for_path(input.path.lexically_normal())) continue;
      if (const auto version = numeric_version(entry.path())) {
        other_versions.emplace_back(*version, entry.path());
      }
    }
    std::stable_sort(other_versions.begin(), other_versions.end(), [](const auto& left, const auto& right) {
      return left.first > right.first;
    });
    for (const auto& [version, directory] : other_versions) {
      (void)version;
      append_unique_packages(result, seen, find_wxapkg_files(directory));
    }
  }

  append_unique_packages(result, seen, input.package_files);
  return result;
}

std::optional<std::string> subpackage_name_from_filename(const fs::path& package) {
  const auto name = package_slot_key(package);
  constexpr std::string_view extension = ".wxapkg";
  constexpr std::string_view wrapped_suffix = "_.wxapkg";
  constexpr std::string_view multi_prefix = "_subpackages_";
  if (!name.ends_with(extension)) return std::nullopt;
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

void remove_now_available_subpackages(InputFolder& input, const std::vector<fs::path>& packages) {
  std::set<std::string> available;
  for (const auto& package : packages) {
    if (const auto name = subpackage_name_from_filename(package)) available.insert(*name);
  }
  const auto remove_available = [&available](std::vector<std::string>& values) {
    values.erase(std::remove_if(values.begin(), values.end(), [&available](const std::string& value) {
      return available.contains(to_lower_ascii(value));
    }), values.end());
  };
  remove_available(input.missing_subpackages);
  remove_available(input.platform_conditional_subpackages);
  remove_available(input.runtime_or_external_subpackages);
}

fs::path output_directory_for_package(const fs::path& merged_directory,
                                      const fs::path& package,
                                      std::set<std::string>& merged_slots,
                                      std::vector<fs::path>& historical_directories) {
  if (merged_slots.insert(package_slot_key(package)).second) return merged_directory;

  auto version = wide_to_utf8(package.parent_path().filename().wstring());
  version = safe_directory_name(std::move(version), "unknown-version");
  auto package_name = wide_to_utf8(package.stem().wstring());
  package_name = safe_directory_name(std::move(package_name), "package");
  const auto historical_directory = make_unique_directory(
      merged_directory / L"_历史版本" / utf8_to_wide(version), package_name);
  historical_directories.push_back(historical_directory);
  return historical_directory;
}

struct RuntimeCacheCopyResult {
  std::size_t files_copied{};
  std::size_t files_failed{};
  std::uintmax_t bytes_copied{};
};

fs::path extended_length_path(const fs::path& path) {
  auto absolute = fs::absolute(path).lexically_normal().wstring();
  if (absolute.starts_with(L"\\\\?\\")) return fs::path(absolute);
  if (absolute.starts_with(L"\\\\")) return fs::path(L"\\\\?\\UNC\\" + absolute.substr(2));
  return fs::path(L"\\\\?\\" + absolute);
}

RuntimeCacheCopyResult copy_runtime_cache(const fs::path& source,
                                          const fs::path& destination,
                                          std::atomic_bool& cancel_requested,
                                          const Logger& logger) {
  RuntimeCacheCopyResult result;
  if (source.empty()) return result;

  std::error_code error;
  const auto extended_source = extended_length_path(source);
  if (!fs::is_directory(extended_source, error) || error) return result;
  const auto extended_destination = extended_length_path(destination);
  fs::create_directories(extended_destination, error);
  if (error) throw std::runtime_error("创建运行资源缓存输出目录失败：" + error.message());

  for (fs::recursive_directory_iterator iterator(extended_source, fs::directory_options::skip_permission_denied, error), end;
       iterator != end; iterator.increment(error)) {
    if (cancel_requested.load()) break;
    if (error) {
      ++result.files_failed;
      error.clear();
      continue;
    }
    const auto entry = *iterator;
    if (!entry.is_regular_file(error) || error) {
      error.clear();
      continue;
    }

    auto relative = fs::relative(entry.path(), extended_source, error);
    if (error || relative.empty()) {
      ++result.files_failed;
      error.clear();
      continue;
    }
    const auto output = destination / relative;
    const auto extended_output = extended_length_path(output);
    fs::create_directories(extended_output.parent_path(), error);
    if (error) {
      ++result.files_failed;
      error.clear();
      continue;
    }
    const auto size = entry.file_size(error);
    if (error) error.clear();
    fs::copy_file(entry.path(), extended_output, fs::copy_options::overwrite_existing, error);
    if (error) {
      ++result.files_failed;
      logger.warning("运行资源复制失败：" + wide_to_utf8(relative.wstring()) + "：" + error.message());
      error.clear();
      continue;
    }
    ++result.files_copied;
    result.bytes_copied += size;
  }
  return result;
}

std::string json_string_array(const std::vector<std::string>& values) {
  std::string json{"["};
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) json += ',';
    json += "\"" + json_escape(values[index]) + "\"";
  }
  json += ']';
  return json;
}

void write_unpack_manifest(const fs::path& project_directory,
                           const InputFolder& input,
                           const std::vector<fs::path>& packages,
                           const std::size_t package_successes,
                           const std::size_t package_failures,
                           const std::size_t files_extracted,
                            const RuntimeCacheCopyResult& runtime_cache,
                            const ReconstructionResult* reconstruction = nullptr) {
  std::size_t dependency_package_count{};
  for (const auto& dependency : input.dependencies) {
    dependency_package_count += dependency.package_files.empty() ? dependency.package_count : dependency.package_files.size();
  }

  std::ostringstream json;
  json << "{\n"
       << "  \"wxid\": \"" << json_escape(input.wxid) << "\",\n"
       << "  \"name\": \"" << json_escape(input.display_name) << "\",\n"
       << "  \"account\": \"" << json_escape(input.account_name) << "\",\n"
       << "  \"availablePackageCount\": " << packages.size() << ",\n"
       << "  \"successfulPackageCount\": " << package_successes << ",\n"
       << "  \"failedPackageCount\": " << package_failures << ",\n"
       << "  \"extractedFileCount\": " << files_extracted << ",\n"
       << "  \"sourceCompleteness\": \""
       << (package_failures == 0 && input.missing_subpackages.empty() && input.missing_dependency_wxids.empty() ?
           (input.platform_conditional_subpackages.empty() && input.runtime_or_external_subpackages.empty()
                ? "declared-packages-present"
                : "local-cache-complete-for-current-platform")
           : "partial") << "\",\n"
       << "  \"analysisCompleteness\": \""
       << (runtime_cache.files_copied != 0 ? "runtime-cache-supplemented"
                                          : (reconstruction && reconstruction->complete() ? "available-content-recovered" : "source-recovery-partial"))
       << "\",\n"
       << "  \"sourceReconstruction\": {\"status\":\""
       << (reconstruction ? (reconstruction->complete() ? "available-content-recovered" : "partial") : "not-attempted")
       << "\",\"report\":\"reconstruction-report.json\",\"missingSourceFileCount\":"
       << (reconstruction ? reconstruction->missing_source_files.size() : 0) << "},\n"
       << "  \"declaredSubpackages\": " << json_string_array(input.declared_subpackages) << ",\n"
       << "  \"subpackagesNotPresentInLocalWxapkgCache\": " << json_string_array(input.missing_subpackages) << ",\n"
       << "  \"platformConditionalSubpackages\": " << json_string_array(input.platform_conditional_subpackages) << ",\n"
       << "  \"runtimeOrExternalSubpackages\": " << json_string_array(input.runtime_or_external_subpackages) << ",\n"
       << "  \"packages\": [\n";
  for (std::size_t index = 0; index < packages.size(); ++index) {
    json << "    {\"name\":\"" << json_escape(filename_utf8(packages[index]))
         << "\",\"source\":\"" << json_escape(wide_to_utf8(packages[index].wstring())) << "\"}";
    if (index + 1 != packages.size()) json << ',';
    json << "\n";
  }
  json << "  ],\n"
       << "  \"dependencyPackageCount\": " << dependency_package_count << ",\n"
       << "  \"totalRelevantPackageCount\": " << (packages.size() + dependency_package_count) << ",\n"
       << "  \"dependencies\": [\n";
  for (std::size_t dependency_index = 0; dependency_index < input.dependencies.size(); ++dependency_index) {
    const auto& dependency = input.dependencies[dependency_index];
    json << "    {\"wxid\":\"" << json_escape(dependency.wxid)
         << "\",\"name\":\"" << json_escape(dependency.display_name)
         << "\",\"version\":\"" << json_escape(dependency.version)
         << "\",\"packageCount\":"
         << (dependency.package_files.empty() ? dependency.package_count : dependency.package_files.size())
         << ",\"packages\":[";
    for (std::size_t package_index = 0; package_index < dependency.package_files.size(); ++package_index) {
      if (package_index != 0) json << ',';
      json << "{\"name\":\"" << json_escape(filename_utf8(dependency.package_files[package_index]))
           << "\",\"source\":\""
           << json_escape(wide_to_utf8(dependency.package_files[package_index].wstring())) << "\"}";
    }
    json << "]}";
    if (dependency_index + 1 != input.dependencies.size()) json << ',';
    json << "\n";
  }
  json << "  ],\n"
       << "  \"runtimeCache\": {\"source\":\""
       << json_escape(wide_to_utf8(input.runtime_cache_path.wstring()))
       << "\",\"filesCopied\":" << runtime_cache.files_copied
       << ",\"filesFailed\":" << runtime_cache.files_failed
       << ",\"bytesCopied\":" << runtime_cache.bytes_copied << "}\n"
       << "}\n";
  write_text_utf8(project_directory / L"解包清单.json", json.str());
}

}  // namespace

BatchRunner::BatchRunner(Logger logger) : logger_(std::move(logger)) {}

std::vector<InputFolder> BatchRunner::prepare_inputs(const std::vector<fs::path>& dropped_paths) const {
  std::vector<InputFolder> inputs;
  std::set<std::wstring> seen;
  for (const auto& dropped : dropped_paths) {
    std::error_code ec;
    if (!fs::is_directory(dropped, ec) || ec) {
      logger_.warning("Ignored non-directory drop: " + wide_to_utf8(dropped.wstring()));
      continue;
    }
    const auto absolute = fs::absolute(dropped, ec).lexically_normal();
    if (ec) {
      logger_.warning("Ignored inaccessible directory: " + wide_to_utf8(dropped.wstring()));
      continue;
    }
    if (!seen.insert(identity_for_path(absolute)).second) continue;
    const auto name = filename_utf8(absolute);
    if (name.empty()) {
      logger_.warning("Ignored directory without a usable name: " + wide_to_utf8(absolute.wstring()));
      continue;
    }
    InputFolder input;
    input.path = absolute;
    input.wxid = name;
    input.display_name = name;
    input.package_count = find_packages(absolute).size();
    inputs.push_back(std::move(input));
  }
  return inputs;
}

std::vector<fs::path> BatchRunner::find_packages(const fs::path& input_directory) {
  std::vector<fs::path> packages;
  std::error_code error;
  packages = find_wxapkg_files(input_directory);
  sort_main_package_first(packages);
  return packages;
}

void BatchRunner::sort_main_package_first(std::vector<fs::path>& packages) {
  std::stable_sort(packages.begin(), packages.end(), [](const fs::path& left, const fs::path& right) {
    const bool left_main = is_main_package(left);
    const bool right_main = is_main_package(right);
    if (left_main != right_main) return left_main;
    if (package_slot_key(left) == package_slot_key(right)) {
      const auto left_version = numeric_version(left.parent_path());
      const auto right_version = numeric_version(right.parent_path());
      if (left_version.has_value() != right_version.has_value()) return left_version.has_value();
      if (left_version && right_version && *left_version != *right_version) return *left_version > *right_version;
    }
    return to_lower_ascii(filename_utf8(left)) < to_lower_ascii(filename_utf8(right));
  });
}

BatchSummary BatchRunner::run(const std::vector<InputFolder>& inputs, std::atomic_bool& cancel_requested) const {
  if (inputs.empty()) throw std::runtime_error("请选择至少一个输入目录后再开始");

  BatchSummary summary;
  const auto output_root = executable_directory() / "output";
  const bool named_project_output = std::all_of(inputs.begin(), inputs.end(), [](const InputFolder& input) {
    return input.auto_discovered;
  });
  const bool single_named_project = named_project_output && inputs.size() == 1;
  const auto output_parent = named_project_output
      ? output_root
      : make_unique_directory(output_root, timestamp_for_directory());
  summary.batch_output = output_parent;
  logger_.info("输出位置：" + wide_to_utf8(output_parent.wstring()));

  for (const auto& original_input : inputs) {
    auto input = original_input;
    if (cancel_requested.load()) {
      logger_.warning("任务已在处理 " + input.display_name + " 前停止");
      break;
    }

    const auto label = input.app_name.empty() ? input.display_name : input.app_name;
    const auto account_source = input.account_name.empty() ? std::string{} : "，来源账号：" + input.account_name;
    logger_.info("正在处理 " + label + "（WXID：" + input.wxid + account_source + "）");
    std::vector<fs::path> packages;
    try {
      packages = refreshed_packages_for_input(input);
      packages.erase(std::remove_if(packages.begin(), packages.end(), [](const fs::path& package) {
        std::error_code error;
        return !fs::is_regular_file(package, error) || error ||
               to_lower_ascii(wide_to_utf8(package.extension().wstring())) != ".wxapkg";
      }), packages.end());
      sort_main_package_first(packages);
      remove_now_available_subpackages(input, packages);
    } catch (const std::exception& error) {
      logger_.error(input.display_name + "：扫描包文件失败：" + error.what());
      ++summary.failed;
      continue;
    }
    if (packages.empty()) {
      logger_.error(input.display_name + "：目录中没有找到 .wxapkg 文件");
      ++summary.failed;
      continue;
    }

    const auto directory_label = named_project_output ? label : input.wxid;
    const auto project_directory = make_unique_directory(
        output_parent, safe_directory_name(directory_label, input.wxid));
    if (single_named_project) summary.batch_output = project_directory;
    logger_.info("主程序包含 " + std::to_string(packages.size()) + " 个包，输出到 " +
                 wide_to_utf8(project_directory.wstring()));

    std::size_t package_successes{};
    std::size_t package_failures{};
    std::size_t main_package_successes{};
    std::size_t main_package_failures{};
    std::size_t files_extracted{};
    std::size_t additional_failures{};
    std::set<std::string> merged_main_slots;
    std::vector<fs::path> historical_directories;
    for (const auto& package : packages) {
      if (cancel_requested.load()) break;
      try {
        const auto destination = output_directory_for_package(
            project_directory, package, merged_main_slots, historical_directories);
        if (destination != project_directory) {
          logger_.info("检测到同名历史包，单独保留到 " + wide_to_utf8(destination.wstring()));
        }
        const auto unpacked = Wxapkg::unpack_file(package, input.wxid, destination, logger_);
        files_extracted += unpacked.files_written;
        ++package_successes;
        ++main_package_successes;
      } catch (const std::exception& error) {
        ++package_failures;
        ++main_package_failures;
        logger_.error(input.display_name + " / " + filename_utf8(package) + "：" + error.what());
      }
    }

    std::vector<fs::path> dependency_directories;
    if (!input.dependencies.empty()) {
      const auto plugin_root = project_directory / L"通用插件";
      logger_.info("检测到 " + std::to_string(input.dependencies.size()) + " 个当前小程序实际引用的通用插件");
      for (const auto& dependency : input.dependencies) {
        if (cancel_requested.load()) break;
        std::vector<fs::path> dependency_packages;
        try {
          dependency_packages = dependency.package_files.empty() ? find_packages(dependency.path) : dependency.package_files;
          dependency_packages.erase(std::remove_if(dependency_packages.begin(), dependency_packages.end(), [](const fs::path& package) {
            std::error_code error;
            return !fs::is_regular_file(package, error) || error ||
                   to_lower_ascii(wide_to_utf8(package.extension().wstring())) != ".wxapkg";
          }), dependency_packages.end());
          sort_main_package_first(dependency_packages);
        } catch (const std::exception& error) {
          ++package_failures;
          logger_.error(dependency.display_name + "：扫描依赖包失败：" + error.what());
          continue;
        }
        if (dependency_packages.empty()) {
          ++package_failures;
          logger_.warning(dependency.display_name + "：本地依赖缓存中没有 .wxapkg 文件");
          continue;
        }

        const auto dependency_label = dependency.display_name.empty() ? dependency.wxid : dependency.display_name;
        const auto dependency_directory = make_unique_directory(
            plugin_root, safe_directory_name(dependency_label, dependency.wxid));
        dependency_directories.push_back(dependency_directory);
        logger_.info("正在附加依赖 " + dependency_label + "（" + dependency.wxid + "，" +
                     std::to_string(dependency_packages.size()) + " 个包）");
        std::set<std::string> merged_dependency_slots;
        for (const auto& package : dependency_packages) {
          if (cancel_requested.load()) break;
          try {
            const auto destination = output_directory_for_package(
                dependency_directory, package, merged_dependency_slots, historical_directories);
            if (destination != dependency_directory) {
              logger_.info("依赖中检测到同名历史包，单独保留到 " + wide_to_utf8(destination.wstring()));
            }
            const auto unpacked = Wxapkg::unpack_file(package, dependency.wxid, destination, logger_);
            files_extracted += unpacked.files_written;
            ++package_successes;
          } catch (const std::exception& error) {
            ++package_failures;
            logger_.error(dependency_label + " / " + filename_utf8(package) + "：" + error.what());
          }
        }
      }
    }

    if (!input.missing_dependency_wxids.empty()) {
      package_failures += input.missing_dependency_wxids.size();
      for (const auto& wxid : input.missing_dependency_wxids) {
        logger_.warning("当前小程序引用了 " + wxid + "，本机尚未保存该插件缓存");
      }
    }

    if (!input.missing_subpackages.empty()) {
      std::string missing;
      for (std::size_t index = 0; index < input.missing_subpackages.size(); ++index) {
        if (index != 0) missing += "、";
        missing += input.missing_subpackages[index];
      }
      logger_.warning("app-config.json 声明了分包 " + missing +
                      "，运行前重新扫描当前账号全部版本目录后仍未发现本地 wxapkg；其余已落盘包仍会全部处理");
    }
    for (const auto& subpackage : input.platform_conditional_subpackages) {
      logger_.info(subpackage +
                   " 是 Unity Loader 的 iOS 高性能条件代码包；Windows 微信不会请求并落盘，已按平台条件分包记录");
    }
    for (const auto& subpackage : input.runtime_or_external_subpackages) {
      logger_.info(subpackage +
                   " 当前配置为从 CDN 或运行资源缓存提供，不要求本地存在独立 wxapkg；已按运行资源分包记录");
    }

    RuntimeCacheCopyResult runtime_cache;
    if (!input.runtime_cache_path.empty() && !cancel_requested.load()) {
      try {
        logger_.info("正在附带当前小程序已落盘的运行资源缓存...");
        runtime_cache = copy_runtime_cache(input.runtime_cache_path,
                                           project_directory / L"_runtime_cache" / L"__GAME_FILE_CACHE",
                                           cancel_requested,
                                           logger_);
        logger_.success("运行资源缓存已复制 " + std::to_string(runtime_cache.files_copied) + " 个文件（" +
                        std::to_string(runtime_cache.bytes_copied) + " 字节）");
        additional_failures += runtime_cache.files_failed;
      } catch (const std::exception& error) {
        ++additional_failures;
        logger_.error("运行资源缓存复制失败：" + std::string(error.what()));
      }
    }

    try {
      write_unpack_manifest(project_directory,
                            input,
                            packages,
                            main_package_successes,
                            main_package_failures,
                            files_extracted,
                            runtime_cache);
    } catch (const std::exception& error) {
      logger_.warning("写入解包清单失败：" + std::string(error.what()));
    }

    logger_.info("本机可用主程序包处理完成：" + std::to_string(main_package_successes) + "/" +
                 std::to_string(packages.size()) + "，共释放 " + std::to_string(files_extracted) + " 个文件");

    if (main_package_successes == 0) {
      ++summary.failed;
      logger_.error(input.display_name + "：所有包处理失败");
      continue;
    }

    bool reconstruction_failed{};
    ReconstructionResult reconstruction;
    try {
      reconstruction = SourceReconstructor(logger_).reconstruct(project_directory);
      reconstruction_failed = !reconstruction.complete();
    } catch (const std::exception& error) {
      reconstruction_failed = true;
      reconstruction.warnings.push_back(error.what());
      logger_.error(input.display_name + "：主程序已解包，但源码重建失败：" + error.what());
    }
    for (const auto& dependency_directory : dependency_directories) {
      try {
        const auto recovered = SourceReconstructor(logger_).reconstruct(dependency_directory);
        reconstruction_failed = reconstruction_failed || !recovered.complete();
      } catch (const std::exception& error) {
        reconstruction_failed = true;
        logger_.error("通用插件源码重建失败：" + wide_to_utf8(dependency_directory.filename().wstring()) + "：" + error.what());
      }
    }
    for (const auto& historical_directory : historical_directories) {
      try {
        const auto recovered = SourceReconstructor(logger_).reconstruct(historical_directory);
        reconstruction_failed = reconstruction_failed || !recovered.complete();
      } catch (const std::exception& error) {
        reconstruction_failed = true;
        logger_.error("历史版本包源码重建失败：" + wide_to_utf8(historical_directory.wstring()) + "：" + error.what());
      }
    }

    try {
      write_unpack_manifest(project_directory, input, packages, main_package_successes,
                            main_package_failures, files_extracted, runtime_cache, &reconstruction);
    } catch (const std::exception& error) {
      ++additional_failures;
      logger_.warning("写入解包清单失败：" + std::string(error.what()));
    }

    if (package_failures == 0 && additional_failures == 0 && !reconstruction_failed &&
        input.missing_subpackages.empty() && input.missing_dependency_wxids.empty() && !cancel_requested.load()) {
      ++summary.succeeded;
      logger_.success(input.display_name + "：反编译完成");
    } else {
      ++summary.partial;
      logger_.warning(input.display_name + "：已完成主要内容，包缺失或处理失败 " +
                      std::to_string(package_failures + input.missing_subpackages.size() + input.missing_dependency_wxids.size()) +
                      " 个，资源复制失败 " + std::to_string(additional_failures) +
                      " 个；源码重建详情见 reconstruction-report.json");
    }
  }

  logger_.info("任务汇总：" + std::to_string(summary.succeeded) + " 个成功，" +
               std::to_string(summary.partial) + " 个部分完成，" + std::to_string(summary.failed) + " 个失败");
  logger_.info("最终输出：" + wide_to_utf8(summary.batch_output.wstring()));
  return summary;
}

}  // namespace wedecode



