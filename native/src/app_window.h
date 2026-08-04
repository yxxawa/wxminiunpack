#pragma once

#include "wedecode/batch_runner.h"
#include "wedecode/wechat_cache_scanner.h"

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wedecode {

class AppWindow {
public:
  explicit AppWindow(HINSTANCE instance);
  ~AppWindow();

  AppWindow(const AppWindow&) = delete;
  AppWindow& operator=(const AppWindow&) = delete;

  int run(int show_command);
  void add_dropped_folders(const std::vector<std::filesystem::path>& folders);
  void set_drop_hover(bool active);

private:
  static constexpr UINT message_log = WM_APP + 101;
  static constexpr UINT message_state = WM_APP + 102;
  static constexpr UINT message_scan_complete = WM_APP + 103;
  static constexpr UINT message_scan_progress = WM_APP + 104;
  static constexpr UINT message_names_complete = WM_APP + 105;
  static constexpr UINT message_name_progress = WM_APP + 106;

  HINSTANCE instance_{};
  HWND window_{};
  HWND drop_overlay_{};
  RECT drop_bounds_{};
  bool drop_bounds_valid_{false};
  bool main_window_shown_{false};
  bool dark_theme_{false};
  int initial_show_command_{SW_SHOWNORMAL};
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller3> controller3_;
  Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
  std::vector<InputFolder> manual_inputs_;
  std::vector<InputFolder> scanned_inputs_;
  std::thread worker_;
  std::thread scan_worker_;
  std::thread name_worker_;
  std::atomic_bool running_{false};
  std::atomic_bool cancel_requested_{false};
  std::atomic_bool scanning_{false};
  std::atomic_bool scan_cancel_requested_{false};
  std::atomic_bool name_cancel_requested_{false};
  std::atomic_bool name_enriching_{false};
  std::atomic_bool scan_progress_posted_{false};
  std::atomic_bool name_progress_posted_{false};
  bool scan_started_{false};
  bool name_refresh_pending_{false};
  CacheScanResult scan_result_;
  std::map<std::string, std::string> name_result_;
  std::set<std::string> name_request_wxids_;
  std::mutex scan_progress_mutex_;
  std::vector<InputFolder> pending_scan_inputs_;
  std::mutex name_progress_mutex_;
  std::map<std::string, std::string> pending_name_updates_;

  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK drop_overlay_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
  void create_window(int show_command);
  void apply_title_bar_theme() const;
  void apply_webview_background() const;
  void set_theme(bool dark, bool persist);
  void send_theme_state();
  void show_main_window();
  void report_webview_startup_failure(const wchar_t* stage, HRESULT result);
  void initialise_webview();
  void resize_webview();
  void create_drop_overlay();
  void destroy_drop_overlay();
  void update_drop_overlay_bounds(int x, int y, int width, int height);
  void select_folders();
  void remove_manual_input(std::size_t index);
  void begin_manual_batch();
  void begin_scan_batch(std::vector<std::size_t> indices);
  void open_scanned_input_directory(std::size_t index);
  void begin_batch(std::vector<InputFolder> inputs);
  void begin_cache_scan(bool force = false);
  void queue_scan_progress(CacheScanUpdate update);
  void merge_scan_progress();
  void merge_cache_scan(CacheScanResult result);
  void begin_name_enrichment();
  void queue_name_progress(std::string wxid, std::string name);
  void merge_name_progress();
  void merge_resolved_names(std::map<std::string, std::string> names);
  void append_log(std::string message);
  void send_json(std::string json);
  void send_manual_inputs();
  void send_scan_results();
  void send_running_state(bool running);
  void send_scan_state(bool scanning);
  void join_worker();
  void join_scan_worker();
  void join_name_worker();
};

}  // namespace wedecode
