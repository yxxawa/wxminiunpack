#pragma once

#include "wedecode/batch_runner.h"
#include "wedecode/common.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wedecode {

struct CacheScanResult {
  std::vector<InputFolder> inputs;
  std::size_t account_count{};
  std::size_t package_count{};
  std::size_t cache_root_count{};
  std::size_t plugin_cache_count{};
  std::size_t dependency_package_count{};
  std::size_t name_cache_hits{};
  std::size_t inspection_cache_hits{};
  std::size_t inspection_cache_misses{};
  bool runtime_name_scan_skipped{};
  std::string error;
};

struct CacheScanUpdate {
  std::vector<InputFolder> inputs;
};

using CacheScanUpdateSink = std::function<void(CacheScanUpdate)>;
using RuntimeNameUpdateSink = std::function<void(std::string wxid, std::string name)>;

// Extract names only when an application identity and its display-name field
// belong to the same runtime record. This is also used by regression fixtures
// so renderer-memory parsing stays independent from process enumeration.
std::map<std::string, std::string> extract_runtime_app_names(
    std::string_view text,
    const std::set<std::string>& relevant_wxids);

// Extract the exact application name embedded in a Tencent application detail
// page. Kept public for parser regression tests; network access remains in the
// background scanner.
std::optional<std::string> extract_marketplace_app_name(std::string_view html);

class WechatCacheScanner {
public:
  explicit WechatCacheScanner(Logger logger = Logger{},
                              std::filesystem::path cache_directory = {});

  CacheScanResult scan(const std::atomic_bool* cancel_requested = nullptr,
                       CacheScanUpdateSink update_sink = {}) const;
  CacheScanResult scan_applet_roots(const std::vector<std::filesystem::path>& applet_roots,
                                    const std::atomic_bool* cancel_requested = nullptr,
                                    CacheScanUpdateSink update_sink = {}) const;
  std::map<std::string, std::string> resolve_runtime_names(
      const std::vector<InputFolder>& inputs,
      const std::atomic_bool* cancel_requested = nullptr,
      RuntimeNameUpdateSink update_sink = {}) const;

private:
  Logger logger_;
  std::filesystem::path cache_directory_;
};

}  // namespace wedecode

