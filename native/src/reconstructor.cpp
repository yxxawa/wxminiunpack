#include "wedecode/reconstructor.h"

#include "wedecode/common.h"

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
  explicit JsonParser(std::string_view input) : input_(input) {}

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
    std::string result;
    while (position_ < input_.size()) {
      const char current = take();
      if (current == '"') return result;
      if (static_cast<unsigned char>(current) < 0x20) throw std::runtime_error("Control character in JSON string");
      if (current != '\\') { result.push_back(current); continue; }
      const char escape = take();
      switch (escape) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
          if (position_ + 4 > input_.size()) throw std::runtime_error("Short JSON unicode escape");
          unsigned codepoint{};
          for (int index = 0; index < 4; ++index) {
            const char hex = take();
            codepoint <<= 4U;
            if (hex >= '0' && hex <= '9') codepoint |= hex - '0';
            else if (hex >= 'a' && hex <= 'f') codepoint |= hex - 'a' + 10;
            else if (hex >= 'A' && hex <= 'F') codepoint |= hex - 'A' + 10;
            else throw std::runtime_error("Invalid JSON unicode escape");
          }
          if (codepoint <= 0x7f) result.push_back(static_cast<char>(codepoint));
          else if (codepoint <= 0x7ff) {
            result.push_back(static_cast<char>(0xc0 | (codepoint >> 6U)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
          } else {
            result.push_back(static_cast<char>(0xe0 | (codepoint >> 12U)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6U) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
          }
          break;
        }
        default: throw std::runtime_error("Invalid JSON escape");
      }
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
  if (path.starts_with("plugin-private://")) path = "plugin_/" + path.substr(std::string("plugin-private://").size());
  const auto marker = path.find("__plugin__");
  if (marker != std::string::npos) path.replace(marker, std::string("__plugin__").size(), "plugin_");
  return normalise_relative_path(path);
}

std::size_t skip_js_string(const std::string& text, std::size_t position) {
  const char quote = text[position++];
  while (position < text.size()) {
    if (text[position] == '\\') { position += 2; continue; }
    if (text[position++] == quote) break;
  }
  return position;
}

std::optional<std::size_t> matching_js_brace(const std::string& text, std::size_t open) {
  if (open >= text.size() || text[open] != '{') return std::nullopt;
  int depth{};
  for (std::size_t position = open; position < text.size(); ++position) {
    const char current = text[position];
    if (current == '\'' || current == '"' || current == '`') { position = skip_js_string(text, position) - 1; continue; }
    if (current == '/' && position + 1 < text.size() && text[position + 1] == '/') {
      const auto end = text.find('\n', position + 2);
      if (end == std::string::npos) return std::nullopt;
      position = end;
      continue;
    }
    if (current == '/' && position + 1 < text.size() && text[position + 1] == '*') {
      const auto end = text.find("*/", position + 2);
      if (end == std::string::npos) return std::nullopt;
      position = end + 1;
      continue;
    }
    if (current == '{') ++depth;
    else if (current == '}' && --depth == 0) return position;
  }
  return std::nullopt;
}

std::optional<std::pair<std::string, std::size_t>> read_js_quoted(const std::string& code, std::size_t position) {
  while (position < code.size() && std::isspace(static_cast<unsigned char>(code[position]))) ++position;
  if (position >= code.size() || (code[position] != '\'' && code[position] != '"')) return std::nullopt;
  const char quote = code[position++];
  std::string value;
  while (position < code.size()) {
    const char current = code[position++];
    if (current == quote) return std::pair{value, position};
    if (current == '\\' && position < code.size()) value.push_back(code[position++]);
    else value.push_back(current);
  }
  return std::nullopt;
}

}  // namespace

SourceReconstructor::SourceReconstructor(Logger logger) : logger_(std::move(logger)) {}

void SourceReconstructor::reconstruct(const fs::path& project_directory) {
  const bool game = fs::exists(project_directory / "game.js");
  if (game) normalise_game_config(project_directory);
  else reconstruct_app_config(project_directory);
  reconstruct_define_modules(project_directory);
  if (!game) create_missing_page_files(project_directory);
  create_project_config(project_directory);
  logger_.success(std::string(game ? "Mini-game" : "Mini-program") + " reconstruction completed: " + wide_to_utf8(project_directory.filename().wstring()));
}

