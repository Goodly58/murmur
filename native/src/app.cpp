#include "app.h"

// Streams carry emoji and a date stamp in their titles; both are noise in a one-line label.
wstring cleanTitle(const wstring& in) {
  wstring out;
  for (size_t i = 0; i < in.size(); i++) {
    const wchar_t c = in[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < in.size()) {  // surrogate pair: astral symbols and emoji
      const uint32_t cp = 0x10000 + (((uint32_t)c - 0xD800) << 10) + ((uint32_t)in[i + 1] - 0xDC00);
      i++;
      if (cp >= 0x1F000 && cp <= 0x1FAFF) continue;
      out += c;
      out += in[i];
      continue;
    }
    if (c == 0xFE0F || c == 0x200D || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2B00 && c <= 0x2BFF)) continue;
    out += c;
  }
  // collapse spaces
  wstring o2;
  for (wchar_t c : out) if (!(c == L' ' && !o2.empty() && o2.back() == L' ')) o2 += c;
  while (!o2.empty() && o2.back() == L' ') o2.pop_back();
  while (!o2.empty() && o2.front() == L' ') o2.erase(o2.begin());
  // a trailing "2026-10-19 20:43" stamp
  if (o2.size() > 17) {
    const size_t n = o2.size();
    const wstring tail = o2.substr(n - 16);
    auto dg = [&](size_t k) { return tail[k] >= L'0' && tail[k] <= L'9'; };
    if (dg(0) && dg(1) && dg(2) && dg(3) && tail[4] == L'-' && dg(5) && dg(6) && tail[7] == L'-' && dg(8) && dg(9) && tail[10] == L' ' && dg(11) && dg(12) && tail[13] == L':' && dg(14) && dg(15)) {
      o2.resize(n - 16);
      while (!o2.empty() && o2.back() == L' ') o2.pop_back();
    }
  }
  return o2;
}

App::App(Engine& engine, Stream& st)
    : eng(engine), stream(st), session([this] {
        Session::Hooks h;
        h.onTick = [this](const SessionSnap& s) {
          applyFade(s.fade);
          if (onTick) onTick(); else changed();
        };
        h.onPhase = [this](const SessionSnap& s, PhaseKind) {
          eng.chime(s.kind == PH_BREAK ? 1 : 0);
          applyAudio(2.5);
          changed();
        };
        h.onDone = [this](const SessionSnap&) { eng.chime(2); finish(); };
        h.onFocus = [this](int64_t ms) {
          if (!model.scene().study) return;  // Calm and Sleep are not study time
          model.addFocus(ms);
          if (++focusTicks_ % 30 == 0) save();
        };
        return h;
      }()) {}

void App::init() {
  session.setPlan(model.planId());
  applyVolumes();
}

App::Effective App::effective() const {
  Effective e;
  e.levels = emptyMix();
  const SessionSnap snap = session.snapshot();
  if (snap.done) return e;
  if (snap.kind == PH_BREAK) { e.levels = BREAK_MIX; return e; }
  const BeatDef b = model.beatOf();
  if (model.s.beatsMode != 0 && b.on && b.level > 0) e.beat = BeatSpec{model.s.beatsMode, (double)BANDS[b.band].hz, b.level};
  e.levels = model.mixOf();
  return e;
}

App::Volumes App::volumes() const {
  const double m = muted ? 0 : model.s.master / 100.0, b = model.s.blend / 100.0;
  return {m, std::pow(clampd(2 * b, 0, 1), 1.5), std::pow(clampd(2 * (1 - b), 0, 1), 1.5)};
}

void App::applyAudio(double ramp) {
  const Effective e = effective();
  eng.setMix(e.levels, e.beat, ramp);
  if (onWake) onWake();
  changed();
}

void App::applyStreamVolume() {
  const Volumes v = volumes();
  stream.setVolume((int)std::lround(100 * std::pow(v.m, 1.5) * v.media * lastFade));
}

void App::applyVolumes() {
  const Volumes v = volumes();
  eng.setMaster(v.m);
  eng.setSoundMul(v.sound);
  applyStreamVolume();
  if (onWake) onWake();
}

void App::applyFade(double f) {
  if (closing || std::fabs(f - lastFade) < 0.002) return;
  lastFade = f;
  eng.setFade(f);
  applyStreamVolume();
}

// ---------------------------------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------------------------------

