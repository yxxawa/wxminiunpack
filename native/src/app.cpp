#include "app_window.h"

#include "wedecode/common.h"
#include "wedecode/drop_target.h"
#include "resource.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl.h>
#include <wrl/event.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace fs = std::filesystem;

namespace wedecode {
namespace {

std::string load_resource_utf8(const HINSTANCE instance, const int resource_id) {
  const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
  if (!resource) throw std::runtime_error("Could not load embedded UI resource");
  const auto loaded = LoadResource(instance, resource);
  if (!loaded) throw std::runtime_error("Could not load embedded UI bytes");
  const auto size = SizeofResource(instance, resource);
  const auto data = static_cast<const char*>(LockResource(loaded));
  if (!data || size == 0) throw std::runtime_error("Embedded UI resource is empty");
  return {data, data + size};
}

fs::path theme_settings_path() {
  return application_data_directory() / L"theme.txt";
}

bool load_dark_theme() {
  std::ifstream input(theme_settings_path(), std::ios::binary);
  std::string theme;
  input >> theme;
  return theme == "dark";
}

void save_theme(const bool dark) {
  const auto path = theme_settings_path();
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (output) output << (dark ? "dark" : "light");
}

std::string state_message(const bool running) {
  return std::string("{\"type\":\"state\",\"running\":") + (running ? "true" : "false") + "}";
}

std::optional<int> json_integer(const std::string_view json, const std::string_view key) {
  const std::string field = "\"" + std::string(key) + "\"";
  auto position = json.find(field);
  if (position == std::string_view::npos) return std::nullopt;
  position = json.find(':', position + field.size());
  if (position == std::string_view::npos) return std::nullopt;
  ++position;
  while (position < json.size() && (json[position] == ' ' || json[position] == '\t')) ++position;
  const char* begin = json.data() + position;
  const char* end = json.data() + json.size();
  int value{};
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc()) return std::nullopt;
  return value;
}

std::optional<std::string> json_string(const std::string_view json, const std::string_view key) {
  const std::string field = "\"" + std::string(key) + "\"";
  auto position = json.find(field);
  if (position == std::string_view::npos) return std::nullopt;
  position = json.find(':', position + field.size());
  if (position == std::string_view::npos) return std::nullopt;
  position = json.find('"', position + 1U);
  if (position == std::string_view::npos) return std::nullopt;
  ++position;

  std::string value;
  value.reserve(json.size() - position);
  bool escaped = false;
  for (; position < json.size(); ++position) {
    const char character = json[position];
    if (escaped) {
      switch (character) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(character); break;
      }
      escaped = false;
      continue;
    }
    if (character == '\\') {
      escaped = true;
      continue;
    }
    if (character == '"') return value;
    value.push_back(character);
  }
  return std::nullopt;
}

std::vector<std::size_t> json_index_array(const std::string_view json, const std::string_view key) {
  std::vector<std::size_t> indices;
  const std::string field = "\"" + std::string(key) + "\"";
  auto position = json.find(field);
  if (position == std::string_view::npos) return indices;
  position = json.find(':', position + field.size());
  if (position == std::string_view::npos) return indices;
  position = json.find('[', position + 1);
  if (position == std::string_view::npos) return indices;
  ++position;

  while (position < json.size()) {
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n' || json[position] == ',')) {
      ++position;
    }
    if (position >= json.size() || json[position] == ']') break;

    int value{};
    const char* begin = json.data() + position;
    const char* end = json.data() + json.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr == begin) break;
    if (value >= 0) indices.push_back(static_cast<std::size_t>(value));
    position = static_cast<std::size_t>(parsed.ptr - json.data());
  }
  return indices;
}

