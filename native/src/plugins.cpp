#include "plugins.h"

#include <cstdlib>
#include <deque>

#include "json.h"
#include "thread.h"
#include "version.h"

namespace {

// White space and control characters become single spaces; trimmed; at most `maxLen` characters.
wstring cleanText(const wstring& in, size_t maxLen) {
  wstring out;
  for (wchar_t c : in) {
    const bool space = c < 0x20 || c == 0x7F || c == L' ' || c == 0xA0;
    if (space) {
      if (!out.empty() && out.back() != L' ') out += L' ';
    } else {
      out += c;
    }
  }
  while (!out.empty() && out.back() == L' ') out.pop_back();
  if (out.size() > maxLen) {
    out.resize(maxLen);
    while (!out.empty() && out.back() == L' ') out.pop_back();
  }
  return out;
}

string readSmall(const wstring& path, size_t limit = 65536) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  string out;
  char buf[4096];
  DWORD got = 0;
  while (out.size() <= limit && ReadFile(f, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
  CloseHandle(f);
  if (out.size() > limit) return "";
  if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);  // a byte order mark
  return out;
}

bool isFile(const wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool isDir(const wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// One argument of a command line, quoted the way CommandLineToArgvW and the C runtime read it back.
wstring quoteArg(const wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == wstring::npos) return a;
  wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') { slashes++; continue; }
    if (c == L'"') { out.append(slashes * 2 + 1, L'\\'); out += L'"'; }
    else { out.append(slashes, L'\\'); out += c; }
    slashes = 0;
  }
  out.append(slashes * 2, L'\\');
  out += L'"';
  return out;
}

// The program a plugin names: a bare name or a relative path is looked for in the plugin's own folder first; whatever is
// not there is left as written, for Windows to find on PATH.
wstring resolveProgram(const PluginInfo& p) {
  const wstring& prog = p.run[0];
  const bool absolute = prog.size() > 1 && (prog[1] == L':' || (prog[0] == L'\\' && prog[1] == L'\\') || prog[0] == L'\\' || prog[0] == L'/');
  if (absolute) return prog;
  for (const wchar_t* ext : {L"", L".exe", L".cmd", L".bat"}) {
    const wstring here = p.dir + L"\\" + prog + ext;
    if (isFile(here)) return here;
  }
  return prog;
}

const char* const README =
    "Murmur plugins\r\n"
    "==============\r\n"
    "\r\n"
    "A plugin is a folder in here with a plugin.json in it:\r\n"
    "\r\n"
    "    {\r\n"
    "      \"name\": \"My plugin\",\r\n"
    "      \"about\": \"One line on what it does\",\r\n"
    "      \"run\": [\"python\", \"my_plugin.py\"],\r\n"
    "      \"settings\": false\r\n"
    "    }\r\n"
    "\r\n"
    "\"run\" is the program Murmur starts, and its arguments. A bare name is looked for in the plugin's folder\r\n"
    "first, then on PATH. The plugin's folder is the program's working directory.\r\n"
    "\r\n"
    "Nothing in this folder runs until you switch it on: the plug button at the top of Murmur's window lists\r\n"
    "what is here. A plugin is an ordinary program with all of your rights, so only put one here that you trust.\r\n"
    "\r\n"
    "What the program is told\r\n"
    "------------------------\r\n"
    "Murmur writes one line of JSON to the program's standard input whenever the session changes:\r\n"
    "\r\n"
    "    {\"event\":\"hello\",\"murmur\":\"1.0.0\",\"protocol\":1}\r\n"
    "    {\"event\":\"state\",\"running\":true,\"phase\":\"focus\",\"scene\":\"sprint\",\"study\":true,\"plan\":\"p15\",\"round\":1,\"rounds\":4,\"remaining\":900}\r\n"
    "\r\n"
    "  running    the clock is going (false while paused, before the first start, and once the plan is over)\r\n"
    "  phase      \"focus\", \"break\", \"timer\" (Calm and Sleep), \"free\" (the open-ended plan) or \"done\"\r\n"
    "  study      true for the study scenes, false for Calm and Sleep\r\n"
    "  round      the round the plan is in, of \"rounds\" (both 0 for a plan without rounds)\r\n"
    "  remaining  seconds left of this phase, or null when it has no end\r\n"
    "\r\n"
    "With \"settings\": true the plugin gets a settings button, and pressing it sends {\"event\":\"settings\"}.\r\n"
    "\r\n"
    "What it can say back\r\n"
    "--------------------\r\n"
    "A line of JSON on standard output:\r\n"
    "\r\n"
    "    {\"status\":\"2 zaps\"}      shown under the plugin's name (at most 40 characters)\r\n"
    "\r\n"
    "Starting and stopping\r\n"
    "---------------------\r\n"
    "The program is started when a plan starts running, and when its settings button is pressed. It should keep\r\n"
    "reading until its input ends. It may also exit whenever it likes, for instance once \"running\" has been false\r\n"
    "for a while: Murmur starts it again the next time something changes while a plan runs.\r\n"
    "\r\n"
    "When Murmur closes, or the plugin is switched off, the program's standard input ends. It has about a second\r\n"
    "to leave and is ended if it is still there.\r\n";

// ---------------------------------------------------------------------------------------------------
// The real thing: a child process, its input and output on pipes, in a job that ends with Murmur.
// ---------------------------------------------------------------------------------------------------

class RealProc : public PluginProc {
 public:
  ~RealProc() override { stop(); }

