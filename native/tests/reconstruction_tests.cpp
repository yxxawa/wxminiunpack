#include "wedecode/reconstructor.h"
#include "wedecode/wxapkg.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function function, const char* message) {
  try { function(); } catch (const std::exception&) { return; }
  throw std::runtime_error(message);
}

void put(std::vector<std::byte>& data, std::size_t offset, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) data[offset++] = static_cast<std::byte>((value >> shift) & 255);
}

std::vector<std::byte> package() {
  std::vector<std::byte> data(38);
  data[0] = std::byte{0xbe}; data[13] = std::byte{0xed};
  put(data, 5, 23); put(data, 9, 1); put(data, 14, 1);
  put(data, 18, 7);
  const std::string name = "/foo.js";
  for (std::size_t i = 0; i < name.size(); ++i) data[22 + i] = static_cast<std::byte>(name[i]);
  put(data, 29, 37); put(data, 33, 1); data[37] = std::byte{'x'};
  return data;
}

void test_package_validation(const fs::path& root) {
  const auto valid = package();
  check(wedecode::Wxapkg::read_file_index(valid).size() == 1, "valid real-format index rejected");
  std::vector<std::byte> duplicate(58);
  std::copy(valid.begin(), valid.begin() + 18, duplicate.begin());
  std::copy(valid.begin() + 18, valid.begin() + 37, duplicate.begin() + 18);
  std::copy(valid.begin() + 18, valid.begin() + 37, duplicate.begin() + 37);
  put(duplicate, 5, 42); put(duplicate, 9, 2); put(duplicate, 14, 2);
  put(duplicate, 29, 56); put(duplicate, 48, 56);
  duplicate[56] = std::byte{'x'}; duplicate[57] = std::byte{'y'};
  check(wedecode::Wxapkg::read_file_index(duplicate).size() == 2, "identical repeated plugin entry rejected");
  put(duplicate, 48, 57);
  rejects([&] { wedecode::Wxapkg::read_file_index(duplicate); }, "conflicting output paths accepted");
  for (int damage = 0; damage < 6; ++damage) {
    auto bytes = valid;
    if (damage == 0) put(bytes, 5, 19); // The old reader incorrectly allowed four extra bytes.
    if (damage == 1) put(bytes, 14, 0xffffffff);
    if (damage == 2) put(bytes, 18, 0xffffffff);
    if (damage == 3) put(bytes, 29, 18); // Payload cannot overlap the index.
    if (damage == 4) put(bytes, 9, 99);
    if (damage == 5) bytes.resize(22);
    rejects([&] { wedecode::Wxapkg::read_file_index(bytes); }, "malformed index accepted");
  }
  rejects([&] { wedecode::Wxapkg::decrypt("wx0123456789abcdef", valid); }, "unknown encryption header accepted");
  const auto fixture = fs::path(__FILEW__).parent_path() / "fixtures" / "encrypted.wxapkg";
  rejects([&] { wedecode::Wxapkg::decrypt("wx0000000000000000", wedecode::read_binary(fixture)); }, "wrong WXID accepted");
  check(!wedecode::is_safe_relative_path(std::string("bad\0name", 8)), "NUL path accepted");
  check(!wedecode::is_safe_relative_path("pages/CON.js"), "reserved device path accepted");
  check(!wedecode::is_safe_relative_path("pages/foo. /index.js"), "Windows path alias accepted");
  auto damaged = valid; put(damaged, 14, 0xffffffff);
  wedecode::write_binary(root / "bad.wxapkg", damaged);
  rejects([&] { wedecode::Wxapkg::unpack_file(root / "bad.wxapkg", "", root / "bad-output", wedecode::Logger{}); }, "damaged package extracted");
  check(!fs::exists(root / "bad-output"), "damaged index wrote partial files");
}