std::string input_list_message(const std::string_view type, const std::vector<InputFolder>& inputs) {
  std::size_t estimated_size = 64U;
  for (const auto& input : inputs) {
    estimated_size += 768U + input.icon_data_uri.size();
    estimated_size += input.dependencies.size() * 192U;
    estimated_size += (input.missing_dependency_wxids.size() + input.missing_subpackages.size() +
                       input.platform_conditional_subpackages.size() +
                       input.runtime_or_external_subpackages.size()) * 48U;
  }
  std::string json;
  json.reserve(estimated_size);
  json = "{\"type\":\"" + std::string(type) + "\",\"folders\":[";
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    if (index) json += ',';
    const auto& input = inputs[index];
    std::size_t total_packages = input.package_count;
    for (const auto& dependency : input.dependencies) total_packages += dependency.package_count;
    json += "{\"name\":\"" + json_escape(input.display_name) +
            "\",\"wxid\":\"" + json_escape(input.wxid) +
            "\",\"kind\":\"" + json_escape(input.kind) +
            "\",\"version\":\"" + json_escape(input.version) +
            "\",\"accountKey\":\"" + json_escape(input.account_key) +
            "\",\"accountName\":\"" + json_escape(input.account_name) +
            "\",\"packageCount\":" + std::to_string(input.package_count) +
            ",\"totalPackageCount\":" + std::to_string(total_packages) +
            ",\"accountCount\":" + std::to_string(input.account_count) +
            ",\"ready\":" + (input.scan_ready ? "true" : "false") +
            ",\"automatic\":" + (input.auto_discovered ? "true" : "false") +
            ",\"icon\":\"" + json_escape(input.icon_data_uri) + "\",\"dependencies\":[";
    for (std::size_t dependency_index = 0; dependency_index < input.dependencies.size(); ++dependency_index) {
      if (dependency_index) json += ',';
      const auto& dependency = input.dependencies[dependency_index];
      json += "{\"name\":\"" + json_escape(dependency.display_name) +
              "\",\"wxid\":\"" + json_escape(dependency.wxid) +
              "\",\"version\":\"" + json_escape(dependency.version) +
              "\",\"packageCount\":" + std::to_string(dependency.package_count) + "}";
    }
    json += "],\"missingDependencies\":[";
    for (std::size_t missing_index = 0; missing_index < input.missing_dependency_wxids.size(); ++missing_index) {
      if (missing_index) json += ',';
      json += "\"" + json_escape(input.missing_dependency_wxids[missing_index]) + "\"";
    }
    json += "],\"missingSubpackages\":[";
    for (std::size_t missing_index = 0; missing_index < input.missing_subpackages.size(); ++missing_index) {
      if (missing_index) json += ',';
      json += "\"" + json_escape(input.missing_subpackages[missing_index]) + "\"";
    }
    json += "],\"platformConditionalSubpackages\":[";
    for (std::size_t subpackage_index = 0; subpackage_index < input.platform_conditional_subpackages.size();
         ++subpackage_index) {
      if (subpackage_index) json += ',';
      json += "\"" + json_escape(input.platform_conditional_subpackages[subpackage_index]) + "\"";
    }
    json += "],\"runtimeOrExternalSubpackages\":[";
    for (std::size_t subpackage_index = 0; subpackage_index < input.runtime_or_external_subpackages.size();
         ++subpackage_index) {
      if (subpackage_index) json += ',';
      json += "\"" + json_escape(input.runtime_or_external_subpackages[subpackage_index]) + "\"";
    }
    json += "],\"hasRuntimeCache\":" + std::string(input.runtime_cache_path.empty() ? "false" : "true") + "}";
  }
  json += "]}";
  return json;
}

std::string scan_name_updates_message(const std::map<std::string, std::string>& names) {
  std::string json = "{\"type\":\"scan-name-updates\",\"items\":[";
  bool first = true;
  for (const auto& [wxid, name] : names) {
    if (!first) json += ',';
    first = false;
    json += "{\"wxid\":\"" + json_escape(wxid) + "\",\"name\":\"" + json_escape(name) + "\"}";
  }
  json += "]}";
  return json;
}

}  // namespace

AppWindow::AppWindow(HINSTANCE instance) : instance_(instance), dark_theme_(load_dark_theme()) {}

AppWindow::~AppWindow() {
  cancel_requested_.store(true);
  scan_cancel_requested_.store(true);
  name_cancel_requested_.store(true);
  join_worker();
  join_scan_worker();
  join_name_worker();
  destroy_drop_overlay();
}

int AppWindow::run(const int show_command) {
  const HRESULT com_result = OleInitialize(nullptr);
  if (FAILED(com_result)) {
    MessageBoxW(nullptr, L"COM initialization failed.", L"微信小程序一键反编译", MB_ICONERROR);
    return 1;
  }

  try {
    create_window(show_command);
    initialise_webview();
  } catch (const std::exception& error) {
    MessageBoxW(window_, utf8_to_wide(error.what()).c_str(), L"微信小程序一键反编译", MB_ICONERROR);
    if (window_) DestroyWindow(window_);
    OleUninitialize();
    return 1;
  }

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  webview_.Reset();
  controller_.Reset();
  OleUninitialize();
  return static_cast<int>(message.wParam);
}

