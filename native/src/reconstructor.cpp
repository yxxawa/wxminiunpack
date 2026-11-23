#include "wedecode/reconstructor.h"

#include "wedecode/common.h"
#include "js_reader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace wedecode {
namespace {

struct JsonValue {
  enum class Type { null_value, boolean, number, string, array, object };
  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue, std::less<>>;

  Type type{Type::null_value};
  bool boolean{};
  double number{};
  std::string string;
  Array array;
  Object object;

  static JsonValue make_object() { JsonValue value; value.type = Type::object; return value; }
  static JsonValue make_array() { JsonValue value; value.type = Type::array; return value; }
  static JsonValue make_string(std::string text) { JsonValue value; value.type = Type::string; value.string = std::move(text); return value; }
  bool is_object() const { return type == Type::object; }
  bool is_array() const { return type == Type::array; }
  bool is_string() const { return type == Type::string; }
};

class JsonParser {
public:
  explicit JsonParser(std::string_view input) : input_(input) {
    if (input_.starts_with("\xef\xbb\xbf")) position_ = 3;
  }

  JsonValue parse() {
    auto value = parse_value();
    skip_space();
    if (position_ != input_.size()) throw std::runtime_error("Unexpected data after JSON document");
    return value;
  }

private:
  std::string_view input_;
  std::size_t position_{};

  void skip_space() {
    while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
  }

  char take() {
    if (position_ >= input_.size()) throw std::runtime_error("Unexpected end of JSON document");
    return input_[position_++];
  }

  bool consume(std::string_view literal) {
    if (input_.substr(position_, literal.size()) != literal) return false;
    position_ += literal.size();
    return true;
  }

  JsonValue parse_value() {
    skip_space();
    if (position_ >= input_.size()) throw std::runtime_error("Expected JSON value");
    const char current = input_[position_];
    if (current == '{') return parse_object();
    if (current == '[') return parse_array();
    if (current == '"') return JsonValue::make_string(parse_string());
    if (current == 't' && consume("true")) { JsonValue value; value.type = JsonValue::Type::boolean; value.boolean = true; return value; }
    if (current == 'f' && consume("false")) { JsonValue value; value.type = JsonValue::Type::boolean; return value; }
    if (current == 'n' && consume("null")) return {};
    return parse_number();
  }

  JsonValue parse_object() {
    JsonValue result = JsonValue::make_object();
    take();
    skip_space();
    if (position_ < input_.size() && input_[position_] == '}') { ++position_; return result; }
    for (;;) {
      skip_space();
      if (take() != '"') throw std::runtime_error("Expected JSON object key");
      --position_;
      auto key = parse_string();
      skip_space();
      if (take() != ':') throw std::runtime_error("Expected ':' after JSON object key");
      result.object.emplace(std::move(key), parse_value());
      skip_space();
      const char separator = take();
      if (separator == '}') break;
      if (separator != ',') throw std::runtime_error("Expected ',' or '}' in JSON object");
    }
    return result;
  }

  JsonValue parse_array() {
    JsonValue result = JsonValue::make_array();
    take();
    skip_space();
    if (position_ < input_.size() && input_[position_] == ']') { ++position_; return result; }
    for (;;) {
      result.array.push_back(parse_value());
      skip_space();
      const char separator = take();
      if (separator == ']') break;
      if (separator != ',') throw std::runtime_error("Expected ',' or ']' in JSON array");
    }
    return result;
  }

  std::string parse_string() {
    if (take() != '"') throw std::runtime_error("Expected JSON string");
    const auto start = position_ - 1;
    while (position_ < input_.size()) {
      const char current = take();
      if (current == '"') {
        const auto decoded = detail::decode_js_string(input_.substr(start, position_ - start));
        if (!decoded) throw std::runtime_error("Invalid JSON unicode escape");
        return *decoded;
      }
      if (static_cast<unsigned char>(current) < 0x20) throw std::runtime_error("Control character in JSON string");
      if (current != '\\') continue;
      const char escape = take();
      if (std::string_view("\"\\/bfnrtu").find(escape) == std::string_view::npos) throw std::runtime_error("Invalid JSON escape");
    }
    throw std::runtime_error("Unterminated JSON string");
  }

