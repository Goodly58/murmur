#include "stream.h"

#include <shlobj.h>

#include "json.h"
#include "net.h"
#include "setup.h"

namespace {
wstring quoted(const wstring& s) { return L"\"" + s + L"\""; }

string urlFor(const string& spec) {
  if (spec.compare(0, 5, "list:") == 0) return "https://www.youtube.com/playlist?list=" + spec.substr(5);
  if (spec.compare(0, 5, "file:") == 0) return spec.substr(5);
  return "https://www.youtube.com/watch?v=" + spec;
}
}  // namespace

Stream::Stream() {
  job_ = CreateJobObjectW(nullptr, nullptr);  // mpv dies with us, even if we crash
  if (job_) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
  }
  watchdog_ = std::thread([this] { watchdogLoop(); });
}

Stream::~Stream() {
  shutdown();
  if (watchdog_.joinable()) watchdog_.join();
  if (job_) CloseHandle(job_);
}

vector<wstring> Stream::searchDirs() {
  vector<wstring> dirs = {L"C:\\mpv", L"C:\\Program Files\\mpv", L"C:\\Program Files (x86)\\mpv", L"C:\\tools\\mpv", L"C:\\ProgramData\\chocolatey\\bin"};
  wchar_t la[MAX_PATH];
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) dirs.push_back(wstring(la) + L"\\Programs\\mpv");
  const wstring tools = toolsDir();  // what the player's Set up button downloads
  if (!tools.empty()) {
    dirs.push_back(tools + L"\\mpv");
    dirs.push_back(tools);
  }
  return dirs;
}

bool Stream::configure() {
  vector<wstring> dirs = searchDirs();
  wstring mpv, ytdlp;
  wchar_t env[MAX_PATH];
  if (GetEnvironmentVariableW(L"MURMUR_MPV", env, MAX_PATH)) mpv = env;
  if (mpv.empty()) mpv = findTool(L"mpv.exe", dirs);
  if (!mpv.empty()) {
    const size_t cut = mpv.find_last_of(L"\\/");
    if (cut != wstring::npos) dirs.push_back(mpv.substr(0, cut));
  }
  ytdlp = findTool(L"yt-dlp.exe", dirs);
  configure(mpv, ytdlp);
  return !mpv.empty();
}

void Stream::configure(const wstring& mpv, const wstring& ytdlp) {
  std::lock_guard<std::mutex> g(mu_);  // the player is looked for again after a setup, while the stream may be in use
  mpv_ = mpv;
  ytdlp_ = ytdlp;
}

void Stream::emit(const StreamEvent& e) {
  std::function<void(const StreamEvent&)> s;
  {
    std::lock_guard<std::mutex> g(sinkMu_);
    s = sink_;
  }
  if (s) s(e);
}

void Stream::killProcess() {
  // caller holds mu_
  procId_++;
  if (pipe_) {
    CancelIoEx(pipe_, nullptr);
    CloseHandle(pipe_);
    pipe_ = nullptr;
  }
  if (proc_) {
    TerminateProcess(proc_, 0);
    CloseHandle(proc_);
    proc_ = nullptr;
  }
}

bool Stream::ensureProcess(bool video) {
  // caller holds mu_
  if (proc_ && pipe_ && video == video_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT) return true;
  killProcess();
  if (mpv_.empty()) return false;
  static std::atomic<int> seq{0};
  const wstring pipeName = L"\\\\.\\pipe\\murmur-mpv-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++seq);
  wstring cl = quoted(mpv_) + L" --no-config --idle=yes --no-terminal --really-quiet --input-ipc-server=" + pipeName +
               L" --volume=" + std::to_wstring(volume_) + L" --volume-max=100 --audio-client-name=Murmur" +
               L" --cache=yes --cache-secs=20 --demuxer-max-bytes=16MiB --demuxer-max-back-bytes=4MiB --ytdl=yes";
  if (!ytdlp_.empty()) cl += L" " + quoted(L"--script-opts=ytdl_hook-ytdl_path=" + ytdlp_);
  wchar_t extra[512];
  if (GetEnvironmentVariableW(L"MURMUR_MPV_EXTRA", extra, 512)) cl += L" " + wstring(extra);  // test hook, e.g. --ao=null
  if (video) cl += L" --force-window=immediate --autofit=480x270 --osd-level=0 --title=Murmur --ytdl-format=bestvideo[height<=360]+bestaudio/best[height<=360]/best";
  else cl += L" --vid=no --audio-display=no --force-window=no --ytdl-format=bestaudio/best";
  STARTUPINFOW si = {};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
    mlog("stream: cannot start mpv (%lu)", GetLastError());
    return false;
  }
  if (job_) AssignProcessToJobObject(job_, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  proc_ = pi.hProcess;

  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 100 && pipe == INVALID_HANDLE_VALUE; i++) {
    pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(proc_, 40) == WAIT_OBJECT_0) break;
    }
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    mlog("stream: mpv pipe never appeared");
    killProcess();
    return false;
  }
  pipe_ = pipe;
  video_ = video;
  const int pid = ++procId_;
  readers_++;
  std::thread([this, pipe, pid] { readerLoop(pipe, pid); }).detach();
  // properties we want to hear about
  send("{\"command\":[\"observe_property\",1,\"media-title\"]}");
  send("{\"command\":[\"observe_property\",2,\"playlist-pos\"]}");
  return true;
}