void AppWindow::create_window(const int show_command) {
  initial_show_command_ = show_command;
  constexpr wchar_t class_name[] = L"WedecodeNativeWindow";
  const UINT dpi = GetDpiForSystem();
  const auto app_icon = static_cast<HICON>(LoadImageW(
      instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
      GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi),
      LR_DEFAULTCOLOR | LR_SHARED));
  const auto small_icon = static_cast<HICON>(LoadImageW(
      instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
      GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi),
      LR_DEFAULTCOLOR | LR_SHARED));

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance_;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = app_icon ? app_icon : LoadIconW(nullptr, IDI_APPLICATION);
  window_class.hIconSm = small_icon ? small_icon : window_class.hIcon;
  window_class.hbrBackground = dark_theme_
      ? static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH))
      : GetSysColorBrush(COLOR_WINDOW);
  window_class.lpszClassName = class_name;
  window_class.lpfnWndProc = AppWindow::window_proc;
  RegisterClassExW(&window_class);

  const int desired_width = MulDiv(940, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  const int desired_height = MulDiv(700, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  RECT work_area{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  const int work_width = work_area.right - work_area.left;
  const int work_height = work_area.bottom - work_area.top;
  const int window_width = std::min(desired_width, work_width * 94 / 100);
  const int window_height = std::min(desired_height, work_height * 94 / 100);
  const int window_x = work_area.left + (work_width - window_width) / 2;
  const int window_y = work_area.top + (work_height - window_height) / 2;

  window_ = CreateWindowExW(0, class_name, L"微信小程序一键反编译", WS_OVERLAPPEDWINDOW,
                            window_x, window_y, window_width, window_height,
                            nullptr, nullptr, instance_, this);
  if (!window_) throw std::runtime_error("Could not create the application window");

  apply_title_bar_theme();
  // Keep the top-level window hidden while the WebView2 environment and the
  // embedded UI are loading. It is shown only after JavaScript reports that
  // the complete interface is ready, avoiding an empty black startup window.
}

void AppWindow::apply_title_bar_theme() const {
  if (!window_) return;

  const BOOL enabled = dark_theme_ ? TRUE : FALSE;
  constexpr auto immersive_dark_mode = static_cast<DWMWINDOWATTRIBUTE>(20);
  if (FAILED(DwmSetWindowAttribute(window_, immersive_dark_mode, &enabled, sizeof(enabled)))) {
    constexpr auto immersive_dark_mode_legacy = static_cast<DWMWINDOWATTRIBUTE>(19);
    DwmSetWindowAttribute(window_, immersive_dark_mode_legacy, &enabled, sizeof(enabled));
  }

  const COLORREF caption_color = dark_theme_ ? RGB(41, 46, 52) : RGB(238, 241, 244);
  const COLORREF text_color = dark_theme_ ? RGB(237, 240, 243) : RGB(36, 42, 49);
  constexpr auto caption_color_attribute = static_cast<DWMWINDOWATTRIBUTE>(35);
  constexpr auto text_color_attribute = static_cast<DWMWINDOWATTRIBUTE>(36);
  DwmSetWindowAttribute(window_, caption_color_attribute, &caption_color, sizeof(caption_color));
  DwmSetWindowAttribute(window_, text_color_attribute, &text_color, sizeof(text_color));
}

void AppWindow::apply_webview_background() const {
  if (!controller_) return;
  ComPtr<ICoreWebView2Controller2> controller2;
  if (FAILED(controller_.As(&controller2)) || !controller2) return;
  const COREWEBVIEW2_COLOR background = dark_theme_
      ? COREWEBVIEW2_COLOR{255, 36, 40, 45}
      : COREWEBVIEW2_COLOR{255, 232, 235, 239};
  controller2->put_DefaultBackgroundColor(background);
}

void AppWindow::set_theme(const bool dark, const bool persist) {
  dark_theme_ = dark;
  if (persist) save_theme(dark_theme_);
  apply_title_bar_theme();
  apply_webview_background();
  if (window_) {
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
  }
}

void AppWindow::send_theme_state() {
  send_json(std::string("{\"type\":\"theme\",\"theme\":\"") +
            (dark_theme_ ? "dark" : "light") + "\"}");
}

void AppWindow::show_main_window() {
  if (!window_ || main_window_shown_) return;
  main_window_shown_ = true;
  resize_webview();
  if (controller_) controller_->put_IsVisible(TRUE);
  ShowWindow(window_, initial_show_command_);
  UpdateWindow(window_);
}

void AppWindow::report_webview_startup_failure(const wchar_t* stage, const HRESULT result) {
  wchar_t message[384]{};
  swprintf_s(message, L"%s初始化失败。\n\n请检查 Microsoft Edge WebView2 Runtime。\nHRESULT: 0x%08X",
             stage, static_cast<unsigned int>(result));
  MessageBoxW(nullptr, message, L"微信小程序一键反编译", MB_OK | MB_ICONERROR);
  if (window_) DestroyWindow(window_);
}

void AppWindow::initialise_webview() {
  const auto user_data_directory = application_data_directory() / L"WebView2";
  fs::create_directories(user_data_directory);
  const auto user_data = user_data_directory.wstring();
  const auto result = CreateCoreWebView2EnvironmentWithOptions(
      nullptr,
      user_data.c_str(),
      nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) {
              report_webview_startup_failure(L"WebView2 运行环境", result);
              return S_OK;
            }
            return environment->CreateCoreWebView2Controller(
                window_,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(controller_result) || !controller) {
                        report_webview_startup_failure(L"WebView2 控制器", controller_result);
                        return S_OK;
                      }
                      controller_ = controller;
                      HRESULT result = controller_->get_CoreWebView2(&webview_);
                      if (FAILED(result)) return result;

                      apply_webview_background();

                      if (SUCCEEDED(controller_.As(&controller3_)) && controller3_) {
                        controller3_->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RASTERIZATION_SCALE);
                      }

                      ComPtr<ICoreWebView2Controller4> controller4;
                      if (SUCCEEDED(controller_.As(&controller4)) && controller4) {
                        controller4->put_AllowExternalDrop(FALSE);
                      }

                      ComPtr<ICoreWebView2Settings> settings;
                      if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
                        settings->put_IsStatusBarEnabled(FALSE);
                        settings->put_AreDevToolsEnabled(FALSE);
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_IsZoomControlEnabled(FALSE);
                      }

                      EventRegistrationToken message_token{};
                      webview_->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [this](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                                LPWSTR received{};
                                if (SUCCEEDED(args->TryGetWebMessageAsString(&received)) && received) {
                                  const std::string message = wide_to_utf8(received);
                                  CoTaskMemFree(received);
                                  if (message.find("\"type\":\"ready\"") != std::string::npos) {
                                    send_theme_state();
                                    send_manual_inputs();
                                    send_scan_results();
                                    send_running_state(running_.load());
                                    show_main_window();
                                    begin_cache_scan();
                                  }
                                  if (message.find("\"type\":\"start-manual\"") != std::string::npos)
                                    begin_manual_batch();
                                  if (message.find("\"type\":\"start-scan\"") != std::string::npos) {
                                    auto indices = json_index_array(message, "indices");
                                    if (indices.empty()) {
                                      const auto index = json_integer(message, "index");
                                      if (index && *index >= 0)
                                        indices.push_back(static_cast<std::size_t>(*index));
                                    }
                                    begin_scan_batch(std::move(indices));
                                  }
                                  if (message.find("\"type\":\"open-scan-folder\"") != std::string::npos) {
                                    const auto index = json_integer(message, "index");
                                    if (index && *index >= 0)
                                      open_scanned_input_directory(static_cast<std::size_t>(*index));
                                  }
                                  if (message.find("\"type\":\"rescan-cache\"") != std::string::npos)
                                    begin_cache_scan(true);
                                  if (message.find("\"type\":\"select-folders\"") != std::string::npos)
                                    select_folders();
                                  if (message.find("\"type\":\"remove-manual-folder\"") != std::string::npos) {
                                    const auto index = json_integer(message, "index");
                                    if (index && *index >= 0)
                                      remove_manual_input(static_cast<std::size_t>(*index));
                                  }
                                  if (message.find("\"type\":\"set-theme\"") != std::string::npos) {
                                    if (message.find("\"theme\":\"dark\"") != std::string::npos) {
                                      set_theme(true, true);
                                    } else if (message.find("\"theme\":\"light\"") != std::string::npos) {
                                      set_theme(false, true);
                                    }
                                  }
                                  if (message.find("\"type\":\"drop-bounds\"") != std::string::npos) {
                                    const auto x = json_integer(message, "x");
                                    const auto y = json_integer(message, "y");
                                    const auto width = json_integer(message, "width");
                                    const auto height = json_integer(message, "height");
                                    if (x && y && width && height)
                                      update_drop_overlay_bounds(*x, *y, *width, *height);
                                  }
                                }
                                return S_OK;
                              })
                              .Get(),
                          &message_token);

                      resize_webview();
                      create_drop_overlay();
                      auto html = load_resource_utf8(instance_, IDR_APP_HTML);
                      if (dark_theme_) {
                        constexpr std::string_view light_root = "<html lang=\"zh-CN\" data-theme=\"light\">";
                        constexpr std::string_view dark_root = "<html lang=\"zh-CN\" data-theme=\"dark\">";
                        if (const auto position = html.find(light_root); position != std::string::npos) {
                          html.replace(position, light_root.size(), dark_root);
                        }
                      }
                      webview_->NavigateToString(utf8_to_wide(html).c_str());
                      return S_OK;
                    }).Get());
          }).Get());
  if (FAILED(result)) throw std::runtime_error("The WebView2 Runtime is unavailable. Install Microsoft Edge WebView2 Evergreen Runtime.");
}