void SourceReconstructor::reconstruct_app_config(const fs::path& project_directory) {
  const auto source = project_directory / "app-config.json";
  if (!fs::exists(source)) {
    logger_.warning("app-config.json was not found; page defaults are skipped");
    return;
  }
  try {
    auto config = JsonParser(read_text_utf8(source)).parse();
    if (!config.is_object()) throw std::runtime_error("app-config.json root must be an object");

    if (const auto global = member(config, "global"); global && global->is_object()) {
      for (const auto& [key, value] : global->object) config.object[key] = value;
    }
    config.object.erase("global");
    config.object.erase("page");

    if (auto entry = member(config, "entryPagePath"); entry && entry->is_string()) entry->string = remove_html_suffix(entry->string);
    if (auto renderer = member(config, "renderer")) {
      if (renderer->is_object()) {
        if (const auto default_renderer = member(*renderer, "default"); default_renderer && default_renderer->is_string()) *renderer = *default_renderer;
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
    config.object.erase("navigateToMiniProgramAppIdList");

    if (auto plugins = member(config, "plugins")) *plugins = JsonValue::make_object();
    if (auto framework = member(config, "componentFramework"); framework && framework->is_object()) {
      if (const auto default_value = member(*framework, "default")) *framework = *default_value;
      else if (const auto all_used = member(*framework, "allUsed"); all_used && all_used->is_array() && !all_used->array.empty()) *framework = all_used->array.front();
    }

    const auto all_pages = string_array(member(config, "pages"));
    std::set<std::string> main_pages(all_pages.begin(), all_pages.end());
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
        for (const auto& page : all_pages) {
          if (page.starts_with(root)) {
            pages.array.push_back(JsonValue::make_string(page.substr(root.size())));
            main_pages.erase(page);
          }
        }
        sub_package.object["pages"] = std::move(pages);
        if (auto plugins = member(sub_package, "plugins")) *plugins = JsonValue::make_object();
        if (!sub_package.object["pages"].array.empty()) normalised.push_back(std::move(sub_package));
      }
      sub_packages->array = std::move(normalised);
    }
    JsonValue main = JsonValue::make_array();
    for (const auto& page : all_pages) if (main_pages.contains(page)) main.array.push_back(JsonValue::make_string(remove_html_suffix(page)));
    if (!main.array.empty()) config.object["pages"] = std::move(main);

    write_text_utf8(project_directory / "app.json", stringify_json(config));
    logger_.success("Generated app.json from app-config.json");
  } catch (const std::exception& error) {
    logger_.warning(std::string("Could not transform app-config.json: ") + error.what());
  }
}

void SourceReconstructor::reconstruct_define_modules(const fs::path& project_directory) {
  std::size_t recovered{};
  std::set<fs::path> source_files;
  for (const auto& item : fs::recursive_directory_iterator(project_directory, fs::directory_options::skip_permission_denied)) {
    if (!item.is_regular_file()) continue;
    const auto name = to_lower_ascii(wide_to_utf8(item.path().filename().wstring()));
    if (name == "app-service.js" || name.ends_with(".appservice.js") || name.ends_with(".webview.js")) source_files.insert(item.path());
  }

  for (const auto& source : source_files) {
    std::string code;
    try { code = read_text_utf8(source); }
    catch (...) { continue; }
    std::size_t position{};
    while ((position = code.find("define(", position)) != std::string::npos) {
      const auto module = read_js_quoted(code, position + 7);
      if (!module) { position += 7; continue; }
      const auto safe_path = sanitise_module_path(module->first);
      const auto function_position = code.find("function", module->second);
      if (function_position == std::string::npos || function_position > code.find(';', module->second)) { position = module->second; continue; }
      const auto open = code.find('{', function_position);
      const auto close = matching_js_brace(code, open == std::string::npos ? code.size() : open);
      if (!close || !is_safe_relative_path(safe_path)) { position = module->second; continue; }
      if (path_from_utf8(safe_path).extension() == ".js") {
        write_text_utf8(project_directory / path_from_utf8(safe_path), code.substr(open + 1, *close - open - 1));
        ++recovered;
      }
      position = *close + 1;
    }
  }
  if (recovered != 0) logger_.success("Recovered " + std::to_string(recovered) + " JavaScript modules from define() containers");
}

void SourceReconstructor::create_missing_page_files(const fs::path& project_directory) {
  const auto app_json = project_directory / "app.json";
  if (!fs::exists(app_json)) return;
  std::set<std::string> pages;
  try {
    const auto config = JsonParser(read_text_utf8(app_json)).parse();
    for (auto page : string_array(member(config, "pages"))) pages.insert(remove_html_suffix(std::move(page)));
    if (const auto sub_packages = member(config, "subPackages"); sub_packages && sub_packages->is_array()) {
      for (const auto& sub_package : sub_packages->array) {
        const auto root = member(sub_package, "root");
        if (!root || !root->is_string()) continue;
        for (const auto& page : string_array(member(sub_package, "pages"))) pages.insert(normalise_relative_path(root->string + page));
      }
    }
  } catch (...) {
    return;
  }

  for (const auto& page : pages) {
    if (!is_safe_relative_path(page)) continue;
    auto base = project_directory / path_from_utf8(page);
    auto json = base; json.replace_extension(".json");
    auto js = base; js.replace_extension(".js");
    auto wxml = base; wxml.replace_extension(".wxml");
    if (!fs::exists(json)) write_text_utf8(json, "{\n  \"component\": true\n}\n", false);
    if (!fs::exists(js)) write_text_utf8(js, "Page({ data: {} })\n", false);
    if (!fs::exists(wxml)) write_text_utf8(wxml, "<text>" + page + ".wxml</text>\n", false);
  }
  if (!pages.empty()) logger_.info("Ensured source files for " + std::to_string(pages.size()) + " pages/components");
}

void SourceReconstructor::create_project_config(const fs::path& project_directory) {
  const auto config = project_directory / "project.private.config.json";
  if (!fs::exists(config)) {
    write_text_utf8(config, "{\n  \"setting\": {\n    \"es6\": false,\n    \"urlCheck\": false\n  }\n}\n");
  }
}

void SourceReconstructor::normalise_game_config(const fs::path& project_directory) {
  const auto game_json = project_directory / "game.json";
  if (!fs::exists(game_json)) return;
  try {
    auto config = JsonParser(read_text_utf8(game_json)).parse();
    if (config.is_object()) {
      config.object.erase("openDataContext");
      write_text_utf8(game_json, stringify_json(config));
    }
  } catch (const std::exception& error) {
    logger_.warning(std::string("Could not normalise game.json: ") + error.what());
  }
}

}  // namespace wedecode

