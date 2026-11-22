#include "spotify.h"

#include <shlobj.h>

#include "json.h"
#include "net.h"

// ---------------------------------------------------------------------------------------------------
// Links and what the command line prints
// ---------------------------------------------------------------------------------------------------

namespace {

constexpr int VOL_GAP_MS = 250;  // volume changes go out at most this often: every one is a program start

string lowerAscii(string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool spotifyId(const string& s) {  // 22 letters and digits
  if (s.size() != 22) return false;
  for (unsigned char c : s) if (!std::isalnum(c)) return false;
  return true;
}

bool spotifyKind(const string& k) { return k == "playlist" || k == "album" || k == "artist"; }

vector<string> splitBy(const string& s, char sep) {
  vector<string> out;
  size_t a = 0;
  for (;;) {
    const size_t b = s.find(sep, a);
    out.push_back(s.substr(a, b == string::npos ? string::npos : b - a));
    if (b == string::npos) break;
    a = b + 1;
  }
  return out;
}

wstring quoteArg(const wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == wstring::npos) return a;
  wstring out = L"\"";
  for (wchar_t c : a) {
    if (c == L'"') out += L'\\';
    out += c;
  }
  return out + L"\"";
}

string readSmallFile(const wstring& path, size_t limit) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  string out;
  char buf[16384];
  DWORD got = 0;
  while (out.size() < limit && ReadFile(f, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
  CloseHandle(f);
  return out.size() >= limit ? "" : out;  // something that big is not what we came for
}

bool fileExists(const wstring& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const wstring& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

wstring envVar(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
  return n > 0 && n < MAX_PATH * 2 ? wstring(buf, n) : L"";
}

wstring dirOf(const wstring& path) {
  const size_t cut = path.find_last_of(L"\\/");
  return cut == wstring::npos ? L"" : path.substr(0, cut);
}

// A copy of Spotifast run from its ZIP keeps everything in `spotifast-data` beside the exe; an installed one uses the
// usual per-user folders.
wstring spotifastConfigDir(const wstring& exe) {
  const wstring portable = dirOf(exe) + L"\\spotifast-data";
  if (!exe.empty() && dirExists(portable)) return portable + L"\\config";
  const wstring ad = envVar(L"APPDATA");
  return ad.empty() ? L"" : ad + L"\\paolino\\spotifast\\config";
}

wstring spotifastCacheDir(const wstring& exe) {
  const wstring portable = dirOf(exe) + L"\\spotifast-data";
  if (!exe.empty() && dirExists(portable)) return portable + L"\\cache";
  const wstring la = envVar(L"LOCALAPPDATA");
  return la.empty() ? L"" : la + L"\\paolino\\spotifast\\cache";
}

}  // namespace

bool validSpotifyUri(const string& uri) {
  const vector<string> p = splitBy(uri, ':');
  return p.size() == 3 && p[0] == "spotify" && spotifyKind(p[1]) && spotifyId(p[2]);
}

string parseSpotifyLink(const string& text) {
  string s = text;
  while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
  while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
  if (s.empty() || s.size() > 400) return "";
  if (lowerAscii(s.substr(0, 8)) == "spotify:") {
    vector<string> p = splitBy(s, ':');
    p[0] = "spotify";
    if (p.size() == 5 && p[1] == "user" && p[3] == "playlist") p = {"spotify", "playlist", p[4]};  // the old form with the owner in it
    if (p.size() != 3) return "";
    const string uri = p[0] + ":" + lowerAscii(p[1]) + ":" + p[2];
    return validSpotifyUri(uri) ? uri : "";
  }
  string u = s;
  const size_t scheme = u.find("://");
  if (scheme != string::npos) {
    const string sc = lowerAscii(u.substr(0, scheme));
    if (sc != "http" && sc != "https") return "";
    u = u.substr(scheme + 3);
  }
  const size_t slash = u.find('/');
  if (slash == string::npos) return "";
  const string host = lowerAscii(u.substr(0, slash));
  if (host != "open.spotify.com" && host != "play.spotify.com") return "";
  string path = u.substr(slash + 1);
  path = path.substr(0, path.find_first_of("?#"));
  vector<string> seg;
  for (const string& part : splitBy(path, '/')) if (!part.empty()) seg.push_back(part);
  // open.spotify.com/intl-de/playlist/<id>, /embed/playlist/<id>, /user/<name>/playlist/<id>
  for (size_t i = 0; i + 1 < seg.size(); i++) {
    const string kind = lowerAscii(seg[i]);
    if (spotifyKind(kind) && spotifyId(seg[i + 1])) return "spotify:" + kind + ":" + seg[i + 1];
  }
  return "";
}

SpotifyNow parseNowPlaying(const string& raw) {
  SpotifyNow n;
  string line = raw.substr(0, raw.find_first_of("\r\n"));
  const vector<string> f = splitBy(line, '\t');
  if (f.size() < 9 || f[0].empty()) return n;
  n.ok = true;
  n.state = lowerAscii(f[0]);
  n.title = widen(f[1]);
  n.artists = widen(f[2]);
  n.album = widen(f[3]);
  n.posMs = std::max<int64_t>(0, _strtoi64(f[4].c_str(), nullptr, 10));
  n.durMs = std::max<int64_t>(0, _strtoi64(f[5].c_str(), nullptr, 10));
  n.volume = f[6].empty() || !std::isdigit((unsigned char)f[6][0]) ? -1 : clampi(std::atoi(f[6].c_str()), 0, 100);
  n.shuffle = lowerAscii(f[7]);
  n.repeat = lowerAscii(f[8]);
  if (n.shuffle != "on" && n.shuffle != "off") n.shuffle.clear();
  if (n.repeat != "off" && n.repeat != "context" && n.repeat != "track") n.repeat.clear();
  if (f.size() > 9 && f[9].compare(0, 8, "https://") == 0 && f[9].size() < 300) n.art = f[9];
  if (f.size() > 11) n.device = widen(f[11]);
  return n;
}

vector<SpotifyDevice> parseDevices(const string& json) {
  vector<SpotifyDevice> out;
  bool ok = false;
  const Json j = parseJson(json, &ok);
  if (!ok || !j.isArr()) return out;
  for (const Json& d : j.a) {
    if (!d.isObj()) continue;
    const Json *id = d.get("id"), *name = d.get("name"), *kind = d.get("kind"), *active = d.get("active");
    if (!id || !id->isStr() || id->s.empty() || id->s.size() > 128) continue;
    bool clean = true;
    for (unsigned char c : id->s) if (!(std::isalnum(c) || c == '-' || c == '_')) clean = false;  // it goes on a command line
    if (!clean) continue;
    SpotifyDevice dev;
    dev.id = id->s;
    dev.name = name ? widen(name->strOr("")) : L"";
    dev.kind = kind ? kind->strOr("") : "";
    dev.active = active && active->boolOr(false);
    out.push_back(dev);
  }
  return out;
}

vector<SpotifyList> parseSpotifastLibrary(const string& json) {
  vector<SpotifyList> out;
  bool ok = false;
  const Json j = parseJson(json, &ok);
  const Json* data = ok && j.isObj() ? j.get("data") : nullptr;
  if (!data || !data->isArr()) return out;
  for (const Json& p : data->a) {
    if (out.size() >= 2000 || !p.isObj()) continue;
    const Json *uri = p.get("uri"), *name = p.get("name");
    if (!uri || !uri->isStr() || !validSpotifyUri(uri->s) || !name || !name->isStr()) continue;
    SpotifyList l;
    l.uri = uri->s;
    for (wchar_t c : widen(name->s)) if (c >= 32 && l.name.size() < 80) l.name += c;
    while (!l.name.empty() && l.name.back() == L' ') l.name.pop_back();
    if (l.name.empty()) continue;
    if (const Json* owner = p.get("owner")) {
      if (owner->isObj()) {
        const Json* dn = owner->get("display_name");
        if (dn && dn->isStr()) l.owner = widen(dn->s).substr(0, 40);
      }
    }
    if (const Json* im = p.get("images")) {
      if (im->isArr() && !im->a.empty() && im->a[0].isObj()) {
        const Json* u = im->a[0].get("url");
        if (u && u->isStr() && u->s.compare(0, 8, "https://") == 0 && u->s.size() < 300) l.art = u->s;
      }
    }
    if (const Json* it = p.get("items")) {
      if (it->isObj()) {
        const Json* t = it->get("total");
        if (t && t->isNum()) l.tracks = (int)clampd(t->n, 0, 1e6);
      }
    }
    out.push_back(std::move(l));
  }
  return out;
}

vector<SpotifyList> loadSpotifastLibrary(const wstring& exePath) {
  const wstring dir = spotifastCacheDir(exePath) + L"\\library";
  // one file per account: the one written last belongs to whoever is signed in now
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return {};
  wstring best;
  ULONGLONG bestTime = 0;
  do {
    const ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && t >= bestTime) { bestTime = t; best = fd.cFileName; }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  if (best.empty()) return {};
  return parseSpotifastLibrary(readSmallFile(dir + L"\\" + best, 24u * 1024 * 1024));
}

bool parseOembed(const string& json, wstring& title, string& art) {
  bool ok = false;
  const Json j = parseJson(json, &ok);
  if (!ok || !j.isObj()) return false;
  const Json *t = j.get("title"), *th = j.get("thumbnail_url");
  if (!t || !t->isStr() || t->s.empty()) return false;
  title.clear();
  for (wchar_t c : widen(t->s)) if (c >= 32 && title.size() < 80) title += c;
  art = th && th->isStr() && th->s.compare(0, 8, "https://") == 0 && th->s.size() < 300 ? th->s : "";
  return !title.empty();
}

bool spotifyLookup(const string& uri, wstring& title, string& art) {
  if (!validSpotifyUri(uri)) return false;
  vector<uint8_t> body;
  if (!httpsGet(L"open.spotify.com", L"/oembed?url=" + widen(uri), body, 7000)) return false;
  return parseOembed(string(body.begin(), body.end()), title, art);
}

// ---------------------------------------------------------------------------------------------------
// The player
// ---------------------------------------------------------------------------------------------------

Spotify::Spotify() {
  clock_ = wallMs;
}

Spotify::~Spotify() { shutdown(); }

bool Spotify::configure() {
  wstring exe = envVar(L"MURMUR_SPOTIFAST");
  if (!exe.empty() && !fileExists(exe)) exe.clear();
  if (exe.empty()) {
    wchar_t buf[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, L"spotifast.exe", nullptr, MAX_PATH, buf, nullptr);
    if (n && n < MAX_PATH) exe = buf;
  }
  if (exe.empty()) {  // the program Windows opens spotify: links with, when that is Spotifast
    wchar_t cmd[MAX_PATH * 2] = {};
    DWORD sz = sizeof cmd;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\spotify\\shell\\open\\command", nullptr, RRF_RT_REG_SZ, nullptr, cmd, &sz) == ERROR_SUCCESS) {
      wstring c = cmd;
      wstring path;
      if (!c.empty() && c[0] == L'"') path = c.substr(1, c.find(L'"', 1) == wstring::npos ? wstring::npos : c.find(L'"', 1) - 1);
      else path = c.substr(0, c.find(L' '));
      wstring low = path;
      for (auto& ch : low) ch = (wchar_t)std::towlower(ch);
      const wstring want = L"\\spotifast.exe";
      if (low.size() > want.size() && low.compare(low.size() - want.size(), want.size(), want) == 0 && fileExists(path)) exe = path;
    }
  }
  if (exe.empty()) {
    for (const wstring& p : {envVar(L"LOCALAPPDATA") + L"\\Programs\\Spotifast\\spotifast.exe", envVar(L"ProgramFiles") + L"\\Spotifast\\spotifast.exe"}) {
      if (exe.empty() && fileExists(p)) exe = p;
    }
  }
  // the built-in player carries the name set in Spotifast's settings ("Spotifast" unless it was changed)
  wstring name = L"Spotifast";
  if (!exe.empty()) {
    bool ok = false;
    const Json j = parseJson(readSmallFile(spotifastConfigDir(exe) + L"\\settings.json", 4u * 1024 * 1024), &ok);
    if (ok && j.isObj()) {
      const Json* dn = j.get("device_name");
      if (dn && dn->isStr() && !dn->s.empty() && dn->s.size() < 100) name = widen(dn->s);
    }
  }
  configure(exe, name);
  return !exe.empty();
}

void Spotify::configure(const wstring& exe, const wstring& builtinName) {
  std::lock_guard<std::mutex> g(mu_);
  exe_ = exe;
  if (!builtinName.empty()) builtin_ = builtinName;
}

bool Spotify::available() const {
  std::lock_guard<std::mutex> g(mu_);
  return !exe_.empty() || (bool)run_;
}

wstring Spotify::exePath() const {
  std::lock_guard<std::mutex> g(mu_);
  return exe_;
}

wstring Spotify::builtinName() const {
  std::lock_guard<std::mutex> g(mu_);
  return builtin_;
}

void Spotify::emit(const SpotifyEvent& e) {
  std::function<void(const SpotifyEvent&)> s;
  {
    std::lock_guard<std::mutex> g(sinkMu_);
    s = sink_;
  }
  if (s) s(e);
}

bool Spotify::stale(int gen) {
  std::lock_guard<std::mutex> g(mu_);
  return quit_ || gen != gen_.load();
}

// Waits, but not past a shutdown or a newer request: whoever asked last should not sit behind an old wait.
void Spotify::nap(int ms) {
  std::function<void(int)> s;
  {
    std::lock_guard<std::mutex> g(mu_);
    s = sleep_;
  }
  if (s) { s(ms); return; }
  std::unique_lock<std::mutex> lk(mu_);
  const int g0 = gen_.load();
  cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return quit_ || gen_.load() != g0; });
}

