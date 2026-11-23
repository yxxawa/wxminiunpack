#pragma once

// A small lexical reader for locating static values in compiled containers.
// It never evaluates package code. Offsets refer to the original bytes.
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wedecode::detail {

inline constexpr std::size_t no_token = std::string_view::npos;

struct JsToken {
  enum class Kind { word, string, literal, punctuation };
  std::size_t begin{}, end{}, match{no_token};
  Kind kind{Kind::punctuation};
  std::string_view text(std::string_view code) const { return code.substr(begin, end - begin); }
};

inline bool identifier_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
}

inline bool regex_after(std::string_view token) {
  return token == "return" || token == "throw" || token == "case" || token == "delete" ||
         token == "void" || token == "typeof" || token == "yield" || token == "await" ||
         token == "in" || token == "of" || token == "else" || token == "do";
}

inline std::size_t skip_literal(std::string_view code, std::size_t start, unsigned depth = 0) {
  if (depth > 128) return code.size();
  const char quote = code[start];
  const bool regex = quote == '/';
  bool character_class{};
  for (std::size_t p = start + 1; p < code.size(); ++p) {
    const char c = code[p];
    if (c == '\\') { ++p; continue; }
    if (regex && c == '[') character_class = true;
    if (regex && c == ']') character_class = false;
    if (quote == '`' && c == '$' && p + 1 < code.size() && code[p + 1] == '{') {
      int braces = 1;
      bool can_regex = true;
      for (p += 2; p < code.size(); ++p) {
        const char inner = code[p];
        if (std::isspace(static_cast<unsigned char>(inner))) continue;
        if (inner == '/' && p + 1 < code.size() && code[p + 1] == '/') {
          p = code.find('\n', p + 2);
          if (p == no_token) return code.size();
          continue;
        }
        if (inner == '/' && p + 1 < code.size() && code[p + 1] == '*') {
          p = code.find("*/", p + 2);
          if (p == no_token) return code.size();
          ++p;
          continue;
        }
        if (inner == '\'' || inner == '"' || inner == '`' || (inner == '/' && can_regex)) {
          p = skip_literal(code, p, depth + 1);
          if (p == code.size()) return p;
          --p;
          can_regex = false;
          continue;
        }
        if (identifier_char(inner)) {
          const auto begin = p;
          while (p + 1 < code.size() && identifier_char(code[p + 1])) ++p;
          can_regex = regex_after(code.substr(begin, p - begin + 1));
          continue;
        }
        if (inner == '{') ++braces;
        if (inner == '}' && --braces == 0) break;
        // Only closing braces change the interpolation nesting.
        if (inner != '}') can_regex = inner != ')' && inner != ']';
      }
      if (p >= code.size()) return code.size();
      continue;
    }
    if (c == quote && !character_class) {
      ++p;
      if (regex) while (p < code.size() && identifier_char(code[p])) ++p;
      return p;
    }
  }
  return code.size();
}

