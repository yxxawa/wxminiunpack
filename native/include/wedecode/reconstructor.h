#pragma once

#include "wedecode/common.h"

#include <filesystem>
#include <string>
#include <vector>

namespace wedecode {

class SourceReconstructor {
public:
  explicit SourceReconstructor(Logger logger);

  // Restores files that can be recovered from the unpacked package. Raw assets
  // remain in place whenever a compiled runtime has no lossless source form.
  void reconstruct(const std::filesystem::path& project_directory);

private:
  Logger logger_;

  void reconstruct_app_config(const std::filesystem::path& project_directory);
  void reconstruct_define_modules(const std::filesystem::path& project_directory);
  void create_missing_page_files(const std::filesystem::path& project_directory);
  void create_project_config(const std::filesystem::path& project_directory);
  void normalise_game_config(const std::filesystem::path& project_directory);
};

}  // namespace wedecode
