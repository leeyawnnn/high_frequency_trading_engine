// Minimal JSON config loader.
//
// The engine is configured from a small JSON file (core pinning, ports, risk
// limits). Rather than pull in a JSON dependency for a handful of values, this
// is a compact recursive-descent parser covering the JSON we actually use:
// objects, arrays, strings (with basic escapes), numbers, booleans, null.
//
// This is STARTUP-ONLY code — it allocates and throws freely. None of it runs
// on the hot path. Values are read by dotted path, e.g.
// cfg.get_int("cores.feed_handler", -1).
#pragma once

#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace hft {

class JsonValue {
 public:
  using Object = std::map<std::string, JsonValue>;
  using Array = std::vector<JsonValue>;
  using Storage =
      std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  JsonValue() : v_(nullptr) {}
  explicit JsonValue(Storage v) : v_(std::move(v)) {}

  bool is_object() const { return std::holds_alternative<Object>(v_); }
  bool is_number() const { return std::holds_alternative<double>(v_); }
  bool is_string() const { return std::holds_alternative<std::string>(v_); }
  bool is_bool() const { return std::holds_alternative<bool>(v_); }
  bool is_null() const { return std::holds_alternative<std::nullptr_t>(v_); }

  const Object& as_object() const { return std::get<Object>(v_); }
  double as_number() const { return std::get<double>(v_); }
  const std::string& as_string() const { return std::get<std::string>(v_); }
  bool as_bool() const { return std::get<bool>(v_); }

  // Find a child by dotted path; returns nullptr if any segment is missing or
  // a non-object is traversed.
  const JsonValue* find(const std::string& dotted) const {
    const JsonValue* cur = this;
    std::size_t start = 0;
    while (start <= dotted.size()) {
      const std::size_t dot = dotted.find('.', start);
      const std::string key = dotted.substr(
          start, dot == std::string::npos ? std::string::npos : dot - start);
      if (!cur->is_object()) return nullptr;
      const Object& o = cur->as_object();
      auto it = o.find(key);
      if (it == o.end()) return nullptr;
      cur = &it->second;
      if (dot == std::string::npos) break;
      start = dot + 1;
    }
    return cur;
  }

 private:
  Storage v_;
};

class Config {
 public:
  static Config parse(const std::string& text) {
    Parser p(text);
    Config c;
    c.root_ = p.parse_document();
    return c;
  }

  static Config load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("config: cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
  }

  const JsonValue& root() const { return root_; }

  long get_int(const std::string& path, long def) const {
    const JsonValue* v = root_.find(path);
    return (v && v->is_number()) ? static_cast<long>(v->as_number()) : def;
  }
  double get_double(const std::string& path, double def) const {
    const JsonValue* v = root_.find(path);
    return (v && v->is_number()) ? v->as_number() : def;
  }
  bool get_bool(const std::string& path, bool def) const {
    const JsonValue* v = root_.find(path);
    return (v && v->is_bool()) ? v->as_bool() : def;
  }
  std::string get_string(const std::string& path, const std::string& def) const {
    const JsonValue* v = root_.find(path);
    return (v && v->is_string()) ? v->as_string() : def;
  }

 private:
  JsonValue root_;

  // --- recursive-descent parser ------------------------------------------
  class Parser {
   public:
    explicit Parser(const std::string& s) : s_(s) {}

    JsonValue parse_document() {
      skip_ws();
      JsonValue v = parse_value();
      skip_ws();
      if (i_ != s_.size()) fail("trailing characters after JSON document");
      return v;
    }

   private:
    const std::string& s_;
    std::size_t i_ = 0;

    [[noreturn]] void fail(const std::string& msg) const {
      throw std::runtime_error("config JSON parse error at offset " +
                               std::to_string(i_) + ": " + msg);
    }

    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    char get() { return i_ < s_.size() ? s_[i_++] : '\0'; }

    void skip_ws() {
      while (i_ < s_.size()) {
        const char c = s_[i_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
          ++i_;
        else
          break;
      }
    }

    JsonValue parse_value() {
      skip_ws();
      switch (peek()) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return JsonValue(JsonValue::Storage(parse_string()));
        case 't': case 'f': return parse_bool();
        case 'n': return parse_null();
        default: return parse_number();
      }
    }

    JsonValue parse_object() {
      JsonValue::Object obj;
      get();  // consume '{'
      skip_ws();
      if (peek() == '}') { get(); return JsonValue(JsonValue::Storage(std::move(obj))); }
      for (;;) {
        skip_ws();
        if (peek() != '"') fail("expected string key in object");
        std::string key = parse_string();
        skip_ws();
        if (get() != ':') fail("expected ':' after key");
        obj.emplace(std::move(key), parse_value());
        skip_ws();
        const char c = get();
        if (c == ',') continue;
        if (c == '}') break;
        fail("expected ',' or '}' in object");
      }
      return JsonValue(JsonValue::Storage(std::move(obj)));
    }

    JsonValue parse_array() {
      JsonValue::Array arr;
      get();  // consume '['
      skip_ws();
      if (peek() == ']') { get(); return JsonValue(JsonValue::Storage(std::move(arr))); }
      for (;;) {
        arr.push_back(parse_value());
        skip_ws();
        const char c = get();
        if (c == ',') continue;
        if (c == ']') break;
        fail("expected ',' or ']' in array");
      }
      return JsonValue(JsonValue::Storage(std::move(arr)));
    }

    std::string parse_string() {
      if (get() != '"') fail("expected '\"'");
      std::string out;
      for (;;) {
        const char c = get();
        if (c == '\0') fail("unterminated string");
        if (c == '"') break;
        if (c == '\\') {
          const char e = get();
          switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            default: out.push_back(e); break;  // includes \uXXXX passthrough-ish
          }
        } else {
          out.push_back(c);
        }
      }
      return out;
    }

    JsonValue parse_bool() {
      if (s_.compare(i_, 4, "true") == 0) { i_ += 4; return JsonValue(JsonValue::Storage(true)); }
      if (s_.compare(i_, 5, "false") == 0) { i_ += 5; return JsonValue(JsonValue::Storage(false)); }
      fail("invalid literal");
    }

    JsonValue parse_null() {
      if (s_.compare(i_, 4, "null") == 0) { i_ += 4; return JsonValue(); }
      fail("invalid literal");
    }

    JsonValue parse_number() {
      const std::size_t start = i_;
      if (peek() == '-' || peek() == '+') get();
      bool any = false;
      while (i_ < s_.size()) {
        const char c = s_[i_];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            c == '+' || c == '-') {
          ++i_;
          any = true;
        } else {
          break;
        }
      }
      if (!any) fail("invalid number");
      return JsonValue(JsonValue::Storage(std::stod(s_.substr(start, i_ - start))));
    }
  };
};

}  // namespace hft