void App::startAll(double ramp) {
  closing = false;
  lastFade = 1;
  eng.setFade(1);
  const bool fresh = !running;
  running = true;
  session.start();
  applyVolumes();
  applyAudio(ramp);
  eng.play();
  if (fresh && pausedAt_ && wallMs() - pausedAt_ > 60000 && model.s.tab == "stream" && loadedSpec.compare(0, 5, "file:") != 0) {
    loadedSpec.clear();  // a live stream left paused for a while is far behind: reload to catch up
    streamState = SS_IDLE;
  }
  pausedAt_ = 0;
  if (model.s.tab == "files") {
    if (!files.empty()) {
      if (!filesLoaded_) { stream.loadFiles(files); filesLoaded_ = true; streamState = SS_LOADING; }
      else stream.setPaused(false);
    }
  } else {
    syncStream(true);
  }
  if (onWake) onWake();
  const SessionSnap snap = session.snapshot();
  if (fresh && snap.cycle && snap.kind == PH_FOCUS && snap.elapsed < 1500) eng.chime(0);
  changed();
}

void App::stopAll() {
  running = false;
  session.pause();
  eng.pause();
  stream.setPaused(true);
  pausedAt_ = wallMs();
  changed();
}

void App::togglePlay() {
  if (running) stopAll(); else startAll();
}

void App::prevPhase() {
  session.restartPhase();
  if (!running) startAll();
}

void App::nextPhase() {
  session.skip();
  if (!running && !session.snapshot().done) startAll();
}

void App::tick() { session.tick(); }

bool isAudioFile(const wstring& path) {
  wstring l = path;
  for (auto& c : l) c = (wchar_t)std::towlower(c);
  for (const wchar_t* ext : {L".mp3", L".m4a", L".aac", L".wav", L".flac", L".ogg", L".oga", L".opus", L".weba", L".wma", L".mka"}) {
    const size_t n = wcslen(ext);
    if (l.size() > n && l.compare(l.size() - n, n, ext) == 0) return true;
  }
  return false;
}

Launch parseLaunch(const vector<wstring>& args) {
  Launch l;
  for (size_t i = 0; i < args.size(); i++) {
    const wstring& a = args[i];
    if (a.empty()) continue;
    auto value = [&](const wchar_t* flag) -> string {  // "--scene sleep" or "--scene=sleep"
      const size_t n = wcslen(flag);
      if (a == flag) return i + 1 < args.size() ? narrow(args[++i]) : "";
      if (a.size() > n + 1 && a.compare(0, n, flag) == 0 && a[n] == L'=') return narrow(a.substr(n + 1));
      return "\x01";
    };
    string v;
    if ((v = value(L"--scene")) != "\x01") { l.scene = v; continue; }
    if ((v = value(L"--plan")) != "\x01") { l.plan = v; continue; }
    if (a == L"--play") { l.play = true; continue; }
    if (a == L"--mini") { l.mini = true; continue; }
    if (a == L"--full") { l.full = true; continue; }
    if (a[0] == L'-') continue;
    if (isAudioFile(a)) l.files.push_back(a);
    else if (!parseYouTubeLink(narrow(a)).empty()) l.links.push_back(a);
  }
  return l;
}

void App::applyLaunch(const Launch& l) {
  if (!l.scene.empty() && model.findScene(l.scene) && (l.scene != model.s.scene || (l.play && !running))) {
    enterScene(l.scene, running ? 1.4 : 0.15, running || l.play);  // while playing, a new scene keeps playing
  }
  if (!l.plan.empty()) selectPlan(l.plan);
  for (const wstring& link : l.links) pasteText(link);
  if (!l.files.empty()) addFiles(l.files);
  if (l.play && !running) startAll();
}

void App::outputLost() {
  if (!running) return;
  stopAll();
  showToast(L"Paused: the sound output was removed", L"", nullptr, 8000);
}

// Plan finished: fade everything out over four seconds, then stop.
void App::finish() {
  closing = true;
  closeStart_ = wallMs();
}

