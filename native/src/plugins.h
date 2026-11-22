// Plugins: other programs that follow Murmur's sessions. A plugin is a folder in %APPDATA%\Murmur\plugins with a
// plugin.json that names a program to run. While a plugin is switched on, Murmur starts its program when a plan starts,
// writes what the session is doing to the program's standard input, one line of JSON per change, and reads short status
// lines back from its standard output. Murmur ships with no plugin, and nothing in the folder runs until it has been
// switched on in the Plugins sheet.
//
//   plugin.json   {"name": "Jolt", "about": "Zaps you when you drift", "run": ["pythonw", "jolt.pyw", "--murmur"], "settings": true}
//   to the plugin {"event":"hello","murmur":"1.0.0","protocol":1}
//                 {"event":"state","running":true,"phase":"focus","scene":"sprint","study":true,"plan":"p15","round":1,"rounds":4,"remaining":900}
//                 {"event":"settings"}                 (only when plugin.json says "settings": true)
//   from it       {"status":"2 zaps"}                  (shown under its name in the sheet)
//
// The program's folder is its working directory. When Murmur closes, or the plugin is switched off, the program's input
// ends; it has a moment to leave by itself and is ended if it does not. It may also leave whenever it likes (to cost
// nothing between sessions): it is started again the next time a plan runs.
#pragma once
#include "common.h"

constexpr int PLUGIN_PROTOCOL = 1;
constexpr int PLUGINS_MAX = 8;             // as many as the sheet has rows for
constexpr size_t PLUGIN_LINE_MAX = 2000;   // a longer line from a plugin is not a message
constexpr size_t PLUGIN_STATUS_MAX = 40;   // characters of a status that are shown
constexpr int64_t PLUGIN_RESTART_MS = 5000;  // a program that just ended is not started again sooner than this

struct PluginInfo {
  string id;              // its folder's name
  wstring name, about;    // from plugin.json; the name falls back to the id
  wstring dir;            // the folder
  vector<wstring> run;    // the program and its arguments
  bool settings = false;  // it has a settings window of its own, which Murmur can ask it to show
  wstring error;          // why it cannot be used ("" when it can), e.g. no "run" in its plugin.json
  // while Murmur runs
  bool on = false;        // switched on (kept in state.json)
  bool alive = false;     // its program is running
  wstring status;         // the last status it sent
  wstring note;           // what went wrong the last time it ran ("" when nothing did)
  bool usable() const { return error.empty(); }
};

// What a plugin is told about the session. `phase` is where the plan stands whether or not the clock is going:
// "focus", "break", "timer" (Calm and Sleep), "free" (the open-ended plan) or "done".
struct PluginState {
  bool running = false;       // the clock is going (false while paused, before the first start, and once the plan is over)
  string phase = "focus";
  string scene, plan;
  bool study = false;         // a study scene (Focus, Sprint, Read, ...) rather than Calm or Sleep
  int round = 0, rounds = 0;  // the round the plan is in and how many it has (0 for a plan without rounds)
  int64_t remainingMs = -1;   // left of this phase; -1 when it has no end
  bool same(const PluginState& o) const {  // everything but the time left
    return running == o.running && phase == o.phase && scene == o.scene && plan == o.plan && study == o.study && round == o.round && rounds == o.rounds;
  }
};

string pluginHello(const string& version);
string pluginLine(const PluginState& s);
// Reads a plugin.json into name, about, run and settings. False (and `error` says why) when it cannot be used.
bool parsePluginManifest(const string& json, PluginInfo& out);
// The status a plugin wrote, cleaned up for the sheet; false when the line is not a status.
bool parsePluginStatus(const string& line, wstring& status);
bool validPluginId(const string& id);

// What comes back from a plugin's program. It arrives on that program's reader thread.
struct PluginEvent {
  enum Type { Line, Exit } type = Line;
  string id;
  int gen = 0;   // which start of the program this belongs to
  string line;   // Line: what it wrote
  int code = 0;  // Exit: its exit code
};

// A running plugin program. The real one is a child process with pipes; tests supply their own.
struct PluginProc {
  virtual ~PluginProc() = default;
  virtual void send(const string& line) = 0;  // never waits for the program
  virtual void stop() = 0;                    // its input ends; if it is still there a moment later it is ended
};
using PluginSink = std::function<void(const PluginEvent&)>;
using PluginSpawner = std::function<std::unique_ptr<PluginProc>(const PluginInfo& p, int gen, PluginSink sink, wstring* error)>;
std::unique_ptr<PluginProc> spawnPluginProcess(const PluginInfo& p, int gen, PluginSink sink, wstring* error);

// Everything here is called on one thread (the window's); only the sink is called from elsewhere.
class Plugins {
 public:
  Plugins();
  ~Plugins();

  void configure(const wstring& dir, const string& version = "");  // where the plugin folders are; it need not exist yet
  const wstring& dir() const { return dir_; }
  bool ensureFolder();                              // makes the folder, with a note on how plugins work, if it is not there
  void setSink(PluginSink s) { sink_ = std::move(s); }
  void setSpawner(PluginSpawner s) { spawn_ = std::move(s); }
  void setClock(std::function<int64_t()> c) { clock_ = std::move(c); }

  // Reads the folder again. `on` says which ids are switched on; a running program whose plugin is gone is stopped.
  void rescan(const std::map<string, bool>& on);
  const vector<PluginInfo>& list() const { return list_; }
  const PluginInfo* find(const string& id) const;
  bool anyOn() const;

  bool setOn(const string& id, bool on);            // false when there is no such plugin, or it cannot be used
  void setState(const PluginState& s);              // tells the running ones what changed, and starts the ones that are on once a plan runs
  void openSettings(const string& id);              // starts it if need be and asks for its settings window
  bool onEvent(const PluginEvent& e);               // a line it wrote, or its program ended; true when something to show changed
  void shutdown();

 private:
  struct Slot {
    std::unique_ptr<PluginProc> proc;
    int gen = 0;              // 0 while it is not running: nothing that arrives then is its
    int64_t startedAt = 0, endedAt = 0;
    int fails = 0;            // times in a row it fell over within moments of starting
    bool dirty = true;        // the session has changed since the program was last told
    PluginState sent;         // what it was last told, and when
    int64_t sentAt = 0;
  };
  bool start(size_t i, bool asked = false);  // asked: the listener did something (a switch, a button), so no holding back
  void stop(size_t i);
  void tell(size_t i);
  PluginState current() const;  // the state as it is this moment: the time left has run on since it was noted
  int64_t now() const { return clock_ ? clock_() : wallMs(); }

  wstring dir_;
  string version_ = "1.0.0";
  vector<PluginInfo> list_;
  vector<Slot> slots_;        // one per entry of list_
  PluginState state_;         // as the app last described it, at stateAt_
  int64_t stateAt_ = 0;
  bool haveState_ = false;
  int gen_ = 0;
  PluginSink sink_;
  PluginSpawner spawn_;
  std::function<int64_t()> clock_;
};
