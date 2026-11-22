// A window-less rig: the real App, Ui and drawing code around an offline audio engine, a recording stream
// and a fake clock. Used by the snapshot renderer and by the UI self-tests.
#pragma once
#include <dxgiformat.h>

#include "ui.h"

// A stand-in for the `spotifast` command line: it keeps a player's state and answers the verbs the way the real program
// was seen to (a transfer does not start anything; after play-uri the old track shows as playing, then paused, before the
// new one is there; the device list leaves the built-in player out while it is the one playing).
struct FakeSpotifast {
  bool running = true;           // false: every verb fails, until it is launched
  int slowStart = 0;             // after a launch, this many `devices` calls still fail
  bool builtin = true, soloist = true;  // the devices it has
  string active;                 // "builtin", "soloist" or "" (none)
  string state = "paused";
  wstring title = L"Old song", artists = L"Someone", device = L"Spotifast Soloist (a1)";
  int64_t pos = 61000, dur = 200000;
  int volume = 60;
  string shuffle = "off", repeat = "track", art = "https://i.scdn.co/image/old";
  string uri;                    // what play-uri last asked for
  int settle = 3, pending = 0;   // looks at now-playing it takes for a new playlist to show; until then the old song does
  bool staleMoves = false;       // the old song shown meanwhile moves on instead of standing still
  bool sameSongAgain = false;    // the playlist begins with the very song that was there before
  bool playFails = false, transferFails = false, deafToTransfer = false, hideActiveBuiltin = false, silent = false;
  int launches = 0;
  vector<string> calls;          // every command line it was given

  static constexpr const char* BUILTIN_ID = "b111";
  static constexpr const char* SOLOIST_ID = "50101-57";

  bool launch() { launches++; running = true; return true; }
  int count(const string& prefix) const {
    int n = 0;
    for (auto& c : calls) if (c.compare(0, prefix.size(), prefix) == 0) n++;
    return n;
  }
  bool said(const string& line) const { return std::find(calls.begin(), calls.end(), line) != calls.end(); }
  int indexOf(const string& prefix) const {
    for (size_t i = 0; i < calls.size(); i++) if (calls[i].compare(0, prefix.size(), prefix) == 0) return (int)i;
    return -1;
  }
  int run(const vector<wstring>& args, string& out) {
    string line;
    for (auto& a : args) line += (line.empty() ? "" : " ") + narrow(a);
    calls.push_back(line);
    out.clear();
    if (!running || args.empty()) return 1;
    const wstring& v = args[0];
    const string arg = args.size() > 1 ? narrow(args[1]) : "";
    if (v == L"devices") {
      if (slowStart > 0) { slowStart--; return 1; }
      out = "[";
      bool first = true;
      auto add = [&](const char* id, const char* name, const char* kind, bool on) {
        out += string(first ? "" : ",") + "{\"active\":" + (on ? "true" : "false") + ",\"id\":\"" + id + "\",\"kind\":\"" + kind + "\",\"name\":\"" + name + "\"}";
        first = false;
      };
      if (builtin && !(hideActiveBuiltin && active == "builtin" && state == "playing")) add(BUILTIN_ID, "Spotifast", "Computer", active == "builtin");
      if (soloist) add(SOLOIST_ID, "Spotifast Soloist (a1)", "Speaker", active == "soloist");
      out += "]";
      return 0;
    }
    if (v == L"now-playing") {
      if (silent) return 0;  // nothing has ever played: it prints nothing
      if (pending > 0) {
        pending--;
        if (pending == 0) {
          state = "playing";
          pos = 400;
          if (!sameSongAgain) { title = L"New song"; artists = L"New artist"; dur = 180000; art = "https://i.scdn.co/image/new"; }
        } else {  // the song from before, shown as playing although the new one is on its way
          state = "playing";
          if (staleMoves) pos += 500;
        }
      } else if (state == "playing") {
        pos += 500;
      }
      out = state + "\t" + narrow(title) + "\t" + narrow(artists) + "\tAlbum\t" + std::to_string(pos) + "\t" + std::to_string(dur) + "\t" + std::to_string(volume) + "\t" + shuffle +
            "\t" + repeat + "\t" + art + "\tno\t" + narrow(device) + "\n";
      return 0;
    }
    if (v == L"transfer") {
      if (transferFails) return 1;
      if (!deafToTransfer) {
        active = arg == BUILTIN_ID ? "builtin" : "soloist";
        device = arg == BUILTIN_ID ? L"Spotifast" : L"Spotifast Soloist (a1)";
        pos = 0;
      }
      return 0;
    }
    if (v == L"play-uri") {
      if (playFails) return 1;
      uri = arg;
      if (active.empty()) { active = soloist ? "soloist" : "builtin"; device = soloist ? L"Spotifast Soloist (a1)" : L"Spotifast"; }
      pending = settle;
      silent = false;
      return 0;
    }
    if (v == L"pause") { state = "paused"; return 0; }
    if (v == L"play") { state = "playing"; return 0; }
    if (v == L"next" || v == L"previous") { title = v == L"next" ? L"Song after" : L"Song before"; pos = 0; return 0; }
    if (v == L"volume") { volume = std::atoi(arg.c_str()); return 0; }
    if (v == L"shuffle") { shuffle = arg; return 0; }
    if (v == L"repeat") { repeat = arg; return 0; }
    return 2;
  }
};

