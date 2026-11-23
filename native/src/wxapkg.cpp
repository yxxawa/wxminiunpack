#include "wedecode/wxapkg.h"

#include "wedecode/common.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace wedecode {
namespace {

using ByteVector = std::vector<std::byte>;

std::uint8_t byte_at(const ByteVector& bytes, const std::size_t index) {
  if (index >= bytes.size()) throw std::runtime_error("wxapkg data ends unexpectedly");
  return std::to_integer<std::uint8_t>(bytes[index]);
}

std::uint32_t read_u32_be(const ByteVector& bytes, const std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 4) throw std::runtime_error("wxapkg index is truncated");
  return (static_cast<std::uint32_t>(byte_at(bytes, offset)) << 24U) |
         (static_cast<std::uint32_t>(byte_at(bytes, offset + 1)) << 16U) |
         (static_cast<std::uint32_t>(byte_at(bytes, offset + 2)) << 8U) |
         static_cast<std::uint32_t>(byte_at(bytes, offset + 3));
}

void require_nt_success(const NTSTATUS status, const char* operation) {
  if (!BCRYPT_SUCCESS(status)) throw std::runtime_error(std::string(operation) + " failed (NTSTATUS " + std::to_string(status) + ")");
}

class AlgorithmHandle {
public:
  AlgorithmHandle() = default;
  ~AlgorithmHandle() { if (handle_) BCryptCloseAlgorithmProvider(handle_, 0); }
  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
  BCRYPT_ALG_HANDLE* put() { return &handle_; }
  BCRYPT_ALG_HANDLE get() const { return handle_; }
private:
  BCRYPT_ALG_HANDLE handle_{};
};

class KeyHandle {
public:
  ~KeyHandle() { if (handle_) BCryptDestroyKey(handle_); }
  BCRYPT_KEY_HANDLE* put() { return &handle_; }
  BCRYPT_KEY_HANDLE get() const { return handle_; }
private:
  BCRYPT_KEY_HANDLE handle_{};
};

ByteVector decrypt_first_block(std::string_view wxid, const ByteVector& encrypted) {
  if (encrypted.size() < 6 + 1024) throw std::runtime_error("Encrypted wxapkg is shorter than its AES segment");

  constexpr std::array<UCHAR, 8> salt{'s', 'a', 'l', 't', 'i', 'e', 's', 't'};
  constexpr std::array<UCHAR, 16> iv_template{'t', 'h', 'e', ' ', 'i', 'v', ':', ' ', '1', '6', ' ', 'b', 'y', 't', 'e', 's'};
  std::array<UCHAR, 32> derived{};

  AlgorithmHandle sha1;
  require_nt_success(BCryptOpenAlgorithmProvider(sha1.put(), BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG), "Opening SHA-1 provider");
  require_nt_success(
      BCryptDeriveKeyPBKDF2(sha1.get(),
                            reinterpret_cast<PUCHAR>(const_cast<char*>(wxid.data())),
                            static_cast<ULONG>(wxid.size()),
                            const_cast<PUCHAR>(salt.data()),
                            static_cast<ULONG>(salt.size()),
                            1000,
                            derived.data(),
                            static_cast<ULONG>(derived.size()),
                            0),
      "Deriving wxapkg AES key");

  AlgorithmHandle aes;
  require_nt_success(BCryptOpenAlgorithmProvider(aes.put(), BCRYPT_AES_ALGORITHM, nullptr, 0), "Opening AES provider");
  require_nt_success(BCryptSetProperty(aes.get(), BCRYPT_CHAINING_MODE,
                                       reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                                       static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)), 0),
                     "Configuring AES-CBC");

  ULONG key_object_length{};
  ULONG received{};
  require_nt_success(BCryptGetProperty(aes.get(), BCRYPT_OBJECT_LENGTH,
                                       reinterpret_cast<PUCHAR>(&key_object_length), sizeof(key_object_length), &received, 0),
                     "Reading AES key object length");
  ByteVector key_object(key_object_length);
  KeyHandle key;
  require_nt_success(BCryptGenerateSymmetricKey(aes.get(), key.put(),
                                                 reinterpret_cast<PUCHAR>(key_object.data()), key_object_length,
                                                 derived.data(), static_cast<ULONG>(derived.size()), 0),
                     "Creating AES key");

  ByteVector plain(1024);
  auto iv = iv_template;
  ULONG plain_size{};
  require_nt_success(BCryptDecrypt(key.get(),
                                   reinterpret_cast<PUCHAR>(const_cast<std::byte*>(encrypted.data() + 6)),
                                   1024,
                                   nullptr,
                                   iv.data(), static_cast<ULONG>(iv.size()),
                                   reinterpret_cast<PUCHAR>(plain.data()), static_cast<ULONG>(plain.size()),
                                   &plain_size,
                                   0),
                     "Decrypting wxapkg AES segment");
  if (plain_size < 1023) throw std::runtime_error("Decrypted wxapkg AES segment is truncated");
  plain.resize(plain_size);
  return plain;
}

}  // namespace

bool Wxapkg::has_valid_header(const std::vector<std::byte>& bytes) {
  return bytes.size() >= 18 && byte_at(bytes, 0) == 0xbe && byte_at(bytes, 13) == 0xed;
}