  JsonValue parse_number() {
    const std::size_t start = position_;
    if (input_[position_] == '-') ++position_;
    while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
    }
    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
    }
    if (start == position_) throw std::runtime_error("Invalid JSON number");
    JsonValue result;
    result.type = JsonValue::Type::number;
    result.number = std::stod(std::string(input_.substr(start, position_ - start)));
    return result;
  }
};

void append_json(const JsonValue& value, std::ostringstream& out, const int indent, const int level) {
  const auto padding = [&](int depth) { return std::string(static_cast<std::size_t>(depth * indent), ' '); };
  switch (value.type) {
    case JsonValue::Type::null_value: out << "null"; break;
    case JsonValue::Type::boolean: out << (value.boolean ? "true" : "false"); break;
    case JsonValue::Type::number: {
      std::ostringstream number;
      number.precision(15);
      number << value.number;
      out << number.str();
      break;
    }
    case JsonValue::Type::string: out << '"' << json_escape(value.string) << '"'; break;
    case JsonValue::Type::array: {
      if (value.array.empty()) { out << "[]"; break; }
      out << "[\n";
      for (std::size_t index = 0; index < value.array.size(); ++index) {
        out << padding(level + 1);
        append_json(value.array[index], out, indent, level + 1);
        out << (index + 1 == value.array.size() ? "\n" : ",\n");
      }
      out << padding(level) << ']';
      break;
    }
    case JsonValue::Type::object: {
      if (value.object.empty()) { out << "{}"; break; }
      out << "{\n";
      std::size_t index{};
      for (const auto& [key, child] : value.object) {
        out << padding(level + 1) << '"' << json_escape(key) << "\": ";
        append_json(child, out, indent, level + 1);
        out << (++index == value.object.size() ? "\n" : ",\n");
      }
      out << padding(level) << '}';
      break;
    }
  }
}

std::string stringify_json(const JsonValue& value) {
  std::ostringstream out;
  append_json(value, out, 2, 0);
  return out.str() + "\n";
}

JsonValue* member(JsonValue& value, std::string_view key) {
  if (!value.is_object()) return nullptr;
  const auto found = value.object.find(key);
  return found == value.object.end() ? nullptr : &found->second;
}

const JsonValue* member(const JsonValue& value, std::string_view key) {
  if (!value.is_object()) return nullptr;
  const auto found = value.object.find(key);
  return found == value.object.end() ? nullptr : &found->second;
}

std::vector<std::string> string_array(const JsonValue* value) {
  std::vector<std::string> result;
  if (!value || !value->is_array()) return result;
  for (const auto& item : value->array) if (item.is_string()) result.push_back(item.string);
  return result;
}

std::string remove_html_suffix(std::string path) {
  constexpr std::string_view suffix = ".html";
  if (path.size() >= suffix.size() && path.ends_with(suffix)) path.resize(path.size() - suffix.size());
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  return path;
}

std::string sanitise_module_path(std::string path) {
  if (path.starts_with("plugin-private://")) path = "__plugin__/" + path.substr(std::string("plugin-private://").size());
  return normalise_relative_path(path);
}

std::optional<std::string> decode_stylesheet(const std::string& code,
                                            const std::vector<detail::JsToken>& tokens,
                                            std::size_t start, const std::string& filename) {
  const auto end = tokens[start].match;
  if (end == detail::no_token) return std::nullopt;
  std::string css;
  for (auto i = start + 1; i < end; ++i) {
    const auto text = tokens[i].text(code);
    if (text == ",") continue;
    if (tokens[i].kind == detail::JsToken::Kind::string) {
      const auto value = detail::decode_js_string(text);
      if (!value) return std::nullopt;
      css += *value;
      continue;
    }
    if (text != "[" || tokens[i].match == detail::no_token) return std::nullopt;
    const auto close = tokens[i].match;
    if (close <= i + 1) return std::nullopt;
    const auto operation = tokens[i + 1].text(code);
    if (operation == "0" && close > i + 3 && tokens[i + 2].text(code) == ",") {
      auto number = trim(std::string_view(code).substr(tokens[i + 3].begin, tokens[close].begin - tokens[i + 3].begin));
      std::size_t consumed{};
      try { const auto value = std::stod(number, &consumed); (void)value; } catch (...) { return std::nullopt; }
      if (consumed != number.size()) return std::nullopt;
      css += number + "rpx";
    } else if (operation == "1" && close == i + 2) {
      // Component scope prefixes are generated by the runtime, not source WXSS.
    } else if (operation == "2" && close == i + 4 && tokens[i + 2].text(code) == ",") {
      auto imported = detail::decode_js_string(tokens[i + 3].text(code));
      if (!imported) return std::nullopt;
      auto path = sanitise_module_path(*imported);
      if (!is_safe_relative_path(path)) return std::nullopt;
      const auto relative = path_from_utf8(path).lexically_normal().lexically_relative(path_from_utf8(filename).parent_path());
      css += "@import \"" + wide_to_utf8(relative.generic_wstring()) + "\";\n";
    } else return std::nullopt;
    i = close;
  }
  return css;
}

}  // namespace