int Spotify::cli(const vector<wstring>& args, string& out) {
  Runner r;
  wstring exe;
  {
    std::lock_guard<std::mutex> g(mu_);
    r = run_;
    exe = exe_;
  }
  out.clear();
  if (r) return r(args, out);
  if (exe.empty()) return -1;
  wstring cl = quoteArg(exe);
  for (const wstring& a : args) cl += L" " + quoteArg(a);
  int code = -1;
  if (!runCapture(cl, out, 8000, &code)) return -1;
  return code;
}

// Starts Spotifast the way its own "start with your computer, in the tray" does: no window, ready for commands. It is the
// listener's own program, so it is not tied to Murmur's lifetime and stays when Murmur closes.
bool Spotify::startHidden() {
  const wstring exe = exePath();
  if (exe.empty()) return false;
  wstring cl = quoteArg(exe) + L" --start-hidden";
  STARTUPINFOW si = {};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, dirOf(exe).c_str(), &si, &pi)) {
    mlog("spotify: cannot start Spotifast (%lu)", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

SpotifyNow Spotify::nowPlaying(int* code) {
  string out;
  const int c = cli({L"now-playing", L"--raw"}, out);
  if (code) *code = c;
  if (c != 0) return SpotifyNow();
  return parseNowPlaying(out);
}

// Spotifast answers no more: it was closed while it played for us. There is nothing left to give back.
void Spotify::gone() {
  {
    std::lock_guard<std::mutex> g(mu_);
    pollAt_ = 0;
    volDirty_ = false;
  }
  holding_.store(false);
  movedTo_.clear();
  sentVol_ = -1;
  mlog("spotify: Spotifast is no longer running");
  SpotifyEvent e;
  e.type = SpotifyEvent::Failed;
  e.gen = gen_.load();
  e.uri = uri_;
  e.reason = "gone";
  uri_.clear();
  emit(e);
}

void Spotify::post(const Cmd& c) {
  bool run = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    if (quit_) return;
    q_.push_back(c);
    if (inline_) run = true;
    else if (!started_) {
      started_ = true;
      worker_ = Thread([this] { loop(); });
    }
  }
  if (run) pump();
  else cv_.notify_all();
}

void Spotify::pump() {
  while (step()) {}
}

void Spotify::loop() {
  for (;;) {
    if (step()) continue;
    std::unique_lock<std::mutex> lk(mu_);
    if (quit_ && q_.empty()) break;
    if (!q_.empty()) continue;
    int64_t until = INT64_MAX;
    if (holding_.load() && volDirty_) until = lastVolAt_ + VOL_GAP_MS;
    if (holding_.load() && pollAt_) until = std::min(until, pollAt_);
    if (until == INT64_MAX) { cv_.wait(lk); continue; }
    const int64_t left = until - (clock_ ? clock_() : wallMs());
    if (left > 0) cv_.wait_for(lk, std::chrono::milliseconds(std::min<int64_t>(left, 60000)));
  }
}

bool Spotify::step() {
  Cmd c;
  int vol = -1;
  bool poll = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t t = clock_ ? clock_() : wallMs();
    if (!q_.empty()) {
      c = q_.front();
      q_.pop_front();
      // a newer Play makes an older one pointless: skip it rather than start a playlist only to replace it
      if (c.kind == Cmd::Play && c.gen != gen_.load()) return true;
    } else if (holding_.load() && volDirty_ && t - lastVolAt_ >= VOL_GAP_MS) {
      volDirty_ = false;
      lastVolAt_ = t;
      vol = wantVol_;
    } else if (holding_.load() && pollAt_ && t >= pollAt_) {
      pollAt_ = 0;
      poll = true;
    } else {
      return false;
    }
  }
  if (vol >= 0) {
    if (vol != sentVol_) {
      string out;
      cli({L"volume", std::to_wstring(vol)}, out);
      sentVol_ = vol;
    }
    return true;
  }
  if (poll) { doPoll(); return true; }
  exec(c);
  return true;
}