void AppWindow::resize_webview() {
  if (controller_) {
    RECT bounds{};
    GetClientRect(window_, &bounds);
    if (controller3_) {
      double rasterization_scale = 1.0;
      if (SUCCEEDED(controller3_->get_RasterizationScale(&rasterization_scale)) && rasterization_scale > 0.0) {
        bounds.right = static_cast<LONG>(std::lround(static_cast<double>(bounds.right) / rasterization_scale));
        bounds.bottom = static_cast<LONG>(std::lround(static_cast<double>(bounds.bottom) / rasterization_scale));
      }
    }
    controller_->put_Bounds(bounds);
  }
  if (drop_bounds_valid_) {
    update_drop_overlay_bounds(
        drop_bounds_.left,
        drop_bounds_.top,
        drop_bounds_.right - drop_bounds_.left,
        drop_bounds_.bottom - drop_bounds_.top);
  }
}

void AppWindow::create_drop_overlay() {
  if (drop_overlay_ || !window_) return;

  constexpr wchar_t overlay_class_name[] = L"WedecodeDropOverlay";
  WNDCLASSEXW overlay_class{};
  overlay_class.cbSize = sizeof(overlay_class);
  overlay_class.hInstance = instance_;
  overlay_class.hCursor = LoadCursorW(nullptr, IDC_HAND);
  overlay_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  overlay_class.lpszClassName = overlay_class_name;
  overlay_class.lpfnWndProc = AppWindow::drop_overlay_proc;
  if (!RegisterClassExW(&overlay_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    append_log("[ERROR] Could not create the native folder drop surface.");
    return;
  }

  // WebView2 owns child HWNDs that cover the full client area. A normal child
  // drop target therefore remains behind Chromium. Use a transparent owned
  // popup so OLE drag/drop and mouse clicks are routed to our UI thread.
  drop_overlay_ = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      overlay_class_name,
      L"",
      WS_POPUP,
      0, 0, 1, 1,
      window_,
      nullptr,
      instance_,
      this);
  if (!drop_overlay_) {
    const auto error = GetLastError();
    append_log("[ERROR] Could not create the native folder drop surface. Win32 error: " + std::to_string(error));
    return;
  }

  // Alpha 1 is visually transparent while still participating in hit testing.
  SetLayeredWindowAttributes(drop_overlay_, 0, 1, LWA_ALPHA);
  auto* drop_target = new FolderDropTarget(*this);
  const auto drop_result = RegisterDragDrop(drop_overlay_, drop_target);
  drop_target->Release();
  if (FAILED(drop_result)) {
    append_log("[ERROR] Could not enable folder drag and drop. HRESULT: " + std::to_string(static_cast<unsigned long>(drop_result)));
    DestroyWindow(drop_overlay_);
    drop_overlay_ = nullptr;
    return;
  }
  ShowWindow(drop_overlay_, SW_HIDE);
}