SourceReconstructor::SourceReconstructor(Logger logger) : logger_(std::move(logger)) {}

ReconstructionResult SourceReconstructor::reconstruct(const fs::path& project_directory) {
  result_ = {};
  const bool game = fs::exists(project_directory / "game.js");
  reconstruct_define_modules(project_directory);
  if (!game) reconstruct_app_config(project_directory);
  if (!game) inspect_missing_page_files(project_directory);
  create_project_config(project_directory);
  JsonValue report = JsonValue::make_object();
  const auto number = [](std::size_t count) { JsonValue value; value.type = JsonValue::Type::number; value.number = static_cast<double>(count); return value; };
  report.object["javascriptModules"] = number(result_.javascript_modules);
  report.object["jsonFiles"] = number(result_.json_files);
  report.object["stylesheets"] = number(result_.stylesheets);
  report.object["status"] = JsonValue::make_string(result_.complete() ? "available-content-recovered" : "partial");
  report.object["templateRecovery"] = JsonValue::make_string("Compiled WXML is preserved in the original containers; missing source files are not replaced with placeholders.");
  for (const auto& [key, values] : std::map<std::string, std::vector<std::string>>{
           {"missingSourceFiles", result_.missing_source_files}, {"warnings", result_.warnings}}) {
    auto list = JsonValue::make_array();
    for (const auto& value : values) list.array.push_back(JsonValue::make_string(value));
    report.object[key] = std::move(list);
  }
  write_text_utf8(project_directory / "reconstruction-report.json", stringify_json(report));
  if (result_.complete()) logger_.success("Recovered available source content: " + wide_to_utf8(project_directory.filename().wstring()));
  else logger_.warning("Source recovery is partial; see reconstruction-report.json (missing files: " + std::to_string(result_.missing_source_files.size()) + ")");
  return result_;
}