// Wires a Spotify player to the fake: no thread, no waiting, the caller's clock.
inline void wireFakeSpotify(Spotify& sp, FakeSpotifast& fake, std::function<int64_t()> clock) {
  sp.setInline(true);
  sp.setRunner([&fake](const vector<wstring>& a, string& out) { return fake.run(a, out); });
  sp.setLauncher([&fake] { return fake.launch(); });
  sp.setSleeper([](int) {});
  sp.setClock(std::move(clock));
  sp.configure(L"", L"Spotifast");
}

// Stand-ins for plugins: a scratch folder with real plugin.json files in it, and programs that only note what they are sent.
struct FakePlugins {
  struct Program {
    string id;
    int gen = 0;
    vector<string> lines;  // everything it was sent
    bool stopped = false;
  };
  struct Proc : PluginProc {
    std::shared_ptr<Program> p;
    void send(const string& line) override { p->lines.push_back(line); }
    void stop() override { p->stopped = true; }
  };
  wstring dir;
  vector<std::shared_ptr<Program>> programs;  // every program that was ever started, oldest first
  vector<wstring> made;                       // the folders and files written, for clear()
  bool refuse = false;                        // starting a program fails

  void wire(Plugins& host, const wstring& folder, std::function<int64_t()> clock) {
    dir = folder;
    host.configure(folder, "1.2.3");
    host.setClock(std::move(clock));
    host.setSpawner([this](const PluginInfo& info, int gen, PluginSink, wstring* error) -> std::unique_ptr<PluginProc> {
      if (refuse) {
        if (error) *error = L"Its program was not found";
        return nullptr;
      }
      auto prog = std::make_shared<Program>();
      prog->id = info.id;
      prog->gen = gen;
      programs.push_back(prog);
      auto proc = std::make_unique<Proc>();
      proc->p = prog;
      return proc;
    });
  }
  // Makes <dir>\<id>\plugin.json holding `json` ("" makes the folder alone).
  void write(const string& id, const string& json) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const wstring folder = dir + L"\\" + widen(id);
    if (CreateDirectoryW(folder.c_str(), nullptr)) made.push_back(folder);
    if (json.empty()) return;
    const wstring file = folder + L"\\plugin.json";
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, json.data(), (DWORD)json.size(), &wrote, nullptr);
    CloseHandle(f);
    made.push_back(file);
  }
  void remove(const string& id) {
    const wstring folder = dir + L"\\" + widen(id);
    DeleteFileW((folder + L"\\plugin.json").c_str());
    RemoveDirectoryW(folder.c_str());
  }
  void clear() {  // takes away everything write() made, and the folder itself if that leaves it empty
    for (size_t i = made.size(); i-- > 0;) {
      if (!DeleteFileW(made[i].c_str())) RemoveDirectoryW(made[i].c_str());
    }
    made.clear();
    DeleteFileW((dir + L"\\README.txt").c_str());
    RemoveDirectoryW(dir.c_str());
  }
  Program* last(const string& id) {  // the newest program started for that plugin
    for (size_t i = programs.size(); i-- > 0;) if (programs[i]->id == id) return programs[i].get();
    return nullptr;
  }
  int started(const string& id) const {
    int n = 0;
    for (auto& p : programs) if (p->id == id) n++;
    return n;
  }
  // What that plugin's running program would hand back: a line it wrote, or its ending.
  PluginEvent line(const string& id, const string& text) {
    PluginEvent e;
    e.id = id;
    e.line = text;
    if (Program* p = last(id)) e.gen = p->gen;
    return e;
  }
  PluginEvent exit(const string& id, int code) {
    PluginEvent e = line(id, "");
    e.type = PluginEvent::Exit;
    e.code = code;
    return e;
  }
};

