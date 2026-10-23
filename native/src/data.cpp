#include "data.h"

const ChannelDef CHANNELS[NCH] = {
    {"brown", L"Brown", "i-brown", L"Brown noise: deep rumble"},
    {"pink", L"Pink", "i-pink", L"Pink noise: soft and balanced"},
    {"white", L"White", "i-white", L"White noise: bright hiss"},
    {"speech", L"Mask", "i-bubble", L"Speech-band noise: blurs nearby voices"},
    {"rain", L"Rain", "i-rain", L"Rain"},
    {"ocean", L"Ocean", "i-ocean", L"Ocean swells"},
    {"wind", L"Wind", "i-wind", L"Wind"},
    {"fire", L"Fire", "i-fire", L"Fireplace crackle"},
    {"cabin", L"Cabin", "i-cabin", L"Cabin hum: engine drone"},
};

int channelIndex(const string& id) {
  for (int i = 0; i < NCH; i++) if (id == CHANNELS[i].id) return i;
  return -1;
}

bool operator==(const Mix& a, const Mix& b) {
  for (int i = 0; i < NCH; i++) if (a[i] != b[i]) return false;
  return true;
}

const BandDef BANDS[NBANDS] = {
    {"delta", L"δ", 2}, {"theta", L"θ", 6}, {"alpha", L"α", 10}, {"beta", L"β", 16}, {"gamma", L"γ", 40},
};

int bandIndex(const string& id) {
  for (int i = 0; i < NBANDS; i++) if (id == BANDS[i].id) return i;
  return -1;
}

bool operator==(const BeatDef& a, const BeatDef& b) { return a.band == b.band && a.level == b.level && a.on == b.on; }

static vector<PhaseDef> cycle(int focusMin, int breakMin, int rounds) {
  vector<PhaseDef> out;
  for (int i = 0; i < rounds; i++) {
    out.push_back({PH_FOCUS, (int64_t)focusMin * 60000});
    if (i < rounds - 1) out.push_back({PH_BREAK, (int64_t)breakMin * 60000});
  }
  return out;
}

static vector<PlanDef> buildPlans() {
  vector<PlanDef> p;
  p.push_back({"p25", L"25·5", cycle(25, 5, 4), 0});
  p.push_back({"p50", L"50·10", cycle(50, 10, 3), 0});
  p.push_back({"p15", L"15·3", cycle(15, 3, 4), 0});
  p.push_back({"free", L"∞", {}, 0});
  auto timer = [](int min) { return vector<PhaseDef>{{PH_TIMER, (int64_t)min * 60000}}; };
  p.push_back({"t10", L"10m", timer(10), 30000});
  p.push_back({"t20", L"20m", timer(20), 30000});
  p.push_back({"t30", L"30m", timer(30), 60000});
  p.push_back({"t60", L"60m", timer(60), 60000});
  p.push_back({"t90", L"90m", timer(90), 60000});
  return p;
}

const PlanDef* findPlan(const string& id) {
  static const vector<PlanDef> plans = buildPlans();
  for (auto& p : plans) if (p.id == id) return &p;
  return nullptr;
}

static Mix mk(std::initializer_list<std::pair<const char*, int>> l) {
  Mix m = emptyMix();
  for (auto& kv : l) m[(size_t)channelIndex(kv.first)] = kv.second;
  return m;
}

const Mix BREAK_MIX = mk({{"ocean", 34}, {"wind", 12}, {"rain", 10}});