  bool begin(const PluginInfo& p, int gen, PluginSink sink, wstring* error) {
    id_ = p.id;
    gen_ = gen;
    sink_ = std::move(sink);
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE inRd = nullptr, outWr = nullptr;
    if (!CreatePipe(&inRd, &in_, &sa, 0)) { in_ = nullptr; return fail(error, L"Could not start"); }
    if (!CreatePipe(&out_, &outWr, &sa, 0)) { out_ = nullptr; CloseHandle(inRd); return fail(error, L"Could not start"); }
    SetHandleInformation(in_, HANDLE_FLAG_INHERIT, 0);   // our ends stay ours, or the program never sees its input end
    SetHandleInformation(out_, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    wstring cl = quoteArg(resolveProgram(p));
    for (size_t i = 1; i < p.run.size(); i++) cl += L" " + quoteArg(p.run[i]);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRd;
    si.hStdOutput = outWr;
    si.hStdError = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    PROCESS_INFORMATION pi = {};
    const BOOL made = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, p.dir.c_str(), &si, &pi);
    const DWORD why = GetLastError();
    CloseHandle(inRd);
    CloseHandle(outWr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!made) {
      mlog("plugin %s: cannot start (%lu)", id_.c_str(), why);
      return fail(error, why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND ? L"Its program was not found" : L"Could not start");
    }
    // It dies with Murmur, even if Murmur crashes. A helper it starts may ask to leave the job (a background service of
    // its own, say), which is why breaking away is allowed.
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
      SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
      AssignProcessToJobObject(job_, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    reader_ = Thread([this] { readLoop(); });
    writer_ = Thread([this] { writeLoop(); });
    return true;
  }

  void send(const string& line) override {
    std::lock_guard<std::mutex> g(mu_);
    if (quit_) return;
    if (q_.size() >= 64) q_.pop_front();  // a program that does not read loses the oldest news, and nothing waits for it
    q_.push_back(line + "\n");
    cv_.notify_one();
  }

  void stop() override {
    {
      std::lock_guard<std::mutex> g(mu_);
      if (quit_) return;
      quit_ = true;
    }
    cv_.notify_all();
    if (writer_.joinable()) {
      while (!writerDone_.load()) {  // a write may be stuck on a program that never reads
        if (in_) CancelIoEx(in_, nullptr);
        Sleep(5);
      }
      writer_.join();
    }
    if (in_) { CloseHandle(in_); in_ = nullptr; }  // its input ends: time to leave
    if (proc_ && WaitForSingleObject(proc_, 1200) == WAIT_TIMEOUT) {
      if (job_) TerminateJobObject(job_, 1); else TerminateProcess(proc_, 1);
      WaitForSingleObject(proc_, 2000);
    }
    if (reader_.joinable()) {
      while (!readerDone_.load()) {  // something it started may still hold the other end of the pipe
        if (out_) CancelIoEx(out_, nullptr);
        Sleep(5);
      }
      reader_.join();
    }
    if (out_) { CloseHandle(out_); out_ = nullptr; }
    if (proc_) { CloseHandle(proc_); proc_ = nullptr; }
    if (job_) { CloseHandle(job_); job_ = nullptr; }
  }

 private:
  bool fail(wstring* error, const wchar_t* why) {
    if (error) *error = why;
    return false;
  }

  void emit(PluginEvent::Type type, const string& line, int code) {
    if (!sink_) return;
    PluginEvent e;
    e.type = type;
    e.id = id_;
    e.gen = gen_;
    e.line = line;
    e.code = code;
    sink_(e);
  }

  void readLoop() {
    string buf;
    char tmp[1024];
    DWORD got = 0;
    ULONGLONG second = 0;
    int inSecond = 0;
    while (ReadFile(out_, tmp, sizeof tmp, &got, nullptr) && got) {
      buf.append(tmp, got);
      size_t nl;
      while ((nl = buf.find('\n')) != string::npos) {
        string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line.size() > PLUGIN_LINE_MAX) continue;
        const ULONGLONG s = GetTickCount64() / 1000;  // a chatty program gets through twenty times a second, no more
        if (s != second) { second = s; inSecond = 0; }
        if (++inSecond <= 20) emit(PluginEvent::Line, line, 0);
      }
      if (buf.size() > 4 * PLUGIN_LINE_MAX) buf.clear();  // a line that never ends
    }
    bool quitting;
    {
      std::lock_guard<std::mutex> g(mu_);
      quitting = quit_;
    }
    if (!quitting) {  // it closed its output or left by itself: say so once it is really gone
      WaitForSingleObject(proc_, INFINITE);
      DWORD code = 0;
      GetExitCodeProcess(proc_, &code);
      emit(PluginEvent::Exit, "", (int)code);
    }
    readerDone_ = true;
  }

