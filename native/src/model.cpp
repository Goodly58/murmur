#include "model.h"

#include <ctime>
#include <random>

#include "plugins.h"
#include "spotify.h"

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

// A task as the file holds it: {"t": title, "d": ticked, "s": [{"t": step, "d": ticked}]}. Anything odd is dropped
// rather than rejected, so a hand-edited file never costs the rest of the settings.
Task cleanTask(const Json* j) {
  Task t;
  if (!j || !j->isObj()) return t;
  const Json* tx = j->get("t");
  t.text = tx && tx->isStr() ? cleanTaskText(widen(tx->s), TASK_TEXT) : L"";
  if (t.text.empty()) return Task();
  const Json* d = j->get("d");
  t.done = d && d->boolOr(false);
  const Json* sj = j->get("s");
  if (sj && sj->isArr()) {
    for (auto& s : sj->a) {
      if (t.steps.size() >= (size_t)TASK_STEPS || !s.isObj()) continue;
      const Json* st = s.get("t");
      TaskStep step;
      step.text = st && st->isStr() ? cleanTaskText(widen(st->s), STEP_TEXT) : L"";
      if (step.text.empty()) continue;
      const Json* sd = s.get("d");
      step.done = sd && sd->boolOr(false);
      t.steps.push_back(step);
    }
  }
  if (!t.steps.empty()) t.done = false;
  return t;
}

// The first version of the to-do kept a task for each round, as an array of four. The one task takes the first of those (with its
// steps) and any other of them as a step after that, while there is room, so a file from that version loses nothing it can hold.
Task foldRoundTasks(const Json* j) {
  Task out;
  if (!j || !j->isArr()) return out;
  for (auto& e : j->a) {
    const Task t = cleanTask(&e);
    if (t.empty()) continue;
    if (out.empty()) { out = t; continue; }
    if (out.steps.size() >= (size_t)TASK_STEPS) break;
    out.done = false;  // it has steps now, and they decide
    out.steps.push_back({cleanTaskText(t.text, STEP_TEXT), t.finished()});
  }
  return out;
}

Json taskJson(const Task& t) {
  Json o = Json::obj();
  if (t.empty()) return o;
  o.set("t", Json::str(narrow(t.text)));
  o.set("d", Json::boolean(t.done));
  Json steps = Json::arr();
  for (auto& s : t.steps) {
    Json so = Json::obj();
    so.set("t", Json::str(narrow(s.text)));
    so.set("d", Json::boolean(s.done));
    steps.push(so);
  }
  o.set("s", steps);
  return o;
}

}  // namespace

int Task::stepsDone() const {
  int n = 0;
  for (auto& s : steps) n += s.done ? 1 : 0;
  return n;
}

bool Task::finished() const {
  if (text.empty()) return false;
  return steps.empty() ? done : stepsDone() == (int)steps.size();
}