void Stream::send(const string& json) {
  // caller holds mu_
  commands.push_back(json);
  if (commands.size() > 200) commands.erase(commands.begin(), commands.begin() + 100);
  if (dry_ || !pipe_) return;
  string line = json + "\n";
  OVERLAPPED ov = {};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD wrote = 0;
  if (!WriteFile(pipe_, line.data(), (DWORD)line.size(), nullptr, &ov)) {
    if (GetLastError() == ERROR_IO_PENDING) {
      if (WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0) CancelIoEx(pipe_, &ov);
      GetOverlappedResult(pipe_, &ov, &wrote, TRUE);
    }
  }
  CloseHandle(ov.hEvent);
}

static string cmdJson(std::initializer_list<Json> args) {
  Json c = Json::arr();
  for (auto& a : args) c.push(a);
  Json o = Json::obj();
  o.set("command", c);
  return jsonString(o);
}

int Stream::load(const string& spec, bool video) {
  int gen;
  bool ok = true;
  {
    std::lock_guard<std::mutex> g(mu_);
    gen = ++gen_;
    spec_ = spec;
    loaded_ = false;
    title_.clear();
    paused_ = false;
    if (dry_) {
      video_ = video;
      commands.push_back("load " + spec + (video ? " video" : ""));
    } else {
      ok = ensureProcess(video);
      if (ok) {
        send(cmdJson({Json::str("set_property"), Json::str("pause"), Json::boolean(false)}));
        send(cmdJson({Json::str("loadfile"), Json::str(urlFor(spec)), Json::str("replace")}));
        deadline_ = wallMs() + 25000;
        cv_.notify_all();
      }
    }
  }
  StreamEvent e;
  e.spec = spec;
  e.gen = gen;
  if (ok) e.type = StreamEvent::Loading;
  else { e.type = StreamEvent::Failed; e.reason = "no player"; }
  emit(e);
  return gen;
}

int Stream::loadFiles(const vector<wstring>& files) {
  if (files.empty()) return gen_.load();
  int gen;
  bool ok = true;
  const string first = "file:" + narrow(files[0]);
  {
    std::lock_guard<std::mutex> g(mu_);
    gen = ++gen_;
    spec_ = first;
    loaded_ = false;
    title_.clear();
    paused_ = false;
    if (dry_) {
      commands.push_back("files " + std::to_string(files.size()) + " " + first);
    } else {
      ok = ensureProcess(false);
      if (ok) {
        send(cmdJson({Json::str("set_property"), Json::str("pause"), Json::boolean(false)}));
        for (size_t i = 0; i < files.size(); i++) {
          send(cmdJson({Json::str("loadfile"), Json::str(narrow(files[i])), Json::str(i == 0 ? "replace" : "append")}));
        }
        deadline_ = wallMs() + 15000;
        cv_.notify_all();
      }
    }
  }
  StreamEvent e;
  e.spec = first;
  e.gen = gen;
  if (ok) e.type = StreamEvent::Loading;
  else { e.type = StreamEvent::Failed; e.reason = "no player"; }
  emit(e);
  return gen;
}

void Stream::setPaused(bool p) {
  std::lock_guard<std::mutex> g(mu_);
  paused_ = p;
  send(cmdJson({Json::str("set_property"), Json::str("pause"), Json::boolean(p)}));
}

