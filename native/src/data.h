// Static catalogue: sounds, brainwave bands, session plans and scenes (mirrors the web app's data.js).
#pragma once
#include "common.h"

constexpr int NCH = 9;
constexpr int MAX_PRESETS = 24;
constexpr int NSCENES = 8;  // stock scenes: the rail, the mini player's row and the digit keys follow this

struct ChannelDef {
  const char* id;
  const wchar_t* name;
  const char* icon;
  const wchar_t* tip;
};
extern const ChannelDef CHANNELS[NCH];
int channelIndex(const string& id);

using Mix = std::array<int, NCH>;  // level 0..100 per channel, same order as CHANNELS
inline Mix emptyMix() { Mix m; m.fill(0); return m; }
bool operator==(const Mix& a, const Mix& b);

struct BandDef {
  const char* id;
  const wchar_t* sym;
  int hz;
};
constexpr int NBANDS = 5;
extern const BandDef BANDS[NBANDS];
int bandIndex(const string& id);

enum PhaseKind { PH_FOCUS = 0, PH_BREAK = 1, PH_TIMER = 2, PH_FREE = 3 };
struct PhaseDef {
  PhaseKind kind;
  int64_t ms;
};
struct PlanDef {
  string id;
  wstring label;
  vector<PhaseDef> phases;  // empty = open-ended
  int64_t fadeMs = 0;       // fade-out at the end of a timer
};
const PlanDef* findPlan(const string& id);

struct BeatDef {
  int band = 2;  // index into BANDS
  int level = 20;
  bool on = false;
};
bool operator==(const BeatDef& a, const BeatDef& b);

struct SceneDef {
  string id;
  wstring name;
  string icon;
  bool study = false;
  Mix mix = emptyMix();
  BeatDef beat;              // on=false when the scene has no beats by default
  vector<string> plans;      // first entry is the default
  vector<string> streams;    // YouTube ids of live streams, tried in order
  vector<string> hintInclude;  // title keywords used to find fresh stream ids
  vector<string> hintExclude;
  bool custom = false;       // a saved preset
  string base;               // for presets: the stock scene it came from
};
const vector<SceneDef>& stockScenes();
const SceneDef* findStock(const string& id);

extern const Mix BREAK_MIX;
