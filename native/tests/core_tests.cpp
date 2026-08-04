#include "wedecode/batch_runner.h"
#include "wedecode/common.h"
#include "wedecode/wxapkg.h"
#include "wedecode/wechat_cache_scanner.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using wedecode::Wxapkg;

namespace {

void put_u32_be(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 16U) & 0xffU);
  bytes[offset + 2] = static_cast<std::byte>((value >> 8U) & 0xffU);
  bytes[offset + 3] = static_cast<std::byte>(value & 0xffU);
}

std::vector<std::byte> make_single_file_package(const std::string& name, const std::string& content) {
  const auto entry_length = static_cast<std::uint32_t>(4 + name.size() + 4 + 4);
  const auto index_end = static_cast<std::size_t>(18 + entry_length);
  std::vector<std::byte> package(index_end + content.size());
  package[0] = static_cast<std::byte>(0xbe);
  put_u32_be(package, 5, entry_length);
  package[13] = static_cast<std::byte>(0xed);
  put_u32_be(package, 14, 1);
  put_u32_be(package, 18, static_cast<std::uint32_t>(name.size()));
  for (std::size_t index = 0; index < name.size(); ++index) package[22 + index] = static_cast<std::byte>(name[index]);
  const auto metadata = 22 + name.size();
  put_u32_be(package, metadata, static_cast<std::uint32_t>(index_end));
  put_u32_be(package, metadata + 4, static_cast<std::uint32_t>(content.size()));
  for (std::size_t index = 0; index < content.size(); ++index) package[index_end + index] = static_cast<std::byte>(content[index]);
  return package;
}

std::vector<std::byte> make_package() {
  return make_single_file_package("/foo.txt", "native wxapkg test\n");
}

}  // namespace