void Spotify::exec(const Cmd& c) {
  string out;
  switch (c.kind) {
    case Cmd::Play: doPlay(c); break;
    case Cmd::Pause:
      if (holding_.load()) cli({L"pause"}, out);
      break;
    case Cmd::Resume:
      if (holding_.load()) {
        if (cli({L"play"}, out) != 0) { gone(); break; }
        std::lock_guard<std::mutex> g(mu_);
        pollAt_ = (clock_ ? clock_() : wallMs()) + 1500;
      }
      break;
    case Cmd::Next:
    case Cmd::Prev:
      if (holding_.load()) {
        cli({c.kind == Cmd::Next ? L"next" : L"previous"}, out);
        std::lock_guard<std::mutex> g(mu_);
        pollAt_ = (clock_ ? clock_() : wallMs()) + 1500;  // the new track's name, once it is there
      }
      break;
    case Cmd::Release: doRelease(); break;
    case Cmd::Poll:
      if (holding_.load()) doPoll();
      break;
  }
}

void Spotify::doPlay(const Cmd& c) {
  SpotifyEvent e;
  e.gen = c.gen;
  e.uri = c.uri;
  bool tookHold = false, touched = false;
  auto fail = [&](const char* why) {
    mlog("spotify: %s", why);
    if (tookHold && !touched) holding_.store(false);  // nothing of Spotifast's was changed: there is nothing to give back
    e.type = SpotifyEvent::Failed;
    e.reason = why;
    emit(e);
  };
  e.type = SpotifyEvent::Loading;
  emit(e);
  if (!available()) { fail("no spotifast"); return; }

  // 1. Spotifast must be running: its verbs fail when it is not, and then it is started in the tray.
  string out;
  int code = cli({L"devices", L"--raw"}, out);
  bool fresh = false;
  if (code != 0) {
    std::function<bool()> launch;
    {
      std::lock_guard<std::mutex> g(mu_);
      launch = launch_;
    }
    if (launch ? !launch() : !startHidden()) { fail("start"); return; }
    fresh = true;
    mlog("spotify: started Spotifast in the tray");
    for (int i = 0; i < 60 && code != 0; i++) {
      nap(500);
      if (stale(c.gen)) return;
      code = cli({L"devices", L"--raw"}, out);
    }
    if (code != 0) { fail("start"); return; }
  }
  vector<SpotifyDevice> devs = parseDevices(out);
  const SpotifyNow before = nowPlaying();
  wstring builtin;
  {
    std::lock_guard<std::mutex> g(mu_);
    builtin = builtin_;
  }

  // 2. Remember how Spotifast was, once, so it can be given back that way.
  if (!holding_.load()) {
    prev_ = Prev();
    prev_.known = before.ok;
    prev_.volume = before.volume;
    prev_.shuffle = before.shuffle;
    prev_.repeat = before.repeat;
    for (const SpotifyDevice& d : devs) if (d.active) prev_.deviceId = d.id;
    if (prev_.deviceId.empty() && before.ok) for (const SpotifyDevice& d : devs) if (d.name == before.device) prev_.deviceId = d.id;
    movedTo_.clear();
    holding_.store(true);
    tookHold = true;
  }

  // 3. Private: move playback to the built-in player, the device that carries Spotifast's own name. A copy that has only
  //    just started lists no devices for a moment; and while the built-in player is the one playing, the list leaves it out.
  if (c.priv) {
    auto own = [&]() -> const SpotifyDevice* {
      for (const SpotifyDevice& d : devs) if (d.name == builtin) return &d;
      return nullptr;
    };
    for (int i = 0; i < 16 && !own() && (fresh || devs.empty()); i++) {
      nap(500);
      if (stale(c.gen)) return;
      if (cli({L"devices", L"--raw"}, out) == 0) devs = parseDevices(out);
    }
    const SpotifyDevice* d = own();
    if (d) builtinId_ = d->id;
    const bool already = d ? d->active : (before.ok && before.device == builtin && before.state == "playing");
    if (!already) {
      if (builtinId_.empty()) { fail("no private player"); return; }
      if (cli({L"transfer", widen(builtinId_)}, out) != 0) { fail("no private player"); return; }
      touched = true;
      movedTo_ = builtinId_;
      nap(300);
      if (stale(c.gen)) return;
    }
  } else if (!movedTo_.empty() && !prev_.deviceId.empty() && prev_.deviceId != movedTo_) {
    // not private any more: back to the device Spotifast was playing on before Murmur moved it away
    if (cli({L"transfer", widen(prev_.deviceId)}, out) == 0) {
      touched = true;
      movedTo_.clear();
      nap(300);
      if (stale(c.gen)) return;
    }
  }

  // 4. A study playlist should not start on the same song every time, and should not run out.
  touched = true;
  cli({L"shuffle", L"on"}, out);
  cli({L"repeat", L"context"}, out);
  int vol;
  {
    std::lock_guard<std::mutex> g(mu_);
    vol = wantVol_;
    volDirty_ = false;
    lastVolAt_ = clock_ ? clock_() : wallMs();
  }
  cli({L"volume", std::to_wstring(vol)}, out);
  sentVol_ = vol;

  // 5. Play. The command answers at once and the new song follows a second or two later; until then the song that was
  //    there before still shows, as playing. The real thing is the same song in two looks in a row, further along in the
  //    second, and not that old song. (A playlist that happens to begin with the very song that was there cannot pass that
  //    test, so after a few seconds a song that plays steadily is taken on trust.)
  if (cli({L"play-uri", widen(c.uri)}, out) != 0) { fail("play"); return; }
  SpotifyNow nowp, last;
  bool up = false;
  for (int i = 0; i < 40 && !up; i++) {
    nap(400);
    if (stale(c.gen)) return;
    nowp = nowPlaying();
    const bool moving = nowp.ok && nowp.state == "playing" && nowp.posMs > 0;
    const bool steady = moving && last.ok && last.title == nowp.title && last.durMs == nowp.durMs && nowp.posMs > last.posMs;
    const bool other = !before.ok || nowp.title != before.title || nowp.durMs != before.durMs;
    up = steady && (other || i >= 12);
    last = moving ? nowp : SpotifyNow();
  }
  if (!up) { fail("timeout"); return; }
  const bool priv = nowp.device == builtin;
  if (c.priv && !priv) {  // it ended up somewhere Spotify records: stop, and say so
    cli({L"pause"}, out);
    fail("not private");
    return;
  }
  uri_ = c.uri;
  priv_ = priv;
  e.type = SpotifyEvent::Playing;
  e.now = nowp;
  e.priv = priv;
  emit(e);
  schedulePoll(nowp);
}