struct Rig {
  Gfx g;
  Engine eng;
  Stream st;
  Spotify sp;
  FakeSpotifast fake;
  FakePlugins plug;  // not wired until a test asks for plugins (plug.wire)
  Spectrum spec;
  std::unique_ptr<App> app;
  std::unique_ptr<Ui> ui;
  int64_t clock = 5000000;
  time_t today = 1760000000;
  vector<std::pair<R, wstring>> edits;                                   // inline editors the Ui asked for
  std::function<void(int, const wstring&)> lastEditDone;  // call it with EDIT_ENTER, EDIT_BLUR or EDIT_CANCEL
  int invalidations = 0;

  bool init(float w = 1180, float h = 760, bool dark = true) {
    if (!g.init()) return false;
    eng.start(false, 48000);
    st.setDryRun(true);
    st.configure(L"mpv.exe", L"yt-dlp.exe");  // pretend both are installed; tests of the setup button take them away
    wireFakeSpotify(sp, fake, [this] { return clock; });
    app = std::make_unique<App>(eng, st, sp);
    sp.setSink([this](const SpotifyEvent& e) { app->onSpotifyEvent(e); });
    app->session.setClock([this] { return clock; });
    app->model.nowFn = [this] { return today; };
    app->init();
    ui = std::make_unique<Ui>(*app, g, spec);
    ui->setTheme(dark);
    ui->setSize(w, h);
    ui->hooks.invalidateAll = [this] { invalidations++; };
    ui->hooks.invalidate = [this](const R&) { invalidations++; };
    ui->hooks.edit = [this](const R& r, const wstring& t, int, bool, std::function<void(int, const wstring&)> done) {
      edits.push_back({r, t});
      lastEditDone = std::move(done);
    };
    app->onChange = [this] { ui->changed(); };
    app->onTick = [this] { ui->changed(); };
    return true;
  }
  void advance(int64_t ms) {
    clock += ms;
    app->tick();
    sp.pump();  // whatever of the fake Spotifast's has come due
  }
  void click(const Hit& h) {
    ui->mouseMove(h.r.cx(), h.r.cy(), false);
    ui->mouseDown(h.r.cx(), h.r.cy());
    ui->mouseUp(h.r.cx(), h.r.cy());
  }
  void click(HitKind k, int i = 0) { click(ui->find(k, i)); }
};

bool savePng(Gfx& g, IWICBitmap* bmp, const wstring& path);
int runSnapshot(int argc, wchar_t** argv);
int runMakeIcon(const wstring& outPath);
