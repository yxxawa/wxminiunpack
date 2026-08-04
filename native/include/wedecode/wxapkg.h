#pragma once

#include "wedecode/common.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wedecode {

struct PackFileEntry {
  std::string packed_path;
  std::uint32_t offset{};
  std::uint32_t size{};
};

struct UnpackResult {
  std::size_t files_written{};
  bool was_encrypted{};
};

class Wxapkg {
public:
  static bool has_valid_header(const std::vector<std::byte>& bytes);
  static std::vector<std::byte> decrypt(std::string_view wxid, const std::vector<std::byte>& encrypted);
  static std::vector<PackFileEntry> read_file_index(const std::vector<std::byte>& package);
  static UnpackResult unpack_file(const std::filesystem::path& package_path,
                                  std::string_view wxid,
                                  const std::filesystem::path& output_directory,
                                  const Logger& logger);
};

}  // namespace wedecode