wstring cleanTaskText(const wstring& in, size_t maxLen) {
  wstring out;
  bool gap = false;
  for (wchar_t c : in) {
    if (c < 32 || c == 0x7f || std::iswspace(c)) { gap = !out.empty(); continue; }
    if (gap) { out += L' '; gap = false; }
    out += c;
  }
  if (out.size() > maxLen) {
    out.resize(maxLen);
    if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF) out.pop_back();  // do not cut an emoji in half
    while (!out.empty() && out.back() == L' ') out.pop_back();
  }
  return out;
}

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
  if (const Json* v = j.get("repeat")) d.repeat = v->boolOr(false);
  if (const Json* v = j.get("sets")) {  // 2 and up is a count; anything else, like a file from before the count, goes on until stopped
    const int n = (int)std::lround(clampd(v->numOr(0), 0, 1000));
    d.sets = n >= 2 ? std::min(n, SETS_MAX) : SETS_FOREVER;
  }
  if (const Json* tk = j.get("task")) d.task = cleanTask(tk);
  else if (const Json* old = j.get("tasks")) d.task = foldRoundTasks(old);  // a file from when every round had its own
  if (const Json* v = j.get("streamOn")) d.streamOn = v->boolOr(true);
  if (const Json* v = j.get("custom")) if (v->isStr() && validSpec(v->s) && v->s.compare(0, 5, "file:") != 0) d.custom = v->s;
  if (const Json* v = j.get("tab")) d.tab = v->strOr("stream") == "files" ? "files" : "stream";
  // {"private": true, "scenes": {"focus": {"uri": "spotify:playlist:...", "name": "...", "art": "https://..."}}}
  if (const Json* sp = j.get("spotify")) {
    if (sp->isObj()) {
      if (const Json* pv = sp->get("private")) d.spotifyPrivate = pv->boolOr(true);
      const Json* scenes = sp->get("scenes");
      if (scenes && scenes->isObj()) {
        for (auto& sl : slots) {
          const Json* a = scenes->get(sl.id);
          if (!a || !a->isObj()) continue;
          const Json *uri = a->get("uri"), *name = a->get("name"), *art = a->get("art");
          if (!uri || !uri->isStr() || !validSpotifyUri(uri->s)) continue;
          Attach at;
          at.uri = uri->s;
          if (name && name->isStr()) at.name = cleanTaskText(widen(name->s), 80);
          if (art && art->isStr() && art->s.compare(0, 8, "https://") == 0 && art->s.size() < 300) at.art = art->s;
          d.attach[sl.id] = at;
        }
      }
    }
  }
  // the plugins that are switched on: {"jolt": true}
  if (const Json* pl = j.get("plugins")) {
    if (pl->isObj()) for (auto& kv : pl->o) if (kv.second.boolOr(false) && validPluginId(kv.first) && d.plugins.size() < 32) d.plugins[kv.first] = true;
  }
  // where the plan was when the window closed: {"scene", "plan", "i", "lap", "ms", "at"}
  if (const Json* r = j.get("run")) {
    if (r->isObj()) {
      const Json *sc = r->get("scene"), *pl = r->get("plan"), *i = r->get("i"), *lap = r->get("lap"), *ms = r->get("ms"), *at = r->get("at");
      const PlanDef* plan = pl && pl->isStr() ? findPlan(pl->s) : nullptr;
      bool known = false;
      if (sc && sc->isStr()) for (auto& sl : slots) if (sl.id == sc->s) known = true;
      if (known && plan && !plan->phases.empty() && i && i->isNum() && ms && ms->isNum() && at && at->isNum() && at->n > 0) {
        RunMark m;
        m.scene = sc->s;
        m.plan = plan->id;
        m.index = clampi((int)i->n, 0, (int)plan->phases.size());  // one past the last is the break that closes a set
        m.lap = lap && lap->isNum() ? clampi((int)lap->n, 0, SETS_MAX) : 0;
        m.ms = (int64_t)clampd(ms->n, 0, 24 * 3600000.0);
        m.at = (int64_t)at->n;
        d.run = m;
      }
    }
  }
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
      d.miniH = clampi(g("h", 0), 0, 8000);
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
  j.set("repeat", Json::boolean(s.repeat));
  j.set("sets", Json::num(s.sets));
  j.set("task", taskJson(s.task));
  j.set("streamOn", Json::boolean(s.streamOn));
  j.set("custom", Json::str(s.custom));
  j.set("tab", Json::str(s.tab));
  Json sp = Json::obj();
  sp.set("private", Json::boolean(s.spotifyPrivate));
  Json spScenes = Json::obj();
  for (auto& kv : s.attach) {
    Json a = Json::obj();
    a.set("uri", Json::str(kv.second.uri));
    a.set("name", Json::str(narrow(kv.second.name)));
    a.set("art", Json::str(kv.second.art));
    spScenes.set(kv.first, a);
  }
  sp.set("scenes", spScenes);
  j.set("spotify", sp);
  if (!s.plugins.empty()) {  // a file from someone without plugins stays as it was
    Json pl = Json::obj();
    for (auto& kv : s.plugins) if (kv.second) pl.set(kv.first, Json::boolean(true));
    j.set("plugins", pl);
  }
  if (s.run.valid()) {
    Json r = Json::obj();
    r.set("scene", Json::str(s.run.scene));
    r.set("plan", Json::str(s.run.plan));
    r.set("i", Json::num(s.run.index));
    r.set("lap", Json::num(s.run.lap));
    r.set("ms", Json::num((double)s.run.ms));
    r.set("at", Json::num((double)s.run.at));
    j.set("run", r);
  }
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
    if (s.miniH > 0) mi.set("h", Json::num(s.miniH));
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
  // a timestamp and three random characters are unlikely to repeat, but presets saved in the same millisecond can,
  // so an id that a preset (or the one waiting to be undone) already has is simply drawn again
  do p.id = newId ? newId() : newPresetId(); while (findPreset(p.id) || (undo_.valid && undo_.p.id == p.id));
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
// Tasks
// ---------------------------------------------------------------------------------------------------

bool Model::setTask(const wstring& textIn) {
  const wstring text = cleanTaskText(textIn, TASK_TEXT);
  if (text.empty() || text == s.task.text) return false;
  s.task.text = text;
  return true;
}

bool Model::addStep(const wstring& textIn) {
  Task& t = s.task;
  const wstring text = cleanTaskText(textIn, STEP_TEXT);
  if (t.empty() || text.empty() || t.steps.size() >= (size_t)TASK_STEPS) return false;
  t.done = false;  // from now on the steps decide
  t.steps.push_back({text, false});
  return true;
}

bool Model::setStep(int i, const wstring& textIn) {
  Task& t = s.task;
  const wstring text = cleanTaskText(textIn, STEP_TEXT);
  if (i < 0 || (size_t)i >= t.steps.size() || text.empty() || text == t.steps[(size_t)i].text) return false;
  t.steps[(size_t)i].text = text;
  return true;
}

bool Model::removeStep(int i) {
  Task& t = s.task;
  if (i < 0 || (size_t)i >= t.steps.size()) return false;
  t.steps.erase(t.steps.begin() + i);
  if (t.steps.empty()) t.done = false;
  return true;
}

