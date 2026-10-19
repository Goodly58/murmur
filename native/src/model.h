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

struct State {
  string scene = "focus";
  std::map<string, string> plans;   // chosen plan per scene or preset
  std::map<string, Mix> mixes;      // unsaved mix tweaks per scene or preset
  std::map<string, BeatDef> beats;  // unsaved beat tweaks
  vector<Preset> presets;
  int beatsMode = 1;                // 0 off, 1 binaural, 2 isochronic
  int master = 70, blend = 50;
  bool streamOn = true;
  string custom;                    // pinned stream spec from a pasted link ("" = none)
  string tab = "stream";            // "stream" or "files"
  std::map<string, double> log;     // yyyy-mm-dd -> focus minutes
  int winX = INT32_MIN, winY = INT32_MIN, winW = 0, winH = 0;
  bool maximized = false;
};

// A stream spec is a bare 11-character YouTube id, "list:<playlist id>", or "file:<path>".
bool validSpec(const string& s);
// Extracts a spec from a pasted YouTube link or bare id; "" if it is not one.
string parseYouTubeLink(const string& text);

class Model {
 public:
  State s;
  std::function<time_t()> nowFn = [] { return time(nullptr); };
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

  // focus statistics
  void addFocus(int64_t ms);
  double todayMinutes() const;
  std::array<double, 7> week() const;  // oldest first, today last
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
