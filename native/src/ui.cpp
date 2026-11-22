// Layout and input. Painting lives in ui_paint.cpp.
#include "ui.h"

namespace {
TextStyle T(float size, int weight, Color c, int align = TX_LEFT) {
  TextStyle s;
  s.size = size;
  s.weight = weight;
  s.color = c;
  s.align = align;
  return s;
}
}  // namespace

wstring fmtClock(int64_t ms, bool up) {
  if (ms >= INF_MS) return L"--:--";
  const int64_t s = std::max<int64_t>(0, up ? ms / 1000 : (ms + 999) / 1000);
  const int h = (int)(s / 3600), m = (int)((s % 3600) / 60), x = (int)(s % 60);
  wchar_t buf[32];
  if (h) _snwprintf(buf, 32, L"%d:%02d:%02d", h, m, x);
  else _snwprintf(buf, 32, L"%02d:%02d", m, x);
  return buf;
}

wstring fmtSpan(int64_t ms) {
  const int64_t mins = std::max<int64_t>(0, (ms + 30000) / 60000);  // to the nearest minute
  const int h = (int)(mins / 60), m = (int)(mins % 60);
  if (h == 0) return std::to_wstring(m) + L" min";
  return std::to_wstring(h) + L" h" + (m ? L" " + std::to_wstring(m) + L" min" : L"");
}

BalanceWords balanceWords(int blend, bool streamTab) {
  BalanceWords w;
  w.left = streamTab ? L"Video" : L"Music";
  w.right = L"Sounds";
  const wstring leftLower = streamTab ? L"video" : L"music";
  if (blend <= 2) { w.lean = w.left + L" only"; w.side = -1; }
  else if (blend < 48) { w.lean = L"More " + leftLower; w.side = -1; }
  else if (blend <= 52) { w.lean = L"Even"; w.side = 0; }
  else if (blend < 98) { w.lean = L"More sounds"; w.side = 1; }
  else { w.lean = L"Sounds only"; w.side = 1; }
  return w;
}

Ui::Ui(App& a, Gfx& gfx, Spectrum& s) : app(a), g(gfx), spec(s) {}

void Ui::add(HitKind k, int i, const R& r, const wstring& tip) {
  if (r.w <= 0 || r.h <= 0) return;
  Hit h;
  h.k = k;
  h.i = i;
  h.r = r;
  h.tip = tip;
  hits_.push_back(std::move(h));
}

Hit Ui::find(HitKind k, int i) {
  ensureLayout();
  for (auto& h : hits_) if (h.k == k && h.i == i) return h;
  return Hit();
}

Hit Ui::hitAt(float x, float y) {
  ensureLayout();
  for (size_t i = hits_.size(); i-- > 0;) if (hits_[i].r.has(x, y)) return hits_[i];
  return Hit();
}

string Ui::posterIdFor() const {
  if (mini_ || !app.model.s.streamOn || app.model.s.tab != "stream") return "";
  const string w = app.wantedSpec();
  if (isSpotifySpec(w)) {  // the cover of the song that plays, or the playlist's own until one does
    const Attach* a = app.model.attached();
    const string url = app.spotifyOn && !app.spNow.art.empty() ? app.spNow.art : (a ? a->art : string());
    return url.empty() ? "" : "s:" + url;
  }
  return w.size() == 11 ? w : "";
}

bool Ui::spotifyCard() const {
  return !mini_ && app.model.s.tab == "stream" && app.model.s.streamOn && app.spotifyWanted();
}

// While a playlist plays the lock says where it really plays; before that, where it is going to.
bool Ui::shownPrivate() const {
  return app.spotifyOn && app.streamState == SS_PLAYING ? app.spPrivate : app.model.s.spotifyPrivate;
}

void Ui::setPoster(const string& id, Com<IWICBitmap> bmp, bool failed) {
  poster_ = Poster();
  poster_.id = id;
  poster_.wic = bmp;
  poster_.failed = failed;
  if (!L.player.w) layoutDirty_ = true;
  invalidate(L.player.inset(-2));
}

ID2D1Bitmap* Ui::posterBitmap() {
  const string want = posterIdFor();
  if (!poster_.wic) return nullptr;
  // one song's cover stays up until the next one's has arrived; a video's thumbnail is never shown for another video
  const bool covers = want.compare(0, 2, "s:") == 0 && poster_.id.compare(0, 2, "s:") == 0;
  if (poster_.id != want && !covers) return nullptr;
  if (poster_.owner != g.rt) {
    poster_.d2d.reset();
    g.rt->CreateBitmapFromWicBitmap(poster_.wic.get(), nullptr, poster_.d2d.put());
    poster_.owner = g.rt;
  }
  return poster_.d2d.get();
}

// ---------------------------------------------------------------------------------------------------
// Layout. All the geometry is computed here, in DIPs, and every clickable thing is registered as a hit
// in paint order (later entries are on top).
// ---------------------------------------------------------------------------------------------------