void test_source_recovery(const fs::path& root) {
  wedecode::write_text_utf8(root / "app-service.js", R"JS(
// define("fake-comment.js",function(){bad()});
var decoy = "define(\"fake-string.js\",function(){bad()});";
define /* space and comment */ ("pages/home.js", function named(require,module,exports) {
  var braces = /[{}]/g; var slash = /\//; var division = (8 / 2) / 2;
  if (division) /}/.test("}");
  var template = `a${({x:`b${"}"}`}).x}c`;
  return { braces: braces, template: template };
});
define("amd.js",[],function(require,module,exports){module.exports=42;});
define("preserved.js",function(){thisMustNotOverwriteRawSource();});
define("plugin-private://wx0123456789abcdef/index.js",function(require,module,exports){module.exports=1;});
__wxAppCode__['pages/home.json']={"navigationBarTitleText":"\u6d4b\u8bd5\ud83d\ude00","usingComponents":{"card":"/components/card"}};
__wxAppCode__['components/card.json']={"component":true,"usingComponents":{}};
)JS");
  wedecode::write_text_utf8(root / "preserved.js", "original source\n");
  wedecode::write_text_utf8(root / "app-wxss.js", R"JS(
__COMMON_STYLESHEETS__['./common.wxss']=[".common{margin:",[0,12],";}\n",];
__wxAppCode__['pages/home.wxss']=setCssToHead([[2,"./common.wxss"],".",[1],"card{width:",[0,20],";content:\x22ok\x22;}\n",]);
setCssToHead(["page{padding:",[0,-3.5],";}\n"],"",{path:"./app.wxss"})();
)JS");
  wedecode::write_text_utf8(root / "app-config.json", R"JSON({
"pages":["pages/home.html","extra/detail.html"],"entryPagePath":"pages/home.html",
"global":{"window":{"navigationBarTitleText":"Original"}},
"page":{"pages/home.html":{"window":{"navigationStyle":"custom"}}},
"subPackages":[{"root":"extra/","pages":["detail.html"],"independent":true,"plugins":{"p":{"provider":"wx0123456789abcdef","version":"1.0"}}}],
"plugins":{"p":{"provider":"wx0123456789abcdef","version":"1.0"}},
"navigateToMiniProgramAppIdList":["wx0123456789abcdef"]})JSON");
  const auto original_bundle = wedecode::read_text_utf8(root / "app-service.js");
  const auto result = wedecode::SourceReconstructor(wedecode::Logger{}).reconstruct(root);
  check(result.javascript_modules == 3, "define modules were lost or false positives extracted");
  const auto module = wedecode::read_text_utf8(root / "pages/home.js");
  check(module.find("return { braces:") != std::string::npos && module.find("a${") != std::string::npos, "regex/template truncated factory body");
  check(!fs::exists(root / "fake-comment.js") && !fs::exists(root / "fake-string.js"), "fake define was extracted");
  check(wedecode::read_text_utf8(root / "preserved.js") == "original source\n", "raw source overwritten");
  check(fs::exists(root / "__plugin__/wx0123456789abcdef/index.js"), "plugin paths diverged from assets");
  const auto page = wedecode::read_text_utf8(root / "pages/home.json");
  check(page.find("测试😀") != std::string::npos && page.find("usingComponents") != std::string::npos, "embedded JSON or Unicode lost");
  check(wedecode::read_text_utf8(root / "pages/home.wxss") == "@import \"../common.wxss\";\n.card{width:20rpx;content:\"ok\";}\n", "WXSS tokens decoded incorrectly");
  check(wedecode::read_text_utf8(root / "common.wxss") == ".common{margin:12rpx;}\n", "shared stylesheet lost");
  check(wedecode::read_text_utf8(root / "app.wxss") == "page{padding:-3.5rpx;}\n", "global stylesheet lost");
  const auto app = wedecode::read_text_utf8(root / "app.json");
  check(app.find("plugins") != std::string::npos && app.find("wx0123456789abcdef") != std::string::npos, "plugin declarations discarded");
  check(app.find("detail.html") == std::string::npos && app.find("detail") != std::string::npos, "subpackage page suffix not normalized");
  check(app.find("navigateToMiniProgramAppIdList") != std::string::npos, "navigation declaration discarded");
  check(!result.complete() && !fs::exists(root / "pages/home.wxml") && !fs::exists(root / "extra/detail.js"), "missing sources replaced with placeholders");
  check(wedecode::read_text_utf8(root / "app-service.js") == original_bundle, "original compiled container modified");
  const auto report = wedecode::read_text_utf8(root / "reconstruction-report.json");
  check(report.find("pages/home.wxml") != std::string::npos && report.find("components/card.wxml") != std::string::npos, "unrestored templates absent from report");
}

void test_game_and_plugin(const fs::path& root) {
  const std::string config = R"({"openDataContext":"open-data","subpackages":[{"root":"data"}]})";
  wedecode::write_text_utf8(root / "game.js", "define('game/module',function(require,module,exports){module.exports=1;});");
  wedecode::write_text_utf8(root / "plugin.js", "define('__plugin__/wx0123456789abcdef/engine.js',function(require,module,exports){module.exports=2;});");
  wedecode::write_text_utf8(root / "game.json", config);
  const auto result = wedecode::SourceReconstructor(wedecode::Logger{}).reconstruct(root);
  check(result.javascript_modules == 2, "game/plugin containers not split");
  check(wedecode::read_text_utf8(root / "game.json") == config, "game configuration modified");
  check(fs::exists(root / "game/module.js"), "extensionless module lost");
}
}  // namespace

int main() {
  const auto root = fs::temp_directory_path() / ("wedecode-recovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    test_package_validation(root / "packages");
    test_source_recovery(root / "program");
    test_game_and_plugin(root / "game");
    fs::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nFixtures retained at " << root << '\n';
    return 1;
  }
}