static vector<SceneDef> buildScenes() {
  vector<SceneDef> v;
  auto add = [&](const char* id, const wchar_t* name, const char* icon, bool study, Mix mix, int band, int level, bool beat,
                 vector<string> plans, vector<string> streams, vector<string> inc, vector<string> exc) {
    SceneDef s;
    s.id = id; s.name = name; s.icon = icon; s.study = study; s.mix = mix;
    s.beat.band = band; s.beat.level = level; s.beat.on = beat;
    s.plans = std::move(plans); s.streams = std::move(streams);
    s.hintInclude = std::move(inc); s.hintExclude = std::move(exc);
    v.push_back(std::move(s));
  };
  add("focus", L"Focus", "i-target", true, mk({{"brown", 45}, {"pink", 18}}), bandIndex("beta"), 22, true,
      {"p25", "p50", "free"}, {"rFZHOHl-L8A", "5yx6BWlEVcY", "3PFJ9SETS4M", "0muHFBSiybw", "1Tl2FtV06qo"},
      {"lofi hip hop radio", "chillhop radio", "lofi house", "summer lofi", "asian lofi"}, {"sleep", "christmas", "halloween", "sad"});
  // Sprint: quick, intense work that is not mentally heavy (email, admin, data entry, repetitive sets). A light
  // noise bed under music with a steady pulse, in the order of the ranking: house and techno, synthwave, liquid
  // drum and bass, video-game study radio, lofi house. Beta beats (16 Hz) are the band that improved vigilance in
  // Lane et al. 1998. Short 15 minute rounds keep the pace up.
  add("sprint", L"Sprint", "i-bolt", true, mk({{"pink", 14}, {"brown", 8}}), bandIndex("beta"), 24, true,
      {"p15", "p25", "free"}, {"8BzYJsT1FhA", "UYOb37KRFqk", "4xDzrJKXOOY", "g6yxzlbV2gI", "Tb2PjZ54jG0", "3PFJ9SETS4M"},
      {"luxury vocal house", "minimal techno deep radio", "synthwave radio", "liquid drum bass radio",
       "video game study lounge", "lofi house radio"},
      {"sleep", "christmas", "halloween"});
  add("read", L"Read", "i-book", true, mk({{"pink", 36}, {"rain", 16}}), bandIndex("alpha"), 20, false,
      {"p50", "p25", "free"}, {"jXAEIWcGXwE", "N0snMcR6aaA", "E_XmwjgRLz8"},
      {"classical music radio", "piano radio", "guitar radio"}, {});
  add("create", L"Create", "i-bulb", true, mk({{"pink", 38}, {"wind", 18}, {"rain", 22}}), bandIndex("alpha"), 20, true,
      {"p25", "p50", "free"}, {"4xDzrJKXOOY", "A8jDx9TLMQc", "E2vONfzoyRI", "CwPCy1GLS38"},
      {"synthwave radio", "relaxing jazz", "jazz lofi radio", "sad lofi radio"}, {});
  add("mask", L"Mask", "i-shield", true, mk({{"speech", 50}, {"brown", 20}}), bandIndex("alpha"), 20, false,
      {"free", "p25", "p50"}, {}, {}, {});
  add("white", L"White", "i-bars", true, mk({{"white", 34}, {"pink", 20}}), bandIndex("alpha"), 20, false,
      {"free", "p25", "p50"}, {}, {}, {});
  add("calm", L"Calm", "i-leaf", false, mk({{"ocean", 40}, {"wind", 14}, {"rain", 10}}), bandIndex("alpha"), 20, false,
      {"t10", "t20", "free"}, {"q_4KI-ChIIs", "JK0pBKoK5FQ", "GSfT7H87zq4"},
      {"fireplace ambience", "synth ambient radio"}, {"halloween", "christmas"});
  add("sleep", L"Sleep", "i-moon", false, mk({{"ocean", 46}, {"brown", 22}}), bandIndex("alpha"), 20, false,
      {"t60", "t30", "t90", "free"}, {"nI725iVsyoQ", "xORCbIptqcc", "VAlMDl00mYY", "JD-kMIpDfnY"},
      {"deep sleep", "sleep ambient", "sleep lofi", "beats to sleep"}, {});
  return v;
}

const vector<SceneDef>& stockScenes() {
  static const vector<SceneDef> s = buildScenes();
  return s;
}

const SceneDef* findStock(const string& id) {
  for (auto& s : stockScenes()) if (s.id == id) return &s;
  return nullptr;
}