  void writeLoop() {
    for (;;) {
      string line;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return quit_ || !q_.empty(); });
        if (quit_) break;
        line = std::move(q_.front());
        q_.pop_front();
      }
      DWORD wrote = 0;
      if (!WriteFile(in_, line.data(), (DWORD)line.size(), &wrote, nullptr)) break;  // it is gone, or we are stopping
    }
    writerDone_ = true;
  }

  string id_;
  int gen_ = 0;
  PluginSink sink_;
  HANDLE proc_ = nullptr, job_ = nullptr, in_ = nullptr, out_ = nullptr;
  Thread reader_, writer_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<string> q_;
  bool quit_ = false;
  std::atomic<bool> readerDone_{false}, writerDone_{false};
};

}  // namespace

std::unique_ptr<PluginProc> spawnPluginProcess(const PluginInfo& p, int gen, PluginSink sink, wstring* error) {
  if (p.run.empty()) {
    if (error) *error = L"Could not start";
    return nullptr;
  }
  auto proc = std::make_unique<RealProc>();
  if (!proc->begin(p, gen, std::move(sink), error)) return nullptr;
  return proc;
}

// ---------------------------------------------------------------------------------------------------
// The words that go back and forth
// ---------------------------------------------------------------------------------------------------

string pluginHello(const string& version) {
  string out = "{\"event\":\"hello\",\"murmur\":";
  jsonEscape(out, version);
  out += ",\"protocol\":" + std::to_string(PLUGIN_PROTOCOL) + "}";
  return out;
}

string pluginLine(const PluginState& s) {
  string out = "{\"event\":\"state\",\"running\":";
  out += s.running ? "true" : "false";
  out += ",\"phase\":";
  jsonEscape(out, s.phase);
  out += ",\"scene\":";
  jsonEscape(out, s.scene);
  out += ",\"study\":";
  out += s.study ? "true" : "false";
  out += ",\"plan\":";
  jsonEscape(out, s.plan);
  out += ",\"round\":" + std::to_string(s.round) + ",\"rounds\":" + std::to_string(s.rounds) + ",\"remaining\":";
  out += s.remainingMs < 0 ? "null" : std::to_string((s.remainingMs + 500) / 1000);
  out += "}";
  return out;
}

bool validPluginId(const string& id) {
  if (id.empty() || id.size() > 40 || id[0] == '.' || id[0] == '_') return false;
  for (unsigned char c : id) if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
  return true;
}

bool parsePluginManifest(const string& json, PluginInfo& out) {
  out.run.clear();
  out.error.clear();
  bool ok = false;
  const Json j = parseJson(json, &ok);
  if (!ok || !j.isObj()) {
    out.error = L"Its plugin.json cannot be read";
    return false;
  }
  if (const Json* v = j.get("name")) {
    const wstring name = cleanText(widen(v->strOr("")), 40);
    if (!name.empty()) out.name = name;
  }
  if (const Json* v = j.get("about")) out.about = cleanText(widen(v->strOr("")), 80);
  if (const Json* v = j.get("settings")) out.settings = v->boolOr(false);
  if (const Json* r = j.get("run")) {
    if (r->isStr()) {
      if (!r->s.empty() && r->s.size() <= 1000) out.run.push_back(widen(r->s));
    } else if (r->isArr() && !r->a.empty() && r->a.size() <= 32) {
      for (const Json& a : r->a) {
        if (!a.isStr() || a.s.size() > 1000 || a.s.find('\0') != string::npos) { out.run.clear(); break; }
        out.run.push_back(widen(a.s));
      }
      if (!out.run.empty() && out.run[0].empty()) out.run.clear();
    }
  }
  if (out.run.empty()) {
    out.error = L"Its plugin.json names no program to run";
    return false;
  }
  return true;
}