void AppWindow::destroy_drop_overlay() {
  if (!drop_overlay_) return;
  RevokeDragDrop(drop_overlay_);
  if (IsWindow(drop_overlay_)) DestroyWindow(drop_overlay_);
  drop_overlay_ = nullptr;
}

void AppWindow::update_drop_overlay_bounds(const int x, const int y, const int width, const int height) {
  if (!window_) return;

  drop_bounds_ = {x, y, x + width, y + height};
  drop_bounds_valid_ = true;
  if (!drop_overlay_) return;

  RECT client{};
  GetClientRect(window_, &client);
  const int client_width = static_cast<int>(client.right - client.left);
  const int client_height = static_cast<int>(client.bottom - client.top);
  const int left = std::clamp(x, 0, std::max(0, client_width - 1));
  const int top = std::clamp(y, 0, std::max(0, client_height - 1));
  const int overlay_width = std::min(width, std::max(0, client_width - left));
  const int overlay_height = std::min(height, std::max(0, client_height - top));
  if (overlay_width < 16 || overlay_height < 16 || IsIconic(window_)) {
    ShowWindow(drop_overlay_, SW_HIDE);
    return;
  }

  POINT screen_origin{left, top};
  ClientToScreen(window_, &screen_origin);
  SetWindowPos(drop_overlay_, HWND_TOP, screen_origin.x, screen_origin.y, overlay_width, overlay_height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  InvalidateRect(drop_overlay_, nullptr, TRUE);
}

void AppWindow::select_folders() {
  ComPtr<IFileOpenDialog> dialog;
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (FAILED(result) || !dialog) {
    append_log("[ERROR] Could not open the folder picker.");
    return;
  }

  FILEOPENDIALOGOPTIONS options{};
  dialog->GetOptions(&options);
  options = static_cast<FILEOPENDIALOGOPTIONS>(
      options | FOS_PICKFOLDERS | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
  dialog->SetOptions(options);
  dialog->SetTitle(L"选择一个或多个小程序文件夹");
  dialog->SetOkButtonLabel(L"选择文件夹");

  result = dialog->Show(window_);
  if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
  if (FAILED(result)) {
    append_log("[ERROR] Folder selection failed.");
    return;
  }

  ComPtr<IShellItemArray> selected_items;
  result = dialog->GetResults(&selected_items);
  if (FAILED(result) || !selected_items) {
    append_log("[ERROR] Could not read the selected folders.");
    return;
  }

  DWORD count{};
  selected_items->GetCount(&count);
  std::vector<fs::path> selected_paths;
  selected_paths.reserve(count);
  for (DWORD index = 0; index < count; ++index) {
    ComPtr<IShellItem> item;
    if (FAILED(selected_items->GetItemAt(index, &item)) || !item) continue;
    PWSTR file_path{};
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &file_path)) && file_path) {
      selected_paths.emplace_back(file_path);
      CoTaskMemFree(file_path);
    }
  }
  if (!selected_paths.empty()) add_dropped_folders(selected_paths);
}

void AppWindow::add_dropped_folders(const std::vector<fs::path>& folders) {
  if (running_.load()) append_log("[WARN] 当前任务正在运行，新目录将在结束后可用。");
  Logger logger([this](const std::string_view message) { append_log(std::string(message)); });
  const auto incoming = BatchRunner(std::move(logger)).prepare_inputs(folders);
  for (auto input : incoming) {
    const auto existing = std::find_if(manual_inputs_.begin(), manual_inputs_.end(), [&input](const InputFolder& item) {
      const bool same_wxid = to_lower_ascii(item.wxid) == to_lower_ascii(input.wxid);
      const bool same_path = to_lower_ascii(wide_to_utf8(item.path.wstring())) ==
                             to_lower_ascii(wide_to_utf8(input.path.wstring()));
      return same_wxid || same_path;
    });
    if (existing == manual_inputs_.end()) manual_inputs_.push_back(std::move(input));
  }
  send_manual_inputs();
  append_log("[INFO] 手动导入页当前已选择 " + std::to_string(manual_inputs_.size()) + " 个目录。");
}

