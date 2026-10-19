// Tiny JSON reader/writer: enough for the settings file and the mpv IPC lines.
#pragma once
#include "common.h"

struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
  bool b = false;
  double n = 0;
  string s;
  vector<Json> a;
  vector<std::pair<string, Json>> o;

  static Json boolean(bool v) { Json j; j.t = Bool; j.b = v; return j; }
  static Json num(double v) { Json j; j.t = Num; j.n = v; return j; }
  static Json str(const string& v) { Json j; j.t = Str; j.s = v; return j; }
  static Json arr() { Json j; j.t = Arr; return j; }
  static Json obj() { Json j; j.t = Obj; return j; }

  bool isObj() const { return t == Obj; }
  bool isArr() const { return t == Arr; }
  bool isStr() const { return t == Str; }
  bool isNum() const { return t == Num; }

  const Json* get(const string& k) const {
    if (t != Obj) return nullptr;
    for (auto& kv : o) if (kv.first == k) return &kv.second;
    return nullptr;
  }
  Json& set(const string& k, Json v) {
    if (t != Obj) { *this = obj(); }
    for (auto& kv : o) if (kv.first == k) { kv.second = std::move(v); return kv.second; }
    o.emplace_back(k, std::move(v));
    return o.back().second;
  }
  void push(Json v) { if (t != Arr) *this = arr(); a.push_back(std::move(v)); }

  double numOr(double d) const { return t == Num ? n : d; }
  string strOr(const string& d) const { return t == Str ? s : d; }
  bool boolOr(bool d) const { return t == Bool ? b : d; }
};

namespace jsonimpl {
struct P {
  const string& s;
  size_t i = 0;
  bool ok = true;
  explicit P(const string& str) : s(str) {}
  void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
  bool lit(const char* w) {
    size_t n = strlen(w);
    if (s.compare(i, n, w) == 0) { i += n; return true; }
    return false;
  }
  static void putUtf8(string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
  }
  uint32_t hex4() {
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
      if (i >= s.size()) { ok = false; return 0; }
      char c = s[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
      else { ok = false; return 0; }
    }
    return v;
  }
  string str() {
    string out;
    if (s[i] != '"') { ok = false; return out; }
    i++;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') { out += c; continue; }
      if (i >= s.size()) { ok = false; return out; }
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp = hex4();
          if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
            i += 2;
            uint32_t lo = hex4();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          putUtf8(out, cp);
          break;
        }
        default: ok = false; return out;
      }
    }
    if (i >= s.size()) { ok = false; return out; }
    i++;
    return out;
  }
  Json val(int depth = 0) {
    ws();
    if (i >= s.size() || depth > 64) { ok = false; return Json(); }
    char c = s[i];
    if (c == '{') {
      i++;
      Json j = Json::obj();
      ws();
      if (i < s.size() && s[i] == '}') { i++; return j; }
      while (ok) {
        ws();
        if (i >= s.size() || s[i] != '"') { ok = false; break; }
        string k = str();
        ws();
        if (i >= s.size() || s[i] != ':') { ok = false; break; }
        i++;
        Json v = val(depth + 1);
        if (!ok) break;
        j.o.emplace_back(std::move(k), std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == '}') { i++; break; }
        ok = false;
      }
      return j;
    }
    if (c == '[') {
      i++;
      Json j = Json::arr();
      ws();
      if (i < s.size() && s[i] == ']') { i++; return j; }
      while (ok) {
        Json v = val(depth + 1);
        if (!ok) break;
        j.a.push_back(std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == ']') { i++; break; }
        ok = false;
      }
      return j;
    }
    if (c == '"') return Json::str(str());
    if (lit("true")) return Json::boolean(true);
    if (lit("false")) return Json::boolean(false);
    if (lit("null")) return Json();
    char* end = nullptr;
    double d = strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i) { ok = false; return Json(); }
    i = (size_t)(end - s.c_str());
    return Json::num(d);
  }
};
}  // namespace jsonimpl

inline Json parseJson(const string& text, bool* ok = nullptr) {
  jsonimpl::P p(text);
  Json j = p.val();
  p.ws();
  bool good = p.ok && p.i == text.size();
  if (ok) *ok = good;
  return good ? j : Json();
}

inline void jsonEscape(string& out, const string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char)c;
    }
  }
  out += '"';
}

inline void jsonDump(const Json& j, string& out, int indent = -1, int level = 0) {
  auto nl = [&](int lv) { if (indent >= 0) { out += '\n'; out.append((size_t)(lv * indent), ' '); } };
  switch (j.t) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Num: {
      char b[48];
      if (j.n == std::floor(j.n) && std::fabs(j.n) < 1e15) snprintf(b, sizeof b, "%.0f", j.n);
      else snprintf(b, sizeof b, "%.6g", j.n);
      out += b;
      break;
    }
    case Json::Str: jsonEscape(out, j.s); break;
    case Json::Arr:
      if (j.a.empty()) { out += "[]"; break; }
      out += '[';
      for (size_t k = 0; k < j.a.size(); k++) {
        if (k) out += ',';
        nl(level + 1);
        jsonDump(j.a[k], out, indent, level + 1);
      }
      nl(level);
      out += ']';
      break;
    case Json::Obj:
      if (j.o.empty()) { out += "{}"; break; }
      out += '{';
      for (size_t k = 0; k < j.o.size(); k++) {
        if (k) out += ',';
        nl(level + 1);
        jsonEscape(out, j.o[k].first);
        out += indent >= 0 ? ": " : ":";
        jsonDump(j.o[k].second, out, indent, level + 1);
      }
      nl(level);
      out += '}';
      break;
  }
}

inline string jsonString(const Json& j, int indent = -1) {
  string s;
  jsonDump(j, s, indent);
  return s;
}