std::vector<std::byte> Wxapkg::decrypt(std::string_view wxid, const std::vector<std::byte>& encrypted) {
  constexpr std::string_view signature = "V1MMWX";
  if (encrypted.size() < signature.size() ||
      std::memcmp(encrypted.data(), signature.data(), signature.size()) != 0) {
    throw std::runtime_error("Unsupported wxapkg format: expected BE/ED or V1MMWX header");
  }
  if (wxid.empty()) throw std::runtime_error("The input folder name is required as WXID for an encrypted wxapkg");
  const auto decrypted_head = decrypt_first_block(wxid, encrypted);
  const auto xor_key = wxid.size() >= 2 ? static_cast<std::uint8_t>(wxid[wxid.size() - 2]) : 0x66;

  ByteVector result;
  result.reserve(1023 + encrypted.size() - (6 + 1024));
  result.insert(result.end(), decrypted_head.begin(), decrypted_head.begin() + 1023);
  for (std::size_t index = 6 + 1024; index < encrypted.size(); ++index) {
    result.push_back(static_cast<std::byte>(byte_at(encrypted, index) ^ xor_key));
  }
  if (!has_valid_header(result)) throw std::runtime_error("wxapkg decryption failed: check the package WXID");
  return result;
}

std::vector<PackFileEntry> Wxapkg::read_file_index(const std::vector<std::byte>& package) {
  if (!has_valid_header(package)) throw std::runtime_error("The wxapkg header markers are invalid");

  // The index length includes the four-byte file count after the 14-byte header.
  const auto index_end = static_cast<std::size_t>(read_u32_be(package, 5)) + 14U;
  const auto file_count = read_u32_be(package, 14);
  if (index_end > package.size() || index_end < 18) throw std::runtime_error("wxapkg file index extends beyond the package");
  if (file_count > (index_end - 18) / 13) throw std::runtime_error("wxapkg file count cannot fit in its index");
  const auto body_length = read_u32_be(package, 9);
  if (body_length != package.size() - index_end) throw std::runtime_error("wxapkg body length does not match the package size");

  std::size_t cursor = 18;
  std::vector<PackFileEntry> entries;
  std::map<std::string, PackFileEntry> paths;
  entries.reserve(file_count);
  for (std::uint32_t index = 0; index < file_count; ++index) {
    if (index_end - cursor < 12) throw std::runtime_error("wxapkg file index is truncated");
    const auto name_length = read_u32_be(package, cursor);
    cursor += 4;
    if (name_length == 0 || name_length > index_end - cursor - 8) throw std::runtime_error("wxapkg file name extends beyond the index");
    std::string packed_path;
    packed_path.reserve(name_length);
    for (std::size_t byte = 0; byte < name_length; ++byte) packed_path.push_back(static_cast<char>(byte_at(package, cursor + byte)));
    cursor += name_length;
    const auto offset = read_u32_be(package, cursor);
    cursor += 4;
    const auto size = read_u32_be(package, cursor);
    cursor += 4;
    if (cursor > index_end) throw std::runtime_error("wxapkg file index is malformed");
    if (static_cast<std::uint64_t>(offset) + size > package.size()) throw std::runtime_error("wxapkg file entry extends beyond the package");
    if (offset < index_end) throw std::runtime_error("wxapkg file entry overlaps the header or index");
    if (!is_safe_relative_path(packed_path)) throw std::runtime_error("wxapkg contains an unsafe file path: " + packed_path);
    auto relative = path_from_utf8(normalise_relative_path(packed_path)).lexically_normal();
    const auto path = wide_to_utf8(relative.generic_wstring());
    const auto [previous, inserted] = paths.emplace(to_lower_ascii(path), PackFileEntry{path, offset, size});
    if (!inserted && (previous->second.size != size ||
        !std::equal(package.begin() + previous->second.offset,
                    package.begin() + previous->second.offset + size, package.begin() + offset))) {
      throw std::runtime_error("wxapkg contains conflicting output paths: " + path);
    }
    entries.push_back({path, offset, size});
  }
  if (cursor != index_end) throw std::runtime_error("wxapkg file index length does not match its entries");
  return entries;
}

UnpackResult Wxapkg::unpack_file(const fs::path& package_path,
                                 std::string_view wxid,
                                 const fs::path& output_directory,
                                 const Logger& logger) {
  auto package = read_binary(package_path);
  const bool encrypted = !has_valid_header(package);
  if (encrypted) {
    logger.info("Encrypted package detected; decrypting with WXID " + std::string(wxid));
    package = decrypt(wxid, package);
  }
  const auto entries = read_file_index(package);
  for (const auto& entry : entries) {
    const auto destination = output_directory / path_from_utf8(entry.packed_path);
    ByteVector content(entry.size);
    if (entry.size != 0) std::memcpy(content.data(), package.data() + entry.offset, entry.size);
    write_binary(destination, content, true);
  }
  logger.success("Unpacked " + std::to_string(entries.size()) + " files from " + wide_to_utf8(package_path.filename().wstring()));
  return {entries.size(), encrypted};
}

}  // namespace wedecode