void AppWindow::set_drop_hover(const bool active) {
  send_json(std::string("{\"type\":\"drop-hover\",\"active\":") + (active ? "true" : "false") + "}");
}

void AppWindow::remove_manual_input(const std::size_t index) {
  if (running_.load() || index >= manual_inputs_.size()) return;
  const auto display_name = manual_inputs_[index].display_name;
  manual_inputs_.erase(manual_inputs_.begin() + static_cast<std::ptrdiff_t>(index));
  send_manual_inputs();
  append_log("[INFO] 已从手动导入列表移除：" + display_name);
}

void AppWindow::begin_cache_scan(const bool force) {
  if (running_.load() || scanning_.load()) return;
  if (scan_started_ && !force) return;
  scan_started_ = true;
  scan_cancel_requested_.store(false);
  scanning_.store(true);
  scan_result_ = {};
  {
    std::lock_guard lock(scan_progress_mutex_);
    pending_scan_inputs_.clear();
  }
  scan_progress_posted_.store(false);
  send_scan_state(true);
  append_log("[INFO] 正在快速发现本机微信缓存项目，项目会分批显示...");
  if (scan_worker_.joinable()) join_scan_worker();
  scan_worker_ = std::thread([this] {
    Logger logger([this](const std::string_view message) { append_log(std::string(message)); });
    try {
      scan_result_ = WechatCacheScanner(std::move(logger)).scan(
          &scan_cancel_requested_,
          [this](CacheScanUpdate update) { queue_scan_progress(std::move(update)); });
    } catch (const std::exception& error) {
      scan_result_.error = error.what();
    }
    if (window_) PostMessageW(window_, message_scan_complete, 0, 0);
  });
}

void AppWindow::queue_scan_progress(CacheScanUpdate update) {
  if (update.inputs.empty()) return;
  {
    std::lock_guard lock(scan_progress_mutex_);
    for (auto& input : update.inputs) pending_scan_inputs_.push_back(std::move(input));
  }
  if (!scan_progress_posted_.exchange(true) && window_) {
    if (!PostMessageW(window_, message_scan_progress, 0, 0)) {
      scan_progress_posted_.store(false);
    }
  }
}

void AppWindow::merge_scan_progress() {
  scan_progress_posted_.store(false);
  std::vector<InputFolder> incoming;
  {
    std::lock_guard lock(scan_progress_mutex_);
    incoming.swap(pending_scan_inputs_);
  }
  if (incoming.empty()) return;

  const auto identity = [](const InputFolder& input) {
    return to_lower_ascii(input.account_key) + "\n" + to_lower_ascii(input.wxid);
  };
  for (auto& input : incoming) {
    const auto key = identity(input);
    const auto existing = std::find_if(scanned_inputs_.begin(), scanned_inputs_.end(),
                                       [&](const InputFolder& current) {
                                         return identity(current) == key;
                                       });
    if (existing == scanned_inputs_.end()) {
      scanned_inputs_.push_back(std::move(input));
    } else if (!existing->scan_ready) {
      *existing = std::move(input);
    }
  }
  std::stable_sort(scanned_inputs_.begin(), scanned_inputs_.end(),
                   [](const InputFolder& left, const InputFolder& right) {
                     if (left.account_key != right.account_key) {
                       return to_lower_ascii(left.account_key) < to_lower_ascii(right.account_key);
                     }
                     return to_lower_ascii(left.wxid) < to_lower_ascii(right.wxid);
                   });
  send_scan_results();
}

void AppWindow::merge_cache_scan(CacheScanResult result) {
  if (!result.error.empty()) append_log("[ERROR] 自动扫描微信缓存失败：" + result.error);
  std::map<std::string, std::pair<int, std::string>> existing_names;
  for (const auto& input : scanned_inputs_) {
    if (input.app_name.empty()) continue;
    const auto key = to_lower_ascii(input.wxid);
    const auto existing = existing_names.find(key);
    if (existing == existing_names.end() || input.app_name_score > existing->second.first) {
      existing_names[key] = {input.app_name_score, input.app_name};
    }
  }
  for (auto& input : result.inputs) {
    const auto existing = existing_names.find(to_lower_ascii(input.wxid));
    if (existing == existing_names.end() || existing->second.first <= input.app_name_score) continue;
    input.app_name_score = existing->second.first;
    input.app_name = existing->second.second;
    input.display_name = existing->second.second;
  }
  if (!result.inputs.empty() || result.error.empty()) scanned_inputs_ = std::move(result.inputs);
  send_scan_results();
  if (scanned_inputs_.empty() && result.error.empty()) {
    append_log("[WARN] 本机没有发现可用的小程序或小游戏缓存，可切换到手动导入页。");
  }
}