void Ui::build() {
  layoutDirty_ = false;
  hits_.clear();
  L = Layout();
  tv = TaskLayout();
  if (sheet_ == 3 && !app.tasksOn()) sheet_ = 0;  // the scene changed under an open Task sheet to one without tasks
  if (mini_) {
    buildMini();
    return;
  }
  const float W = W_, H = H_;
  const Model& m = app.model;
  const SessionSnap snap = app.session.snapshot();
  const bool brk = !snap.done && snap.kind == PH_BREAK;
  const Palette& P = g.pal;

  const float by = 100;
  const float sideW = (float)clampd(0.22 * W, 396, 520);
  L.top = {0, 0, W, 56};
  L.presets = {0, 56, W, 44};
  L.rail = {0, by, 232, H - by};
  L.side = {W - sideW, by, sideW, H - by};
  L.stage = {232, by, W - 232 - sideW, H - by};

  // ---- top bar
  float x = W - 16;
  L.keys = {x - 32, 12, 32, 32};
  x = L.keys.x - 14;
  L.info = {x - 32, 12, 32, 32};
  x = L.info.x - 14;
  L.miniBtn = {x - 32, 12, 32, 32};
  x = L.miniBtn.x - 14;
  L.plugBtn = {x - 32, 12, 32, 32};
  x = L.plugBtn.x - 14;
  L.master = {x - 112, 18, 112, 20};
  L.mute = {L.master.x - 4 - 32, 12, 32, 32};
  x = L.mute.x - 14;
  L.beatSeg = {x - 106, 11, 106, 34};
  for (int i = 0; i < 3; i++) L.beatBtn[i] = {L.beatSeg.x + 3 + i * 34, L.beatSeg.y + 3, 32, 28};

  // ---- presets bar
  const TextStyle pn = T(12.5f, 500, P.text);
  TextStyle lab = T(12, 500, P.text3);
  lab.spacing = .02f;
  const float labelW = g.measure(L"Custom", lab);
  const float px0 = 20 + 16 + 7 + labelW + 14;
  const bool upd = m.presetDirty();
  const float pEnd = W - 16 - (upd ? 46 : 0);
  L.pClip = {px0, 56, std::max(0.f, pEnd - px0), 44};
  float cx = px0 - presetScroll_;
  for (size_t i = 0; i < m.s.presets.size(); i++) {
    const Preset& p = m.s.presets[i];
    const bool on = p.id == m.s.scene;
    float tw;
    if (editing_ == p.id) tw = 140;
    else tw = g.measure(on && m.presetDirty() ? p.name + L" •" : p.name, pn);
    float w = 1 + 10 + 15 + 7 + tw + 10 + 1;
    if (w > 230) { tw -= w - 230; w = 230; }
    L.chip.push_back({cx, 64, w, 28});
    L.chipText.push_back(tw);
    cx += w + 8;
  }
  const float aw = m.s.presets.empty() ? 1 + 10 + 15 + 6 + g.measure(L"Save mix", pn) + 10 + 1 : 29;
  L.pAdd = {cx, 64, aw, 28};
  L.pTotal = cx + aw - (px0 - presetScroll_);
  L.pUpd = {W - 16 - 32, 62, 32, 32};

  // ---- scenes and stats
  const auto& stock = stockScenes();
  for (int i = 0; i < NSCENES; i++) L.scene[i] = {12, by + 16 + i * 46.f, 207, 44};
  const float statsH = 91;
  L.stats = {12, H - 16 - statsH, 207, statsH};

  // ---- stage
  const bool shortWin = H <= 760 && W > 980;
  const float gap = shortWin ? 14 : 22;
  L.playD = shortWin ? 60 : 68;
  const float specH = (float)clampd(0.22 * H, 130, 260);
  L.specH = specH;
  L.spec = {L.stage.x, H - specH, L.stage.w, specH};
  const float segH = 34;
  const float areaTop = by + 20, areaBot = H - (specH + 8);
  const float avail = areaBot - areaTop;
  float ringPx = (float)clampd(std::min(0.34 * W, 0.46 * H), 210, 460);
  ringPx = std::max(190.f, std::min(ringPx, avail - (segH + gap + gap + L.playD)));
  L.ringPx = ringPx;
  const float stackH = segH + gap + ringPx + gap + L.playD;
  float top = areaTop + (avail - stackH) / 2;
  top = std::max(top, by + 6);
  const float mid = L.stage.cx();

  L.planIds = m.scene().plans;
  std::stable_sort(L.planIds.begin(), L.planIds.end(), [](const string& a, const string& b) { return (a == "free") < (b == "free"); });
  const TextStyle ps = T(12.5f, 500, P.text);
  vector<float> bw;
  float segW = 6 + 2.f * (float)(L.planIds.size() - 1);
  for (auto& id : L.planIds) {
    const PlanDef* pd = findPlan(id);
    const float w = g.measure(pd ? pd->label : L"?", ps) + 24;
    bw.push_back(w);
    segW += w;
  }
  // The plans are centred over the ring and the buttons that go with them hang off their right end: the loop button, only
  // while the chosen plan has rounds to loop (an ∞ plan or a timer has none, so there is nothing to show), then the task
  // button when the stage has no room for the card. So the plans stay put when the loop button comes and goes; only a
  // window so narrow that the buttons would reach the side panel moves them left, by as much as it takes.
  const bool loopable = planHasRounds(m.planId());
  const float loopW = loopable ? segH : 0, loopGap = loopable ? 8.f : 0;
  // Tasks: a card beside the ring when the stage has room for one, otherwise a button that opens a sheet.
  const bool tasks = app.tasksOn();
  const float cardX = mid + ringPx / 2 + 22, cardW = std::min(320.f, L.stage.r() - 20 - cardX);
  const bool pinned = tasks && cardW >= 236;
  const float taskW = tasks && !pinned ? segH : 0, taskGap = taskW > 0 ? 8.f : 0;
  float groupX = mid - segW / 2;
  groupX -= std::max(0.f, groupX + segW + loopGap + loopW + taskGap + taskW - (L.stage.r() - 12));
  L.planSeg = {groupX, top, segW, segH};
  float gx = L.planSeg.r();
  if (loopable) { L.loopBtn = {gx + loopGap, top, loopW, segH}; gx = L.loopBtn.r(); }
  if (taskW > 0) L.taskBtn = {gx + taskGap, top, taskW, segH};
  float bx = L.planSeg.x + 3;
  for (float w : bw) { L.plan.push_back({bx, L.planSeg.y + 3, w, 28}); bx += w + 2; }
  const float ringTop = top + segH + gap;
  L.ring = {mid - ringPx / 2, ringTop, ringPx, ringPx};
  if (pinned) {
    buildTask(cardX, std::max(by + 8, ringTop + (ringPx - TK_BODY_MAX) / 2), cardW, true);
    L.taskCard = tv.card;
  }
  const float ty = ringTop + ringPx + gap + L.playD / 2;
  L.play = {mid - L.playD / 2, ty - L.playD / 2, L.playD, L.playD};
  L.prev = {L.play.x - 20 - 44, ty - 22, 44, 44};
  L.next = {L.play.r() + 20, ty - 22, 44, 44};
  L.transport = {L.prev.x, ty - L.playD / 2, L.next.r() - L.prev.x, L.playD};

  // ---- side panel (scrolls when the window is short)
  {
    const float sx = L.side.x + 17, sw = L.side.w - 33;
    float y = L.side.y + 16 - sideScroll_;
    const bool streamTab = m.s.tab == "stream";
    L.tabsSeg = {sx, y, 72, 34};
    L.tab[0] = {sx + 3, y + 3, 32, 28};
    L.tab[1] = {sx + 37, y + 3, 32, 28};
    L.power = {sx + sw - 32, y + 1, 32, 32};
    L.link = {L.power.x - 4 - 32, y + 1, 32, 32};
    L.spot = {L.link.x - 4 - 32, y + 1, 32, 32};
    y += 34 + 10;
    if (streamTab && spotifyCard()) {
      // a playlist: its cover and what plays, with the lock; under it previous, the playlist's name, next and remove
      L.player = {sx, y, sw, SP_CARD_H};
      L.cover = {sx + 12, y + (SP_CARD_H - SP_COVER) / 2, SP_COVER, SP_COVER};
      L.lock = {L.player.r() - 8 - 30, L.player.y + 8, 30, 30};
      y += SP_CARD_H + 8;
      L.vbar = {sx, y, sw, 32};
      L.vPrev = {sx, y, 32, 32};
      L.vClear = {sx + sw - 32, y, 32, 32};
      L.vNext = {L.vClear.x - 4 - 32, y, 32, 32};
      L.vTitle = {sx + 36, y, L.vNext.x - 4 - (sx + 36), 32};
      y += 32;
      if (linkOpen_) {
        y += 8;
        L.linkForm = {sx, y, sw, 32};
        L.linkInput = {sx, y, sw - 38, 32};
        L.linkGo = {sx + sw - 32, y, 32, 32};
        y += 32;
      }
    } else if (streamTab) {
      if (m.s.streamOn) {
        const size_t n = app.candidates().size();
        const bool custom = !m.s.custom.empty();
        L.playerNone = app.streamState == SS_NONE;
        const float ph = L.playerNone ? 72 : std::round(sw * 9 / 16);
        L.player = {sx, y, sw, ph};
        L.watch = {L.player.r() - 8 - 32, L.player.y + 8, 32, 32};
        if (!L.playerNone && app.toolsMissing() && app.setupState != SETUP_RUNNING) {
          const float bw = std::min(196.f, sw - 32);
          L.setup = {L.player.cx() - bw / 2, L.player.y + std::round(ph * 0.42f), bw, 36};
        }
        y += ph;
        if (!L.playerNone) {
          y += 8;
          L.vbar = {sx, y, sw, 32};
          float vr = sx + sw;
          if (custom) { L.vClear = {vr - 32, y, 32, 32}; vr -= 36; }
          L.vPrev = {sx, y, 32, 32};
          L.vNext = {vr - 32, y, 32, 32};
          L.vTitle = {sx + 36, y, vr - 36 - sx - 36, 32};
          if (custom || n < 2) { L.vPrev = R(); L.vNext = R(); L.vTitle = {sx, y, vr - sx, 32}; }
          y += 32;
          if (linkOpen_) {
            y += 8;
            L.linkForm = {sx, y, sw, 32};
            L.linkInput = {sx, y, sw - 38, 32};
            L.linkGo = {sx + sw - 32, y, 32, 32};
            y += 32;
          }
        } else if (linkOpen_) {
          y += 8;
          L.linkForm = {sx, y, sw, 32};
          L.linkInput = {sx, y, sw - 38, 32};
          L.linkGo = {sx + sw - 32, y, 32, 32};
          y += 32;
        }
      } else if (linkOpen_) {
        L.linkForm = {sx, y, sw, 32};
        L.linkInput = {sx, y, sw - 38, 32};
        L.linkGo = {sx + sw - 32, y, 32, 32};
        y += 32;
      }
    } else {
      if (app.files.empty()) {
        L.dropzone = {sx, y, sw, 96};
        y += 96;
      } else {
        if (app.files.size() >= 2) {
          y += 8;
          L.fileRows = (int)std::min<size_t>(app.files.size(), 4);
          L.tracks = {sx, y, sw, L.fileRows * 29.f};
          y += L.tracks.h;
        }
        y += 8;
        L.fbar = {sx, y, sw, 32};
        L.vPrev = {sx, y, 32, 32};
        L.vClear = {sx + sw - 32, y, 32, 32};
        L.vNext = {sx + sw - 68, y, 32, 32};
        L.vTitle = {sx + 36, y, sw - 108, 32};
        y += 32;
      }
    }
    y += 12;
    L.blendRow = {sx, y, sw, 20};
    L.blendL = {sx, y + 1, 18, 18};
    L.blendR = {sx + sw - 18, y + 1, 18, 18};
    L.blendSlider = {sx + 28, y, sw - 56, 20};
    L.blendCap = {sx, y + 21, sw, 16};
    y += 20 + 29;
    L.mixLabel = {sx, y, 60, 32};
    L.reset = {sx + sw - 32, y, 32, 32};
    y += 32 + 10;
    const float tw = (sw - 32) / 5, th = tw * 1.1f;
    for (int i = 0; i < 10; i++) L.tile[i] = {sx + (i % 5) * (tw + 8), y + (i / 5) * (th + 8), tw, th};
    y += 2 * th + 8 + 8;
    const float bwid = (sw - 24) / 5;
    for (int i = 0; i < 5; i++) L.band[i] = {sx + i * (bwid + 6), y, bwid, 44};
    y += 44;
    L.sideContentH = y + sideScroll_ - L.side.y + 16;
  }

  // ---- hits, in paint order
  static const wchar_t* beatTips[3] = {L"Beats off (B)", L"Binaural beats: headphones (B)", L"Isochronic beats: speakers (B)"};
  for (int i = 0; i < 3; i++) add(H_BEATMODE, i, L.beatBtn[i], beatTips[i]);
  add(H_MUTE, 0, L.mute, L"Mute (M)");
  add(H_MASTER, 0, L.master.inset(0, -4), L"Volume");
  add(H_INFO, 0, L.info, L"Evidence");
  add(H_MINI, 0, L.miniBtn, L"Mini player (N)");
  add(H_PLUGS, 0, L.plugBtn, L"Plugins");
  add(H_KEYS, 0, L.keys, L"Shortcuts (?)");

  // presets: chips are clipped to the scrolling area
  for (size_t i = 0; i < L.chip.size(); i++) {
    const Preset& p = m.s.presets[i];
    if (editing_ == p.id) continue;
    const R& c = L.chip[i];
    if (c.r() <= L.pClip.x || c.x >= L.pClip.r()) continue;
    const float x0 = std::max(c.x, L.pClip.x), x1 = std::min(c.r(), L.pClip.r());
    add(H_CHIP, (int)i, {x0, c.y, x1 - x0, c.h}, p.name + L" (double-click to rename)");
    const R xr = {c.r() - 4 - 20, c.y + 4, 20, 20};
    if (xr.x >= L.pClip.x && xr.r() <= L.pClip.r()) add(H_CHIPX, (int)i, xr, L"Delete");
  }
  if (L.pAdd.r() > L.pClip.x && L.pAdd.x < L.pClip.r()) {
    const float x0 = std::max(L.pAdd.x, L.pClip.x), x1 = std::min(L.pAdd.r(), L.pClip.r());
    if (m.s.presets.size() < (size_t)MAX_PRESETS) add(H_PADD, 0, {x0, L.pAdd.y, x1 - x0, L.pAdd.h}, L"Save the current mix as a preset");
  }
  if (upd) add(H_PUPD, 0, L.pUpd, L"Update this preset with the current mix");

  for (int i = 0; i < NSCENES; i++) add(H_SCENE, i, L.scene[i], stock[(size_t)i].name + L" (" + std::to_wstring(i + 1) + L")");
  {
    const int streak = m.streak();
    if (streak >= 2) add(H_STATS, 0, L.stats, std::to_wstring(streak) + L" days in a row with focus on them");
  }
  for (size_t i = 0; i < L.plan.size(); i++) add(H_PLAN, (int)i, L.plan[i]);
  if (L.loopBtn.w > 0) {
    const int choice = app.setsChoice();
    add(H_LOOP, 0, L.loopBtn,
        !m.s.repeat ? L"Loop the rounds: click to choose how many sets (R)"
        : choice == SETS_FOREVER ? L"Looping until you stop it: click to change (R turns it off)"
                                 : L"Looping " + std::to_wstring(choice) + L" sets: click to change (R turns it off)");
  }
  if (L.taskBtn.w > 0) add(H_TASK, 0, L.taskBtn, m.s.task.empty() ? L"Write the task (T)" : L"The task (T)");
  add(H_PREV, 0, L.prev, L"Restart phase (←)");
  add(H_PLAY, 0, L.play, snap.done ? L"Start again (Space)" : L"Play / pause (Space)");
  if (snap.cycle && !snap.done) add(H_NEXT, 0, L.next, L"Skip phase (→)");
  if (snap.done) add(H_AGAIN, 0, ringGeo(snap).again, L"Start again (Space)");  // not there when the ring is too small for it
  if (pinned) {
    addTaskHits();
  } else if (tasks) {  // no card: the line under the ring opens the sheet
    const R line = ringLine(snap);
    if (line.w > 0 && !m.s.task.empty()) add(H_TASK, 1, line, m.s.task.text);
  }

  // side panel hits are limited to the visible part of the panel
  const R sideView = {L.side.x, L.side.y, L.side.w, L.side.h};
  auto addSide = [&](HitKind k, int i, const R& r, const wstring& tip = L"") {
    if (!r.hits(sideView)) return;
    const float y0 = std::max(r.y, sideView.y), y1 = std::min(r.b(), sideView.b());
    add(k, i, {r.x, y0, r.w, y1 - y0}, tip);
  };
  addSide(H_TAB, 0, L.tab[0], L"Stream");
  addSide(H_TAB, 1, L.tab[1], L"Your music");
  if (m.s.tab == "stream") {
    {
      const Attach* at = m.attached();
      addSide(H_SPOT, 0, L.spot, at ? L"Spotify playlist: " + (at->name.empty() ? wstring(L"change it") : at->name) + L" (S)" : L"Play a Spotify playlist in this scene (S)");
    }
    addSide(H_LINK, 0, L.link, L"Paste a YouTube or Spotify link (Ctrl+V)");
    addSide(H_VIDEO, 0, L.power, m.s.streamOn ? L"Stream on / off" : L"Turn the stream on");
    if (spotifyCard()) {
      addSide(H_PLAYER, 0, L.player);
      addSide(H_LOCK, 0, L.lock, shownPrivate() ? L"Private: Spotify does not record what plays here. Click to turn off"
                                                : L"Not private: Spotify records this. Click to play privately");
      addSide(H_VPREV, 0, L.vPrev, L"Previous song");
      addSide(H_VNEXT, 0, L.vNext, L"Next song");
      addSide(H_VCLEAR, 0, L.vClear, L"Remove the playlist: back to the scene\u2019s stream");
      if (linkOpen_) addSide(H_LINKGO, 0, L.linkGo, L"Load");
    } else if (m.s.streamOn) {
      if (!L.playerNone) {
        addSide(H_PLAYER, 0, L.player);
        addSide(H_WATCH, 0, L.watch, app.videoWindow ? L"Close the video window (V)" : L"Watch the video (V)");
        addSide(H_SETUP, 0, L.setup, app.setupState == SETUP_FAILED ? L"Try the download again" : L"Download mpv and yt-dlp (about 50 MB)");
        addSide(H_VPREV, 0, L.vPrev, L"Previous stream");
        addSide(H_VNEXT, 0, L.vNext, L"Next stream");
        addSide(H_VCLEAR, 0, L.vClear, L"Back to the scene's stream");
      }
      if (linkOpen_) addSide(H_LINKGO, 0, L.linkGo, L"Load");
    }
  } else {
    if (app.files.empty()) addSide(H_FILEPICK, 0, L.dropzone);
    else {
      addSide(H_FPREV, 0, L.vPrev, L"Previous");
      addSide(H_FNEXT, 0, L.vNext, L"Next");
      addSide(H_FCLEAR, 0, L.vClear, L"Clear");
    }
  }
  addSide(H_BLEND, 0, L.blendSlider.inset(0, -4), m.s.tab == "stream" && !spotifyCard() ? L"Balance: left is the video, right is the sounds" : L"Balance: left is your music, right is the sounds");
  const bool modified = m.mixModified();
  if (modified) addSide(H_RESET, 0, L.reset, L"Reset mix");
  if (!brk) {
    for (int i = 0; i < 10; i++) {
      wstring tip = i < NCH ? CHANNELS[i].tip : L"Beats: tap to switch, drag for level";
      addSide(H_TILE, i, L.tile[i], tip);
    }
    for (int i = 0; i < 5; i++) addSide(H_BAND, i, L.band[i]);
  }

  // toast on top
  if (app.toast.active() && !app.toast.action.empty()) {
    TextStyle ts = T(13, 400, P.text);
    TextStyle as = T(13, 600, P.accent);
    const float tw2 = g.measure(app.toast.msg, ts), aw2 = g.measure(app.toast.action, as) + 20;
    const float w = 16 + tw2 + 12 + aw2 + 8, h = 45;
    const R tr = {(W - w) / 2, H - 24 - h, w, h};
    add(H_TOASTACT, 0, {tr.r() - 8 - aw2, tr.y + 8, aw2, 29}, L"");
  }

  // modal sheets replace everything else
  if (sheet_ == 4 && L.loopBtn.w <= 0) sheet_ = 0;  // the plan changed under an open loop picker to one with nothing to loop
  if (sheet_ == 5) {  // the scene's playlist changed under the open picker (a link was pasted, or the scene switched): it is done
    const Attach* at = m.attached();
    if ((at ? at->uri : string()) != pickUri_ || m.s.scene != pickScene_) sheet_ = 0;
  }
  if (sheet_) {
    hits_.clear();
    if (sheet_ == 3) {  // the Task sheet is as tall as what is in it, so lay the task out first
      const R s0 = sheetRect();
      taskSheetH_ = buildTask(s0.x + 20, s0.y + SHEET_TASK_TOP, s0.w - 40, false) - s0.y;
    }
    const R s = sheetRect();
    add(H_SHEETBACK, 0, {0, 0, W, H});
    add(H_SIDE, 0, s);
    if (sheet_ == 4) for (int i = 0; i < LOOP_CELLS; i++) add(H_LSET, i, loopCellRect(i));  // clicking away closes it, there is no cross
    else add(H_SHEETCLOSE, 0, {s.r() - 12 - 32, s.y + 12, 32, 32}, L"Close");
    if (sheet_ == 3) addTaskHits();
    if (sheet_ == 6) {
      const vector<PluginInfo>& list = app.plugins.list();
      for (int i = 0; i < (int)list.size(); i++) {
        const PluginInfo& p = list[(size_t)i];
        if (!p.usable()) continue;  // one that cannot run has nothing to press
        if (p.on && p.settings) add(H_PLUGSET, i, plugGear(i));
        add(H_PLUG, i, plugSwitch(i).inset(-6, -8));
      }
      add(H_PLUGDIR, 0, plugFolder());
    }
    if (sheet_ == 5) {
      const R lr = pickListRect();
      pickScroll_ = (float)clampd(pickScroll_, 0, std::max(0.f, (float)pick_.size() * PK_ROW - lr.h));
      for (int i = 0; i < (int)pick_.size(); i++) {
        const R row = {lr.x, lr.y + i * PK_ROW - pickScroll_, lr.w, PK_ROW - 2};
        if (row.b() <= lr.y || row.y >= lr.b()) continue;
        const float y0 = std::max(row.y, lr.y), y1 = std::min(row.b(), lr.b());
        add(H_PICK, i, {row.x, y0, row.w, y1 - y0});
      }
    }
    if (sheet_ == 1) {
      static const float itemH[5] = {106, 80, 80, 105, 80};
      float iy = s.y + 50;
      for (int i = 0; i < 5; i++) {
        if (i != 4) add(H_EVLINK, i, {s.r() - 20 - 24, iy + 14, 24, 24}, L"Open the study");
        iy += itemH[i];
      }
    }
  }

  // keep scroll offsets inside their ranges; rebuild once if one moved
  bool again = false;
  const float maxSide = std::max(0.f, L.sideContentH - L.side.h);
  if (sideScroll_ > maxSide) { sideScroll_ = maxSide; again = true; }
  if (sideScroll_ < 0) { sideScroll_ = 0; again = true; }
  const float maxP = std::max(0.f, L.pTotal - L.pClip.w);
  if (presetScroll_ > maxP) { presetScroll_ = maxP; again = true; }
  if (presetScroll_ < 0) { presetScroll_ = 0; again = true; }
  if (m.s.scene != lastScene_) {
    lastScene_ = m.s.scene;
    ensureChipVisible();
    again = true;
  }
  if (again) build();
}

