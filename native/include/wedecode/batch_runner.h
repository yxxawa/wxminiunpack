#pragma once

#include "wedecode/common.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace wedecode {

struct PackageDependency {
  std::filesystem::path path;
  std::string wxid;
  std::string display_name;
  std::string version;
  std::size_t package_count{};
  std::vector<std::filesystem::path> package_files;
};

struct InputFolder {
  std::filesystem::path path;
  std::string wxid;
  std::string display_name;
  std::string app_name;
  std::string kind{"手动目录"};
  std::string version;
  std::string account_key;
  std::string account_name;
  std::filesystem::path icon_path;
  std::string icon_data_uri;
  std::filesystem::path runtime_cache_path;
  std::size_t package_count{};
  std::size_t account_count{1};
  int app_name_score{};
  bool scan_ready{true};
  bool auto_discovered{};
  std::vector<std::filesystem::path> package_files;
  std::vector<std::string> declared_subpackages;
  std::vector<std::string> missing_subpackages;
  std::vector<std::string> platform_conditional_subpackages;
  std::vector<std::string> runtime_or_external_subpackages;
  std::vector<PackageDependency> dependencies;
  std::vector<std::string> missing_dependency_wxids;
};

struct BatchSummary {
  std::filesystem::path batch_output;
  std::size_t succeeded{};
  std::size_t partial{};
  std::size_t failed{};
};

class BatchRunner {
public:
  explicit BatchRunner(Logger logger);

  std::vector<InputFolder> prepare_inputs(const std::vector<std::filesystem::path>& dropped_paths) const;
  BatchSummary run(const std::vector<InputFolder>& inputs, std::atomic_bool& cancel_requested) const;

private:
  Logger logger_;

  static std::vector<std::filesystem::path> find_packages(const std::filesystem::path& input_directory);
  static void sort_main_package_first(std::vector<std::filesystem::path>& packages);
};

}  // namespace wedecode