void AppWindow::begin_name_enrichment() {
  std::vector<InputFolder> unresolved;
  std::set<std::string> unresolved_wxids;
  for (const auto& input : scanned_inputs_) {
    if (!input.scan_ready || input.kind == "插件包" || input.app_name_score >= 95) continue;
    unresolved.push_back(input);
    unresolved_wxids.insert(to_lower_ascii(input.wxid));
  }
  if (unresolved.empty()) return;

  if (name_enriching_.load()) {
    const bool has_new_wxid = std::any_of(unresolved_wxids.begin(), unresolved_wxids.end(),
                                          [this](const std::string& wxid) {
                                            return !name_request_wxids_.contains(wxid);
                                          });
    if (has_new_wxid) name_refresh_pending_ = true;
    return;
  }

  if (name_worker_.joinable()) join_name_worker();
  name_request_wxids_ = std::move(unresolved_wxids);
  name_result_.clear();
  {
    std::lock_guard lock(name_progress_mutex_);
    pending_name_updates_.clear();
  }
  name_progress_posted_.store(false);
  name_cancel_requested_.store(false);
  name_enriching_.store(true);
  name_worker_ = std::thread([this, unresolved = std::move(unresolved)] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Logger logger([this](const std::string_view message) { append_log(std::string(message)); });
    try {
      name_result_ = WechatCacheScanner(std::move(logger))
                         .resolve_runtime_names(
                             unresolved,
                             &name_cancel_requested_,
                             [this](std::string wxid, std::string name) {
                               queue_name_progress(std::move(wxid), std::move(name));
                             });
    } catch (const std::exception& error) {
      append_log(std::string("[WARN] 后台项目名称补全结束：") + error.what());
      name_result_.clear();
    }
    if (window_) PostMessageW(window_, message_names_complete, 0, 0);
  });
}

void AppWindow::queue_name_progress(std::string wxid, std::string name) {
  if (wxid.empty() || name.empty()) return;
  {
    std::lock_guard lock(name_progress_mutex_);
    pending_name_updates_[to_lower_ascii(wxid)] = std::move(name);
  }
  if (!name_progress_posted_.exchange(true) && window_) {
    if (!PostMessageW(window_, message_name_progress, 0, 0)) {
      name_progress_posted_.store(false);
    }
  }
}

void AppWindow::merge_name_progress() {
  name_progress_posted_.store(false);
  std::map<std::string, std::string> incoming;
  {
    std::lock_guard lock(name_progress_mutex_);
    incoming.swap(pending_name_updates_);
  }
  merge_resolved_names(std::move(incoming));
}

void AppWindow::merge_resolved_names(std::map<std::string, std::string> names) {
  if (names.empty()) return;
  std::map<std::string, std::string> applied;
  for (auto& input : scanned_inputs_) {
    // Package-derived names with a score above the runtime enrichment score
    // remain authoritative. Runtime candidates may improve while more memory
    // regions are scanned, so later updates at the same score still replace
    // an earlier provisional runtime title.
    if (input.app_name_score > 94) continue;
    const auto resolved = names.find(to_lower_ascii(input.wxid));
    if (resolved == names.end() || resolved->second.empty()) continue;
    if (input.app_name == resolved->second && input.app_name_score == 94) continue;
    input.app_name = resolved->second;
    input.display_name = resolved->second;
    input.app_name_score = 94;
    applied[to_lower_ascii(input.wxid)] = resolved->second;
  }
  if (!applied.empty()) send_json(scan_name_updates_message(applied));
}

void AppWindow::begin_manual_batch() {
  begin_batch(manual_inputs_);
}

void AppWindow::begin_scan_batch(std::vector<std::size_t> indices) {
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

  std::vector<InputFolder> selected_inputs;
  selected_inputs.reserve(indices.size());
  for (const auto index : indices) {
    if (index < scanned_inputs_.size()) selected_inputs.push_back(scanned_inputs_[index]);
  }
  if (selected_inputs.empty()) {
    append_log("[ERROR] 请先在扫描结果中选择至少一个小程序或小游戏。");
    return;
  }

  append_log("[INFO] 已选择 " + std::to_string(selected_inputs.size()) + " 个项目，将依次反编译。");
  begin_batch(std::move(selected_inputs));
}

void AppWindow::open_scanned_input_directory(const std::size_t index) {
  if (index >= scanned_inputs_.size()) {
    append_log("[ERROR] 打开缓存目录失败：扫描项目索引已经失效，请重新扫描。");
    return;
  }

  const auto& input = scanned_inputs_[index];
  fs::path directory = input.path;
  std::error_code error;
  if (!fs::is_directory(directory, error) || error) {
    error.clear();
    const auto package = std::find_if(input.package_files.begin(), input.package_files.end(), [&error](const fs::path& candidate) {
      error.clear();
      return fs::is_regular_file(candidate, error) && !error;
    });
    if (package != input.package_files.end()) directory = package->parent_path();
  }

  error.clear();
  if (directory.empty() || !fs::is_directory(directory, error) || error) {
    append_log("[ERROR] 打开缓存目录失败：" + input.display_name + " 的源目录已经不存在。");
    return;
  }

  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      window_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    append_log("[ERROR] 打开缓存目录失败，ShellExecute 错误码：" + std::to_string(result));
    return;
  }
  append_log("[INFO] 已打开 " + input.display_name + " 的 wxapkg 所在目录：" + wide_to_utf8(directory.wstring()));
}