void Ui::ensureChipVisible() {
  const Model& m = app.model;
  for (size_t i = 0; i < L.chip.size(); i++) {
    if (m.s.presets[i].id != m.s.scene) continue;
    const R& c = L.chip[i];
    if (c.x < L.pClip.x) presetScroll_ -= L.pClip.x - c.x + 8;
    else if (c.r() > L.pClip.r()) presetScroll_ += c.r() - L.pClip.r() + 8;
    return;
  }
}

void Ui::scrollPresets(float delta) {
  presetScroll_ += delta;
  layoutDirty_ = true;
  invalidate(L.presets);
}

// ---------------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------------

void Ui::clearTip() {
  tipPending_ = false;
  if (tipShown_) {
    tipShown_ = false;
    invalidate(tipRect_.inset(-6));
  }
}

void Ui::tipTimer() {
  tipPending_ = false;
  if (hover_.k == H_NONE || hover_.tip.empty() || dragActive()) return;
  tipShown_ = true;
  tipText_ = hover_.tip;
  invalidateAll();
}

void Ui::mouseLeave() {
  if (hover_.k != H_NONE) invalidate(hover_.r.inset(-3));
  hover_ = Hit();
  clearTip();
}

void Ui::setSlider(HitKind k, float x, float y) {
  const R& r = k == H_MASTER ? L.master : L.blendSlider;
  const bool up = k == H_MASTER && L.masterVert;  // the compact player's volume stands upright: louder is higher
  const float p = up ? 1 - (y - (r.y + 7)) / std::max(1.f, r.h - 14) : (x - (r.x + 7)) / std::max(1.f, r.w - 14);
  const int v = (int)std::lround(clampd(p, 0, 1) * 100);
  if (k == H_MASTER) app.setMaster(v); else app.setBlend(v);
}