bool parsePluginStatus(const string& line, wstring& status) {
  if (line.empty() || line[0] != '{') return false;
  bool ok = false;
  const Json j = parseJson(line, &ok);
  if (!ok || !j.isObj()) return false;
  const Json* v = j.get("status");
  if (!v || !v->isStr()) return false;
  status = cleanText(widen(v->s), PLUGIN_STATUS_MAX);
  return true;
}

// ---------------------------------------------------------------------------------------------------
// Plugins
// ---------------------------------------------------------------------------------------------------

Plugins::Plugins() {}

Plugins::~Plugins() { shutdown(); }

void Plugins::configure(const wstring& dir, const string& version) {
  dir_ = dir;
  version_ = version.empty() ? string(MURMUR_VERSION) : version;
}

bool Plugins::ensureFolder() {
  if (dir_.empty()) return false;
  if (!isDir(dir_) && !CreateDirectoryW(dir_.c_str(), nullptr)) return false;
  const wstring note = dir_ + L"\\README.txt";
  if (!isFile(note) && !isDir(note)) {
    HANDLE f = CreateFileW(note.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD wrote = 0;
      WriteFile(f, README, (DWORD)std::strlen(README), &wrote, nullptr);
      CloseHandle(f);
    }
  }
  return true;
}

const PluginInfo* Plugins::find(const string& id) const {
  for (const PluginInfo& p : list_) if (p.id == id) return &p;
  return nullptr;
}

bool Plugins::anyOn() const {
  for (const PluginInfo& p : list_) if (p.on) return true;
  return false;
}