bool Model::tickTask() {
  Task& t = s.task;
  if (t.empty()) return false;
  if (t.steps.empty()) {
    t.done = !t.done;
  } else {
    const bool all = !t.finished();  // not finished: tick the rest; finished: untick everything
    for (auto& st : t.steps) st.done = all;
  }
  return true;
}

bool Model::tickStep(int i) {
  Task& t = s.task;
  if (i < 0 || (size_t)i >= t.steps.size()) return false;
  t.steps[(size_t)i].done = !t.steps[(size_t)i].done;
  return true;
}

Task Model::takeTask() {
  Task t = std::move(s.task);
  s.task = Task();
  return t;
}

bool Model::putTask(const Task& t) {
  if (t.empty() || !s.task.empty()) return false;
  s.task = t;
  return true;
}

bool Model::clearFinishedTask() {
  if (!s.task.finished()) return false;
  s.task = Task();
  return true;
}

// ---------------------------------------------------------------------------------------------------
// Spotify playlists
// ---------------------------------------------------------------------------------------------------

const Attach* Model::attached() const {
  auto it = s.attach.find(s.scene);
  return it == s.attach.end() ? nullptr : &it->second;
}

bool Model::setAttach(const string& uri, const wstring& name, const string& art) {
  if (!validSpotifyUri(uri)) return false;
  Attach a;
  a.uri = uri;
  a.name = cleanTaskText(name, 80);
  a.art = art;
  // a name already learnt for this playlist (on another scene, or here before) is better than none
  if (a.name.empty()) for (auto& kv : s.attach) if (kv.second.uri == uri && !kv.second.name.empty()) { a.name = kv.second.name; if (a.art.empty()) a.art = kv.second.art; }
  s.attach[s.scene] = a;
  return true;
}

bool Model::setAttachInfo(const string& uri, const wstring& nameIn, const string& art) {
  const wstring name = cleanTaskText(nameIn, 80);
  bool any = false;
  for (auto& kv : s.attach) {
    if (kv.second.uri != uri) continue;
    if (!name.empty() && kv.second.name != name) { kv.second.name = name; any = true; }
    if (!art.empty() && kv.second.art != art) { kv.second.art = art; any = true; }
  }
  return any;
}

Attach Model::takeAttach() {
  Attach a;
  auto it = s.attach.find(s.scene);
  if (it != s.attach.end()) {
    a = it->second;
    s.attach.erase(it);
  }
  return a;
}

// ---------------------------------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------------------------------

string Model::dayKey(time_t t) const {
  t -= (time_t)DAY_STARTS_AT * 3600;  // so 3:59 am still belongs to the evening before, and 4:00 am starts the day
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

static bool isDayKey(const string& k) {  // yyyy-mm-dd
  if (k.size() != 10 || k[4] != '-' || k[7] != '-') return false;
  for (size_t i = 0; i < k.size(); i++) if (i != 4 && i != 7 && !std::isdigit((unsigned char)k[i])) return false;
  return true;
}

double applyLogMoves(State& s, const string& json) {
  bool ok = false;
  const Json j = parseJson(json, &ok);
  const Json* moves = ok && j.isObj() ? j.get("moves") : nullptr;
  if (!moves || !moves->isArr()) return 0;
  double total = 0;
  for (const Json& m : moves->a) {
    if (!m.isObj()) continue;
    const Json *from = m.get("from"), *to = m.get("to"), *minutes = m.get("minutes");
    if (!from || !to || !minutes || !from->isStr() || !to->isStr() || !minutes->isNum()) continue;
    if (!isDayKey(from->s) || !isDayKey(to->s) || from->s == to->s || !(minutes->n > 0)) continue;
    auto it = s.log.find(from->s);
    if (it == s.log.end()) continue;
    const double take = std::min(minutes->n, it->second);
    it->second -= take;
    if (it->second < 0.0005) s.log.erase(it);  // nothing left: the day is simply not in the log
    s.log[to->s] += take;
    total += take;
  }
  while (s.log.size() > 60) s.log.erase(s.log.begin());
  return total;
}

double applyLogNote(State& s, const wstring& notePath) {
  HANDLE f = CreateFileW(notePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return 0;
  string note;
  char buf[4096];
  DWORD got = 0;
  while (note.size() < (1u << 20) && ReadFile(f, buf, sizeof buf, &got, nullptr) && got) note.append(buf, got);
  CloseHandle(f);
  if (!DeleteFileW(notePath.c_str())) return 0;  // what cannot be taken away must not be applied again at every start
  const double moved = applyLogMoves(s, note);
  mlog("log: moved %.1f minutes between days (log-moves.json)", moved);
  return moved;
}

double Model::todayMinutes() const {
  auto it = s.log.find(dayKey(nowFn()));
  return it == s.log.end() ? 0 : it->second;
}

int Model::streak() const {
  const time_t now = nowFn();
  auto worked = [&](int daysAgo) {
    auto it = s.log.find(dayKey(now - (time_t)daysAgo * 86400));
    return it != s.log.end() && it->second >= STREAK_MIN_MINUTES;
  };
  int n = 0, d = worked(0) ? 0 : 1;  // today may still be ahead: the run then stands as of yesterday
  while (n < 60 && worked(d)) { n++; d++; }
  return n;
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