void Ui::mouseMove(float x, float y, bool leftDown) {
  ensureLayout();
  if (sliderDrag_ != H_NONE && leftDown) { setSlider(sliderDrag_, x, y); return; }
  if (press_.k == H_TILE && leftDown) {
    const float dy = pressY_ - y;
    if (!dragging_ && std::fabs(dy) < 5) return;
    if (!dragging_) { dragging_ = true; invalidate(press_.r.inset(-2)); }
    const Hit* tile = nullptr;
    for (auto& h : hits_) if (h.k == H_TILE && h.i == press_.i) tile = &h;
    const float th = tile ? tile->r.h : 70;
    const int v = (int)std::lround(clampd(dragStartVal_ + dy / th * 100, 0, 100));
    if (press_.i == 9) app.setBeatLevel(v); else app.setLevel(press_.i, v);
    return;
  }
  const Hit h = hitAt(x, y);
  if (h != hover_) {
    if (hover_.k != H_NONE) invalidate(hover_.r.inset(-3));
    // the delete cross shows on the hovered chip; its neighbour on the same chip needs a repaint too
    if (h.k != H_NONE) invalidate(h.r.inset(-3));
    if (hover_.k == H_CHIP || hover_.k == H_CHIPX || h.k == H_CHIP || h.k == H_CHIPX) invalidate(L.presets);
    // a task row lights up and shows its delete cross while the pointer is anywhere on it
    auto taskPart = [](HitKind k) { return k == H_TCHECK || k == H_TTEXT || k == H_TDEL || k == H_TADD; };
    if (taskPart(hover_.k) || taskPart(h.k)) invalidate(sheet_ ? sheetRect() : L.taskCard.inset(-4));
    if (hover_.k == H_LSET || h.k == H_LSET) invalidate(sheetRect().inset(-4));  // the line of words under the cells follows the pointer
    hover_ = h;
    clearTip();
    if (!h.tip.empty() && !leftDown) tipPending_ = true;
  }
}

void Ui::mouseDown(float x, float y) {
  ensureLayout();
  clearTip();
  const Hit h = hitAt(x, y);
  press_ = h;
  pressY_ = y;
  dragging_ = false;
  if (h.k == H_MASTER || h.k == H_BLEND) {
    sliderDrag_ = h.k;
    if (hooks.capture) hooks.capture(true);
    setSlider(h.k, x, y);
  } else if (h.k == H_TILE) {
    dragStartVal_ = h.i == 9 ? app.model.beatOf().level : app.model.mixOf()[(size_t)h.i];
    if (hooks.capture) hooks.capture(true);
  } else if (h.k == H_SHEETBACK) {
    // closes on release
  }
}

