#include "resolver.h"

#include "data.h"
#include "json.h"
#include "net.h"

namespace {

// lower case, letters and digits only, single spaces: "Lofi Hip Hop Radio - beats" -> "lofi hip hop radio beats"
wstring normal(const wstring& s) {
  wstring out;
  bool gap = true;
  for (wchar_t c : s) {
    if (std::iswalnum(c)) {
      out += (wchar_t)std::towlower(c);
      gap = false;
    } else if (!gap) {
      out += L' ';
      gap = true;
    }
  }
  while (!out.empty() && out.back() == L' ') out.pop_back();
  return out;
}

bool validId(const string& id) {
  if (id.size() != 11) return false;
  for (char c : id) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
  return true;
}

wstring cachePath() { return appDataDir() + L"\\streams.json"; }

string slurp(const wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  string out;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
  CloseHandle(f);
  return out;
}

void spit(const wstring& path, const string& data) {
  const wstring tmp = path + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD wrote = 0;
  WriteFile(f, data.data(), (DWORD)data.size(), &wrote, nullptr);
  CloseHandle(f);
  MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// cache: {"at": <unix seconds>, "fresh": {"focus": ["id", ...], ...}}
bool loadCache(std::map<string, vector<string>>& out, int64_t* at) {
  bool ok = false;
  const Json j = parseJson(slurp(cachePath()), &ok);
  if (!ok || !j.isObj()) return false;
  const Json* atj = j.get("at");
  *at = atj ? (int64_t)atj->numOr(0) : 0;
  const Json* f = j.get("fresh");
  if (!f || !f->isObj()) return false;
  for (auto& kv : f->o) {
    vector<string> ids;
    if (kv.second.isArr()) for (auto& v : kv.second.a) if (v.isStr() && validId(v.s)) ids.push_back(v.s);
    if (!ids.empty()) out[kv.first] = ids;
  }
  return !out.empty();
}

void saveCache(const std::map<string, vector<string>>& m) {
  Json fresh = Json::obj();
  for (auto& kv : m) {
    Json a = Json::arr();
    for (auto& id : kv.second) a.push(Json::str(id));
    fresh.set(kv.first, a);
  }
  Json j = Json::obj();
  j.set("at", Json::num((double)(wallMs() / 1000)));
  j.set("fresh", fresh);
  spit(cachePath(), jsonString(j, 1));
}

}  // namespace

vector<StreamEntry> parseFlatList(const string& output) {
  vector<StreamEntry> out;
  size_t a = 0;
  while (a < output.size()) {
    size_t b = output.find('\n', a);
    if (b == string::npos) b = output.size();
    string line = output.substr(a, b - a);
    a = b + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const size_t t1 = line.find('\t');
    if (t1 == string::npos) continue;
    const size_t t2 = line.find('\t', t1 + 1);
    if (t2 == string::npos) continue;
    StreamEntry e;
    e.id = line.substr(0, t1);
    e.status = line.substr(t1 + 1, t2 - t1 - 1);
    e.title = widen(line.substr(t2 + 1));
    if (validId(e.id)) out.push_back(std::move(e));
  }
  return out;
}

std::map<string, vector<string>> matchStreams(const vector<StreamEntry>& entries) {
  std::map<string, vector<string>> out;
  vector<std::pair<string, wstring>> live;
  for (auto& e : entries) if (e.status == "is_live") live.push_back({e.id, normal(e.title)});
  for (const SceneDef& sc : stockScenes()) {
    if (sc.hintInclude.empty()) continue;
    vector<string> ids;
    for (const string& hint : sc.hintInclude) {
      const wstring h = normal(widen(hint));
      for (auto& l : live) {
        if (l.second.find(h) == wstring::npos) continue;
        bool bad = false;
        for (const string& x : sc.hintExclude) if (l.second.find(normal(widen(x))) != wstring::npos) bad = true;
        if (bad || std::find(ids.begin(), ids.end(), l.first) != ids.end()) continue;
        ids.push_back(l.first);
        if (ids.size() >= 8) break;
      }
      if (ids.size() >= 8) break;
    }
    if (!ids.empty()) out[sc.id] = ids;
  }
  return out;
}

void startResolver(HWND hwnd, UINT msg, const wstring& ytdlp, bool force) {
  std::thread([hwnd, msg, ytdlp, force] {
    std::map<string, vector<string>> fresh;
    int64_t at = 0;
    const bool cached = loadCache(fresh, &at);
    const int64_t age = wallMs() / 1000 - at;
    if (cached) PostMessageW(hwnd, msg, 0, (LPARAM) new std::map<string, vector<string>>(fresh));
    if (ytdlp.empty() || (!force && cached && age < 20 * 3600 && age >= 0)) return;

    vector<StreamEntry> all;
    // the label and radio channels that carry the streams the scenes ask for
    for (const wchar_t* ch : {L"@LofiGirl", L"@ChillhopMusic", L"@TheGrandSound", L"@artofminimaltechno", L"@LiquidHourDnB", L"@gamechops", L"@naxosjapan"}) {
      string out;
      int code = 0;
      const wstring cl = L"\"" + ytdlp + L"\" --flat-playlist --playlist-end 60 --no-warnings --print \"%(id)s\t%(live_status)s\t%(title)s\" https://www.youtube.com/" + ch + L"/streams";
      if (runCapture(cl, out, 45000, &code) && code == 0) {
        auto part = parseFlatList(out);
        all.insert(all.end(), part.begin(), part.end());
      }
    }
    if (all.empty()) return;  // offline, or YouTube is unreachable: keep what we have
    auto matched = matchStreams(all);
    if (matched.empty()) return;
    saveCache(matched);
    PostMessageW(hwnd, msg, 0, (LPARAM) new std::map<string, vector<string>>(std::move(matched)));
  }).detach();
}
