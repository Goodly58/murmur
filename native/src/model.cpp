#include "model.h"

#include <ctime>
#include <random>

// ---------------------------------------------------------------------------------------------------
// Stream specs and links
// ---------------------------------------------------------------------------------------------------

static bool idChars(const string& s, size_t minLen, size_t maxLen) {
  if (s.size() < minLen || s.size() > maxLen) return false;
  for (unsigned char c : s) if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
  return true;
}

bool validSpec(const string& s) {
  if (s.compare(0, 5, "list:") == 0) return idChars(s.substr(5), 2, 64);
  return s.size() == 11 && idChars(s, 11, 11);
}

static string lower(string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static string queryParam(const string& query, const string& key) {
  size_t i = 0;
  while (i <= query.size()) {
    size_t e = query.find('&', i);
    if (e == string::npos) e = query.size();
    const string kv = query.substr(i, e - i);
    const size_t eq = kv.find('=');
    if (eq != string::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
    i = e + 1;
  }
  return "";
}

string parseYouTubeLink(const string& text) {
  string s = text;
  while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
  while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
  if (s.empty() || s.size() > 400) return "";
  if (s.size() == 11 && idChars(s, 11, 11)) return s;
  string u = s;
  const size_t scheme = u.find("://");
  if (scheme != string::npos) {
    const string sc = lower(u.substr(0, scheme));
    if (sc != "http" && sc != "https") return "";
    u = u.substr(scheme + 3);
  }
  const size_t slash = u.find_first_of("/?#");
  string host = lower(u.substr(0, slash));
  string rest = slash == string::npos ? "" : u.substr(slash);
  const size_t at = host.find('@');
  if (at != string::npos) return "";
  for (const char* pre : {"www.", "m.", "music."}) if (host.compare(0, strlen(pre), pre) == 0) host = host.substr(strlen(pre));
  const bool yt = host == "youtube.com" || host == "youtube-nocookie.com";
  if (!yt && host != "youtu.be") return "";
  const size_t hash = rest.find('#');
  if (hash != string::npos) rest = rest.substr(0, hash);
  string path = rest, query;
  const size_t q = rest.find('?');
  if (q != string::npos) { path = rest.substr(0, q); query = rest.substr(q + 1); }
  string v = queryParam(query, "v");
  if (host == "youtu.be") {
    string p = path.size() > 1 ? path.substr(1) : "";
    v = p.substr(0, p.find('/'));
  }
  for (const char* pre : {"/embed/", "/live/", "/shorts/", "/v/"}) {
    if (path.compare(0, strlen(pre), pre) == 0) v = path.substr(strlen(pre), 11);
  }
  if (v.size() == 11 && idChars(v, 11, 11)) return v;
  const string list = queryParam(query, "list");
  if (!list.empty() && idChars(list, 2, 64)) return "list:" + list;
  return "";
}

// ---------------------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------------------

namespace {

bool presetIdOk(const string& id) {
  if (id.size() < 3 || id.size() > 21 || id[0] != 'c') return false;
  for (size_t i = 1; i < id.size(); i++) if (!std::isalnum((unsigned char)id[i])) return false;
  return true;
}

Mix cleanMix(const Json* j) {
  Mix m = emptyMix();
  if (j && j->isObj()) {
    for (int i = 0; i < NCH; i++) {
      const Json* v = j->get(CHANNELS[i].id);
      if (v && v->isNum()) m[(size_t)i] = clampi((int)std::lround(v->n), 0, 100);
    }
  }
  return m;
}

Json mixJson(const Mix& m) {
  Json o = Json::obj();
  for (int i = 0; i < NCH; i++) if (m[(size_t)i] > 0) o.set(CHANNELS[i].id, Json::num(m[(size_t)i]));
  return o;
}

bool cleanBeat(const Json* j, BeatDef& out) {
  if (!j || !j->isObj()) return false;
  const Json* b = j->get("band");
  if (!b || !b->isStr()) return false;
  const int bi = bandIndex(b->s);
  if (bi < 0) return false;
  out.band = bi;
  const Json* l = j->get("level");
  out.level = l && l->isNum() ? clampi((int)std::lround(l->n), 0, 100) : 20;
  const Json* on = j->get("on");
  out.on = on && on->boolOr(false);
  return true;
}

Json beatJson(const BeatDef& b) {
  Json o = Json::obj();
  o.set("band", Json::str(BANDS[b.band].id));
  o.set("level", Json::num(b.level));
  o.set("on", Json::boolean(b.on));
  return o;
}

const char* modeName(int m) { return m == 0 ? "off" : (m == 1 ? "binaural" : "isochronic"); }

}  // namespace

bool Model::parse(const string& text, State& out) {
  bool ok = false;
  Json j = parseJson(text, &ok);
  State d;
  if (!ok || !j.isObj()) { out = d; return false; }

  struct Slot { string id; const SceneDef* base; };
  vector<Slot> slots;
  for (auto& sc : stockScenes()) slots.push_back({sc.id, &sc});
  if (const Json* pr = j.get("presets")) {
    if (pr->isArr()) {
      for (auto& pj : pr->a) {
        if (!pj.isObj() || d.presets.size() >= (size_t)MAX_PRESETS) continue;
        const Json* bj = pj.get("base");
        const SceneDef* base = bj && bj->isStr() ? findStock(bj->s) : nullptr;
        const Json* idj = pj.get("id");
        if (!base || !idj || !idj->isStr() || !presetIdOk(idj->s)) continue;
        bool dup = false;
        for (auto& sl : slots) if (sl.id == idj->s) dup = true;
        if (dup) continue;
        Preset p;
        p.id = idj->s;
        p.base = base->id;
        const Json* nm = pj.get("name");
        wstring name = nm && nm->isStr() ? widen(nm->s) : L"";
        while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
        while (!name.empty() && iswspace(name.back())) name.pop_back();
        if (name.size() > 24) name.resize(24);
        p.name = name.empty() ? base->name : name;
        const Json* pl = pj.get("plan");
        p.plan = base->plans[0];
        if (pl && pl->isStr()) for (auto& x : base->plans) if (x == pl->s) p.plan = x;
        p.mix = cleanMix(pj.get("mix"));
        BeatDef b;
        if (!cleanBeat(pj.get("beat"), b)) { b.band = bandIndex("alpha"); b.level = 20; b.on = false; }
        p.beat = b;
        const Json* vj = pj.get("video");
        if (vj && vj->isStr() && validSpec(vj->s) && vj->s.compare(0, 5, "file:") != 0) p.video = vj->s;
        d.presets.push_back(p);
        slots.push_back({p.id, base});
      }
    }
  }
  if (const Json* sc = j.get("scene")) {
    if (sc->isStr()) for (auto& sl : slots) if (sl.id == sc->s) d.scene = sc->s;
  }
  const Json* plans = j.get("plans");
  const Json* mixes = j.get("mixes");
  const Json* beats = j.get("beats");
  for (auto& sl : slots) {
    if (plans && plans->isObj()) {
      const Json* v = plans->get(sl.id);
      if (v && v->isStr()) for (auto& x : sl.base->plans) if (x == v->s) d.plans[sl.id] = x;
    }
    if (mixes && mixes->isObj()) {
      const Json* v = mixes->get(sl.id);
      if (v && v->isObj()) d.mixes[sl.id] = cleanMix(v);
    }
    if (beats && beats->isObj()) {
      BeatDef b;
      if (cleanBeat(beats->get(sl.id), b)) d.beats[sl.id] = b;
    }
  }
  if (const Json* v = j.get("beatsMode")) {
    if (v->isStr()) d.beatsMode = v->s == "off" ? 0 : (v->s == "isochronic" ? 2 : 1);
  }
  if (const Json* v = j.get("master")) d.master = clampi((int)std::lround(v->numOr(70)), 0, 100);
  if (const Json* v = j.get("blend")) d.blend = clampi((int)std::lround(v->numOr(50)), 0, 100);
  if (const Json* v = j.get("streamOn")) d.streamOn = v->boolOr(true);
  if (const Json* v = j.get("custom")) if (v->isStr() && validSpec(v->s) && v->s.compare(0, 5, "file:") != 0) d.custom = v->s;
  if (const Json* v = j.get("tab")) d.tab = v->strOr("stream") == "files" ? "files" : "stream";
  if (const Json* lg = j.get("log")) {
    if (lg->isObj()) {
      std::map<string, double> m;
      for (auto& kv : lg->o) {
        const string& k = kv.first;
        const bool shape = k.size() == 10 && k[4] == '-' && k[7] == '-' && std::isdigit((unsigned char)k[0]);
        if (shape && kv.second.isNum() && kv.second.n >= 0) m[k] = kv.second.n;
      }
      while (m.size() > 60) m.erase(m.begin());
      d.log = m;
    }
  }
  if (const Json* w = j.get("win")) {
    if (w->isObj()) {
      auto g = [&](const char* k, int def) { const Json* v = w->get(k); return v && v->isNum() ? (int)v->n : def; };
      d.winX = g("x", INT32_MIN);
      d.winY = g("y", INT32_MIN);
      d.winW = g("w", 0);
      d.winH = g("h", 0);
      const Json* mx = w->get("max");
      d.maximized = mx && mx->boolOr(false);
    }
  }
  if (const Json* mi = j.get("mini")) {
    if (mi->isObj()) {
      auto g = [&](const char* k, int def) { const Json* v = mi->get(k); return v && v->isNum() ? (int)v->n : def; };
      const Json* on = mi->get("on");
      d.mini = on && on->boolOr(false);
      d.miniX = g("x", INT32_MIN);
      d.miniY = g("y", INT32_MIN);
    }
  }
  out = std::move(d);
  return true;
}

string Model::serialize() const {
  Json j = Json::obj();
  j.set("scene", Json::str(s.scene));
  Json pr = Json::arr();
  for (auto& p : s.presets) {
    Json o = Json::obj();
    o.set("id", Json::str(p.id));
    o.set("name", Json::str(narrow(p.name)));
    o.set("base", Json::str(p.base));
    o.set("plan", Json::str(p.plan));
    o.set("mix", mixJson(p.mix));
    o.set("beat", beatJson(p.beat));
    o.set("video", Json::str(p.video));
    pr.push(o);
  }
  j.set("presets", pr);
  Json plans = Json::obj();
  for (auto& kv : s.plans) plans.set(kv.first, Json::str(kv.second));
  j.set("plans", plans);
  Json mixes = Json::obj();
  for (auto& kv : s.mixes) mixes.set(kv.first, mixJson(kv.second));
  j.set("mixes", mixes);
  Json beats = Json::obj();
  for (auto& kv : s.beats) beats.set(kv.first, beatJson(kv.second));
  j.set("beats", beats);
  j.set("beatsMode", Json::str(modeName(s.beatsMode)));
  j.set("master", Json::num(s.master));
  j.set("blend", Json::num(s.blend));
  j.set("streamOn", Json::boolean(s.streamOn));
  j.set("custom", Json::str(s.custom));
  j.set("tab", Json::str(s.tab));
  Json lg = Json::obj();
  for (auto& kv : s.log) lg.set(kv.first, Json::num(std::round(kv.second * 1000) / 1000));
  j.set("log", lg);
  Json w = Json::obj();
  w.set("x", Json::num(s.winX == INT32_MIN ? 0 : s.winX));
  w.set("y", Json::num(s.winY == INT32_MIN ? 0 : s.winY));
  w.set("w", Json::num(s.winW));
  w.set("h", Json::num(s.winH));
  w.set("max", Json::boolean(s.maximized));
  if (s.winX != INT32_MIN) j.set("win", w);
  Json mi = Json::obj();
  mi.set("on", Json::boolean(s.mini));
  if (s.miniX != INT32_MIN) {
    mi.set("x", Json::num(s.miniX));
    mi.set("y", Json::num(s.miniY));
  }
  j.set("mini", mi);
  return jsonString(j, 1);
}

// ---------------------------------------------------------------------------------------------------
// Scenes and presets
// ---------------------------------------------------------------------------------------------------

const vector<SceneDef>& Model::scenes() const {
  if (!cacheValid_) {
    cache_ = stockScenes();
    for (auto& p : s.presets) {
      const SceneDef* base = findStock(p.base);
      if (!base) continue;
      SceneDef sc = *base;
      sc.id = p.id;
      sc.name = p.name;
      sc.custom = true;
      sc.base = base->id;
      sc.mix = p.mix;
      sc.beat = p.beat;
      sc.plans.clear();
      sc.plans.push_back(p.plan);
      for (auto& x : base->plans) if (x != p.plan) sc.plans.push_back(x);
      if (!p.video.empty()) sc.streams = {p.video};
      cache_.push_back(std::move(sc));
    }
    cacheValid_ = true;
  }
  return cache_;
}

const SceneDef* Model::findScene(const string& id) const {
  for (auto& sc : scenes()) if (sc.id == id) return &sc;
  return nullptr;
}

const SceneDef& Model::scene() const {
  const SceneDef* sc = findScene(s.scene);
  return sc ? *sc : stockScenes()[0];
}

string Model::planId() const {
  auto it = s.plans.find(s.scene);
  return it != s.plans.end() ? it->second : scene().plans[0];
}

Mix Model::mixOf() const {
  auto it = s.mixes.find(s.scene);
  return it != s.mixes.end() ? it->second : scene().mix;
}

BeatDef Model::beatOf() const {
  auto it = s.beats.find(s.scene);
  return it != s.beats.end() ? it->second : scene().beat;
}

bool Model::planDirty() const {
  auto it = s.plans.find(s.scene);
  return it != s.plans.end() && it->second != scene().plans[0];
}

bool Model::presetDirty() const {
  return scene().custom && (s.mixes.count(s.scene) || s.beats.count(s.scene) || planDirty());
}

bool Model::mixModified() const {
  return s.mixes.count(s.scene) || s.beats.count(s.scene) || (scene().custom && planDirty());
}

void Model::setScene(const string& id) {
  if (findScene(id)) s.scene = id;
}

void Model::setPlan(const string& id) {
  for (auto& p : scene().plans) if (p == id) { s.plans[s.scene] = id; return; }
}

void Model::setLevel(int ch, int v) {
  Mix m = mixOf();
  m[(size_t)ch] = clampi(v, 0, 100);
  s.mixes[s.scene] = m;
}

void Model::setBeatLevel(int v) {
  BeatDef b = beatOf();
  b.level = clampi(v, 0, 100);
  b.on = b.level > 0;
  s.beats[s.scene] = b;
}

void Model::toggleChannel(int ch) {
  const int v = mixOf()[(size_t)ch];
  if (v > 0) lastLevel[(size_t)ch] = v;
  setLevel(ch, v > 0 ? 0 : (lastLevel[(size_t)ch] ? lastLevel[(size_t)ch] : 35));
}

void Model::toggleBeat() {
  if (s.beatsMode == 0) s.beatsMode = 1;
  BeatDef b = beatOf();
  b.on = !b.on;
  if (!b.level) b.level = 20;
  s.beats[s.scene] = b;
}

void Model::setBand(int band) {
  BeatDef b = beatOf();
  b.band = clampi(band, 0, NBANDS - 1);
  b.on = true;
  if (!b.level) b.level = 20;
  if (s.beatsMode == 0) s.beatsMode = 1;
  s.beats[s.scene] = b;
}

bool Model::resetMix() {
  s.mixes.erase(s.scene);
  s.beats.erase(s.scene);
  if (scene().custom && planDirty()) {
    s.plans.erase(s.scene);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------------------------------

string Model::autoName() const {
  const Mix m = mixOf();
  vector<int> idx;
  for (int i = 0; i < NCH; i++) if (m[(size_t)i] > 0) idx.push_back(i);
  std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return m[(size_t)a] > m[(size_t)b]; });
  wstring name;
  for (size_t k = 0; k < idx.size() && k < 2; k++) {
    if (k) name += L" + ";
    name += CHANNELS[idx[k]].name;
  }
  if (name.empty()) name = scene().name;
  if (name.size() > 24) name.resize(24);
  return narrow(name);
}

const Preset* Model::findPreset(const string& id) const {
  for (auto& p : s.presets) if (p.id == id) return &p;
  return nullptr;
}

static string newPresetId() {
  static std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
  const char* a = "0123456789abcdefghijklmnopqrstuvwxyz";
  string id = "c";
  uint64_t t = (uint64_t)wallMs();
  string b;
  while (t) { b += a[t % 36]; t /= 36; }
  std::reverse(b.begin(), b.end());
  id += b;
  for (int i = 0; i < 3; i++) id += a[rng() % 36];
  return id;
}

string Model::savePreset() {
  if (s.presets.size() >= (size_t)MAX_PRESETS) return "";
  const string src = s.scene;
  const bool srcCustom = scene().custom;
  Preset p;
  p.id = newPresetId();
  p.name = widen(autoName());
  p.base = scene().custom ? scene().base : scene().id;
  p.plan = planId();
  p.mix = mixOf();
  p.beat = beatOf();
  p.video = s.custom;
  // the working copy moves into the new preset; whatever it was based on returns to its own saved state
  s.mixes.erase(src);
  s.beats.erase(src);
  if (srcCustom) s.plans.erase(src);
  s.presets.push_back(p);
  dropCache();
  s.scene = p.id;
  return p.id;
}

bool Model::updatePreset() {
  Preset* p = nullptr;
  for (auto& x : s.presets) if (x.id == s.scene) p = &x;
  if (!p) return false;
  p->mix = mixOf();
  p->beat = beatOf();
  p->plan = planId();
  if (!s.custom.empty()) p->video = s.custom;
  s.mixes.erase(p->id);
  s.beats.erase(p->id);
  s.plans.erase(p->id);
  dropCache();
  return true;
}

Model::Deleted Model::deletePreset(const string& id) {
  Deleted d;
  size_t i = 0;
  for (; i < s.presets.size(); i++) if (s.presets[i].id == id) break;
  if (i >= s.presets.size()) return d;
  undo_ = Undo();
  undo_.valid = true;
  undo_.p = s.presets[i];
  undo_.index = i;
  undo_.active = s.scene == id;
  if (s.mixes.count(id)) { undo_.hasMix = true; undo_.mix = s.mixes[id]; }
  if (s.beats.count(id)) { undo_.hasBeat = true; undo_.beat = s.beats[id]; }
  if (s.plans.count(id)) undo_.plan = s.plans[id];
  s.presets.erase(s.presets.begin() + (long)i);
  s.mixes.erase(id);
  s.beats.erase(id);
  s.plans.erase(id);
  d.ok = true;
  d.wasActive = undo_.active;
  if (d.wasActive) s.scene = undo_.p.base;
  dropCache();
  return d;
}

string Model::undoDelete() {
  if (!undo_.valid) return "";
  undo_.valid = false;
  lastUndoActive_ = undo_.active;
  const size_t at = std::min(undo_.index, s.presets.size());
  s.presets.insert(s.presets.begin() + (long)at, undo_.p);
  if (undo_.hasMix) s.mixes[undo_.p.id] = undo_.mix;
  if (undo_.hasBeat) s.beats[undo_.p.id] = undo_.beat;
  if (!undo_.plan.empty()) s.plans[undo_.p.id] = undo_.plan;
  dropCache();
  return undo_.p.id;
}

bool Model::renamePreset(const string& id, const wstring& nameIn) {
  wstring name = nameIn;
  while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
  while (!name.empty() && iswspace(name.back())) name.pop_back();
  if (name.size() > 24) name.resize(24);
  if (name.empty()) return false;
  for (auto& p : s.presets) if (p.id == id) { p.name = name; dropCache(); return true; }
  return false;
}

// ---------------------------------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------------------------------

string Model::dayKey(time_t t) const {
  tm lt;
  localtime_s(&lt, &t);
  char b[16];
  snprintf(b, sizeof b, "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
  return b;
}

void Model::addFocus(int64_t ms) {
  s.log[dayKey(nowFn())] += (double)ms / 60000.0;
  while (s.log.size() > 60) s.log.erase(s.log.begin());
}

double Model::todayMinutes() const {
  auto it = s.log.find(dayKey(nowFn()));
  return it == s.log.end() ? 0 : it->second;
}

std::array<double, 7> Model::week() const {
  std::array<double, 7> w{};
  const time_t now = nowFn();
  for (int i = 6; i >= 0; i--) {
    auto it = s.log.find(dayKey(now - (time_t)i * 86400));
    w[(size_t)(6 - i)] = it == s.log.end() ? 0 : it->second;
  }
  return w;
}