void Ui::mouseUp(float x, float y) {
  ensureLayout();
  const Hit down = press_;
  const bool wasDragging = dragging_;
  press_ = Hit();
  dragging_ = false;
  if (sliderDrag_ != H_NONE) {
    sliderDrag_ = H_NONE;
    if (hooks.capture) hooks.capture(false);
    return;
  }
  if (down.k == H_TILE) {
    if (hooks.capture) hooks.capture(false);
    invalidate(down.r.inset(-2));
    if (!wasDragging && hitAt(x, y) == down) activate(down);
    return;
  }
  if (down.k == H_NONE) return;
  if (hitAt(x, y) == down) activate(down);
}

void Ui::doubleClick(float x, float y) {
  const Hit h = hitAt(x, y);
  if (h.k == H_CHIP) { startRename(app.model.s.presets[(size_t)h.i].id); return; }
  mouseDown(x, y);
  mouseUp(x, y);
}

void Ui::rightClick(float x, float y) {
  const Hit h = hitAt(x, y);
  if (h.k != H_CHIP && h.k != H_CHIPX) return;
  const string id = app.model.s.presets[(size_t)h.i].id;
  if (!hooks.menu) return;
  const int pick = hooks.menu(x, y, {L"Rename", L"Delete"});
  if (pick == 0) startRename(id);
  else if (pick == 1) app.presetDelete(id);
}

void Ui::wheel(float x, float y, int delta) {
  ensureLayout();
  const float steps = delta / 120.f;
  const Hit h = hitAt(x, y);
  if (sheet_ == 5) {
    const R lr = pickListRect();
    pickScroll_ = (float)clampd(pickScroll_ - steps * PK_ROW * 2, 0, std::max(0.f, (float)pick_.size() * PK_ROW - lr.h));
    pickAuto_ = false;
    layoutDirty_ = true;
    invalidate(sheetRect());
    return;
  }
  if (sheet_) return;
  if (h.k == H_TILE) {
    const int cur = h.i == 9 ? app.model.beatOf().level : app.model.mixOf()[(size_t)h.i];
    const int v = clampi(cur + (int)std::lround(steps * 5), 0, 100);
    if (h.i == 9) app.setBeatLevel(v); else app.setLevel(h.i, v);
    return;
  }
  if (h.k == H_MASTER || h.k == H_MUTE) { app.setMaster(app.model.s.master + (int)std::lround(steps * 5)); return; }
  if (h.k == H_BLEND) { app.setBlend(app.model.s.blend + (int)std::lround(steps * 5)); return; }
  if (L.presets.has(x, y)) { scrollPresets(-steps * 60); return; }
  if (L.side.has(x, y) && L.sideContentH > L.side.h) {
    sideScroll_ = (float)clampd(sideScroll_ - steps * 60, 0, L.sideContentH - L.side.h);
    layoutDirty_ = true;
    invalidate(L.side);
  }
}

int Ui::cursorAt(float x, float y) {
  const Hit h = hitAt(x, y);
  switch (h.k) {
    case H_NONE: case H_SIDE: case H_SHEETBACK: case H_STATS: return 0;
    case H_TILE: return 2;
    default: return 1;
  }
}

bool Ui::key(int vk, bool ctrl, bool shift) {
  if (ctrl) return false;
  if (sheet_ == 5) {  // the playlist picker: letters go to the filter (see character), these move and pick
    if (vk == VK_ESCAPE) {
      if (pickFilter_.empty()) openSheet(0);
      else { pickFilter_.clear(); pickSel_ = 0; pickScroll_ = 0; refilter(); invalidate(sheetRect()); }
    }
    else if (vk == VK_RETURN) { if (!pick_.empty()) pickRow(pickSel_); }
    else if (vk == VK_UP) pickMove(-1);
    else if (vk == VK_DOWN) pickMove(1);
    else if (vk == VK_PRIOR) pickMove(-pickRowsShown());
    else if (vk == VK_NEXT) pickMove(pickRowsShown());
    else if (vk == VK_BACK && !pickFilter_.empty()) { pickFilter_.pop_back(); pickSel_ = 0; pickScroll_ = 0; refilter(); invalidate(sheetRect()); }
    return true;
  }
  if (sheet_) {
    if (vk == VK_ESCAPE || vk == VK_RETURN) { openSheet(0); return true; }
    if (vk == 'T' && sheet_ == 3) taskNext();
    if (sheet_ == 4) {  // the loop picker: a digit picks that many sets, the arrows step along the cells, R still turns it on or off
      if (vk >= '1' && vk < '1' + LOOP_CELLS - 1) { app.chooseSets(vk - '0'); openSheet(0); }
      else if (vk == VK_LEFT || vk == VK_RIGHT) {
        const int cell = loopCell(app.setsChoice());
        app.chooseSets(loopChoice(clampi((cell < 0 ? LOOP_CELLS - 1 : cell) + (vk == VK_RIGHT ? 1 : -1), 0, LOOP_CELLS - 1)));
      }
      else if (vk == 'R') app.toggleLoop();
    }
    return true;
  }
  switch (vk) {
    case VK_ESCAPE:
      if (linkOpen_) { linkOpen_ = false; layoutDirty_ = true; invalidateAll(); return true; }
      return false;
    case VK_SPACE: app.togglePlay(); return true;
    case VK_LEFT: app.prevPhase(); return true;
    case VK_RIGHT: { const SessionSnap s = app.session.snapshot(); if (s.cycle && !s.done) app.nextPhase(); return true; }
    case VK_UP: app.setMaster(app.model.s.master + 5); return true;
    case VK_DOWN: app.setMaster(app.model.s.master - 5); return true;
    case 'M': app.toggleMute(); return true;
    case 'B': app.cycleBeatsMode(); return true;
    case 'R': app.toggleLoop(); return true;
    case 'T': taskNext(); return true;
    case 'S':
      if (mini_) return false;
      pickKeyChar_ = true;  // the S that opens the picker also arrives as a typed letter: that one is not for the filter
      openSheet(5);
      return true;
    case 'V': app.toggleVideoWindow(); return true;
    case 'N': if (hooks.toggleMini) hooks.toggleMini(); return true;
    case VK_OEM_2: if (shift) { openSheet(2); return true; } return false;
    default: break;
  }
  if (vk >= '1' && vk < '1' + NSCENES) { app.selectScene(stockScenes()[(size_t)(vk - '1')].id); return true; }
  return false;
}

void Ui::startRename(const string& id) {
  ensureLayout();
  const Model& m = app.model;
  size_t idx = 0;
  bool found = false;
  for (size_t i = 0; i < m.s.presets.size(); i++) if (m.s.presets[i].id == id) { idx = i; found = true; }
  if (!found || !hooks.edit) return;
  editing_ = id;
  layoutDirty_ = true;
  ensureLayout();
  const R c = L.chip[idx];
  const R box = {c.x + 1 + 10 + 15 + 7, c.y + 4, 140, 20};
  invalidate(L.presets);
  hooks.edit(box, m.s.presets[idx].name, 24, id == m.s.scene, [this, id](bool ok, const wstring& text) {
    editing_.clear();
    layoutDirty_ = true;
    if (ok) {
      wstring t = text;
      while (!t.empty() && t.front() == L' ') t.erase(t.begin());
      while (!t.empty() && t.back() == L' ') t.pop_back();
      if (!t.empty()) app.presetRename(id, t);
    }
    invalidateAll();
  });
}

void Ui::startLinkEdit() {
  ensureLayout();
  if (!hooks.edit || L.linkInput.w <= 0) return;
  const R box = {L.linkInput.x + 10, L.linkInput.y + 6, L.linkInput.w - 20, 20};
  hooks.edit(box, L"", 200, false, [this](bool ok, const wstring& text) {
    // Enter loads the link; leaving the field or Escape just closes it
    linkOpen_ = false;
    layoutDirty_ = true;
    if (ok && !text.empty()) app.pasteText(text);
    invalidateAll();
  });
}