bool App::closeTick(int64_t nowMs) {
  if (!closing) return true;
  const double k = std::min(1.0, (double)(nowMs - closeStart_) / 4000.0);
  lastFade = 1 - k;
  eng.setFade(lastFade);
  applyStreamVolume();
  if (k >= 1) {
    closing = false;
    stopAll();
    lastFade = 1;
    eng.setFade(1);
    applyStreamVolume();
    changed();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------------
// Scenes and plans
// ---------------------------------------------------------------------------------------------------

void App::enterScene(const string& id, double ramp, bool start) {
  model.setScene(id);
  vIndex_ = vTried_ = 0;
  session.setPlan(model.planId());
  loadedSpec.clear();
  if (start) startAll(ramp); else applyAudio(ramp);
  changed();
  save();
}

void App::selectScene(const string& id) {
  if (!model.findScene(id) || (id == model.s.scene && running)) return;
  enterScene(id, running ? 1.4 : 0.15, true);
}

void App::selectPlan(const string& id) {
  const vector<string>& plans = model.scene().plans;
  if (std::find(plans.begin(), plans.end(), id) == plans.end()) return;
  model.setPlan(id);
  session.setPlan(id);
  if (running) { session.start(); applyAudio(1.2); }
  changed();
  save();
}

// ---------------------------------------------------------------------------------------------------
// Mix
// ---------------------------------------------------------------------------------------------------

void App::setLevel(int ch, int v) {
  model.setLevel(ch, v);
  if (!running) startAll(); else applyAudio(0.12);
  save();
}

void App::setBeatLevel(int v) {
  model.setBeatLevel(v);
  if (!running) startAll(); else applyAudio(0.12);
  save();
}

void App::toggleChannel(int ch) {
  model.toggleChannel(ch);
  if (!running) startAll(); else applyAudio(0.12);
  save();
}

void App::toggleBeat() {
  model.toggleBeat();
  if (!running) startAll(); else applyAudio(0.12);
  save();
}

void App::setBand(int band) {
  model.setBand(band);
  if (!running) startAll(); else applyAudio(0.2);
  save();
}

void App::resetMix() {
  if (model.resetMix()) {  // a preset also returns to its saved plan
    session.setPlan(model.planId());
    if (running) session.start();
  }
  if (running) applyAudio(0.4); else changed();
  save();
}

void App::setBeatsMode(int mode) {
  model.s.beatsMode = clampi(mode, 0, 2);
  if (running) applyAudio(0.3); else changed();
  save();
}

void App::cycleBeatsMode() { setBeatsMode((model.s.beatsMode + 1) % 3); }

void App::setMaster(int v) {
  model.s.master = clampi(v, 0, 100);
  muted = false;
  applyVolumes();
  changed();
  save();
}

void App::setBlend(int v) {
  model.s.blend = clampi(v, 0, 100);
  applyVolumes();
  changed();
  save();
}

void App::toggleMute() {
  muted = !muted;
  applyVolumes();
  changed();
}

// ---------------------------------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------------------------------

void App::presetAdd() {
  if (model.s.presets.size() >= (size_t)MAX_PRESETS) { showToast(L"Preset limit reached"); return; }
  const string id = model.savePreset();
  if (id.empty()) return;
  // the working copy moved into the preset: nothing audible changes, and the session keeps running
  changed();
  save();
}

void App::presetUpdate() {
  if (model.updatePreset()) { changed(); save(); }
}

void App::presetLoad(const string& id) { selectScene(id); }

void App::presetDelete(const string& id) {
  const Model::Deleted d = model.deletePreset(id);
  if (!d.ok) return;
  if (d.wasActive) enterScene(model.s.scene, running ? 1.4 : 0.15, running);
  else changed();
  save();
  showToast(L"Preset deleted", L"Undo", [this] { presetUndo(); });
}

void App::presetUndo() {
  const string id = model.undoDelete();
  toast = Toast();
  if (id.empty()) return;
  if (model.lastUndoWasActive()) enterScene(id, running ? 1.4 : 0.15, running);
  else changed();
  save();
}

void App::presetRename(const string& id, const wstring& name) {
  if (model.renamePreset(id, name)) { changed(); save(); }
}

// ---------------------------------------------------------------------------------------------------
// Toasts
// ---------------------------------------------------------------------------------------------------

void App::showToast(const wstring& msg, const wstring& action, std::function<void()> fn, int ms) {
  toast.msg = msg;
  toast.action = action;
  toast.onAction = std::move(fn);
  toast.until = wallMs() + ms;
  changed();
}

void App::expireToast(int64_t nowMs) {
  if (toast.active() && nowMs >= toast.until) { toast = Toast(); changed(); }
}

// ---------------------------------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------------------------------

// Streams to try for the current scene: freshly found ids first, then the built-in list. A preset that
// pins its own stream uses only that.
vector<string> App::candidates() const {
  vector<string> out;
  const SceneDef& sc = model.scene();
  auto add = [&](const string& s) {
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  };
  const Preset* p = sc.custom ? model.findPreset(sc.id) : nullptr;
  if (!(p && !p->video.empty())) {
    auto it = fresh.find(sc.custom ? sc.base : sc.id);
    if (it != fresh.end()) for (auto& s : it->second) add(s);
  }
  for (auto& s : sc.streams) add(s);
  return out;
}

string App::wantedSpec() const {
  if (!model.s.streamOn) return "";
  if (!model.s.custom.empty()) return model.s.custom;
  const vector<string> c = candidates();
  if (c.empty()) return "";
  return c[(size_t)vIndex_ % c.size()];
}

void App::syncStream(bool play) {
  const string w = wantedSpec();
  if (!model.s.streamOn || w.empty()) {
    if (!loadedSpec.empty()) stream.stop();
    loadedSpec.clear();
    streamState = (model.s.streamOn && w.empty()) ? SS_NONE : SS_IDLE;
    streamTitle.clear();
    changed();
    return;
  }
  if (streamState == SS_NONE) streamState = SS_IDLE;
  if (model.s.tab != "stream" || !play) { changed(); return; }
  if (w == loadedSpec && (streamState == SS_LOADING || streamState == SS_PLAYING)) {
    stream.setPaused(false);
    applyStreamVolume();
    return;
  }
  streamState = SS_LOADING;
  streamTitle.clear();
  loadedSpec = w;
  loadedGen_ = stream.load(w, videoWindow);
  applyStreamVolume();
  changed();
}

void App::onStreamEvent(const StreamEvent& e) {
  if (e.type == StreamEvent::Closed) {
    if (!running || loadedSpec.empty()) { loadedSpec.clear(); streamState = SS_IDLE; changed(); return; }
    if (videoWindow) {  // the video window was closed: carry on with sound only
      videoWindow = false;
      const string spec = loadedSpec;
      loadedSpec.clear();
      syncStream(true);
      (void)spec;
    } else {
      streamState = SS_ERROR;
      loadedSpec.clear();
      changed();
    }
    return;
  }
  if (e.gen != loadedGen_ && model.s.tab == "stream") return;  // an answer to an older request
  switch (e.type) {
    case StreamEvent::Playing:
      streamState = SS_PLAYING;
      streamTitle = cleanTitle(e.title);
      vTried_ = 0;
      applyStreamVolume();
      changed();
      break;
    case StreamEvent::Title:
      streamTitle = cleanTitle(e.title);
      changed();
      break;
    case StreamEvent::Index:
      fileIndex = e.index;
      changed();
      break;
    case StreamEvent::Failed: {
      if (e.reason == "no player" && !warnedNoPlayer_) {
        warnedNoPlayer_ = true;
        showToast(L"Streams and music need mpv.", L"Set up", [this] { setupStreaming(); }, 12000);
      }
      if (model.s.tab == "files") { streamState = SS_ERROR; changed(); break; }
      const size_t n = candidates().size();
      const bool noYtdlp = e.reason != "no player" && stream.ytdlpPath().empty();  // every stream would fail the same way
      if (e.reason == "no player" || noYtdlp || !model.s.custom.empty() || n == 0 || (size_t)(++vTried_) >= n) {
        streamState = SS_ERROR;
        loadedSpec.clear();
        if (noYtdlp && !warnedNoYtdlp_) {
          warnedNoYtdlp_ = true;
          showToast(L"Streams need yt-dlp.", L"Set up", [this] { setupStreaming(); }, 12000);
        } else if (e.reason != "no player" && !noYtdlp && model.s.custom.empty() && n > 0 && onNeedFresh) {
          onNeedFresh();  // the built-in ids may have gone stale
        }
        changed();
      } else {
        vIndex_++;
        loadedSpec.clear();
        syncStream(true);
      }
      break;
    }
    default: break;
  }
}

void App::stepStream(int dir) {
  if (model.s.tab == "files") {
    if (dir > 0) stream.next(); else stream.prev();
    return;
  }
  const size_t n = candidates().size();
  if (!model.s.custom.empty() || n < 2) return;
  vIndex_ = (int)(((size_t)vIndex_ + n + (size_t)(dir < 0 ? n - 1 : 1)) % n);
  vTried_ = 0;
  loadedSpec.clear();
  if (!running) startAll(); else syncStream(true);
}

void App::retryStream() {
  vTried_ = 0;
  loadedSpec.clear();
  if (running) syncStream(true); else startAll();
}

// ---------------------------------------------------------------------------------------------------
// Streaming tools: mpv plays, yt-dlp reads YouTube. A PC without them gets a one-click download.
// ---------------------------------------------------------------------------------------------------

int App::toolsMissing() const {
  int missing = 0;
  if (!stream.available()) missing |= TOOL_MPV;
  if (model.s.tab == "stream" && stream.ytdlpPath().empty()) missing |= TOOL_YTDLP;  // your own music needs only mpv
  return missing;
}

void App::setupStreaming() {
  const int need = toolsMissing();
  if (setupState == SETUP_RUNNING || !need) return;
  setupNeed_ = need;
  setupState = SETUP_RUNNING;
  setupStep = L"Starting";
  setupFraction = 0;
  setupError.clear();
  toast = Toast();
  changed();
  if (startSetup) startSetup(need);
  else onSetupDone(false, L"Setup isn't available here.");
}

void App::onSetupProgress(const wstring& step, double fraction) {
  if (setupState != SETUP_RUNNING) return;
  setupStep = step;
  setupFraction = clampd(fraction, 0, 1);
  changed();
}

void App::onSetupDone(bool ok, const wstring& error) {
  const bool still = ((setupNeed_ & TOOL_MPV) && !stream.available()) || ((setupNeed_ & TOOL_YTDLP) && stream.ytdlpPath().empty());
  if (ok && still) {
    mlog("setup: finished, but the tools were not found afterwards");
    ok = false;
  }
  if (!ok) {
    setupState = SETUP_FAILED;
    setupError = error.empty() ? L"Setup didn't finish." : error;
    showToast(setupError, L"Try again", [this] { setupStreaming(); }, 10000);
    changed();
    return;
  }
  setupState = SETUP_IDLE;
  setupError.clear();
  warnedNoPlayer_ = warnedNoYtdlp_ = false;
  showToast(L"Streaming is ready", L"", nullptr, 5000);
  // carry on where the failed start left off
  vTried_ = 0;
  loadedSpec.clear();
  streamState = SS_IDLE;
  if (model.s.tab == "files") {
    filesLoaded_ = false;
    if (running && !files.empty()) { stream.loadFiles(files); filesLoaded_ = true; streamState = SS_LOADING; }
  } else if (running) {
    syncStream(true);
  }
  changed();
}

void App::clearCustom() {
  model.s.custom.clear();
  vTried_ = 0;
  loadedSpec.clear();
  if (running) syncStream(true); else changed();
  save();
}

void App::toggleStreamOn() {
  model.s.streamOn = !model.s.streamOn;
  loadedSpec.clear();
  syncStream(running);
  save();
}

void App::toggleVideoWindow() {
  videoWindow = !videoWindow;
  if (running && streamState != SS_NONE && !loadedSpec.empty()) { loadedSpec.clear(); syncStream(true); }
  changed();
}

void App::pasteText(const wstring& text) {
  const string spec = parseYouTubeLink(narrow(text));
  if (spec.empty()) { showToast(L"That is not a YouTube link"); return; }
  model.s.custom = spec;
  model.s.streamOn = true;
  model.s.tab = "stream";
  vTried_ = 0;
  loadedSpec.clear();
  if (!running) startAll(); else syncStream(true);
  save();
}

void App::addFiles(const vector<wstring>& paths) {
  vector<wstring> ok;
  for (auto& p : paths) if (isAudioFile(p)) ok.push_back(p);
  if (ok.empty()) { showToast(L"No audio files in that drop"); return; }
  files.insert(files.end(), ok.begin(), ok.end());
  model.s.tab = "files";
  filesLoaded_ = false;
  stream.stop();
  loadedSpec.clear();
  if (!running) startAll();
  else { stream.loadFiles(files); filesLoaded_ = true; streamState = SS_LOADING; }
  changed();
  save();
}

void App::clearFiles() {
  files.clear();
  filesLoaded_ = false;
  stream.stop();
  streamState = SS_IDLE;
  changed();
}

void App::setTab(const string& tab) {
  if (tab == model.s.tab) return;
  model.s.tab = tab;
  if (tab == "files") {
    stream.stop();
    loadedSpec.clear();
    filesLoaded_ = false;
    if (running && !files.empty()) { stream.loadFiles(files); filesLoaded_ = true; streamState = SS_LOADING; }
  } else {
    stream.stop();
    filesLoaded_ = false;
    if (running) syncStream(true);
  }
  changed();
  save();
}
