#pragma once

#include "wedecode/common.h"

#include <filesystem>
#include <string>
#include <vector>

namespace wedecode {

struct ReconstructionResult {
  std::size_t javascript_modules{};
  std::size_t json_files{};
  std::size_t stylesheets{};
  std::vector<std::string> missing_source_files;
  std::vector<std::string> warnings;
  bool complete() const { return missing_source_files.empty() && warnings.empty(); }
};

class SourceReconstructor {
public:
  explicit SourceReconstructor(Logger logger);

  // Restores files that can be recovered from the unpacked package. Raw assets
  // remain in place whenever a compiled runtime has no lossless source form.
  ReconstructionResult reconstruct(const std::filesystem::path& project_directory);

private:
  Logger logger_;
  ReconstructionResult result_;

  void reconstruct_app_config(const std::filesystem::path& project_directory);
  void reconstruct_define_modules(const std::filesystem::path& project_directory);
  void inspect_missing_page_files(const std::filesystem::path& project_directory);
  void create_project_config(const std::filesystem::path& project_directory);
};

}  // namespace wedecode