// The next look at what is playing: just after the track ends. Nothing is asked in between, because every question is a
// program start; a change made in Spotifast itself shows when the window comes back to the front (refresh()).
void Spotify::schedulePoll(const SpotifyNow& n) {
  std::lock_guard<std::mutex> g(mu_);
  const int64_t t = clock_ ? clock_() : wallMs();
  if (n.ok && n.state == "playing" && n.durMs > 0) pollAt_ = t + (int64_t)clampd((double)(n.durMs - n.posMs) + 1500, 3000, 15 * 60000.0);
  else pollAt_ = t + 60000;
}

void Spotify::doPoll() {
  int code = 0;
  const SpotifyNow n = nowPlaying(&code);
  if (!holding_.load()) return;
  if (code != 0) { gone(); return; }
  if (n.ok) {
    wstring builtin;
    {
      std::lock_guard<std::mutex> g(mu_);
      builtin = builtin_;
    }
    priv_ = n.device == builtin;
    SpotifyEvent e;
    e.type = SpotifyEvent::Now;
    e.gen = gen_.load();
    e.uri = uri_;
    e.now = n;
    e.priv = priv_;
    emit(e);
  }
  schedulePoll(n);
}

void Spotify::doRelease() {
  if (!holding_.load()) return;
  string out;
  cli({L"pause"}, out);
  if (prev_.known) {
    if (!prev_.shuffle.empty()) cli({L"shuffle", widen(prev_.shuffle)}, out);
    if (!prev_.repeat.empty()) cli({L"repeat", widen(prev_.repeat)}, out);
    if (prev_.volume >= 0) cli({L"volume", std::to_wstring(prev_.volume)}, out);
  }
  // back to the device it was on, when Murmur moved it away from one
  if (!movedTo_.empty() && !prev_.deviceId.empty() && prev_.deviceId != movedTo_) cli({L"transfer", widen(prev_.deviceId)}, out);
  {
    std::lock_guard<std::mutex> g(mu_);
    pollAt_ = 0;
    volDirty_ = false;
  }
  holding_.store(false);
  uri_.clear();
  movedTo_.clear();
  sentVol_ = -1;
  SpotifyEvent e;
  e.type = SpotifyEvent::Released;
  e.gen = gen_.load();
  emit(e);
}