int main() {
  try {
    assert(wedecode::is_safe_relative_path("/pages/index.js"));
    assert(!wedecode::is_safe_relative_path("../../outside.txt"));
    assert(!wedecode::is_safe_relative_path("C:/outside.txt"));

    const auto package = make_package();
    assert(Wxapkg::has_valid_header(package));
    const auto entries = Wxapkg::read_file_index(package);
    assert(entries.size() == 1);
    assert(entries.front().packed_path == "foo.txt");

    const std::set<std::string> runtime_wxids{
        "wx1000000000000001", "wx2000000000000002", "wx3000000000000003",
        "wx4000000000000004", "wx5000000000000005", "wx6000000000000006",
        "wx7000000000000007", "wx8000000000000008", "wx9000000000000009"};
    const auto runtime_names = wedecode::extract_runtime_app_names(
        R"RUNTIME(
          {"appId":"wx1000000000000001","appName":"项目甲"}
          {"nickName":"项目乙","hostAppId":"wx2000000000000002"}
          {"appid":"wx3000000000000003","showInfo":{"appId":"wx4000000000000004","appName":"内部项目"}}
          {"wxid":"wx5000000000000005","wxNickName":"项目丙"}
          {"appId":"wx6000000000000006","hostAppId":"wx7000000000000007","appName":"歧义项目"}
          {"appId":"wx8000000000000008","category":"games"}
          {\"hostAppId\":\"wx9000000000000009\",\"nickName\":\"转义项目\"}
        )RUNTIME",
        runtime_wxids);
    assert(runtime_names.at("wx1000000000000001") == "项目甲");
    assert(runtime_names.at("wx2000000000000002") == "项目乙");
    assert(!runtime_names.contains("wx3000000000000003"));
    assert(runtime_names.at("wx4000000000000004") == "内部项目");
    assert(runtime_names.at("wx5000000000000005") == "项目丙");
    assert(!runtime_names.contains("wx6000000000000006"));
    assert(!runtime_names.contains("wx7000000000000007"));
    assert(!runtime_names.contains("wx8000000000000008"));
    assert(runtime_names.at("wx9000000000000009") == "转义项目");

    const std::wstring utf16_runtime_record =
        LR"({"hostAppId":"wx1000000000000001","appNickName":"宽字符项目"})";
    const std::string utf16_runtime_bytes(
        reinterpret_cast<const char*>(utf16_runtime_record.data()),
        utf16_runtime_record.size() * sizeof(wchar_t));
    const auto utf16_runtime_names = wedecode::extract_runtime_app_names(
        utf16_runtime_bytes, {"wx1000000000000001"});
    assert(utf16_runtime_names.at("wx1000000000000001") == "宽字符项目");

    const auto marketplace_parameter_name = wedecode::extract_marketplace_app_name(
        R"HTML(<div dt-params="pkgname=wx1b48b798ed3c9cfb&amp;appname=%E5%A4%A7%E4%BE%A0%E9%97%AF%E5%A4%A9%E4%B8%8B&amp;is_game=1"></div>)HTML");
    assert(marketplace_parameter_name && *marketplace_parameter_name == "大侠闯天下");
    const auto marketplace_title_name = wedecode::extract_marketplace_app_name(
        R"HTML(<html><head><title>打个螺丝游戏下载-打个螺丝微信小游戏-打个螺丝官方小程序入口-应用宝官网</title></head></html>)HTML");
    assert(marketplace_title_name && *marketplace_title_name == "打个螺丝");
    assert(!wedecode::extract_marketplace_app_name(
        R"HTML(<html><head><title>应用宝官网</title></head></html>)HTML"));

    const auto root = fs::temp_directory_path() /
        ("wedecode-native-test-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    const auto unicode_directory = wedecode::make_unique_directory(root, "测试小程序");
    assert(unicode_directory.filename() == fs::path(L"测试小程序"));

    const auto input = root / "app.wxapkg";
    wedecode::write_binary(input, package);
    const auto output = root / "output";
    const auto result = Wxapkg::unpack_file(input, "wx0123456789abcdef", output, wedecode::Logger{});
    assert(result.files_written == 1);
    assert(!result.was_encrypted);
    assert(wedecode::read_text_utf8(output / "foo.txt") == "native wxapkg test\n");

    const auto encrypted_fixture = fs::path(__FILEW__).parent_path() / L"fixtures" / L"encrypted.wxapkg";
    assert(fs::is_regular_file(encrypted_fixture));
    const auto encrypted_bytes = wedecode::read_binary(encrypted_fixture);
    assert(!Wxapkg::has_valid_header(encrypted_bytes));
    const auto decrypted_bytes = Wxapkg::decrypt("wx0123456789abcdef", encrypted_bytes);
    assert(Wxapkg::has_valid_header(decrypted_bytes));
    const auto decrypted_entries = Wxapkg::read_file_index(decrypted_bytes);
    assert(decrypted_entries.size() == 1);
    assert(decrypted_entries.front().packed_path == "foo.txt");

    const auto encrypted_input = root / "encrypted.wxapkg";
    wedecode::write_binary(encrypted_input, encrypted_bytes);
    const auto encrypted_output = root / "encrypted-output";
    const auto encrypted_result = Wxapkg::unpack_file(
        encrypted_input, "wx0123456789abcdef", encrypted_output, wedecode::Logger{});
    assert(encrypted_result.files_written == 1);
    assert(encrypted_result.was_encrypted);
    assert(wedecode::read_text_utf8(encrypted_output / "foo.txt") == "native wxapkg encrypted test\n");

    const auto imported_folder = root / "wx0123456789abcdef";
    fs::create_directories(imported_folder / "nested");
    wedecode::write_binary(imported_folder / "nested" / "__PLUGINCODE__.wxapkg", encrypted_bytes);
    const auto prepared = wedecode::BatchRunner(wedecode::Logger{}).prepare_inputs({imported_folder});
    assert(prepared.size() == 1);
    assert(prepared.front().path == fs::absolute(imported_folder).lexically_normal());
    assert(prepared.front().wxid == "wx0123456789abcdef");
    assert(prepared.front().display_name == "wx0123456789abcdef");
    assert(prepared.front().package_count == 1);

    const auto applet_root = root / "wechat-account-a" / "applet";
    const auto icon_path = applet_root / "icon" / "wx0123456789abcdef_test.png";
    wedecode::write_binary(icon_path,
                           {std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}});
    const auto icon_uri = wedecode::png_data_uri(icon_path);
    assert(icon_uri.starts_with("data:image/png;base64,"));
    assert(wedecode::png_data_uri(icon_path, 2).empty());
    const auto cached_version = applet_root / "packages" / "wx0123456789abcdef" / "7";
    fs::create_directories(cached_version);
    const auto named_package = make_single_file_package(
        "/app-service.js",
        R"(const build = { appId: "wx0123456789abcdef", appName: "测试小程序", appVersion: "1.0.0" };)" );
    wedecode::write_binary(cached_version / "__APP__.wxapkg", named_package);
    wedecode::write_binary(
        cached_version / "_subpackages_script-bundle_.wxapkg",
        make_single_file_package(
            "/app-config.json",
            R"({"subPackages":[{"name":"script-bundle","root":"script-bundle/"},{"name":"data-package","root":"/data-package/"},{"name":"wasmcode2","root":"/wasmcode2/"},{"name":"truly-missing","root":"truly-missing"}],"plugins":{"shared":{"provider":"wx1111111111111111","version":"1.0.0"}}})"));
    wedecode::write_binary(
        cached_version / "__WITHOUT_MULTI_PLUGINCODE__.wxapkg",
        make_single_file_package(
            "/game.js",
            R"(const config={loadDataPackageFromSubpackage:!1};const marker="kDelayLoadWasmcode2Time";if(GameGlobal.canUseH5Renderer&&GameGlobal.isIOSHighPerformanceMode){wx.loadSubpackage({name:"wasmcode2"});})"));
    const auto older_cached_version = applet_root / "packages" / "wx0123456789abcdef" / "6";
    fs::create_directories(older_cached_version);
    wedecode::write_binary(older_cached_version / "__APP__.wxapkg", make_single_file_package("/stale.txt", "old"));
    wedecode::write_binary(
        older_cached_version / "_data-package_.wxapkg",
        make_single_file_package("/data-package/game.data", "cached data package"));
    const auto runtime_cache = applet_root / "local" / "wx0123456789abcdef" / "usr" / "__GAME_FILE_CACHE";
    fs::create_directories(runtime_cache);
    wedecode::write_text_utf8(runtime_cache / "cached-resource.bin", "runtime cache");

    const auto plugin_version = applet_root / "packages" / "wx1111111111111111" / "3";
    fs::create_directories(plugin_version);
    wedecode::write_binary(
        plugin_version / "__PLUGINCODE__.wxapkg",
        make_single_file_package("/plugin.json", R"({"pluginName":"测试通用插件 A","main":"index.js"})"));
    const auto older_plugin_version = applet_root / "packages" / "wx1111111111111111" / "2";
    fs::create_directories(older_plugin_version);
    wedecode::write_binary(
        older_plugin_version / "__PLUGINCODE__.wxapkg",
        make_single_file_package("/plugin.json", R"({"pluginName":"测试通用插件 A","main":"legacy.js"})"));

    const auto second_applet_root = root / "wechat-account-b" / "applet";
    const auto second_cached_version = second_applet_root / "packages" / "wx0123456789abcdef" / "8";
    fs::create_directories(second_cached_version);
    wedecode::write_binary(second_cached_version / "__APP__.wxapkg", named_package);
    wedecode::write_binary(
        second_cached_version / "_subpackages_script-bundle_.wxapkg",
        make_single_file_package(
            "/app-config.json",
            R"({"plugins":{"shared":{"provider":"wx1111111111111111","version":"2.0.0"}}})"));
    const auto second_plugin_version = second_applet_root / "packages" / "wx1111111111111111" / "4";
    fs::create_directories(second_plugin_version);
    wedecode::write_binary(
        second_plugin_version / "__PLUGINCODE__.wxapkg",
        make_single_file_package("/plugin.json", R"({"pluginName":"测试通用插件 B","main":"index.js"})"));

    const auto scanner_cache = root / "scanner-state";
    std::vector<std::string> scan_logs;
    std::mutex progress_mutex;
    std::vector<wedecode::InputFolder> progressive_inputs;
    std::vector<std::size_t> progressive_batch_sizes;
    const auto scan = wedecode::WechatCacheScanner(wedecode::Logger([&](const std::string_view message) {
      scan_logs.emplace_back(message);
    }), scanner_cache).scan_applet_roots(
        {applet_root, second_applet_root},
        nullptr,
        [&](wedecode::CacheScanUpdate update) {
          std::lock_guard lock(progress_mutex);
          progressive_batch_sizes.push_back(update.inputs.size());
          for (auto& input : update.inputs) progressive_inputs.push_back(std::move(input));
        });
    assert(scan.account_count == 2);
    assert(scan.package_count == 10);
    assert(scan.plugin_cache_count == 2);
    assert(scan.dependency_package_count == 3);
    assert(scan.inputs.size() == 2);
    assert(scan.inspection_cache_hits == 0);
    assert(scan.inspection_cache_misses == 4);
    assert(progressive_inputs.size() >= 4);
    assert(progressive_batch_sizes.size() == progressive_inputs.size());
    assert(std::all_of(progressive_batch_sizes.begin(), progressive_batch_sizes.end(),
                       [](const std::size_t size) { return size == 1; }));
    assert(std::count_if(progressive_inputs.begin(), progressive_inputs.end(), [](const auto& input) {
      return !input.scan_ready && input.display_name == input.wxid && input.kind == "正在整理";
    }) >= 2);
    assert(std::any_of(progressive_inputs.begin(), progressive_inputs.end(),
                       [&](const auto& input) {
                         return !input.scan_ready && input.wxid == "wx0123456789abcdef" &&
                                input.icon_data_uri == icon_uri;
                       }));
    assert(std::count_if(progressive_inputs.begin(), progressive_inputs.end(), [](const auto& input) {
      return !input.scan_ready && input.display_name == "测试小程序" &&
             input.app_name == "测试小程序" && input.app_name_score > 0;
    }) >= 2);
    assert(fs::is_regular_file(scanner_cache / "app-name-map.json"));
    assert(std::any_of(scan_logs.begin(), scan_logs.end(), [](const std::string& message) {
      return message.find("并发目录扫描") != std::string::npos;
    }));
    assert(std::any_of(scan_logs.begin(), scan_logs.end(), [](const std::string& message) {
      return message.find("并发包分析") != std::string::npos;
    }));

    std::vector<wedecode::InputFolder> repeat_progressive_inputs;
    const auto repeat_scan =
        wedecode::WechatCacheScanner(wedecode::Logger{}, scanner_cache)
            .scan_applet_roots(
                {applet_root, second_applet_root},
                nullptr,
                [&](wedecode::CacheScanUpdate update) {
                  for (auto& input : update.inputs) {
                    repeat_progressive_inputs.push_back(std::move(input));
                  }
                });
    assert(repeat_scan.inputs.size() == scan.inputs.size());
    assert(repeat_scan.inspection_cache_hits == 4);
    assert(repeat_scan.inspection_cache_misses == 0);
    assert(repeat_scan.name_cache_hits == 1);
    assert(std::any_of(repeat_progressive_inputs.begin(),
                       repeat_progressive_inputs.end(),
                       [](const auto& input) {
                         return !input.scan_ready && input.app_name == "测试小程序" &&
                                input.display_name == "测试小程序";
                       }));
    for (std::size_t index = 0; index < scan.inputs.size(); ++index) {
      assert(repeat_scan.inputs[index].account_key == scan.inputs[index].account_key);
      assert(repeat_scan.inputs[index].wxid == scan.inputs[index].wxid);
      assert(repeat_scan.inputs[index].path == scan.inputs[index].path);
      assert(repeat_scan.inputs[index].package_files == scan.inputs[index].package_files);
    }

    const wedecode::InputFolder* first_account{};
    const wedecode::InputFolder* second_account{};
    for (const auto& scanned_input : scan.inputs) {
      if (scanned_input.account_key == "test-0") first_account = &scanned_input;
      if (scanned_input.account_key == "test-1") second_account = &scanned_input;
    }
    assert(first_account);
    assert(second_account);
    assert(first_account->account_name.find("微信账号 1") == 0);
    assert(second_account->account_name.find("微信账号 2") == 0);

    assert(first_account->wxid == "wx0123456789abcdef");
    assert(first_account->version == "7");
    assert(first_account->package_count == 5);
    assert(first_account->package_files.size() == 5);
    assert(std::count_if(first_account->package_files.begin(), first_account->package_files.end(), [](const fs::path& package) {
      return package.filename() == "__APP__.wxapkg";
    }) == 2);
    assert(std::any_of(first_account->package_files.begin(), first_account->package_files.end(), [](const fs::path& package) {
      return package.filename() == "_data-package_.wxapkg";
    }));
    assert(first_account->declared_subpackages.size() == 4);
    assert(first_account->missing_subpackages.size() == 1);
    assert(first_account->missing_subpackages.front() == "truly-missing");
    assert(first_account->platform_conditional_subpackages.size() == 1);
    assert(first_account->platform_conditional_subpackages.front() == "wasmcode2");
    assert(first_account->runtime_or_external_subpackages.empty());
    assert(first_account->runtime_cache_path == fs::absolute(runtime_cache).lexically_normal());
    assert(first_account->account_count == 1);
    assert(first_account->dependencies.size() == 1);
    assert(first_account->dependencies.front().wxid == "wx1111111111111111");
    assert(first_account->dependencies.front().display_name == "测试通用插件 A");
    assert(first_account->dependencies.front().path == fs::absolute(plugin_version).lexically_normal());
    assert(first_account->dependencies.front().package_count == 2);
    assert(first_account->dependencies.front().package_files.size() == 2);
    assert(first_account->missing_dependency_wxids.empty());
    assert(first_account->app_name == "测试小程序");
    assert(first_account->display_name == "测试小程序");
    assert(first_account->kind == "小程序");
    assert(first_account->auto_discovered);
    assert(first_account->path == fs::absolute(cached_version).lexically_normal());
    assert(first_account->icon_path == fs::absolute(icon_path).lexically_normal());
    assert(first_account->icon_data_uri == icon_uri);

    auto unresolved_name_input = *first_account;
    unresolved_name_input.app_name.clear();
    unresolved_name_input.display_name = unresolved_name_input.wxid;
    unresolved_name_input.app_name_score = 0;
    std::vector<std::pair<std::string, std::string>> runtime_name_updates;
    const auto resolved_runtime_names =
        wedecode::WechatCacheScanner(wedecode::Logger{}, scanner_cache)
            .resolve_runtime_names(
                {unresolved_name_input},
                nullptr,
                [&](std::string wxid, std::string name) {
                  runtime_name_updates.emplace_back(std::move(wxid), std::move(name));
                });
    assert(resolved_runtime_names.at("wx0123456789abcdef") == "测试小程序");
    assert(std::any_of(runtime_name_updates.begin(), runtime_name_updates.end(), [](const auto& update) {
      return update.first == "wx0123456789abcdef" && update.second == "测试小程序";
    }));

    wedecode::write_binary(
        cached_version / "_late-package_.wxapkg",
        make_single_file_package("/late-package/value.txt", "downloaded after scan"));

    const auto changed_scan =
        wedecode::WechatCacheScanner(wedecode::Logger{}, scanner_cache)
            .scan_applet_roots({applet_root, second_applet_root});
    assert(changed_scan.inspection_cache_hits == 3);
    assert(changed_scan.inspection_cache_misses == 1);
    assert(changed_scan.name_cache_hits == 1);
    const auto changed_first_account =
        std::find_if(changed_scan.inputs.begin(), changed_scan.inputs.end(), [](const auto& item) {
          return item.account_key == "test-0" && item.wxid == "wx0123456789abcdef";
        });
    assert(changed_first_account != changed_scan.inputs.end());
    assert(changed_first_account->package_count == 6);
    assert(changed_first_account->app_name == "测试小程序");
    assert(std::any_of(changed_first_account->package_files.begin(),
                       changed_first_account->package_files.end(),
                       [](const fs::path& package) {
                         return package.filename() == "_late-package_.wxapkg";
                       }));

    assert(second_account->wxid == "wx0123456789abcdef");
    assert(second_account->version == "8");
    assert(second_account->dependencies.size() == 1);
    assert(second_account->dependencies.front().display_name == "测试通用插件 B");
    assert(second_account->dependencies.front().path == fs::absolute(second_plugin_version).lexically_normal());
    assert(second_account->path == fs::absolute(second_cached_version).lexically_normal());

    const auto naming_root = root / "naming-regression" / "applet";
    const auto weak_name_version = naming_root / "packages" / "wx2222222222222222" / "1";
    fs::create_directories(weak_name_version);
    wedecode::write_binary(
        weak_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app.json",
            R"({"name":"Data","title":"Dev","navigationBarTitleText":"登录"})"));
    const auto strong_name_version = naming_root / "packages" / "wx3333333333333333" / "1";
    fs::create_directories(strong_name_version);
    wedecode::write_binary(
        strong_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/game.json",
            R"({"name":"Effect","title":"Font","appName":"清晰游戏名"})"));
    const auto encoded_name_version = naming_root / "packages" / "wx4444444444444444" / "1";
    fs::create_directories(encoded_name_version);
    wedecode::write_binary(
        encoded_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app.json",
            R"({"appName":"%E5%83%8F%E7%B4%A0%E5%86%92%E9%99%A9"})"));
    const auto expression_name_version = naming_root / "packages" / "wx5555555555555555" / "1";
    fs::create_directories(expression_name_version);
    wedecode::write_binary(
        expression_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app-service.js",
            R"(const build={appId:"wx5555555555555555",appName:"+e.appName+",appVersion:"1.0.0"};)"));
    const auto isolated_auxiliary_version = naming_root / "packages" / "wx6666666666666666" / "1";
    fs::create_directories(isolated_auxiliary_version);
    wedecode::write_binary(
        isolated_auxiliary_version / "__APP__.wxapkg",
        make_single_file_package(
            "/chunks/profile.js",
            R"(const profile={appName:"背包道具"};)"));
    const auto repeated_auxiliary_version = naming_root / "packages" / "wx7777777777777777" / "1";
    fs::create_directories(repeated_auxiliary_version);
    wedecode::write_binary(
        repeated_auxiliary_version / "_chunk-a_.wxapkg",
        make_single_file_package(
            "/chunks/a.js",
            R"(const first={gameName:"像素冒险"};)"));
    wedecode::write_binary(
        repeated_auxiliary_version / "_chunk-b_.wxapkg",
        make_single_file_package(
            "/chunks/b.js",
            R"(const second={gameName:"像素冒险"};)"));
    const auto developer_project_name_version = naming_root / "packages" / "wx8888888888888888" / "1";
    fs::create_directories(developer_project_name_version);
    wedecode::write_binary(
        developer_project_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/project.private.config.json",
            R"({"projectname":"openDataContext"})"));
    const auto browser_app_name_version = naming_root / "packages" / "wx9999999999999999" / "1";
    fs::create_directories(browser_app_name_version);
    wedecode::write_binary(
        browser_app_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app-service.js",
            R"(const navigatorShim={appCodeName:"Mozilla",appName:"Netscape",appVersion:"5.0 "+runtime};)"));
    const auto user_nickname_version = naming_root / "packages" / "wxaaaaaaaaaaaaaaaa" / "1";
    fs::create_directories(user_nickname_version);
    wedecode::write_binary(
        user_nickname_version / "__APP__.wxapkg",
        make_single_file_package(
            "/game.js",
            R"(const userInfo={nickName:"微信用户",avatarUrl:"avatar.png"};)"));
    const auto contextual_app_name_version = naming_root / "packages" / "wxbbbbbbbbbbbbbbbb" / "1";
    fs::create_directories(contextual_app_name_version);
    wedecode::write_binary(
        contextual_app_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app-service.js",
            R"(const build={appId:"wxbbbbbbbbbbbbbbbb",appName:"Sora2",appVersion:"1.0.0"};)"));
    const auto global_title_version = naming_root / "packages" / "wxcccccccccccccccc" / "1";
    fs::create_directories(global_title_version);
    wedecode::write_binary(
        global_title_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app-config.json",
            R"({"page":{"pages/login.html":{"window":{"navigationBarTitleText":"登录"}}},"global":{"window":{"navigationBarTitleText":"小程序：嫂嫂模拟器"}}})"));
    const auto h5_game_name_version = naming_root / "packages" / "wxdddddddddddddddd" / "1";
    fs::create_directories(h5_game_name_version);
    wedecode::write_binary(
        h5_game_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/game.js",
            R"(const cfg={customer_appid:"wxdddddddddddddddd",h5_game_name:"向僵尸开炮"};)"));
    const auto uppercase_game_name_version = naming_root / "packages" / "wxeeeeeeeeeeeeeeee" / "1";
    fs::create_directories(uppercase_game_name_version);
    wedecode::write_binary(
        uppercase_game_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/game.js",
            R"(const cfg={APP_ID:"wxeeeeeeeeeeeeeeee",GAME_NAME:"消消大挑战",APP_VERSION:"1.0.8"};)"));
    const auto unicode_game_title_version = naming_root / "packages" / "wxabcdefabcdefabcd" / "1";
    fs::create_directories(unicode_game_title_version);
    wedecode::write_binary(
        unicode_game_title_version / "__APP__.wxapkg",
        make_single_file_package(
            "/game.js",
            R"("wxabcdefabcdefabcd"==cfg.appId?(cfg.gameTitle="\u5012\u6c34\u6311\u6218"):0;)"));
    const auto paired_generic_name_version = naming_root / "packages" / "wx1234abcdef567890" / "1";
    fs::create_directories(paired_generic_name_version);
    wedecode::write_binary(
        paired_generic_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/assets/resources/import/minigame-config.json",
            R"([1,0,0,[["cc.JsonAsset",["_name","json"],1]],[[0,0,1,3]],[[0,"MiniGameConfig",{"name":"平阳四副头","partnerAppId":"wx1234abcdef567890"}]],0,0,[],[],[]])"));
    const auto cross_promotion_version = naming_root / "packages" / "wxfedcbafedcbafedc" / "1";
    fs::create_directories(cross_promotion_version);
    wedecode::write_binary(
        cross_promotion_version / "__APP__.wxapkg",
        make_single_file_package(
            "/game.js",
            R"(const current="wxfedcbafedcbafedc";const items=[{appID:"wx1111111111111111",appName:"夜店之王"},{appID:"wx2222222222222222",appName:"英雄杀"}];)"));
    const auto known_plugin_consumer_version = naming_root / "packages" / "wx13579bdf2468ace0" / "1";
    fs::create_directories(known_plugin_consumer_version);
    wedecode::write_binary(
        known_plugin_consumer_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app-config.json",
            R"({"appName":"插件映射测试","plugins":{"unity":{"provider":"wxe5a48f1ed5f544b7","version":"1.0.0"}}})"));
    const auto known_plugin_version = naming_root / "packages" / "wxe5a48f1ed5f544b7" / "1";
    fs::create_directories(known_plugin_version);
    wedecode::write_binary(
        known_plugin_version / "__PLUGINCODE__.wxapkg",
        make_single_file_package("/plugin.json", R"({"main":"index.js"})"));
    const auto percent_project_name_version =
        naming_root / "packages" / "wx2468ace013579bdf" / "1";
    fs::create_directories(percent_project_name_version);
    wedecode::write_binary(
        percent_project_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/project.private.config.json",
            R"({"projectname":"%E7%96%AF%E7%8B%82%E6%9D%A5%E8%A7%A3%E5%8E%8B"})"));
    const auto distant_h5_name_version =
        naming_root / "packages" / "wx02468ace13579bdf" / "1";
    fs::create_directories(distant_h5_name_version);
    const auto distant_h5_source =
        std::string(R"(const currentWxid="wx02468ace13579bdf";)") +
        std::string(48U * 1024U, ' ') +
        R"(const productConfig={h5_game_name:"无限轮回"};)";
    wedecode::write_binary(
        distant_h5_name_version / "__APP__.wxapkg",
        make_single_file_package("/game.js", distant_h5_source));
    const auto product_build_name_version =
        naming_root / "packages" / "wx4680ace13579bdf2" / "1";
    fs::create_directories(product_build_name_version);
    const auto product_build_source =
        std::string(R"(const wxAppId="wx4680ace13579bdf2";)") +
        std::string(32U * 1024U, ' ') +
        R"(uploadLogs({appName:"俄罗斯方块",version:config.gameVersion,buildNumber:build.BUILD_NUMBER,env:"release"});)";
    wedecode::write_binary(
        product_build_name_version / "__APP__.wxapkg",
        make_single_file_package("/game.js", product_build_source));
    const auto script_fragment_name_version =
        naming_root / "packages" / "wx5791ace02468bdf3" / "1";
    fs::create_directories(script_fragment_name_version);
    wedecode::write_binary(
        script_fragment_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app-service.js",
            R"(const build={appId:"wx5791ace02468bdf3",appName:").concat(r,",appVersion:"1.0.0"};)"));
    const auto entry_title_name_version =
        naming_root / "packages" / "wx6802bdf13579ace4" / "1";
    fs::create_directories(entry_title_name_version);
    wedecode::write_binary(
        entry_title_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app-config.json",
            R"({"entryPagePath":"pages/home/index.html","page":{"pages/home/index.html":{"window":{"navigationBarTitleText":"动态入口项目名"}},"pages/login.html":{"window":{"navigationBarTitleText":"登录"}}},"global":{"window":{"navigationBarTitleText":""}}})"));
    const auto serialized_ui_name_version =
        naming_root / "packages" / "wx7913ce02468abdf5" / "1";
    fs::create_directories(serialized_ui_name_version);
    wedecode::write_binary(
        serialized_ui_name_version / "__APP__.wxapkg",
        make_single_file_package(
            "/subpackages/loading/import/05/052f01e2d.9dc7a.json",
            R"([0,"AppName",3,[[3,"贪吃蛇",36,36,1,1,1,-60,[10]]],0,0,[],[],[]])"));
    const auto app_nickname_version =
        naming_root / "packages" / "wx0f0e0d0c0b0a0908" / "1";
    fs::create_directories(app_nickname_version);
    wedecode::write_binary(
        app_nickname_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app-service.js",
            R"(const wxAppInfo={hostAppId:"wx0f0e0d0c0b0a0908",wxNickName:"实时项目名称"};)"));
    const auto local_identity_version =
        naming_root / "packages" / "wx1020304050607080" / "1";
    fs::create_directories(local_identity_version);
    wedecode::write_binary(
        local_identity_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app.json",
            R"({"navigationBarTitleText":"登录"})"));
    const auto local_identity_record =
        naming_root / "local" / "wx1020304050607080" / "usr" / "temp" / "appinfo.json";
    fs::create_directories(local_identity_record.parent_path());
    wedecode::write_text_utf8(
        local_identity_record,
        R"({"hostAppId":"wx1020304050607080","appNickName":"本地动态项目"})");

    const auto product_config_version =
        naming_root / "packages" / "wx0a0b0c0d0e0f0102" / "1";
    fs::create_directories(product_config_version);
    wedecode::write_binary(
        product_config_version / "__APP__.wxapkg",
        make_single_file_package(
            "/game.json",
            R"({"navigationBarTitleText":"加载中"})"));
    const auto product_config_log =
        naming_root / "local" / "wx0a0b0c0d0e0f0102" / "usr" / "miniprogramLog" / "log1";
    fs::create_directories(product_config_log.parent_path());
    wedecode::write_text_utf8(
        product_config_log,
        "download /欢乐斗地主产品配置/base.json\n"
        "download /欢乐斗地主产品配置/release.json\n");

    const auto unrelated_local_version =
        naming_root / "packages" / "wx1122334455667788" / "1";
    fs::create_directories(unrelated_local_version);
    wedecode::write_binary(
        unrelated_local_version / "__APP__.wxapkg",
        make_single_file_package(
            "/game.json",
            R"({"navigationBarTitleText":"加载中"})"));
    const auto unrelated_local_log =
        naming_root / "local" / "wx1122334455667788" / "usr" / "miniprogramLog" / "log1";
    fs::create_directories(unrelated_local_log.parent_path());
    wedecode::write_text_utf8(
        unrelated_local_log,
        R"({"player":{"nickName":"微信用户"},"survey":"策略游戏，例如无尽冬日等"})");

    const auto naming_scan =
        wedecode::WechatCacheScanner(wedecode::Logger{}, root / "naming-scanner-state")
            .scan_applet_roots({naming_root});
    assert(naming_scan.inputs.size() == 27);
    const auto weak_name = std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
      return item.wxid == "wx2222222222222222";
    });
    const auto strong_name = std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
      return item.wxid == "wx3333333333333333";
    });
    const auto encoded_name = std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
      return item.wxid == "wx4444444444444444";
    });
    const auto expression_name = std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
      return item.wxid == "wx5555555555555555";
    });
    const auto isolated_auxiliary_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx6666666666666666";
        });
    const auto repeated_auxiliary_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx7777777777777777";
        });
    const auto developer_project_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx8888888888888888";
        });
    const auto browser_app_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx9999999999999999";
        });
    const auto user_nickname =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wxaaaaaaaaaaaaaaaa";
        });
    const auto contextual_app_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wxbbbbbbbbbbbbbbbb";
        });
    const auto global_title_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wxcccccccccccccccc";
        });
    const auto h5_game_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wxdddddddddddddddd";
        });
    const auto uppercase_game_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wxeeeeeeeeeeeeeeee";
        });
    const auto unicode_game_title =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wxabcdefabcdefabcd";
        });
    const auto paired_generic_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx1234abcdef567890";
        });
    const auto cross_promotion_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wxfedcbafedcbafedc";
        });
    const auto known_plugin_consumer =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx13579bdf2468ace0";
        });
    const auto percent_project_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx2468ace013579bdf";
        });
    const auto distant_h5_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx02468ace13579bdf";
        });
    const auto product_build_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx4680ace13579bdf2";
        });
    const auto script_fragment_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx5791ace02468bdf3";
        });
    const auto entry_title_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx6802bdf13579ace4";
        });
    const auto serialized_ui_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx7913ce02468abdf5";
        });
    const auto app_nickname =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx0f0e0d0c0b0a0908";
        });
    const auto local_identity_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx1020304050607080";
        });
    const auto product_config_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx0a0b0c0d0e0f0102";
        });
    const auto unrelated_local_name =
        std::find_if(naming_scan.inputs.begin(), naming_scan.inputs.end(), [](const auto& item) {
          return item.wxid == "wx1122334455667788";
        });
    assert(weak_name != naming_scan.inputs.end());
    assert(strong_name != naming_scan.inputs.end());
    assert(encoded_name != naming_scan.inputs.end());
    assert(expression_name != naming_scan.inputs.end());
    assert(isolated_auxiliary_name != naming_scan.inputs.end());
    assert(repeated_auxiliary_name != naming_scan.inputs.end());
    assert(developer_project_name != naming_scan.inputs.end());
    assert(browser_app_name != naming_scan.inputs.end());
    assert(user_nickname != naming_scan.inputs.end());
    assert(contextual_app_name != naming_scan.inputs.end());
    assert(global_title_name != naming_scan.inputs.end());
    assert(h5_game_name != naming_scan.inputs.end());
    assert(uppercase_game_name != naming_scan.inputs.end());
    assert(unicode_game_title != naming_scan.inputs.end());
    assert(paired_generic_name != naming_scan.inputs.end());
    assert(cross_promotion_name != naming_scan.inputs.end());
    assert(known_plugin_consumer != naming_scan.inputs.end());
    assert(percent_project_name != naming_scan.inputs.end());
    assert(distant_h5_name != naming_scan.inputs.end());
    assert(product_build_name != naming_scan.inputs.end());
    assert(script_fragment_name != naming_scan.inputs.end());
    assert(entry_title_name != naming_scan.inputs.end());
    assert(serialized_ui_name != naming_scan.inputs.end());
    assert(app_nickname != naming_scan.inputs.end());
    assert(local_identity_name != naming_scan.inputs.end());
    assert(product_config_name != naming_scan.inputs.end());
    assert(unrelated_local_name != naming_scan.inputs.end());
    assert(weak_name->app_name.empty());
    assert(weak_name->display_name == weak_name->wxid);
    assert(strong_name->app_name == "清晰游戏名");
    assert(strong_name->display_name == "清晰游戏名");
    assert(encoded_name->app_name == "像素冒险");
    assert(expression_name->app_name.empty());
    assert(isolated_auxiliary_name->app_name.empty());
    assert(repeated_auxiliary_name->app_name == "像素冒险");
    assert(developer_project_name->app_name.empty());
    assert(browser_app_name->app_name.empty());
    assert(user_nickname->app_name.empty());
    assert(contextual_app_name->app_name == "Sora2");
    assert(global_title_name->app_name == "嫂嫂模拟器");
    assert(h5_game_name->app_name == "向僵尸开炮");
    assert(uppercase_game_name->app_name == "消消大挑战");
    assert(unicode_game_title->app_name == "倒水挑战");
    assert(paired_generic_name->app_name == "平阳四副头");
    assert(cross_promotion_name->app_name.empty());
    assert(known_plugin_consumer->app_name == "插件映射测试");
    assert(known_plugin_consumer->dependencies.size() == 1);
    assert(known_plugin_consumer->dependencies.front().wxid == "wxe5a48f1ed5f544b7");
    assert(known_plugin_consumer->dependencies.front().display_name == "Unity Loader 插件");
    assert(percent_project_name->app_name == "疯狂来解压");
    assert(distant_h5_name->app_name == "无限轮回");
    assert(product_build_name->app_name == "俄罗斯方块");
    assert(script_fragment_name->app_name.empty());
    assert(entry_title_name->app_name == "动态入口项目名");
    assert(serialized_ui_name->app_name == "贪吃蛇");
    assert(app_nickname->app_name == "实时项目名称");
    assert(local_identity_name->app_name == "本地动态项目");
    assert(product_config_name->app_name == "欢乐斗地主");
    assert(unrelated_local_name->app_name.empty());

    std::atomic_bool cancel_requested{};
    const auto batch_summary = wedecode::BatchRunner(wedecode::Logger{}).run({*first_account}, cancel_requested);
    assert(batch_summary.succeeded == 1);
    const auto project_output = batch_summary.batch_output;
    assert(wedecode::read_text_utf8(project_output / "app-service.js").find("测试小程序") != std::string::npos);
    assert(wedecode::read_text_utf8(project_output / "data-package" / "game.data") == "cached data package");
    assert(wedecode::read_text_utf8(project_output / "late-package" / "value.txt") == "downloaded after scan");
    assert(wedecode::read_text_utf8(project_output / L"_历史版本" / L"6" / L"__APP__" / L"stale.txt") == "old");
    const auto dependency_output = project_output / L"通用插件" / L"测试通用插件 A";
    assert(wedecode::read_text_utf8(dependency_output / "plugin.json").find("index.js") != std::string::npos);
    assert(wedecode::read_text_utf8(
        dependency_output / L"_历史版本" / L"2" / L"__PLUGINCODE__" / L"plugin.json").find("legacy.js") != std::string::npos);
    const auto manifest = wedecode::read_text_utf8(project_output / L"解包清单.json");
    assert(manifest.find("\"totalRelevantPackageCount\": 8") != std::string::npos);
    assert(manifest.find("\"dependencyPackageCount\": 2") != std::string::npos);
    assert(manifest.find("\"platformConditionalSubpackages\": [\"wasmcode2\"]") != std::string::npos);
    assert(manifest.find("\"subpackagesNotPresentInLocalWxapkgCache\": [\"truly-missing\"]") != std::string::npos);

    const auto output_root = project_output.parent_path();
    auto multi_first = *first_account;
    auto multi_second = *second_account;
    multi_first.app_name = "multi-project-a";
    multi_first.display_name = "multi-project-a";
    multi_second.app_name = "multi-project-b";
    multi_second.display_name = "multi-project-b";
    fs::remove_all(output_root / "multi-project-a");
    fs::remove_all(output_root / "multi-project-b");

    const auto multi_summary = wedecode::BatchRunner(wedecode::Logger{}).run(
        {multi_first, multi_second}, cancel_requested);
    assert(multi_summary.succeeded == 2);
    assert(multi_summary.batch_output == output_root);
    assert(wedecode::read_text_utf8(output_root / "multi-project-a" / "app-service.js").find("测试小程序") != std::string::npos);
    assert(wedecode::read_text_utf8(output_root / "multi-project-b" / "app-service.js").find("测试小程序") != std::string::npos);
    assert(fs::exists(output_root / "multi-project-a" / L"解包清单.json"));
    assert(fs::exists(output_root / "multi-project-b" / L"解包清单.json"));

    const auto nested_root = root / "subpackage-wrapper-regression" / "applet";
    const auto nested_version = nested_root / "packages" / "wxabcdefabcdefabcd" / "1";
    fs::create_directories(nested_version);
    wedecode::write_binary(
        nested_version / "__APP__.wxapkg",
        make_single_file_package(
            "/app-config.json",
            R"({"subPackages":[{"name":"pages2","root":"pages2/"},{"name":"subpages","root":"subpages/"},{"name":"videotools","root":"videotools/"}]})"));
    wedecode::write_binary(
        nested_version / "bundle.wxapkg",
        make_single_file_package("/subpackages/pages2/index.js", R"(console.log("pages2");)"));
    wedecode::write_binary(
        nested_version / "_subpages_.wxapkg",
        make_single_file_package("/subpages/index.js", R"(console.log("subpages");)"));
    wedecode::write_binary(
        nested_version / "_videotools_.wxapkg",
        make_single_file_package("/videotools/index.js", R"(console.log("videotools");)"));
    const auto nested_scan = wedecode::WechatCacheScanner(wedecode::Logger{}, root / "nested-scanner-state")
                                 .scan_applet_roots({nested_root}, nullptr, nullptr);
    assert(nested_scan.inputs.size() == 1);
    assert(nested_scan.inputs.front().missing_subpackages.empty());

    fs::remove_all(project_output);
    fs::remove_all(output_root / "multi-project-a");
    fs::remove_all(output_root / "multi-project-b");
    fs::remove_all(nested_root.parent_path());
    fs::remove_all(root / "nested-scanner-state");
    fs::remove_all(root);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}


