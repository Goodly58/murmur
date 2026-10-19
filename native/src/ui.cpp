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
constexpr float SHEET_EVIDENCE_H = 546, SHEET_KEYS_H = 372;
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
  if (!app.model.s.streamOn || app.model.s.tab != "stream") return "";
  const string w = app.wantedSpec();
  return w.size() == 11 ? w : "";
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
  if (!poster_.wic || poster_.id != want) return nullptr;
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
  for (int i = 0; i < 7; i++) L.scene[i] = {12, by + 16 + i * 46.f, 207, 44};
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
  L.planSeg = {mid - segW / 2, top, segW, segH};
  float bx = L.planSeg.x + 3;
  for (float w : bw) { L.plan.push_back({bx, L.planSeg.y + 3, w, 28}); bx += w + 2; }
  const float ringTop = top + segH + gap;
  L.ring = {mid - ringPx / 2, ringTop, ringPx, ringPx};
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
    y += 34 + 10;
    if (streamTab) {
      if (m.s.streamOn) {
        const size_t n = app.candidates().size();
        const bool custom = !m.s.custom.empty();
        L.playerNone = app.streamState == SS_NONE;
        const float ph = L.playerNone ? 72 : std::round(sw * 9 / 16);
        L.player = {sx, y, sw, ph};
        L.watch = {L.player.r() - 8 - 32, L.player.y + 8, 32, 32};
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
    y += 20 + 22;
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

  for (int i = 0; i < 7; i++) add(H_SCENE, i, L.scene[i], stock[(size_t)i].name + L" (" + std::to_wstring(i + 1) + L")");
  for (size_t i = 0; i < L.plan.size(); i++) add(H_PLAN, (int)i, L.plan[i]);
  add(H_PREV, 0, L.prev, L"Restart phase (←)");
  add(H_PLAY, 0, L.play, L"Play / pause (Space)");
  if (snap.cycle && !snap.done) add(H_NEXT, 0, L.next, L"Skip phase (→)");

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
    addSide(H_LINK, 0, L.link, L"Paste a YouTube link (Ctrl+V)");
    addSide(H_VIDEO, 0, L.power, m.s.streamOn ? L"Stream on / off" : L"Turn the stream on");
    if (m.s.streamOn) {
      if (!L.playerNone) {
        addSide(H_PLAYER, 0, L.player);
        addSide(H_WATCH, 0, L.watch, app.videoWindow ? L"Close the video window (V)" : L"Watch the video (V)");
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
  addSide(H_BLEND, 0, L.blendSlider.inset(0, -4), L"Video / music ↔ sounds");
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
  if (sheet_) {
    hits_.clear();
    const float sw = std::min(440.f, W - 32), sh = sheet_ == 1 ? SHEET_EVIDENCE_H : SHEET_KEYS_H;
    const R s = {(W - sw) / 2, std::max(8.f, (H - sh) / 2), sw, sh};
    add(H_SHEETBACK, 0, {0, 0, W, H});
    add(H_SIDE, 0, s);
    add(H_SHEETCLOSE, 0, {s.r() - 12 - 32, s.y + 12, 32, 32}, L"Close");
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

void Ui::setSlider(HitKind k, float x) {
  const R& r = k == H_MASTER ? L.master : L.blendSlider;
  const float p = (x - (r.x + 7)) / std::max(1.f, r.w - 14);
  const int v = (int)std::lround(clampd(p, 0, 1) * 100);
  if (k == H_MASTER) app.setMaster(v); else app.setBlend(v);
}

void Ui::mouseMove(float x, float y, bool leftDown) {
  ensureLayout();
  if (sliderDrag_ != H_NONE && leftDown) { setSlider(sliderDrag_, x); return; }
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
    setSlider(h.k, x);
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
    case H_NONE: case H_SIDE: case H_SHEETBACK: return 0;
    case H_TILE: return 2;
    default: return 1;
  }
}

bool Ui::key(int vk, bool ctrl, bool shift) {
  if (ctrl) return false;
  if (sheet_) {
    if (vk == VK_ESCAPE || vk == VK_RETURN) { openSheet(0); return true; }
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
    case 'V': app.toggleVideoWindow(); return true;
    case VK_OEM_2: if (shift) { openSheet(2); return true; } return false;
    default: break;
  }
  if (vk >= '1' && vk <= '7') { app.selectScene(stockScenes()[(size_t)(vk - '1')].id); return true; }
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
      if (app.streamState == SS_ERROR) app.retryStream();
      else if (!app.running) app.startAll();
      break;
    case H_VPREV: case H_FPREV: app.stepStream(-1); break;
    case H_VNEXT: case H_FNEXT: app.stepStream(1); break;
    case H_VCLEAR: app.clearCustom(); break;
    case H_FILEPICK: if (hooks.pickFiles) hooks.pickFiles(); break;
    case H_FCLEAR: app.clearFiles(); break;
    case H_SHEETCLOSE: case H_SHEETBACK: openSheet(0); break;
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