void Ui::activate(const Hit& h) {
  Model& m = app.model;
  // a hit can outlive a state change (a preset removed between press and release): ignore stale indices
  if ((h.k == H_CHIP || h.k == H_CHIPX) && (h.i < 0 || (size_t)h.i >= m.s.presets.size())) return;
  if (h.k == H_PLAN && (h.i < 0 || (size_t)h.i >= L.planIds.size())) return;
  switch (h.k) {
    case H_SCENE: app.selectScene(stockScenes()[(size_t)h.i].id); break;
    case H_PLAN: if ((size_t)h.i < L.planIds.size()) app.selectPlan(L.planIds[(size_t)h.i]); break;
    case H_LOOP: openSheet(4); break;
    case H_SPOT: openSheet(5); break;
    case H_PLUGS: openSheet(6); break;
    case H_PLUG: case H_PLUGSET: {
      const vector<PluginInfo>& list = app.plugins.list();
      if (h.i < 0 || (size_t)h.i >= list.size()) break;  // the folder was read again between press and release
      const string id = list[(size_t)h.i].id;
      if (h.k == H_PLUG) app.setPluginOn(id, !list[(size_t)h.i].on); else app.pluginSettings(id);
      break;
    }
    case H_PLUGDIR: if (app.plugins.ensureFolder() && hooks.openUrl) hooks.openUrl(app.plugins.dir()); break;
    case H_LOCK: app.toggleSpotifyPrivate(); break;
    case H_PICK: pickRow(h.i); break;
    case H_LSET: app.chooseSets(loopChoice(h.i)); openSheet(0); break;
    case H_AGAIN: if (app.session.done()) app.startAll(); break;
    case H_PREV: app.prevPhase(); break;
    case H_NEXT: app.nextPhase(); break;
    case H_PLAY: app.togglePlay(); break;
    case H_TILE: if (h.i == 9) app.toggleBeat(); else app.toggleChannel(h.i); break;
    case H_BAND: app.setBand(h.i); break;
    case H_RESET: app.resetMix(); break;
    case H_BEATMODE: app.setBeatsMode(h.i); break;
    case H_MUTE: app.toggleMute(); break;
    case H_INFO: openSheet(1); break;
    case H_KEYS: openSheet(2); break;
    case H_MINI: if (hooks.toggleMini) hooks.toggleMini(); break;
    case H_CHIP: app.presetLoad(m.s.presets[(size_t)h.i].id); break;
    case H_CHIPX: app.presetDelete(m.s.presets[(size_t)h.i].id); break;
    case H_PADD: {
      const size_t before = m.s.presets.size();
      app.presetAdd();
      if (m.s.presets.size() > before) startRename(m.s.scene);
      break;
    }
    case H_PUPD: app.presetUpdate(); break;
    case H_TAB: app.setTab(h.i == 0 ? "stream" : "files"); break;
    case H_LINK:
      linkOpen_ = !linkOpen_;
      layoutDirty_ = true;
      invalidateAll();
      if (linkOpen_) startLinkEdit();
      break;
    case H_LINKGO: startLinkEdit(); break;
    case H_VIDEO: app.toggleStreamOn(); break;
    case H_WATCH: app.toggleVideoWindow(); break;
    case H_PLAYER:
      if (spotifyCard()) {
        if (app.streamState == SS_ERROR) app.retryStream();
        else if (!app.running) app.startAll();
        break;
      }
      if (app.toolsMissing()) app.setupStreaming();  // nothing can play until mpv and yt-dlp are here
      else if (app.streamState == SS_ERROR) app.retryStream();
      else if (!app.running) app.startAll();
      break;
    case H_SETUP: app.setupStreaming(); break;
    case H_VPREV: case H_FPREV: app.stepStream(-1); break;
    case H_VNEXT: case H_FNEXT: app.stepStream(1); break;
    case H_VCLEAR:
      if (!m.s.custom.empty()) app.clearCustom();  // a pasted link is in front of everything
      else app.detachSpotify();
      break;
    case H_FILEPICK: if (hooks.pickFiles) hooks.pickFiles(); break;
    case H_FCLEAR: app.clearFiles(); break;
    case H_SHEETCLOSE: case H_SHEETBACK: openSheet(0); break;
    case H_TASK: openSheet(3); break;
    case H_TCHECK: if (h.i == 0) app.taskTick(); else app.taskTickStep(h.i - 1); break;
    case H_TTEXT: editTask(h.i); break;
    case H_TDEL: if (h.i == 0) app.taskClear(); else app.taskRemoveStep(h.i - 1); break;
    case H_TADD: editTask(h.i == 0 ? 0 : NEW_STEP); break;
    case H_EVLINK: {
      static const wchar_t* urls[4] = {L"https://pmc.ncbi.nlm.nih.gov/articles/PMC11283987/", L"https://academic.oup.com/jcr/article/39/4/784/1795620",
                                       L"https://staffprofiles.bournemouth.ac.uk/display/journal-article/296432", L"https://pubmed.ncbi.nlm.nih.gov/30073406/"};
      if (h.i >= 0 && h.i < 4 && hooks.openUrl) hooks.openUrl(urls[h.i]);
      break;
    }
    case H_TOASTACT: {
      auto fn = app.toast.onAction;
      if (fn) fn();
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------------
// The task: one for the whole plan, up to four steps under it. It is shown as a card beside the ring when the stage has
// room, and otherwise in a sheet; both are the same rows (laid out once in buildTask), and the ring says it in a line.
// ---------------------------------------------------------------------------------------------------

R Ui::sheetRect() const {
  if (sheet_ == 6) {  // as tall as the plugins there are
    const float sw = std::min(PG_W, W_ - 32);
    const int n = (int)app.plugins.list().size();
    const float sh = PG_HEAD + (n ? n * PG_ROW : PG_EMPTY) + PG_FOOT;
    return {(W_ - sw) / 2, std::max(8.f, (H_ - sh) / 2), sw, sh};
  }
  if (sheet_ == 5) {  // as tall as its rows, whatever the list holds, so it does not jump while you type
    const float sw = std::min(PK_W, W_ - 32);
    const float sh = PK_HEAD + PK_SEARCH + PK_GAP + pickRowsShown() * PK_ROW + PK_FOOT;
    return {(W_ - sw) / 2, std::max(8.f, (H_ - sh) / 2), sw, sh};
  }
  if (sheet_ == 4) {  // the loop picker hangs under its button, kept over the stage
    const double lo = L.stage.x + 8, hi = std::max(lo, (double)(L.stage.r() - 8 - LP_W));
    return {(float)clampd(L.loopBtn.cx() - LP_W / 2, lo, hi), L.loopBtn.b() + 8, LP_W, LP_H};
  }
  const float sw = std::min(440.f, W_ - 32);
  const float sh = sheet_ == 1 ? SHEET_EVIDENCE_H : sheet_ == 2 ? SHEET_KEYS_H : taskSheetH_;
  const float tall = sheet_ == 3 ? SHEET_TASK_MAX : sh;  // the Task sheet grows downwards, so its top stays put as steps arrive
  return {(W_ - sw) / 2, std::max(8.f, (H_ - tall) / 2), sw, sh};
}

// The Plugins sheet: a row for each plugin (its name, a line under it, a settings button when it has a window of its own,
// its switch), and under the rows the button that opens the folder plugins are put in.
R Ui::plugRow(int i) const {
  const R s = sheetRect();
  return {s.x + 12, s.y + PG_HEAD + i * PG_ROW, s.w - 24, PG_ROW - 8};
}

R Ui::plugSwitch(int i) const {
  const R r = plugRow(i);
  return {r.r() - 14 - 38, r.cy() - 11, 38, 22};
}

R Ui::plugGear(int i) const {
  const R sw = plugSwitch(i);
  return {sw.x - 12 - 30, sw.cy() - 15, 30, 30};
}

R Ui::plugFolder() const {
  const R s = sheetRect();
  return {s.x + 20, s.b() - PG_FOOT + 10, 128, 32};
}

// The loop picker: one row of cells (1 to 6 and ∞) in a segment like the plans', a heading above and a line of words below.
R Ui::loopSeg() const {
  const R s = sheetRect();
  return {s.x + LP_PAD, s.y + LP_HEAD, s.w - 2 * LP_PAD, LP_CELL_H + 2 * LP_SEG};
}

R Ui::loopCellRect(int i) const {
  const R sg = loopSeg();
  return {sg.x + LP_SEG + i * (LP_CELL_W + 2), sg.y + LP_SEG, LP_CELL_W, LP_CELL_H};
}

// What a choice comes to: the sets and how long they take, which is what you want to know before you pick one.
wstring Ui::loopCaption(int choice) const {
  if (choice == SETS_FOREVER) return L"Until you stop it";
  const wstring span = fmtSpan(planLoopMs(app.model.planId(), choice));
  return (choice == 1 ? wstring(L"Once, no loop") : std::to_wstring(choice) + L" sets") + L" · " + span;
}

float Ui::buildTask(float x, float y, float w, bool card, float titleH, float stepH, float foot) {
  tv = TaskLayout();
  if (!app.tasksOn()) return y;
  tv.on = true;
  const Task& t = app.model.s.task;
  const float pad = card ? TK_PAD : 0, x0 = x + pad, cw = w - 2 * pad;
  float ry = y;
  if (card) {  // the sheet has its own heading
    tv.label = {x0, y, cw, TK_HEAD};
    ry += TK_HEAD;
  }
  auto row = [&](int kind, int part, float h) {
    TaskRow r;
    r.kind = kind;
    r.part = part;
    if (kind == TR_EMPTY) {  // a ghost field: "what are you working on?"
      r.row = {x0, ry + 3, cw, h - 6};
      r.box = {x0 + 10, ry + (h - 14) / 2, 14, 14};
      r.text = {r.box.r() + 8, ry, std::max(0.f, x0 + cw - 8 - (r.box.r() + 8)), h};
      r.del = R();
    } else {
      r.row = {x0 - 6, ry, cw + 12, h};
      const float indent = kind == TR_TITLE ? 0.f : 22.f;
      r.box = {x0 + indent, ry + (h - 16) / 2, 16, 16};
      r.del = {x0 + cw - 22, ry + (h - 22) / 2, 22, 22};
      r.text = {r.box.r() + 10, ry, std::max(0.f, r.del.x - 4 - (r.box.r() + 10)), h};
    }
    tv.rows.push_back(r);
    ry += h;
  };
  if (t.empty()) {
    row(TR_EMPTY, 0, titleH);
  } else {
    row(TR_TITLE, 0, titleH);
    for (size_t i = 0; i < t.steps.size(); i++) row(TR_STEP, (int)i + 1, stepH);
    if (t.steps.size() < (size_t)TASK_STEPS) row(TR_ADD, NEW_STEP, stepH);
  }
  const float bottom = ry + foot;
  if (card) tv.card = {x, y, w, bottom - y};
  return bottom;
}

void Ui::addTaskHits() {
  if (!tv.on) return;
  const Task& t = app.model.s.task;
  for (const TaskRow& r : tv.rows) {
    if (r.kind == TR_EMPTY) { add(H_TADD, 0, r.row, L"Write the task (T)"); continue; }
    if (r.kind == TR_ADD) { add(H_TADD, 1, r.row, L"Add a step (T)"); continue; }
    const wstring& text = r.part == 0 ? t.text : t.steps[(size_t)r.part - 1].text;
    const bool clipped = g.measure(text, taskStyle(r.part == 0, false)) > r.text.w;
    add(H_TTEXT, r.part, r.text, clipped ? text : L"");
    add(H_TCHECK, r.part, r.box.inset(-5), r.part == 0 ? L"Tick off the task" : L"Tick off the step");
    add(H_TDEL, r.part, r.del, r.part == 0 ? L"Delete the task" : L"Delete the step");
  }
}

// Starts an editor over one part of the task: 0 is the task (or the empty field), n is step n, NEW_STEP is a step to be written.
// Enter on a new task or a new step carries on to the next step, so the whole list can be typed in one go; leaving the
// field, or Esc, never does.
void Ui::editTask(int part) {
  ensureLayout();
  if (!hooks.edit || !tv.on) return;
  const Task& t = app.model.s.task;
  const TaskRow* row = nullptr;
  for (const TaskRow& r : tv.rows) {
    if (part == NEW_STEP ? r.kind == TR_ADD : (r.kind != TR_ADD && r.part == part)) row = &r;
  }
  if (!row || (part > 0 && part != NEW_STEP && (size_t)part > t.steps.size())) return;
  const wstring cur = part == 0 ? t.text : part == NEW_STEP ? L"" : t.steps[(size_t)part - 1].text;
  const R box = row->text;
  taskEdit_ = part;
  invalidateAll();
  hooks.edit(box, cur, (int)(part == 0 ? TASK_TEXT : STEP_TEXT), false, [this, part](int how, const wstring& text) {
    taskEdit_ = -1;
    layoutDirty_ = true;
    bool more = false;
    if (how != EDIT_CANCEL) {
      Model& m = app.model;
      const bool wasEmpty = m.s.task.empty();
      const size_t steps = m.s.task.steps.size();
      if (part == 0) {
        app.taskSet(text);
        more = how == EDIT_ENTER && wasEmpty && !m.s.task.empty();
      } else if (part == NEW_STEP) {
        app.taskAddStep(text);
        more = how == EDIT_ENTER && m.s.task.steps.size() > steps;
      } else {
        app.taskSetStep(part - 1, text);
      }
    }
    invalidateAll();
    if (more && app.model.s.task.steps.size() < (size_t)TASK_STEPS) editTask(NEW_STEP);
  });
}

// T: write the next thing. The task if there is none, then its steps one after another, and the task again once all four are
// there. Without a card the sheet opens first (the compact player always has the task on screen).
void Ui::taskNext() {
  if (!app.tasksOn()) {
    app.showToast(L"Tasks are for the focus scenes, not Calm or Sleep");
    return;
  }
  ensureLayout();  // whether there is a card depends on the layout as it is now
  if (!taskPinned() && sheet_ != 3) openSheet(3);
  ensureLayout();
  if (!tv.on) return;
  const Task& t = app.model.s.task;
  editTask(t.empty() || t.steps.size() >= (size_t)TASK_STEPS ? 0 : NEW_STEP);
}

// ---------------------------------------------------------------------------------------------------
// The compact player: the ring, the transport, a row of scenes and, under them, the task with its steps, to tick off and to
// edit like the card in the full window. The volume stands upright under the expand button. It stays on top of other windows
// (the window layer does that) so the clock and what you are doing are always in sight, and it is only as tall as what it
// holds: a task with four steps makes it taller, a scene without tasks ends under the scenes.
// ---------------------------------------------------------------------------------------------------

float Ui::miniHeight() const {
  const float ring = (float)clampd(MINI_W - 88, 170, 240);
  const float body = 14 + ring + 14 + 64 + 16 + 36;  // the ring, the transport and the scenes, with the gaps between them
  if (!app.tasksOn()) return body + MINI_PAD;
  const Task& t = app.model.s.task;
  float rows = MT_TITLE;  // the task, or the field that asks for one
  if (!t.empty()) rows += (float)(t.steps.size() + (t.steps.size() < (size_t)TASK_STEPS ? 1 : 0)) * MT_STEP;  // its steps, and the row that adds one
  return body + MT_GAP + rows + MT_FOOT;
}

void Ui::setMini(bool on) {
  if (mini_ == on) return;
  mini_ = on;
  sheet_ = 0;
  linkOpen_ = false;
  hover_ = Hit();
  press_ = Hit();
  clearTip();
  layoutDirty_ = true;
  invalidateAll();
}

void Ui::buildMini() {
  layoutDirty_ = false;
  hits_.clear();
  const float W = W_, H = H_;
  const SessionSnap snap = app.session.snapshot();
  const auto& stock = stockScenes();
  const Palette& P = g.pal;

  const float ringPx = (float)clampd((double)W - 88, 170, 240);  // by the width alone: the height follows the content
  L.ringPx = ringPx;
  L.ring = {(W - ringPx) / 2, 14, ringPx, ringPx};
  L.miniBtn = {W - 10 - 32, 10, 32, 32};
  // the volume: a thin upright slider under the expand button, clear of the ring, with mute at its foot
  L.masterVert = true;
  L.master = {L.miniBtn.cx() - 10, L.miniBtn.b() + 8, 20, 92};
  L.mute = {L.miniBtn.cx() - 14, L.master.b() + 4, 28, 28};
  const float playD = 64;
  const float ty = L.ring.b() + 14 + playD / 2;
  L.play = {W / 2 - playD / 2, ty - playD / 2, playD, playD};
  L.prev = {L.play.x - 18 - 40, ty - 20, 40, 40};
  L.next = {L.play.r() + 18, ty - 20, 40, 40};
  L.transport = {L.prev.x, L.play.y, L.next.r() - L.prev.x, playD};
  const float sy = L.play.b() + 16;
  const float bw = 34, gap = 4, x0 = (W - (NSCENES * bw + (NSCENES - 1) * gap)) / 2;
  for (int i = 0; i < NSCENES; i++) L.scene[i] = {x0 + i * (bw + gap), sy, bw, 36};
  // the task and its steps where the spectrum used to be: the same rows as the card in the full window, a little tighter.
  // (Calm and Sleep have no tasks, and the player simply ends under the scenes.)
  if (app.tasksOn()) {
    const float top = sy + 36 + MT_GAP;
    const float bottom = buildTask(14, top, W - 28, false, MT_TITLE, MT_STEP, MT_FOOT);
    L.taskCard = {0, top - MT_GAP / 2, W, bottom - (top - MT_GAP / 2)};  // its part of the window (it has no outline of its own)
  }
  (void)H;

  add(H_MINI, 0, L.miniBtn, L"Full window (N)");
  add(H_MUTE, 0, L.mute, L"Mute (M)");
  add(H_MASTER, 0, L.master.inset(-4, 0), L"Volume");
  for (int i = 0; i < NSCENES; i++) add(H_SCENE, i, L.scene[i], stock[(size_t)i].name + L" (" + std::to_wstring(i + 1) + L")");
  add(H_PREV, 0, L.prev, L"Restart phase (\u2190)");
  add(H_PLAY, 0, L.play, snap.done ? L"Start again (Space)" : L"Play / pause (Space)");
  if (snap.cycle && !snap.done) add(H_NEXT, 0, L.next, L"Skip phase (\u2192)");
  if (snap.done) add(H_AGAIN, 0, ringGeo(snap).again, L"Start again (Space)");
  addTaskHits();
  if (app.toast.active() && !app.toast.action.empty()) {
    TextStyle ts = T(13, 400, P.text);
    TextStyle as = T(13, 600, P.accent);
    const float tw = g.measure(app.toast.msg, ts), aw = g.measure(app.toast.action, as) + 20;
    const float w = 16 + tw + 12 + aw + 8, h = 45;
    const R tr = {(W - w) / 2, H - 24 - h, w, h};
    add(H_TOASTACT, 0, {tr.r() - 8 - aw, tr.y + 8, aw, 29}, L"");
  }
}

// ---------------------------------------------------------------------------------------------------
// The playlist picker: the listener's Spotify playlists under a filter line. Typing narrows the list, the arrows and Enter
// pick, and a pasted link is used as it is. What is picked belongs to the scene that is open.
// ---------------------------------------------------------------------------------------------------

void Ui::openSheet(int which) {
  if (which == 6) {
    if (mini_) return;
    app.rescanPlugins();  // whatever was put in the folder since the last look is listed
  }
  if (which == 5) {
    if (mini_) return;
    const Attach* at = app.model.attached();
    pickUri_ = at ? at->uri : "";
    pickScene_ = app.model.s.scene;
    pickFilter_.clear();
    pickSel_ = 0;
    pickScroll_ = 0;
    pickAuto_ = true;
    refilter();
    if (hooks.loadPlaylists) hooks.loadPlaylists();  // the list may have changed since it was last read
  } else {
    pickKeyChar_ = false;
  }
  sheet_ = which;
  layoutDirty_ = true;
  invalidateAll();
}

int Ui::pickRowsShown() const {
  return clampi((int)((H_ - 48 - PK_HEAD - PK_SEARCH - PK_GAP - PK_FOOT) / PK_ROW), PK_ROWS_MIN, PK_ROWS_MAX);
}

R Ui::pickSearchRect() const {
  const R s = sheetRect();
  return {s.x + 20, s.y + PK_HEAD, s.w - 40, PK_SEARCH};
}

R Ui::pickListRect() const {
  const R s = sheetRect();
  return {s.x + 10, s.y + PK_HEAD + PK_SEARCH + PK_GAP, s.w - 20, pickRowsShown() * PK_ROW};
}

void Ui::setPlaylists(vector<SpotifyList> lists) {
  lists_ = std::move(lists);
  listsLoaded_ = true;
  if (sheet_ == 5) refilter();
  layoutDirty_ = true;
  invalidateAll();
}

// What the filter leaves. A link in the filter line is its own single row; otherwise names that start with what was typed
// come before names that only contain it, each group in the library's own order. With nothing typed, a scene that has a
// playlist gets a first row that takes it off again.
void Ui::refilter() {
  auto lower = [](wstring w) {
    for (auto& c : w) c = (wchar_t)std::towlower(c);
    return w;
  };
  wstring f = lower(pickFilter_);
  while (!f.empty() && f.front() == L' ') f.erase(f.begin());
  while (!f.empty() && f.back() == L' ') f.pop_back();
  pick_.clear();
  if (!parseSpotifyLink(narrow(pickFilter_)).empty()) {
    pick_.push_back({PickRow::Link, -1});
  } else {
    if (f.empty() && app.model.attached()) pick_.push_back({PickRow::None, -1});
    vector<int> starts, holds;
    for (size_t i = 0; i < lists_.size(); i++) {
      if (f.empty()) { holds.push_back((int)i); continue; }
      const size_t at = lower(lists_[i].name).find(f);
      if (at == 0) starts.push_back((int)i);
      else if (at != wstring::npos) holds.push_back((int)i);
    }
    for (int i : starts) pick_.push_back({PickRow::List, i});
    for (int i : holds) pick_.push_back({PickRow::List, i});
  }
  // with nothing typed the keyboard starts on the playlist the scene has, so Enter keeps it and the arrows leave from it
  if (pickAuto_ && f.empty() && !pickUri_.empty()) {
    for (size_t i = 0; i < pick_.size(); i++) {
      if (pick_[i].kind == PickRow::List && lists_[(size_t)pick_[i].index].uri == pickUri_) {
        pickSel_ = (int)i;
        pickScroll_ = std::max(0.f, (i + 1) * PK_ROW - pickRowsShown() * PK_ROW + PK_ROW);
      }
    }
  }
  pickSel_ = clampi(pickSel_, 0, std::max(0, (int)pick_.size() - 1));
  layoutDirty_ = true;
}

void Ui::pickMove(int d) {
  pickAuto_ = false;
  if (pick_.empty()) return;
  pickSel_ = clampi(pickSel_ + d, 0, (int)pick_.size() - 1);
  const float top = pickSel_ * PK_ROW, view = pickRowsShown() * PK_ROW;
  if (top < pickScroll_) pickScroll_ = top;
  else if (top + PK_ROW > pickScroll_ + view) pickScroll_ = top + PK_ROW - view;
  layoutDirty_ = true;
  invalidate(sheetRect());
}

void Ui::pickRow(int i) {
  if (i < 0 || (size_t)i >= pick_.size()) return;  // the list changed between press and release
  const PickRow r = pick_[(size_t)i];
  if (r.kind == PickRow::None) {
    openSheet(0);
    app.detachSpotify();
  } else if (r.kind == PickRow::Link) {
    const string uri = parseSpotifyLink(narrow(pickFilter_));
    openSheet(0);
    app.attachSpotify(uri);
  } else if (r.index >= 0 && (size_t)r.index < lists_.size()) {
    const SpotifyList l = lists_[(size_t)r.index];
    openSheet(0);
    app.attachSpotify(l.uri, l.name, l.art);
  }
}

bool Ui::character(wchar_t c) {
  if (sheet_ != 5) return false;
  if (pickKeyChar_) { pickKeyChar_ = false; return true; }
  if (c < 32 || c == 127) return true;  // Enter, Esc and Backspace arrive as keys
  if (pickFilter_.size() >= PK_FILTER_MAX) return true;
  pickAuto_ = false;
  pickFilter_ += c;
  pickSel_ = 0;
  pickScroll_ = 0;
  refilter();
  invalidate(sheetRect());
  return true;
}

void Ui::pasteInto(const wstring& text) {
  if (sheet_ != 5) return;
  const string uri = parseSpotifyLink(narrow(text));
  if (!uri.empty()) {  // a link: that is the playlist
    openSheet(0);
    app.attachSpotify(uri);
    return;
  }
  for (wchar_t c : text) {
    if (c == L'\r' || c == L'\n') break;
    if (c >= 32 && c != 127 && pickFilter_.size() < PK_FILTER_MAX) pickFilter_ += c;
  }
  pickSel_ = 0;
  pickScroll_ = 0;
  refilter();
  invalidate(sheetRect());
}