void AppWindow::begin_batch(std::vector<InputFolder> batch_inputs) {
  if (running_.exchange(true)) return;
  if (batch_inputs.empty()) {
    running_.store(false);
    append_log("[ERROR] 请选择一个扫描结果，或在手动导入页添加目录。");
    send_running_state(false);
    return;
  }
  if (worker_.joinable()) join_worker();

  cancel_requested_.store(false);
  send_json("{\"type\":\"session-start\"}");
  send_running_state(true);
  worker_ = std::thread([this, batch_inputs = std::move(batch_inputs)] {
    Logger logger([this](const std::string_view message) { append_log(std::string(message)); });
    try {
      BatchRunner(std::move(logger)).run(batch_inputs, cancel_requested_);
    } catch (const std::exception& error) {
      append_log(std::string("[ERROR] 任务执行失败：") + error.what());
    }
    if (window_) PostMessageW(window_, message_state, FALSE, 0);
  });
}

void AppWindow::append_log(std::string message) {
  if (!window_) return;
  auto* text = new std::string(std::move(message));
  if (!PostMessageW(window_, message_log, 0, reinterpret_cast<LPARAM>(text))) delete text;
}

void AppWindow::send_json(std::string json) {
  if (!webview_) return;
  const auto wide = utf8_to_wide(json);
  webview_->PostWebMessageAsJson(wide.c_str());
}

void AppWindow::send_manual_inputs() {
  send_json(input_list_message("manual-inputs", manual_inputs_));
}

void AppWindow::send_scan_results() {
  send_json(input_list_message("scan-results", scanned_inputs_));
}

void AppWindow::send_running_state(const bool running) {
  send_json(state_message(running));
}

void AppWindow::send_scan_state(const bool scanning) {
  send_json(std::string("{\"type\":\"scan-state\",\"scanning\":") +
            (scanning ? "true" : "false") + "}");
}

void AppWindow::join_worker() {
  if (worker_.joinable()) worker_.join();
}

void AppWindow::join_scan_worker() {
  if (scan_worker_.joinable()) scan_worker_.join();
}

void AppWindow::join_name_worker() {
  if (name_worker_.joinable()) name_worker_.join();
}

LRESULT CALLBACK AppWindow::window_proc(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) {
  AppWindow* app = nullptr;
  if (message == WM_NCCREATE) {
    const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    app = static_cast<AppWindow*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    app->window_ = window;
  } else {
    app = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  }
  return app ? app->handle_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK AppWindow::drop_overlay_proc(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) {
  AppWindow* app = nullptr;
  if (message == WM_NCCREATE) {
    const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    app = static_cast<AppWindow*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  } else {
    app = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  }

  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    case WM_LBUTTONUP:
      if (app) app->select_folders();
      return 0;
    case WM_ERASEBKGND:
      return TRUE;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const HDC dc = BeginPaint(window, &paint);
      FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      EndPaint(window, &paint);
      return 0;
    }
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

LRESULT AppWindow::handle_message(const UINT message, const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
    case WM_MOVE:
    case WM_SIZE:
      resize_webview();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* minimum = reinterpret_cast<MINMAXINFO*>(lparam);
      const UINT dpi = window_ ? GetDpiForWindow(window_) : GetDpiForSystem();
      minimum->ptMinTrackSize.x = MulDiv(680, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
      minimum->ptMinTrackSize.y = MulDiv(440, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
      return 0;
    }
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(window_, nullptr,
                   suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
      resize_webview();
      return 0;
    }
    case message_log: {
      std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lparam));
      send_json("{\"type\":\"log\",\"message\":\"" + json_escape(*text) + "\"}");
      return 0;
    }
    case message_state:
      running_.store(wparam != FALSE);
      send_running_state(wparam != FALSE);
      return 0;
    case message_scan_complete:
      join_scan_worker();
      {
        std::lock_guard lock(scan_progress_mutex_);
        pending_scan_inputs_.clear();
      }
      scan_progress_posted_.store(false);
      scanning_.store(false);
      merge_cache_scan(std::move(scan_result_));
      send_scan_state(false);
      begin_name_enrichment();
      return 0;
    case message_scan_progress:
      merge_scan_progress();
      return 0;
    case message_names_complete:
      join_name_worker();
      name_enriching_.store(false);
      merge_name_progress();
      merge_resolved_names(std::move(name_result_));
      name_request_wxids_.clear();
      if (name_refresh_pending_) {
        name_refresh_pending_ = false;
        begin_name_enrichment();
      }
      return 0;
    case message_name_progress:
      merge_name_progress();
      return 0;
    case WM_CLOSE:
      cancel_requested_.store(true);
      scan_cancel_requested_.store(true);
      name_cancel_requested_.store(true);
      DestroyWindow(window_);
      return 0;
    case WM_DESTROY:
      cancel_requested_.store(true);
      scan_cancel_requested_.store(true);
      name_cancel_requested_.store(true);
      join_worker();
      join_scan_worker();
      join_name_worker();
      destroy_drop_overlay();
      window_ = nullptr;
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window_, message, wparam, lparam);
  }
}

}  // namespace wedecode

int APIENTRY wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, const int show_command) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  wedecode::AppWindow application(instance);
  return application.run(show_command);
}

