// The app's data model: persistent state, scene/preset resolution, mix editing and focus statistics.
// Pure logic with no window and no audio, so it can be tested directly.
#pragma once
#include "common.h"
#include "data.h"
#include "json.h"

struct Preset {
  string id;
  wstring name;
  string base;   // stock scene it came from
  string plan;
  Mix mix = emptyMix();
  BeatDef beat;
  string video;  // stream spec to pin ("" = use the scene's streams)
};

// The to-do: ONE task for the whole plan, all its rounds together (25·5 is four rounds and one task). It can be split into up
// to four steps, but only the task matters: whoever wants no steps writes none, and no step belongs to a round.
constexpr int TASK_STEPS = 4;
constexpr size_t TASK_TEXT = 60, STEP_TEXT = 48;  // the longest title and step, in characters

struct TaskStep {
  wstring text;
  bool done = false;
};

struct Task {
  wstring text;                // empty: nothing is planned
  bool done = false;           // only for a task without steps; with steps, they decide
  vector<TaskStep> steps;
  bool empty() const { return text.empty(); }
  int stepsDone() const;
  bool finished() const;       // the task, or every one of its steps, is ticked
};

// A title or a step as typed: white space and control characters become single spaces, trimmed, at most `maxLen`.
wstring cleanTaskText(const wstring& in, size_t maxLen);

// A Spotify playlist (or album, or artist) tied to a scene: while it is there it plays instead of the scene's live streams.
struct Attach {
  string uri;    // "spotify:playlist:<id>"
  wstring name;  // what the playlist is called, once known
  string art;    // its cover image URL, once known
};

// Where a plan was when the window closed, so a restart a few minutes later carries on from there instead of from the top.
constexpr int64_t RUN_MARK_FRESH_MS = 30 * 60000;  // older than this and the plan simply starts over
struct RunMark {
  string scene, plan;
  int index = 0, lap = 0;  // the phase it was in and the sets already finished
  int64_t ms = 0;          // how far into that phase
  int64_t at = 0;          // when this was noted (wall clock, ms); 0 = nothing to pick up
  bool valid() const { return at != 0; }
};

// Days in a row count once this much focus is on them.
constexpr double STREAK_MIN_MINUTES = 10;

// A day runs from 4 am to 4 am, not from midnight: a late night still counts for the day it began, and the "today" in the
// corner starts again in the small hours instead of in the middle of a session.
constexpr int DAY_STARTS_AT = 4;  // hour of the day, local time

struct State {
  string scene = "focus";
  std::map<string, string> plans;   // chosen plan per scene or preset
  std::map<string, Mix> mixes;      // unsaved mix tweaks per scene or preset
  std::map<string, BeatDef> beats;  // unsaved beat tweaks
  vector<Preset> presets;
  int beatsMode = 1;                // 0 off, 1 binaural, 2 isochronic
  int master = 70, blend = 50;
  bool repeat = false;              // loop the rounds of a plan like 25·5 instead of stopping after the last
  int sets = SETS_FOREVER;          // with the loop on: how many times the plan runs (2 and up), or until it is stopped
  Task task;                        // the one task for the plan, whichever plan it is
  bool streamOn = true;
  string custom;                    // pinned stream spec from a pasted link ("" = none)
  string tab = "stream";            // "stream" or "files"
  std::map<string, Attach> attach;  // a Spotify playlist per scene or preset, in place of its live streams
  bool spotifyPrivate = true;       // play Spotify only where Spotify does not record it (Spotifast's built-in player)
  std::map<string, bool> plugins;   // the plugins that are switched on, by their folder's name (see plugins.h)
  RunMark run;                      // where the plan was when the window last closed
  std::map<string, double> log;     // yyyy-mm-dd -> focus minutes
  int winX = INT32_MIN, winY = INT32_MIN, winW = 0, winH = 0;
  bool maximized = false;
  bool mini = false;                // the compact always-on-top player
  int miniX = INT32_MIN, miniY = INT32_MIN;
  int miniH = 0;                    // how tall its window was there, in pixels (it changes with the task; 0 = not known)
};