int Spotify::play(const string& uri, bool priv, int volume) {
  Cmd c;
  c.kind = Cmd::Play;
  c.uri = uri;
  c.priv = priv;
  {
    std::lock_guard<std::mutex> g(mu_);
    c.gen = ++gen_;
    wantVol_ = clampi(volume, 0, 100);
  }
  post(c);
  return c.gen;
}

void Spotify::setPaused(bool p) {
  Cmd c;
  c.kind = p ? Cmd::Pause : Cmd::Resume;
  post(c);
}

void Spotify::setVolume(int v) {
  bool run = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    v = clampi(v, 0, 100);
    if (v == wantVol_ && !volDirty_) return;
    wantVol_ = v;
    volDirty_ = true;
    run = inline_;
  }
  if (run) pump();
  else cv_.notify_all();
}

void Spotify::next() { Cmd c; c.kind = Cmd::Next; post(c); }
void Spotify::prev() { Cmd c; c.kind = Cmd::Prev; post(c); }
void Spotify::refresh() { if (holding_.load()) { Cmd c; c.kind = Cmd::Poll; post(c); } }

void Spotify::release(int waitMs) {
  {
    std::lock_guard<std::mutex> g(mu_);
    ++gen_;  // whatever is still waiting for a playlist to start gives up
    for (auto it = q_.begin(); it != q_.end();) it = it->kind == Cmd::Play ? q_.erase(it) : it + 1;
  }
  Cmd c;
  c.kind = Cmd::Release;
  post(c);
  const int64_t end = wallMs() + waitMs;
  while (waitMs > 0 && holding_.load() && wallMs() < end) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void Spotify::shutdown() {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (quit_) return;
    quit_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}