void SourceReconstructor::reconstruct_app_config(const fs::path& project_directory) {
  // An embedded source app.json is more authoritative than the runtime config.
  if (fs::exists(project_directory / "app.json")) return;
  const auto source = project_directory / "app-config.json";
  if (!fs::exists(source)) {
    logger_.warning("app-config.json was not found; page defaults are skipped");
    if (fs::exists(project_directory / "app-service.js")) result_.warnings.push_back("No app.json or app-config.json was available");
    return;
  }
  try {
    auto config = JsonParser(read_text_utf8(source)).parse();
    if (!config.is_object()) throw std::runtime_error("app-config.json root must be an object");

    if (const auto global = member(config, "global"); global && global->is_object()) {
      for (const auto& [key, value] : global->object) config.object[key] = value;
    }
    config.object.erase("global");
    if (const auto pages = member(config, "page"); pages && pages->is_object()) {
      for (const auto& [name, metadata] : pages->object) {
        const auto path = sanitise_module_path(remove_html_suffix(name) + ".json");
        if (!is_safe_relative_path(path) || !metadata.is_object()) continue;
        auto page = metadata;
        if (const auto window = member(metadata, "window"); window && window->is_object()) {
          page.object.erase("window");
          for (const auto& [key, value] : window->object) page.object[key] = value;
        }
        if (!fs::exists(project_directory / path_from_utf8(path))) {
          write_text_utf8(project_directory / path_from_utf8(path), stringify_json(page), false);
          ++result_.json_files;
        }
      }
    }
    config.object.erase("page");

    if (auto entry = member(config, "entryPagePath"); entry && entry->is_string()) entry->string = remove_html_suffix(entry->string);
    if (auto renderer = member(config, "renderer")) {
      if (renderer->is_object()) {
        if (const auto default_renderer = member(*renderer, "default"); default_renderer && default_renderer->is_string()) *renderer = JsonValue::make_string(default_renderer->string);
        else *renderer = JsonValue::make_string("webview");
      }
    }

    if (const auto ext_appid = member(config, "extAppid"); ext_appid && ext_appid->is_string()) {
      JsonValue ext = JsonValue::make_object();
      JsonValue enabled; enabled.type = JsonValue::Type::boolean; enabled.boolean = true;
      ext.object["extEnable"] = enabled;
      ext.object["extAppid"] = *ext_appid;
      if (const auto ext_data = member(config, "ext")) ext.object["ext"] = *ext_data;
      write_text_utf8(project_directory / "ext.json", stringify_json(ext));
    }
    config.object.erase("ext");
    if (auto framework = member(config, "componentFramework"); framework && framework->is_object()) {
      if (const auto default_value = member(*framework, "default")) { auto value = *default_value; *framework = std::move(value); }
      else if (const auto all_used = member(*framework, "allUsed"); all_used && all_used->is_array() && !all_used->array.empty()) { auto value = all_used->array.front(); *framework = std::move(value); }
    }

    auto all_pages = string_array(member(config, "pages"));
    for (auto& page : all_pages) page = remove_html_suffix(page);
    std::set<std::string> main_pages(all_pages.begin(), all_pages.end());
    if (!member(config, "subPackages")) {
      if (const auto sub = member(config, "subpackages")) {
        config.object["subPackages"] = *sub;
        config.object.erase("subpackages");
      }
    }
    if (auto sub_packages = member(config, "subPackages"); sub_packages && sub_packages->is_array()) {
      JsonValue::Array normalised;
      for (auto sub_package : sub_packages->array) {
        if (!sub_package.is_object()) continue;
        auto root_member = member(sub_package, "root");
        if (!root_member || !root_member->is_string()) continue;
        auto root = normalise_relative_path(root_member->string);
        if (!root.ends_with('/')) root.push_back('/');
        root_member->string = root;
        JsonValue pages = JsonValue::make_array();
        std::set<std::string> sub_pages;
        for (auto page : string_array(member(sub_package, "pages"))) {
          page = remove_html_suffix(page);
          if (page.starts_with(root)) page.erase(0, root.size());
          if (sub_pages.insert(page).second) pages.array.push_back(JsonValue::make_string(page));
          main_pages.erase(root + page);
        }
        for (const auto& page : all_pages) {
          if (page.starts_with(root)) {
            const auto relative = page.substr(root.size());
            if (sub_pages.insert(relative).second) pages.array.push_back(JsonValue::make_string(relative));
            main_pages.erase(page);
          }
        }
        sub_package.object["pages"] = std::move(pages);
        normalised.push_back(std::move(sub_package));
      }
      sub_packages->array = std::move(normalised);
    }
    JsonValue main = JsonValue::make_array();
    for (const auto& page : all_pages) if (main_pages.contains(page)) main.array.push_back(JsonValue::make_string(remove_html_suffix(page)));
    config.object["pages"] = std::move(main);

    write_text_utf8(project_directory / "app.json", stringify_json(config));
    logger_.success("Generated app.json from app-config.json");
  } catch (const std::exception& error) {
    result_.warnings.push_back(std::string("Could not transform app-config.json: ") + error.what());
    logger_.warning(std::string("Could not transform app-config.json: ") + error.what());
  }
}