void Plugins::rescan(const std::map<string, bool>& on) {
  vector<PluginInfo> found;
  if (!dir_.empty()) {
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((dir_ + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const string id = narrow(fd.cFileName);
        if (!validPluginId(id)) continue;  // also "." and "..", and folders set aside with a leading dot or underscore
        PluginInfo p;
        p.id = id;
        p.dir = dir_ + L"\\" + fd.cFileName;
        p.name = widen(id);
        const string text = readSmall(p.dir + L"\\plugin.json");
        if (text.empty() && !isFile(p.dir + L"\\plugin.json")) continue;  // a folder without one is not a plugin
        parsePluginManifest(text, p);
        found.push_back(std::move(p));
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
  std::sort(found.begin(), found.end(), [](const PluginInfo& a, const PluginInfo& b) {
    const int c = _wcsicmp(a.name.c_str(), b.name.c_str());
    return c != 0 ? c < 0 : a.id < b.id;
  });
  if (found.size() > (size_t)PLUGINS_MAX) found.resize((size_t)PLUGINS_MAX);

  // what was running carries on under the same id; what is no longer there is stopped
  vector<Slot> slots(found.size());
  for (size_t i = 0; i < list_.size(); i++) {
    size_t to = found.size();
    for (size_t k = 0; k < found.size(); k++) if (found[k].id == list_[i].id) to = k;
    if (to == found.size() || !found[to].usable()) { stop(i); continue; }
    found[to].alive = list_[i].alive;
    found[to].status = list_[i].status;
    found[to].note = list_[i].note;
    slots[to] = std::move(slots_[i]);
  }
  for (size_t k = 0; k < found.size(); k++) {
    const auto it = on.find(found[k].id);
    found[k].on = found[k].usable() && it != on.end() && it->second;
  }
  list_ = std::move(found);
  slots_ = std::move(slots);
  for (size_t i = 0; i < list_.size(); i++) {
    if (!list_[i].on && list_[i].alive) stop(i);
    slots_[i].fails = 0;  // looking again is a fresh try for one that kept falling over
  }
  if (haveState_) setState(current());
}

bool Plugins::setOn(const string& id, bool on) {
  for (size_t i = 0; i < list_.size(); i++) {
    if (list_[i].id != id) continue;
    if (on && !list_[i].usable()) return false;
    list_[i].on = on;
    slots_[i].fails = 0;
    slots_[i].endedAt = 0;
    if (!on) {
      stop(i);
      list_[i].note.clear();
    } else {
      slots_[i].dirty = true;
      if (haveState_ && state_.running) start(i, true);
    }
    return true;
  }
  return false;
}

PluginState Plugins::current() const {
  PluginState s = state_;
  if (s.running && s.remainingMs >= 0) s.remainingMs = std::max<int64_t>(0, s.remainingMs - (now() - stateAt_));
  return s;
}

void Plugins::setState(const PluginState& s) {
  state_ = s;
  stateAt_ = now();
  haveState_ = true;
  for (size_t i = 0; i < list_.size(); i++) {
    Slot& sl = slots_[i];
    if (!list_[i].on) continue;
    if (!sl.dirty) {
      bool moved = !s.same(sl.sent);
      if (!moved && (s.remainingMs < 0) != (sl.sent.remainingMs < 0)) moved = true;
      if (!moved && s.remainingMs >= 0) {  // the clock was moved (a round restarted or skipped back into): the time left is news too
        const int64_t expected = sl.sent.running ? sl.sent.remainingMs - (now() - sl.sentAt) : sl.sent.remainingMs;
        if (std::llabs(s.remainingMs - expected) > 2500) moved = true;
      }
      if (moved) sl.dirty = true;
    }
    if (!sl.dirty) continue;
    if (list_[i].alive) tell(i);
    else if (s.running) start(i);  // it is started (again) when there is something new to say while a plan runs
  }
}

void Plugins::tell(size_t i) {
  Slot& sl = slots_[i];
  if (!sl.proc) return;
  const PluginState cur = current();  // one started or switched on in the middle of a round hears what is left of it now
  sl.proc->send(pluginLine(cur));
  sl.sent = cur;
  sl.sentAt = now();
  sl.dirty = false;
}

bool Plugins::start(size_t i, bool asked) {
  PluginInfo& p = list_[i];
  Slot& sl = slots_[i];
  if (p.alive) return true;
  if (!p.usable()) return false;
  const int64_t t = now();
  if (!asked) {
    if (sl.fails >= 3) return false;                                        // it keeps falling over: wait to be asked
    if (sl.endedAt && t - sl.endedAt < PLUGIN_RESTART_MS) return false;      // it has only just ended
  }
  wstring err;
  sl.gen = ++gen_;
  sl.startedAt = t;
  sl.proc = (spawn_ ? spawn_ : PluginSpawner(spawnPluginProcess))(p, sl.gen, sink_, &err);
  if (!sl.proc) {
    p.note = err.empty() ? L"Could not start" : err;
    sl.endedAt = t;
    sl.fails++;
    return false;
  }
  p.alive = true;
  p.note.clear();
  p.status.clear();
  sl.proc->send(pluginHello(version_));
  if (haveState_) tell(i);
  return true;
}

void Plugins::stop(size_t i) {
  Slot& sl = slots_[i];
  if (sl.proc) {
    sl.proc->stop();
    sl.proc.reset();
  }
  sl.gen = 0;  // whatever it still had on the way is no longer wanted
  sl.dirty = true;
  list_[i].alive = false;
  list_[i].status.clear();
}

void Plugins::openSettings(const string& id) {
  for (size_t i = 0; i < list_.size(); i++) {
    if (list_[i].id != id || !list_[i].on || !list_[i].settings) continue;
    if (!start(i, true)) return;
    slots_[i].proc->send("{\"event\":\"settings\"}");
    return;
  }
}

bool Plugins::onEvent(const PluginEvent& e) {
  for (size_t i = 0; i < list_.size(); i++) {
    Slot& sl = slots_[i];
    if (list_[i].id != e.id || sl.gen != e.gen || !sl.gen) continue;
    PluginInfo& p = list_[i];
    if (e.type == PluginEvent::Line) {
      wstring status;
      if (!parsePluginStatus(e.line, status) || status == p.status) return false;
      p.status = status;
      return true;
    }
    const int64_t t = now();
    const bool quick = t - sl.startedAt < 3000;
    stop(i);
    sl.endedAt = 0;
    if (e.code != 0) {
      sl.endedAt = t;  // only one that fell over is kept from coming straight back
      p.note = L"Stopped (exit code " + std::to_wstring(e.code) + L")";
      sl.fails = quick ? sl.fails + 1 : 0;
      mlog("plugin %s: ended with exit code %d", p.id.c_str(), e.code);
    } else {
      p.note.clear();
      sl.fails = 0;
      sl.dirty = false;  // it left by itself, knowing all there was to know: it comes back when something changes
    }
    return true;
  }
  return false;
}

void Plugins::shutdown() {
  for (size_t i = 0; i < list_.size(); i++) stop(i);
}