// Moves minutes of focus time from one day to another in the log, for correcting a day after the day boundary moved.
// `json` is {"moves": [{"from": "2026-11-08", "to": "2026-11-07", "minutes": 80.4}]}; a move never takes more than the
// day has, entries that make no sense are skipped, and the log is kept to 60 days. Returns how many minutes moved.
double applyLogMoves(State& s, const string& json);
// The same from a note left by hand in the data folder (log-moves.json). The note is deleted before it is used, so a crash in
// between can never apply it twice; if it cannot be deleted it is not used at all. Returns how many minutes moved.
double applyLogNote(State& s, const wstring& notePath);

// A stream spec is a bare 11-character YouTube id, "list:<playlist id>", or "file:<path>".
bool validSpec(const string& s);
// Extracts a spec from a pasted YouTube link or bare id; "" if it is not one.
string parseYouTubeLink(const string& text);

class Model {
 public:
  State s;
  std::function<time_t()> nowFn = [] { return time(nullptr); };
  std::function<string()> newId;  // for tests: where a new preset's id comes from (unset: a timestamp and random characters)
  std::array<int, NCH> lastLevel{};

  static bool parse(const string& json, State& out);
  string serialize() const;
  void load(const string& json) {
    State t;
    if (parse(json, t)) s = std::move(t);
    dropCache();
  }

  // scenes and presets share one list: a preset is a scene-like slot
  const vector<SceneDef>& scenes() const;
  const SceneDef& scene() const;
  const SceneDef* findScene(const string& id) const;
  void dropCache() const { cacheValid_ = false; }

  string planId() const;
  Mix mixOf() const;
  BeatDef beatOf() const;
  bool planDirty() const;
  bool presetDirty() const;   // the active preset has unsaved changes
  bool mixModified() const;   // shows the reset button

  void setScene(const string& id);
  void setPlan(const string& id);
  void setLevel(int ch, int v);
  void setBeatLevel(int v);
  void toggleChannel(int ch);
  void toggleBeat();
  void setBand(int band);
  bool resetMix();            // true if the plan went back too (the session must follow)

  // presets
  string autoName() const;
  string savePreset();        // new preset id, or "" at the limit
  bool updatePreset();
  struct Deleted { bool ok = false, wasActive = false; };
  Deleted deletePreset(const string& id);
  string undoDelete();        // restored preset id, or ""
  bool hasUndo() const { return undo_.valid; }
  bool lastUndoWasActive() const { return lastUndoActive_; }
  void clearUndo() { undo_.valid = false; }
  bool renamePreset(const string& id, const wstring& name);
  const Preset* findPreset(const string& id) const;

  // the task: each returns false when it changed nothing
  bool setTask(const wstring& text);                       // a new task, or a new title for the one there
  bool addStep(const wstring& text);                       // needs a task, some text and fewer than four steps
  bool setStep(int i, const wstring& text);
  bool removeStep(int i);
  bool tickTask();                                         // with steps, ticks them all, or unticks them all when they are
  bool tickStep(int i);
  Task takeTask();                                         // empties it and hands back what it held
  bool putTask(const Task& t);                             // only when there is no task
  bool clearFinishedTask();                                // true when it was finished and is gone

  // the Spotify playlist of the scene that is open (nullptr: it plays its live streams)
  const Attach* attached() const;
  bool setAttach(const string& uri, const wstring& name, const string& art);  // false when the link is not a playlist's
  bool setAttachInfo(const string& uri, const wstring& name, const string& art);  // names every scene that has this one
  Attach takeAttach();                                                       // detaches it and hands back what it was

  // focus statistics
  void addFocus(int64_t ms);
  double todayMinutes() const;
  std::array<double, 7> week() const;  // oldest first, today last
  // Days in a row with focus on them, up to today. A day not yet worked does not break the run: it counts from yesterday.
  int streak() const;
  string dayKey(time_t t) const;

 private:
  struct Undo {
    bool valid = false, hasMix = false, hasBeat = false, active = false;
    Preset p;
    size_t index = 0;
    Mix mix;
    BeatDef beat;
    string plan;
  } undo_;
  bool lastUndoActive_ = false;
  mutable vector<SceneDef> cache_;
  mutable bool cacheValid_ = false;
};