void SourceReconstructor::reconstruct_define_modules(const fs::path& project_directory) {
  std::set<fs::path> source_files;
  for (fs::recursive_directory_iterator it(project_directory, fs::directory_options::skip_permission_denied), end; it != end; ++it) {
    const auto& item = *it;
    if (item.is_directory()) {
      const auto name = wide_to_utf8(item.path().filename().wstring());
      if (name == "runtime-cache" || name == "_历史版本" || name == "通用插件") it.disable_recursion_pending();
      continue;
    }
    if (!item.is_regular_file()) continue;
    const auto name = to_lower_ascii(wide_to_utf8(item.path().filename().wstring()));
    if (name == "app-service.js" || name == "app-wxss.js" || name == "page-frame.js" ||
        name == "plugin.js" || name == "game.js" || name == "subcontext.js" ||
        name.ends_with(".appservice.js") || name.ends_with(".webview.js") || name.ends_with(".html")) source_files.insert(item.path());
  }

  for (const auto& source : source_files) {
    std::string code;
    try { code = read_text_utf8(source); }
    catch (const std::exception& error) { result_.warnings.push_back(error.what()); continue; }
    if (source.extension() == ".html") {
      std::string scripts;
      const auto lower = to_lower_ascii(code);
      std::size_t position{};
      while ((position = lower.find("<script", position)) != std::string::npos) {
        const auto begin = lower.find('>', position + 7);
        if (begin == std::string::npos) break;
        const auto end = lower.find("</script", begin + 1);
        if (end == std::string::npos) break;
        scripts.append(code, begin + 1, end - begin - 1);
        scripts.push_back('\n');
        position = end + 8;
      }
      code = std::move(scripts);
    }
    const auto tokens = detail::tokenize_js(code);
    const auto is = [&](std::size_t i, std::string_view text) { return i < tokens.size() && tokens[i].text(code) == text; };
    const auto save = [&](std::string path, std::string_view content, std::size_t& counter) {
      path = sanitise_module_path(std::move(path));
      if (!is_safe_relative_path(path)) return;
      const auto output = project_directory / path_from_utf8(path);
      if (fs::exists(output)) return;
      write_text_utf8(output, content, false);
      ++counter;
    };
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      if (is(i, "define") && (i == 0 || (!is(i - 1, ".") && !is(i - 1, "function"))) && is(i + 1, "(") &&
          i + 4 < tokens.size() && tokens[i + 2].kind == detail::JsToken::Kind::string && is(i + 3, ",")) {
        auto factory = i + 4;
        if (is(factory, "[") && tokens[factory].match != detail::no_token && is(tokens[factory].match + 1, ",")) factory = tokens[factory].match + 2;
        if (!is(factory, "function")) continue;
        ++factory;
        if (factory < tokens.size() && tokens[factory].kind == detail::JsToken::Kind::word) ++factory;
        if (!is(factory, "(") || tokens[factory].match == detail::no_token) continue;
        const auto open = tokens[factory].match + 1;
        if (!is(open, "{") || tokens[open].match == detail::no_token) continue;
        const auto close = tokens[open].match;
        const auto module = detail::decode_js_string(tokens[i + 2].text(code));
        if (module) {
          auto path = sanitise_module_path(*module);
          auto extension = path_from_utf8(path).extension();
          if (extension.empty()) { path += ".js"; extension = ".js"; }
          if (extension == ".js" || extension == ".wxs") {
            save(path, std::string_view(code).substr(tokens[open].end, tokens[close].begin - tokens[open].end), result_.javascript_modules);
          }
        }
        i = close;
        continue;
      }
      if ((is(i, "__wxAppCode__") || is(i, "__COMMON_STYLESHEETS__")) && is(i + 1, "[") &&
          i + 5 < tokens.size() && tokens[i + 2].kind == detail::JsToken::Kind::string &&
          is(i + 3, "]") && is(i + 4, "=") && !is(i + 5, "=")) {
        const auto name = detail::decode_js_string(tokens[i + 2].text(code));
        if (!name) continue;
        const auto path = sanitise_module_path(*name);
        const auto value = i + 5;
        if (path.ends_with(".json") && is(value, "{") && tokens[value].match != detail::no_token) {
          const auto close = tokens[value].match;
          try {
            const auto json = JsonParser(std::string_view(code).substr(tokens[value].begin, tokens[close].end - tokens[value].begin)).parse();
            save(path, stringify_json(json), result_.json_files);
          } catch (const std::exception& error) {
            result_.warnings.push_back("Could not recover " + path + ": " + error.what());
          }
          i = close;
        } else if (path.ends_with(".wxss")) {
          const auto array = is(value, "[") ? value : (is(value, "setCssToHead") && is(value + 1, "(") ? value + 2 : detail::no_token);
          if (array != detail::no_token && is(array, "[")) {
            if (const auto css = decode_stylesheet(code, tokens, array, path)) save(path, *css, result_.stylesheets);
            else result_.warnings.push_back("Unsupported stylesheet tokens in " + path);
          }
        }
      }
      // Global WXSS calls identify their source through a {path:"./app.wxss"} argument.
      if (is(i, "setCssToHead") && is(i + 1, "(") && is(i + 2, "[") && tokens[i + 1].match != detail::no_token) {
        const auto end = tokens[i + 1].match;
        const auto array_end = tokens[i + 2].match;
        if (array_end == detail::no_token) continue;
        for (auto p = array_end + 1; p + 2 < end; ++p) {
          if (!is(p, "path") || !is(p + 1, ":")) continue;
          const auto name = detail::decode_js_string(tokens[p + 2].text(code));
          if (!name || !name->ends_with(".wxss")) continue;
          const auto path = sanitise_module_path(*name);
          if (const auto css = decode_stylesheet(code, tokens, i + 2, path)) save(path, *css, result_.stylesheets);
          else result_.warnings.push_back("Unsupported stylesheet tokens in " + path);
          break;
        }
      }
    }
  }
  logger_.info("Recovered " + std::to_string(result_.javascript_modules) + " JS modules, " +
               std::to_string(result_.json_files) + " JSON files and " + std::to_string(result_.stylesheets) + " WXSS files");
}