inline std::vector<JsToken> tokenize_js(std::string_view code) {
  std::vector<JsToken> tokens;
  std::vector<std::size_t> brackets;
  bool can_regex = true;
  for (std::size_t p = 0; p < code.size();) {
    const char c = code[p];
    if (std::isspace(static_cast<unsigned char>(c))) { ++p; continue; }
    if (code.substr(p, 2) == "//") {
      const auto end = code.find('\n', p + 2);
      p = end == no_token ? code.size() : end;
      continue;
    }
    if (code.substr(p, 2) == "/*") {
      const auto end = code.find("*/", p + 2);
      p = end == no_token ? code.size() : end + 2;
      continue;
    }
    JsToken token;
    token.begin = p;
    if (c == '\'' || c == '"' || c == '`' || (c == '/' && can_regex)) {
      p = skip_literal(code, p);
      token.kind = c == '\'' || c == '"' ? JsToken::Kind::string : JsToken::Kind::literal;
      can_regex = false;
    } else if (identifier_char(c)) {
      while (p < code.size() && identifier_char(code[p])) ++p;
      token.kind = JsToken::Kind::word;
      can_regex = regex_after(code.substr(token.begin, p - token.begin));
    } else {
      ++p;
      can_regex = c != ')' && c != ']';
      if (c == '(' || c == '[' || c == '{') brackets.push_back(tokens.size());
      else if (c == ')' || c == ']' || c == '}') {
        if (!brackets.empty()) {
          const auto open = brackets.back();
          const auto opening = code[tokens[open].begin];
          if ((c == ')' && opening == '(') || (c == ']' && opening == '[') || (c == '}' && opening == '{')) {
            token.match = open;
            tokens[open].match = tokens.size();
            brackets.pop_back();
            if (c == ')' && open > 0) {
              const auto before = tokens[open - 1].text(code);
              can_regex = before == "if" || before == "while" || before == "for" || before == "with" || before == "switch" || before == "catch";
            }
          }
        }
      }
      if ((c == '+' || c == '-') && p < code.size() && code[p] == c) { ++p; can_regex = false; }
    }
    token.end = p;
    tokens.push_back(token);
  }
  return tokens;
}

inline void append_utf8(std::string& out, unsigned value) {
  if (value <= 0x7f) out.push_back(static_cast<char>(value));
  else if (value <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (value >> 6)));
    out.push_back(static_cast<char>(0x80 | (value & 63)));
  } else if (value <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (value >> 12)));
    out.push_back(static_cast<char>(0x80 | ((value >> 6) & 63)));
    out.push_back(static_cast<char>(0x80 | (value & 63)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (value >> 18)));
    out.push_back(static_cast<char>(0x80 | ((value >> 12) & 63)));
    out.push_back(static_cast<char>(0x80 | ((value >> 6) & 63)));
    out.push_back(static_cast<char>(0x80 | (value & 63)));
  }
}

inline std::optional<std::string> decode_js_string(std::string_view raw) {
  if (raw.size() < 2 || (raw.front() != '\'' && raw.front() != '"') || raw.back() != raw.front()) return std::nullopt;
  std::string out;
  unsigned high_surrogate{};
  for (std::size_t p = 1; p + 1 < raw.size(); ++p) {
    if (raw[p] != '\\') { out.push_back(raw[p]); continue; }
    if (++p + 1 >= raw.size()) return std::nullopt;
    const char escape = raw[p];
    switch (escape) {
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'v': out.push_back('\v'); break;
      case '0': out.push_back('\0'); break;
      case '\r': if (p + 1 < raw.size() && raw[p + 1] == '\n') ++p; break;
      case '\n': break;
      case 'x': case 'u': {
        const auto length = escape == 'x' ? 2U : 4U;
        unsigned value{};
        for (unsigned digit = 0; digit < length; ++digit) {
          if (++p + 1 >= raw.size()) return std::nullopt;
          const auto hex = raw[p];
          value <<= 4;
          if (hex >= '0' && hex <= '9') value += hex - '0';
          else if (hex >= 'a' && hex <= 'f') value += hex - 'a' + 10;
          else if (hex >= 'A' && hex <= 'F') value += hex - 'A' + 10;
          else return std::nullopt;
        }
        if (value >= 0xd800 && value <= 0xdbff) {
          if (high_surrogate || raw.substr(p + 1, 2) != "\\u") return std::nullopt;
          high_surrogate = value;
          break;
        }
        if (value >= 0xdc00 && value <= 0xdfff && high_surrogate) {
          value = 0x10000 + ((high_surrogate - 0xd800) << 10) + value - 0xdc00;
          high_surrogate = 0;
        } else if (high_surrogate || (value >= 0xdc00 && value <= 0xdfff)) return std::nullopt;
        append_utf8(out, value);
        break;
      }
      default: out.push_back(escape); break;
    }
  }
  if (high_surrogate) return std::nullopt;
  return out;
}

}  // namespace wedecode::detail