void Stream::setVolume(int v) {
  std::lock_guard<std::mutex> g(mu_);
  volume_ = clampi(v, 0, 100);
  send(cmdJson({Json::str("set_property"), Json::str("volume"), Json::num(volume_)}));
}

void Stream::stop() {
  std::lock_guard<std::mutex> g(mu_);
  spec_.clear();
  loaded_ = false;
  deadline_ = 0;
  send(cmdJson({Json::str("stop")}));
}

void Stream::next() { std::lock_guard<std::mutex> g(mu_); send(cmdJson({Json::str("playlist-next")})); }
void Stream::prev() { std::lock_guard<std::mutex> g(mu_); send(cmdJson({Json::str("playlist-prev")})); }

void Stream::shutdown() {
  {
    std::lock_guard<std::mutex> g(mu_);
    quit_ = true;
    if (pipe_ && !dry_) send(cmdJson({Json::str("quit")}));
    killProcess();
    cv_.notify_all();
  }
  for (int i = 0; i < 200 && readers_.load() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void Stream::readerLoop(HANDLE pipe, int procId) {
  OVERLAPPED ov = {};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  char buf[4096];
  string acc;
  for (;;) {
    ResetEvent(ov.hEvent);
    DWORD got = 0;
    BOOL ok = ReadFile(pipe, buf, sizeof buf, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) break;
    if (!GetOverlappedResult(pipe, &ov, &got, TRUE) || !got) break;
    acc.append(buf, got);
    size_t p;
    while ((p = acc.find('\n')) != string::npos) {
      handleLine(acc.substr(0, p), procId);
      acc.erase(0, p + 1);
    }
  }
  CloseHandle(ov.hEvent);
  bool report = false;
  int gen = 0;
  {
    std::lock_guard<std::mutex> g(mu_);
    report = procId == procId_ && !quit_;
    gen = gen_.load();
  }
  if (report) {
    StreamEvent e;
    e.type = StreamEvent::Closed;
    e.gen = gen;
    emit(e);
  }
  readers_--;
}

void Stream::handleLine(const string& line, int procId) {
  bool ok = false;
  Json j = parseJson(line, &ok);
  if (!ok || !j.isObj()) return;
  const Json* ev = j.get("event");
  if (!ev || !ev->isStr()) return;
  StreamEvent e;
  bool fire = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    if (procId != procId_) return;
    e.gen = gen_.load();
    e.spec = spec_;
    if (ev->s == "property-change") {
      const Json* n = j.get("name");
      const Json* d = j.get("data");
      if (n && n->strOr("") == "media-title" && d && d->isStr()) {
        title_ = widen(d->s);
        if (loaded_) { e.type = StreamEvent::Title; e.title = title_; fire = true; }
      } else if (n && n->strOr("") == "playlist-pos" && d && d->isNum()) {
        e.type = StreamEvent::Index;
        e.index = (int)d->n;
        fire = spec_.compare(0, 5, "file:") == 0;
      }
    } else if (ev->s == "file-loaded") {
      loaded_ = true;
      deadline_ = 0;
      e.type = StreamEvent::Playing;
      e.title = title_;
      fire = !spec_.empty();
    } else if (ev->s == "end-file") {
      const Json* r = j.get("reason");
      const string reason = r ? r->strOr("") : "";
      if (reason == "error" || (reason == "eof" && loaded_ && spec_.compare(0, 5, "file:") != 0)) {
        const Json* fe = j.get("file_error");
        e.type = StreamEvent::Failed;
        e.reason = reason == "eof" ? "ended" : (fe ? fe->strOr("error") : "error");
        deadline_ = 0;
        loaded_ = false;
        fire = !spec_.empty();
      }
    }
  }
  if (fire) emit(e);
}

void Stream::watchdogLoop() {
  std::unique_lock<std::mutex> lk(mu_);
  while (!quit_) {
    if (deadline_ == 0) { cv_.wait(lk); continue; }
    const int64_t now = wallMs();
    if (now < deadline_) { cv_.wait_for(lk, std::chrono::milliseconds(deadline_ - now)); continue; }
    deadline_ = 0;
    if (!loaded_ && !spec_.empty() && !dry_) {
      StreamEvent e;
      e.type = StreamEvent::Failed;
      e.reason = "timeout";
      e.spec = spec_;
      e.gen = gen_.load();
      lk.unlock();
      emit(e);
      lk.lock();
    }
  }
}