void SourceReconstructor::inspect_missing_page_files(const fs::path& project_directory) {
  const auto app_json = fs::exists(project_directory / "app.json") ? project_directory / "app.json" : project_directory / "plugin.json";
  std::set<std::string> pages;
  if (fs::exists(app_json)) try {
    const auto config = JsonParser(read_text_utf8(app_json)).parse();
    for (auto page : string_array(member(config, "pages"))) pages.insert(remove_html_suffix(std::move(page)));
    if (const auto exported = member(config, "pages"); exported && exported->is_object()) {
      for (const auto& [name, page] : exported->object) {
        (void)name;
        if (page.is_string()) pages.insert(remove_html_suffix(page.string));
      }
    }
    const auto sub_packages = member(config, "subPackages") ? member(config, "subPackages") : member(config, "subpackages");
    if (sub_packages && sub_packages->is_array()) {
      for (const auto& sub_package : sub_packages->array) {
        const auto root = member(sub_package, "root");
        if (!root || !root->is_string()) continue;
        auto prefix = normalise_relative_path(root->string);
        if (!prefix.ends_with('/')) prefix.push_back('/');
        for (const auto& page : string_array(member(sub_package, "pages"))) pages.insert(remove_html_suffix(prefix + page));
      }
    }
  } catch (const std::exception& error) {
    result_.warnings.push_back(std::string("Could not inspect page configuration: ") + error.what());
  }

  for (const auto& page : pages) {
    if (!is_safe_relative_path(page)) continue;
    for (const auto extension : {".js", ".wxml"}) {
      const auto name = page + extension;
      if (!fs::exists(project_directory / path_from_utf8(name))) result_.missing_source_files.push_back(name);
    }
  }
  // Components recovered from embedded JSON need templates as well.
  for (fs::recursive_directory_iterator it(project_directory, fs::directory_options::skip_permission_denied), end; it != end; ++it) {
    const auto& item = *it;
    if (item.is_directory()) {
      const auto name = wide_to_utf8(item.path().filename().wstring());
      if (name == "runtime-cache" || name == "_历史版本" || name == "通用插件") it.disable_recursion_pending();
      continue;
    }
    if (!item.is_regular_file() || item.path().extension() != ".json") continue;
    try {
      const auto config = JsonParser(read_text_utf8(item.path())).parse();
      const auto component = member(config, "component");
      if (!component || component->type != JsonValue::Type::boolean || !component->boolean) continue;
      auto wxml = item.path(); wxml.replace_extension(".wxml");
      if (!fs::exists(wxml)) result_.missing_source_files.push_back(wide_to_utf8(wxml.lexically_relative(project_directory).generic_wstring()));
    } catch (...) { /* Non-source asset JSON does not affect page recovery. */ }
  }
  std::sort(result_.missing_source_files.begin(), result_.missing_source_files.end());
  result_.missing_source_files.erase(std::unique(result_.missing_source_files.begin(), result_.missing_source_files.end()), result_.missing_source_files.end());
}

void SourceReconstructor::create_project_config(const fs::path& project_directory) {
  const auto config = project_directory / "project.private.config.json";
  if (!fs::exists(config)) {
    write_text_utf8(config, "{\n  \"setting\": {\n    \"es6\": false,\n    \"urlCheck\": false\n  }\n}\n");
  }
}

}  // namespace wedecode

