// Painting. Everything is drawn from the App's state and the layout built in ui.cpp; nothing is cached
// except the thumbnail, and every block checks the dirty rectangle first so a small repaint stays small.
#include "fft.h"
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
Color A(Color c, float a) { c.a *= a; return c; }

wstring baseName(const wstring& path) {
  const size_t i = path.find_last_of(L"\\/");
  return i == wstring::npos ? path : path.substr(i + 1);
}
}  // namespace

// ---------------------------------------------------------------------------------------------------
// Spectrum
// ---------------------------------------------------------------------------------------------------

bool Spectrum::frame(const Engine& eng, bool live, bool* asleep) {
  constexpr int N = 4096;
  constexpr double MIN_DB = -105, MAX_DB = -28;
  *asleep = false;
  float peak = 0, moved = 0;
  if (live) {
    static std::vector<float> buf(N);
    eng.copyScope(buf.data(), N);
    std::vector<std::complex<double>> x((size_t)N);
    for (int i = 0; i < N; i++) {
      const double w = 0.42 - 0.5 * std::cos(2 * 3.14159265358979 * i / N) + 0.08 * std::cos(4 * 3.14159265358979 * i / N);  // Blackman, as the web analyser
      x[(size_t)i] = buf[(size_t)i] * w;
    }
    fftInPlace(x);
    if (mags.size() != (size_t)N / 2) mags.assign((size_t)N / 2, 0.f);
    for (int k = 0; k < N / 2; k++) mags[(size_t)k] = 0.78f * mags[(size_t)k] + 0.22f * (float)(std::abs(x[(size_t)k]) / N);
    const double hz = (double)eng.sampleRate() / N;
    auto level = [&](int k) {
      if (k < 0) k = 0;
      if (k >= N / 2) k = N / 2 - 1;
      const double db = 20 * std::log10(std::max(1e-12, (double)mags[(size_t)k]));
      return (float)clampd((db - MIN_DB) / (MAX_DB - MIN_DB), 0, 1);
    };
    for (int i = 0; i < POINTS; i++) {
      const double a = FMIN * std::pow(FMAX / FMIN, (double)i / POINTS) / hz, b = FMIN * std::pow(FMAX / FMIN, (double)(i + 1) / POINTS) / hz;
      float s;
      if (b - a < 1) {
        const double c = (a + b) / 2;
        const int k = (int)std::floor(c);
        const double t = c - k;
        s = (float)(level(k) * (1 - t) + level(k + 1) * t);
      } else {
        float sum = 0;
        int n = 0;
        for (int k = (int)std::floor(a); k <= (int)std::floor(b) && k < N / 2; k++) { sum += level(k); n++; }
        s = n ? sum / n : 0;
      }
      vals[i] += (s - vals[i]) * 0.45f;
    }
  } else {
    for (int i = 0; i < POINTS; i++) vals[i] *= 0.82f;
    std::fill(mags.begin(), mags.end(), 0.f);
  }
  for (int i = 0; i < POINTS; i++) {
    peak = std::max(peak, vals[i]);
    moved += std::fabs(vals[i] - shown[i]);
  }
  moved /= POINTS;  // average change: single jittery bins do not keep the loop awake
  bool repaint = false;
  if (moved > 0.01f) { repaint = true; idle = 0; }
  else if (++idle > 30) *asleep = true;
  if (!live && peak < 0.004f) { repaint = true; *asleep = true; }
  return repaint;
}

// ---------------------------------------------------------------------------------------------------
// Entry point and small pieces
// ---------------------------------------------------------------------------------------------------

void Ui::paint(ID2D1RenderTarget* rt, float dpi, const R& dirty) {
  ensureLayout();
  g.bind(rt, dpi);
  const float s = dpi / 96.f;
  const float x0 = std::floor(dirty.x * s) / s, y0 = std::floor(dirty.y * s) / s;
  dirty_ = {x0, y0, std::ceil(dirty.r() * s) / s - x0, std::ceil(dirty.b() * s) / s - y0};
  g.pushClip(dirty_);
  g.fill(dirty_, g.pal.bg);
  if (mini_) {
    paintMini();
  } else {
    paintTop();
    paintPresets();
    paintRail();
    paintStage();
    paintSide();
  }
  paintOverlays();
  g.popClip();
}

void Ui::seg(const R& r) {
  g.fillRound(r, 10, g.pal.panel);
  g.strokeRound(r, 10, g.pal.line, 1);
}

void Ui::segButton(const R& r, bool selected, bool hovered) {
  (void)hovered;
  if (!selected) return;
  g.fillRound(r, 8, g.pal.raised);
  g.strokeRound(r.inset(-1), 9, g.pal.line2, 1);
}

void Ui::iconBtn(const R& r, const char* icon, bool hovered, float size, float alpha, bool tintAccent) {
  if (hovered) g.fillRound(r, 8, g.pal.hover);
  Color c = tintAccent ? g.pal.accent : (hovered ? g.pal.text : g.pal.text2);
  g.icon(icon, r.cx(), r.cy(), size, A(c, alpha));
}

void Ui::slider(const R& r, int value) {
  const Palette& P = g.pal;
  const float v = clampi(value, 0, 100) / 100.f;
  const float tx = r.x + 7 + (r.w - 14) * v, cy = r.cy();
  g.fillRound({r.x, cy - 1.5f, r.w, 3}, 1.5f, P.line2);
  g.fillRound({r.x, cy - 1.5f, tx - r.x, 3}, 1.5f, P.accent);
  g.fillCircle(tx, cy, 10, P.bg);
  g.fillCircle(tx, cy, 7, P.text);
}

void Ui::shadow(const R& r, float rad, float alpha) {
  for (int j = 10; j >= 1; j--) {
    const float t = 1 - j / 11.f;
    g.fillRound(r.inset(-j * 1.7f, -j * 1.7f).inset(0, 0), rad + j * 1.7f, rgb(0x000000, alpha * t * t * 0.16f + 0.002f));
  }
}

void Ui::tag(float x, float y, const wstring& text, int tone, float* outW) {
  const Palette& P = g.pal;
  const TextStyle st = T(12, 400, tone == 1 ? P.ok : tone == 2 ? P.danger : P.text2);
  const float w = g.measure(text, st) + 22;
  const R r = {x, y, w, 23};
  const Color border = tone == 1 ? A(P.ok, .55f) : tone == 2 ? A(P.danger, .55f) : P.line2;
  g.strokeRound(r, 11.5f, border, 1);
  g.text(text, {r.x + 11, r.y, w, r.h}, st);
  if (outW) *outW = w;
}

wstring Ui::timeText(const SessionSnap& s) const {
  const bool free = s.kind == PH_FREE;
  return fmtClock(s.done ? 0 : free ? s.elapsed : s.remaining, free);
}

// ---------------------------------------------------------------------------------------------------
// Top bar and presets
// ---------------------------------------------------------------------------------------------------

void Ui::paintTop() {
  if (!visible(L.top)) return;
  const Palette& P = g.pal;
  const Model& m = app.model;
  g.fill({0, 55, W_, 1}, P.line);
  g.icon("logo", 32, 28, 24, P.accent);
  TextStyle b = T(15, 600, P.text);
  b.spacing = .01f;
  g.text(L"Murmur", {54, 0, 140, 56}, b);

  seg(L.beatSeg);
  static const char* icons[3] = {"i-off", "i-phones", "i-speaker"};
  for (int i = 0; i < 3; i++) {
    const bool sel = m.s.beatsMode == i, h = hot(H_BEATMODE, i);
    segButton(L.beatBtn[i], sel, h);
    g.icon(icons[i], L.beatBtn[i].cx(), L.beatBtn[i].cy(), 18, sel ? P.accent : (h ? P.text : P.text2));
  }
  iconBtn(L.mute, app.muted || m.s.master == 0 ? "i-mute" : "i-vol", hot(H_MUTE));
  slider(L.master, m.s.master);
  iconBtn(L.info, "i-info", hot(H_INFO));
  iconBtn(L.miniBtn, "i-mini", hot(H_MINI));
  iconBtn(L.keys, "i-keys", hot(H_KEYS));
}

void Ui::paintPresets() {
  if (!visible(L.presets)) return;
  const Palette& P = g.pal;
  const Model& m = app.model;
  g.fill({0, 99, W_, 1}, P.line);
  g.icon("i-bookmark", 28, 78, 16, P.text3);
  TextStyle lab = T(12, 500, P.text3);
  lab.spacing = .02f;
  g.text(L"Custom", {43, 56, 120, 44}, lab);

  g.pushClip(L.pClip);
  const TextStyle base = T(12.5f, 500, P.text2);
  for (size_t i = 0; i < L.chip.size(); i++) {
    const Preset& p = m.s.presets[i];
    const R c = L.chip[i];
    if (!visible(c)) continue;
    const bool on = p.id == m.s.scene;
    const bool hv = hot(H_CHIP, (int)i) || hot(H_CHIPX, (int)i);
    const Color bg = on ? mixColor(P.panel, withAlpha(P.accent, 1), P.accentDim.a) : P.panel;
    g.fillRound(c, 8, bg);
    g.strokeRound(c, 8, on ? P.accent : (hv ? P.line2 : P.line), 1);
    const Color ink = on ? P.accent : (hv ? P.text : P.text2);
    const SceneDef* sd = findStock(p.base);
    g.icon(sd ? sd->icon.c_str() : "i-target", c.x + 1 + 10 + 7.5f, c.cy(), 15, ink);
    if (editing_ != p.id) {
      TextStyle st = base;
      st.color = ink;
      st.ellipsis = true;
      const wstring nm = on && m.presetDirty() ? p.name + L" •" : p.name;
      g.text(nm, {c.x + 1 + 10 + 15 + 7, c.y, L.chipText[i] + 0.5f, c.h}, st);
    }
    if (hv && editing_ != p.id) {  // delete cross over the chip's right end
      const R xr = {c.r() - 4 - 20, c.y + 4, 20, 20};
      g.fillRound({xr.x - 14, c.y + 1, 14 + 22, c.h - 2}, 7, bg);
      g.fillRound({xr.x - 6, xr.y - 3, 6, 26}, 0, bg);
      if (hot(H_CHIPX, (int)i)) g.fillRound(xr, 5, P.hover);
      g.icon("i-x", xr.cx(), xr.cy(), 12, hot(H_CHIPX, (int)i) ? P.danger : P.text3);
    }
  }
  // add button
  if (visible(L.pAdd)) {
    const bool hv = hot(H_PADD);
    const bool full = m.s.presets.empty();
    const bool atLimit = m.s.presets.size() >= (size_t)MAX_PRESETS;
    const Color ink = atLimit ? A(P.text3, .5f) : (hv ? P.text : P.text3);
    g.strokeRound(L.pAdd, 8, hv ? P.text3 : P.line2, 1, true);
    g.icon("i-plus", L.pAdd.x + (full ? 1 + 10 + 7.5f : L.pAdd.w / 2), L.pAdd.cy(), 15, ink);
    if (full) g.text(L"Save mix", {L.pAdd.x + 1 + 10 + 15 + 6, L.pAdd.y, 100, L.pAdd.h}, T(12.5f, 500, ink));
  }
  g.popClip();
  if (m.presetDirty()) iconBtn(L.pUpd, "i-check", hot(H_PUPD), 20, 1, true);
}

// ---------------------------------------------------------------------------------------------------
// Rail: scenes and the focus statistics
// ---------------------------------------------------------------------------------------------------

void Ui::paintRail() {
  if (!visible(L.rail)) return;
  const Palette& P = g.pal;
  const Model& m = app.model;
  g.fill({231, L.rail.y, 1, L.rail.h}, P.line);
  const auto& stock = stockScenes();
  for (int i = 0; i < NSCENES; i++) {
    const R r = L.scene[i];
    if (!visible({0, r.y, 232, r.h})) continue;
    const bool cur = m.s.scene == stock[(size_t)i].id, hv = hot(H_SCENE, i);
    if (cur) {
      g.fillRound(r, 10, P.raised);
      g.strokeRound(r.inset(-1), 11, P.line, 1);
      g.fillRound({-3, r.y + 11, 6, r.h - 22}, 3, P.accent);
    } else if (hv) {
      g.fillRound(r, 10, P.hover);
    }
    const Color ink = cur ? P.text : (hv ? P.text : P.text2);
    g.icon(stock[(size_t)i].icon.c_str(), r.x + 22, r.cy(), 20, cur ? P.accent : ink);
    g.text(stock[(size_t)i].name, {r.x + 44, r.y, 120, r.h}, T(14, 500, ink));
    TextStyle k = T(11, 400, P.text3, TX_RIGHT);
    k.tabular = true;
    g.text(std::to_wstring(i + 1), {r.r() - 12 - 20, r.y, 20, r.h}, k);
  }

  if (visible(L.stats)) {
    const R s = L.stats;
    g.fill({s.x, s.y, s.w, 1}, P.line);
    const int today = (int)std::floor(m.todayMinutes());
    wchar_t buf[32];
    if (today >= 60) _snwprintf(buf, 32, L"%dh %dm", today / 60, today % 60);
    else _snwprintf(buf, 32, L"%dm", today);
    TextStyle big = T(22, 300, P.text);
    big.tabular = true;
    big.spacing = -.01f;
    const float baseY = s.y + 14 + 24;
    const float w = g.textAt(buf, s.x, baseY, big);
    g.textAt(L"today", s.x + w + 8, baseY, T(12, 400, P.text3));
    const auto week = m.week();
    double mx = 30;
    for (double v : week) mx = std::max(mx, v);
    const float colW = (s.w - 6 * 5) / 7.f, bottom = s.b();
    for (int i = 0; i < 7; i++) {
      const float h = std::max(2.f, (float)std::max(4.0, week[(size_t)i] / mx * 100) / 100.f * 34);
      const float bx = s.x + i * (colW + 5) + colW / 2 - 3;
      g.fillRound({bx, bottom - h, 6, h}, 2, i == 6 ? P.accent : P.line2);
    }
  }
}

// ---------------------------------------------------------------------------------------------------
// Stage: plan selector, ring, transport and the spectrum
// ---------------------------------------------------------------------------------------------------

void Ui::paintStage() {
  if (!visible(L.stage)) return;
  const Palette& P = g.pal;
  const Model& m = app.model;
  const SessionSnap s = app.session.snapshot();

  if (visible(L.planSeg.inset(-3))) {
    seg(L.planSeg);
    const string cur = m.planId();
    for (size_t i = 0; i < L.plan.size(); i++) {
      const bool sel = L.planIds[i] == cur, hv = hot(H_PLAN, (int)i);
      segButton(L.plan[i], sel, hv);
      const PlanDef* pd = findPlan(L.planIds[i]);
      g.text(pd ? pd->label : L"?", L.plan[i], T(12.5f, 500, sel || hv ? P.text : P.text2, TX_CENTER));
    }
  }
  paintRing();
  paintTransport();
  paintSpectrum();
}

void Ui::paintTransport() {
  if (!visible(L.transport.inset(-3))) return;
  const Palette& P = g.pal;
  const SessionSnap s = app.session.snapshot();
  auto round = [&](const R& r, const char* icon, bool big, bool hv, bool disabled) {
    const float a = disabled ? .35f : 1;
    const float cx = r.cx(), cy = r.cy(), rad = r.w / 2;
    if (big) {
      const Color c = hv ? mixColor(P.accent, rgb(0xffffff), .07f) : P.accent;
      g.fillCircle(cx, cy, rad, c);
      g.icon(icon, cx, cy, r.w < 64 ? 24.f : 26.f, P.accentInk, true);
    } else {
      g.fillCircle(cx, cy, rad, A(P.panel, a));
      g.strokeCircle(cx, cy, rad - .5f, A(hv ? P.text3 : P.line2, a), 1);
      g.icon(icon, cx, cy, 20, A(hv ? P.text : P.text2, a));
    }
  };
  round(L.prev, "i-prev", false, hot(H_PREV), false);
  round(L.play, app.running ? "i-pause" : "i-play", true, hot(H_PLAY), false);
  round(L.next, "i-next", false, hot(H_NEXT), !(s.cycle && !s.done));
}

void Ui::paintRing() {
  if (!visible(L.ring.inset(-8))) return;
  const Palette& P = g.pal;
  const SessionSnap s = app.session.snapshot();
  const R rr = L.ring;
  const float k = L.ringPx / 320.f, cx = rr.cx(), cy = rr.cy();
  const bool brk = !s.done && s.kind == PH_BREAK;
  const Color ink = s.done ? P.ok : brk ? P.calm : P.accent;

  g.strokeCircle(cx, cy, 150 * k, P.line, 2 * k);
  const double prog = s.done ? 1.0 : clampd(s.progress, 0, 1);
  if (prog >= 0.0005) g.arc(cx, cy, 150 * k, -90, (float)(prog * 360), brk ? P.calm : P.accent, 3 * k);

  const int n = s.duration >= INF_MS ? 60 : clampi((int)std::ceil((double)s.duration / 60000.0), 1, 60);
  const int lit = (int)std::floor(prog * n + 1e-6);
  for (int i = 0; i < n; i++) {
    const float a = (-90.f + (float)i / n * 360.f) * 3.14159265f / 180.f, ca = std::cos(a), sa = std::sin(a);
    g.line(cx + 137 * k * ca, cy + 137 * k * sa, cx + 143 * k * ca, cy + 143 * k * sa, i < lit ? P.text3 : P.line2, 1.5f * k, true);
  }

  // centre: time, phase, round dots
  const wstring clock = timeText(s);
  const float ts = (float)clampd(L.ringPx * .24, 44, 80) * (clock.size() > 5 ? .74f : 1.f);  // h:mm:ss is wider
  const float topH = s.done ? 44.f : ts;
  const float blockH = topH + 8 + 16 + 8 + 8;
  const float y0 = cy - blockH / 2;
  if (s.done) {
    g.icon("i-check", cx, y0 + 22, 44, P.ok, false, 1.4f);
  } else {
    TextStyle t = T(ts, 200, P.text, TX_CENTER);
    t.tabular = true;
    t.spacing = -.02f;
    g.text(clock, {cx - L.ringPx * .45f, y0, L.ringPx * .9f, ts}, t);
  }
  wstring label = s.done ? L"Done" : brk ? L"Break" : app.model.scene().name;
  for (auto& c : label) c = (wchar_t)std::towupper(c);
  TextStyle ph = T(11, 600, ink, TX_CENTER);
  ph.spacing = .16f;
  ph.ellipsis = true;
  const float pw = L.ringPx * .7f;
  g.text(label, {cx - pw / 2, y0 + topH + 8, pw, 16}, ph);
  if (s.cycle && s.rounds > 0) {
    const int doneCount = s.done ? s.rounds : brk ? s.round : s.round - 1;
    const float total = s.rounds * 7.f + (s.rounds - 1) * 7.f;
    const float dy = y0 + topH + 8 + 16 + 8 + 4;
    for (int i = 0; i < s.rounds; i++) {
      const float dx = cx - total / 2 + i * 14 + 3.5f;
      if (i < doneCount) g.fillCircle(dx, dy, 3.5f, P.text3);
      else if (i == s.round - 1 && !brk && !s.done) g.fillCircle(dx, dy, 3.5f, P.accent);
      else g.strokeCircle(dx, dy, 2.75f, P.line2, 1.5f);
    }
  }
}

void Ui::paintSpectrum() {
  const R s = L.spec;
  if (!visible(s)) return;
  const Palette& P = g.pal;
  const float h = s.h;
  g.pushClip(s);
  for (int k = 1; k <= 3; k++) {
    const float y = g.snap(s.b() - h * k / 4.f) + .5f / g.scale;
    g.fill({s.x, y - .5f / g.scale, s.w, 1.f / g.scale}, P.line);
  }
  constexpr int N = Spectrum::POINTS;
  float xs[N], ys[N];
  for (int i = 0; i < N; i++) {
    xs[i] = s.x + (float)i / (N - 1) * s.w;
    ys[i] = s.b() - 2 - std::pow(spec.vals[i], 1.35f) * (h - 8);
  }
  auto build = [&](bool closed) {
    Com<ID2D1PathGeometry> geo;
    g.d2d->CreatePathGeometry(geo.put());
    Com<ID2D1GeometrySink> sink;
    geo->Open(sink.put());
    sink->BeginFigure(D2D1::Point2F(xs[0], ys[0]), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 1; i < N - 1; i++) {
      sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(xs[i], ys[i]), D2D1::Point2F((xs[i] + xs[i + 1]) / 2, (ys[i] + ys[i + 1]) / 2)));
    }
    sink->AddLine(D2D1::Point2F(xs[N - 1], ys[N - 1]));
    if (closed) {
      sink->AddLine(D2D1::Point2F(s.r(), s.b()));
      sink->AddLine(D2D1::Point2F(s.x, s.b()));
    }
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geo;
  };
  Com<ID2D1PathGeometry> fill = build(true), stroke = build(false);
  g.verticalGradient(s, A(P.accent, .34f), A(P.accent, 0), fill.get());
  Com<ID2D1StrokeStyle> js;
  D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
  g.d2d->CreateStrokeStyle(sp, nullptr, 0, js.put());
  g.rt->DrawGeometry(stroke.get(), g.brush(A(P.accent, .95f)), 1.5f, js.get());

  // frequency axis (the compact player has no room for it)
  if (!mini_) {
  const double span = std::log(Spectrum::FMAX / Spectrum::FMIN);
  auto at = [&](double f) { return s.x + (float)(std::log(f / Spectrum::FMIN) / span) * s.w; };
  for (double f : {50., 100., 200., 500., 1000., 2000., 5000., 10000.}) {
    const float x = g.snap(at(f));
    g.fill({x, s.b() - 7 - 4, 1.f / g.scale, 4}, P.line2);
  }
  struct Lab { double f; const wchar_t* t; };
  for (const Lab& l : {Lab{100, L"100"}, Lab{1000, L"1k"}, Lab{10000, L"10k"}}) {
    TextStyle t = T(10, 400, P.text3, TX_CENTER);
    t.tabular = true;
    g.text(l.t, {at(l.f) - 30, s.b() - 7 - 14, 60, 14}, t);
  }
  }
  g.popClip();
  spec.markPainted();
}

// ---------------------------------------------------------------------------------------------------
// Side panel: stream, balance and the mix
// ---------------------------------------------------------------------------------------------------

void Ui::paintSide() {
  if (!visible(L.side)) return;
  const Palette& P = g.pal;
  const Model& m = app.model;
  g.fill({L.side.x, L.side.y, 1, L.side.h}, P.line);
  g.pushClip({L.side.x + 1, L.side.y, L.side.w - 1, L.side.h});

  seg(L.tabsSeg);
  static const char* tabIcons[2] = {"i-video", "i-note"};
  for (int i = 0; i < 2; i++) {
    const bool sel = (m.s.tab == "stream") == (i == 0), hv = hot(H_TAB, i);
    segButton(L.tab[i], sel, hv);
    g.icon(tabIcons[i], L.tab[i].cx(), L.tab[i].cy(), 18, sel ? P.accent : (hv ? P.text : P.text2));
  }
  if (m.s.tab == "stream") {
    iconBtn(L.link, "i-link", hot(H_LINK), 20, 1, linkOpen_);
    iconBtn(L.power, "i-video", hot(H_VIDEO), 20, m.s.streamOn ? 1.f : .5f);
    if (m.s.streamOn) {
      paintPlayer();
      if (!L.playerNone && visible(L.vbar)) {
        if (L.vPrev.w > 0) iconBtn(L.vPrev, "i-prev", hot(H_VPREV), 20);
        if (L.vNext.w > 0) iconBtn(L.vNext, "i-next", hot(H_VNEXT), 20);
        if (!m.s.custom.empty()) iconBtn(L.vClear, "i-x", hot(H_VCLEAR), 20);
        TextStyle vt = T(12.5f, 400, P.text2, TX_CENTER);
        vt.ellipsis = true;
        wstring title = app.streamTitle;
        if (title.empty() && app.streamState == SS_LOADING) title = L"";
        g.text(title, L.vTitle, vt);
      }
      if (linkOpen_ && visible(L.linkForm)) {
        g.fillRound(L.linkInput, 8, P.panel);
        g.strokeRound(L.linkInput, 8, P.line2, 1);
        iconBtn(L.linkGo, "i-check", hot(H_LINKGO), 20, 1, true);
      }
    }
  } else {
    paintFiles();
  }

  // balance between the stream or music and the sounds
  if (visible(L.blendRow)) {
    g.icon(m.s.tab == "stream" ? "i-video" : "i-note", L.blendL.cx(), L.blendL.cy(), 18, P.text3);
    slider(L.blendSlider, m.s.blend);
    g.icon("i-pink", L.blendR.cx(), L.blendR.cy(), 18, P.text3);
  }
  paintMix();
  g.popClip();
}

void Ui::paintPlayer() {
  const R p = L.player;
  if (!visible(p.inset(-2))) return;
  const Palette& P = g.pal;
  if (L.playerNone) {
    g.fillRound(p, 12, P.panel);
    g.strokeRound(p, 12, P.line);
    g.icon("i-video", p.cx(), p.cy(), 28, P.text3);
    return;
  }
  g.fillRound(p, 12, rgb(0x000000));
  if (ID2D1Bitmap* bmp = posterBitmap()) {
    const D2D1_SIZE_F sz = bmp->GetSize();
    const float sc = std::max(p.w / sz.width, p.h / sz.height), dw = sz.width * sc, dh = sz.height * sc;
    g.pushRoundClip(p, 12);
    g.rt->DrawBitmap(bmp, D2D1::RectF(p.cx() - dw / 2, p.cy() - dh / 2, p.cx() + dw / 2, p.cy() + dh / 2), .82f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    g.popLayer();
  }
  g.strokeRound(p, 12, P.line, 1);
  if (app.toolsMissing()) {
    paintSetup();
    return;
  }

  const float cx = p.cx(), cy = p.cy();
  const StreamState st = app.streamState;
  if (st == SS_LOADING) {
    g.strokeCircle(cx, cy, 13, rgb(0xffffff, .25f), 2);
    g.arc(cx, cy, 13, spinnerAngle, 90, rgb(0xffffff), 2);
  } else if (st == SS_ERROR) {
    g.icon("i-alert", cx, cy, 28, P.danger);
  } else if (!app.running) {
    g.fillCircle(cx, cy, 26, rgb(0x0c0e11, hot(H_PLAYER) ? .8f : .6f));
    g.strokeCircle(cx, cy, 25.5f, rgb(0xffffff, .3f), 1);
    g.icon("i-play", cx + 1, cy, 24, rgb(0xffffff), true);
  }
  // open the video in its own window
  if (hot(H_PLAYER) || hot(H_WATCH) || app.videoWindow) {
    const R w = L.watch;
    g.fillRound(w, 8, rgb(0x0c0e11, hot(H_WATCH) ? .85f : .6f));
    g.icon(app.videoWindow ? "i-shrink" : "i-expand", w.cx(), w.cy(), 18, rgb(0xffffff));
  }
}

// The player on a PC without mpv or yt-dlp: one button that fetches them, and a bar while it does.
void Ui::paintSetup() {
  const Palette& P = g.pal;
  const R p = L.player;
  const bool running = app.setupState == SETUP_RUNNING, failed = app.setupState == SETUP_FAILED;
  g.fillRound(p, 12, rgb(0x000000, .62f));  // calms whatever thumbnail is behind
  g.icon(failed ? "i-alert" : "i-download", p.cx(), p.y + std::round(p.h * 0.26f), 26, failed ? P.danger : rgb(0xffffff, .72f));
  const float bw = std::min(196.f, p.w - 32), by = p.y + std::round(p.h * 0.42f);
  if (running) {
    g.text(app.setupStep, {p.x + 16, by, p.w - 32, 18}, T(12.5f, 500, rgb(0xffffff, .85f), TX_CENTER));
    const R track = {p.cx() - bw / 2, by + 28, bw, 4};
    g.fillRound(track, 2, rgb(0xffffff, .2f));
    g.fillRound({track.x, track.y, std::max(4.f, (float)(bw * app.setupFraction)), 4}, 2, P.accent);
    return;
  }
  const bool hv = hot(H_SETUP);
  g.fillRound(L.setup, L.setup.h / 2, hv ? mixColor(P.accent, rgb(0xffffff), .07f) : P.accent);
  g.text(failed ? L"Try again" : L"Set up streaming", L.setup, T(13, 600, P.accentInk, TX_CENTER));
  TextStyle cap = T(12, 400, failed ? P.danger : rgb(0xffffff, .6f), TX_CENTER);
  cap.ellipsis = true;
  g.text(failed ? app.setupError : wstring(L"mpv and yt-dlp, about 50 MB, once"), {p.x + 16, L.setup.b() + 10, p.w - 32, 16}, cap);
}

void Ui::paintFiles() {
  const Palette& P = g.pal;
  if (app.files.empty()) {
    const R d = L.dropzone;
    if (!visible(d)) return;
    const bool hv = hot(H_FILEPICK);
    g.strokeRound(d, 12, hv ? P.text3 : P.line2, 1, true);
    g.icon("i-note", d.cx(), d.cy() - 12, 20, hv ? P.text : P.text3);
    g.text(L"Drop audio", {d.x, d.cy() + 4, d.w, 22}, T(14, 400, hv ? P.text : P.text3, TX_CENTER));
    return;
  }
  if (L.fileRows > 0) {
    const int n = (int)app.files.size();
    int start = std::max(0, std::min(app.fileIndex - 1, n - L.fileRows));
    for (int r = 0; r < L.fileRows; r++) {
      const R row = {L.tracks.x, L.tracks.y + r * 29.f, L.tracks.w, 28};
      const int idx = start + r;
      const bool cur = idx == app.fileIndex;
      if (cur) g.fillRound(row, 6, P.raised);
      TextStyle st = T(12.5f, 400, cur ? P.text : P.text3);
      st.ellipsis = true;
      g.text(baseName(app.files[(size_t)idx]), {row.x + 8, row.y, row.w - 16, row.h}, st);
    }
  }
  if (visible(L.fbar)) {
    iconBtn(L.vPrev, "i-prev", hot(H_FPREV));
    iconBtn(L.vNext, "i-next", hot(H_FNEXT));
    iconBtn(L.vClear, "i-x", hot(H_FCLEAR));
    TextStyle vt = T(12.5f, 400, P.text2, TX_CENTER);
    vt.ellipsis = true;
    wstring now = app.streamTitle;
    if (now.empty() && app.fileIndex >= 0 && (size_t)app.fileIndex < app.files.size()) now = baseName(app.files[(size_t)app.fileIndex]);
    g.text(now, L.vTitle, vt);
  }
}

void Ui::paintMix() {
  const Palette& P = g.pal;
  const Model& m = app.model;
  const SessionSnap snap = app.session.snapshot();
  const bool brk = !snap.done && snap.kind == PH_BREAK;

  TextStyle lab = T(12, 500, P.text3);
  lab.spacing = .02f;
  if (visible(L.mixLabel)) g.text(L"Mix", L.mixLabel, lab);
  if (m.mixModified() && visible(L.reset)) iconBtn(L.reset, "i-reset", hot(H_RESET));

  const Mix mix = brk ? BREAK_MIX : m.mixOf();
  const BeatDef beat = m.beatOf();
  const bool beatsOn = beat.on && m.s.beatsMode != 0;
  for (int i = 0; i < 10; i++) {
    const R t = L.tile[i];
    if (!visible(t)) continue;
    const bool isBeat = i == 9;
    const int lv = isBeat ? beat.level : mix[(size_t)i];
    const bool on = isBeat ? beatsOn : lv > 0;
    const bool drag = dragging_ && press_.k == H_TILE && press_.i == i;
    const float a = brk ? .5f : (isBeat && m.s.beatsMode == 0 ? .4f : 1.f);
    g.fillRound(t, 10, A(P.raised, 1));
    if (on) {
      const float fh = (t.h - 2) * lv / 100.f;
      g.pushRoundClip(t.inset(1), 9);
      g.fill({t.x, t.b() - 1 - fh, t.w, fh + 1}, A(P.accentDim, 1));
      g.fill({t.x, t.b() - 1 - fh, t.w, 1}, A(P.accent, .55f));
      g.popLayer();
    }
    g.strokeRound(t, 10, A(drag ? P.accent : (hot(H_TILE, i) ? P.line2 : P.line), 1), 1);
    const Color ink = A(on ? P.text : P.text3, a);
    const float top = t.cy() - 45.f / 2;
    if (isBeat) {
      g.text(BANDS[beat.band].sym, {t.x, top, t.w, 22}, T(20, 400, ink, TX_CENTER));
      wchar_t hz[16];
      _snwprintf(hz, 16, L"%d Hz", BANDS[beat.band].hz);
      g.text(hz, {t.x, top + 29, t.w, 16}, T(11, 500, ink, TX_CENTER));
    } else {
      g.icon(CHANNELS[i].icon, t.cx(), top + 11, 22, ink);
      g.text(CHANNELS[i].name, {t.x, top + 29, t.w, 16}, T(11, 500, ink, TX_CENTER));
    }
    if (drag) {
      TextStyle v = T(10.5f, 400, P.accent, TX_RIGHT);
      v.tabular = true;
      g.text(std::to_wstring(lv), {t.r() - 6 - 30, t.y + 5, 30, 14}, v);
    }
  }
  for (int i = 0; i < 5; i++) {
    const R b = L.band[i];
    if (!visible(b)) continue;
    const bool sel = beat.band == i;
    const float a = brk ? .5f : (!beatsOn ? .4f : 1.f);
    g.fillRound(b, 10, A(P.raised, 1));
    g.strokeRound(b, 10, A(sel ? P.accent : (hot(H_BAND, i) ? P.line2 : P.line), 1), 1);
    g.text(BANDS[i].sym, {b.x, b.cy() - 14.3f, b.w, 17.6f}, T(16, 500, A(sel ? P.accent : P.text2, a), TX_CENTER));
    wchar_t hz[8];
    _snwprintf(hz, 8, L"%d", BANDS[i].hz);
    TextStyle sm = T(10, 400, A(P.text3, a), TX_CENTER);
    sm.tabular = true;
    g.text(hz, {b.x, b.cy() + 4, b.w, 11}, sm);
  }
}

// ---------------------------------------------------------------------------------------------------
// Overlays: toast, tooltip, drop target and the two sheets
// ---------------------------------------------------------------------------------------------------

void Ui::paintOverlays() {
  const Palette& P = g.pal;
  if (dropping_) {
    const R d = {12, 12, W_ - 24, H_ - 24};
    g.fillRound(d, 18, A(P.bg, .84f));
    g.strokeRound(d, 18, P.accent, 2, true);
    g.icon("i-note", d.cx(), d.cy(), 56, P.accent);
  }
  if (app.toast.active()) {
    const TextStyle ts = T(13, 400, P.text), as = T(13, 600, P.accent);
    const bool hasAct = !app.toast.action.empty();
    const float tw = g.measure(app.toast.msg, ts), aw = hasAct ? g.measure(app.toast.action, as) + 20 : 0;
    const float w = hasAct ? 16 + tw + 12 + aw + 8 : 16 + tw + 16, h = hasAct ? 45.f : 35.f;
    const R tr = {(W_ - w) / 2, H_ - 24 - h, w, h};
    if (visible(tr.inset(-20))) {
      shadow(tr, 12, 1);
      g.fillRound(tr, 12, P.raised);
      g.strokeRound(tr, 12, P.line2, 1);
      g.text(app.toast.msg, {tr.x + 16, tr.y, tw + 2, tr.h}, ts);
      if (hasAct) {
        const R ab = {tr.r() - 8 - aw, tr.y + 8, aw, 29};
        if (hot(H_TOASTACT)) g.fillRound(ab, 8, P.hover);
        g.text(app.toast.action, ab, T(13, 600, P.accent, TX_CENTER));
      }
    }
  }
  if (sheet_) paintSheet();
  if (tipShown_ && !tipText_.empty() && !sheet_) {
    const TextStyle ts = T(12, 400, P.text);
    const float w = g.measure(tipText_, ts) + 20, h = 26;
    const R a = hover_.r;
    float x = a.cx() - w / 2, y = a.b() + 8;
    if (x + w > W_ - 8) x = W_ - 8 - w;
    if (x < 8) x = 8;
    if (y + h > H_ - 8) y = a.y - 8 - h;
    tipRect_ = {x, y, w, h};
    g.fillRound(tipRect_, 7, P.raised);
    g.strokeRound(tipRect_, 7, P.line2, 1);
    g.text(tipText_, {x + 10, y, w, h}, ts);
  }
}

void Ui::paintSheet() {
  const Palette& P = g.pal;
  g.fill({0, 0, W_, H_}, rgb(0x05060a, .6f));
  const float sw = std::min(440.f, W_ - 32), sh = sheet_ == 1 ? SHEET_EVIDENCE_H : SHEET_KEYS_H;
  const R s = {(W_ - sw) / 2, std::max(8.f, (H_ - sh) / 2), sw, sh};
  shadow(s, 14, 1.4f);
  g.fillRound(s, 14, P.panel);
  g.strokeRound(s, 14, P.line2, 1);
  g.text(sheet_ == 1 ? L"Evidence" : L"Keys", {s.x + 20, s.y + 12, 200, 32}, T(14, 600, P.text));
  iconBtn({s.r() - 12 - 32, s.y + 12, 32, 32}, "i-x", hot(H_SHEETCLOSE));

  if (sheet_ == 1) {
    static const float itemH[5] = {106, 80, 80, 105, 80};
    static const char* icons[5] = {"i-white", "i-vol", "i-bubble", "i-phones", "i-off"};
    static const wchar_t* titles[5] = {L"Noise", L"Volume", L"Lyrics", L"Beats", L"432 Hz · solfeggio"};
    const float x2 = s.x + 20 + 22 + 14, colW = s.w - 40 - 22 - 14 - 24 - 14;
    float iy = s.y + 50;
    auto bar = [&](float y, const wchar_t* name, double gval, bool pos, const wchar_t* val) {
      g.text(name, {x2, y, 52, 18}, T(12.5f, 400, P.text2));
      const float bx = x2 + 62, bw = colW - 62 - 54;
      g.fillRound({bx, y + 6, bw, 6}, 3, P.line);
      g.fill({bx + bw / 2, y + 3, 1, 12}, P.text3);
      const float fw = (float)(gval * bw);
      if (pos) g.fillRound({bx + bw / 2, y + 6, fw, 6}, 3, P.accent);
      else g.fillRound({bx + bw / 2 - fw, y + 6, fw, 6}, 3, P.danger);
      g.text(val, {x2 + colW - 44, y, 44, 18}, T(12.5f, 500, P.text, TX_RIGHT));
    };
    for (int i = 0; i < 5; i++) {
      if (i) g.fill({s.x + 20, iy, s.w - 40, 1}, P.line);
      g.icon(icons[i], s.x + 20 + 11, iy + 14 + 11, 20, P.text2);
      g.text(titles[i], {x2, iy + 14, colW, 20}, T(14, 600, P.text));
      float y = iy + 14 + 28;
      switch (i) {
        case 0:
          bar(y, L"ADHD", .249, true, L"+0.25");
          bar(y + 25, L"Others", .212, false, L"−0.21");
          break;
        case 1: { float w, x = x2; tag(x, y, L"50 dB", 0, &w); x += w + 6; tag(x, y, L"70 dB", 1, &w); x += w + 6; tag(x, y, L"85 dB", 2, &w); break; }
        case 2: { float w, x = x2; tag(x, y, L"Instrumental", 1, &w); x += w + 6; tag(x, y, L"Words", 2, &w); break; }
        case 3: {
          bar(y, L"Pooled", .45, true, L"+0.45");
          tag(x2, y + 25, L"Mixed", 0, nullptr);
          break;
        }
        default: tag(x2, y, L"No evidence", 2, nullptr);
      }
      if (i != 4) {
        const R lk = {s.r() - 20 - 24, iy + 14, 24, 24};
        g.icon("i-link", lk.cx(), lk.cy(), 18, hot(H_EVLINK, i) ? P.accent : P.text3);
      }
      iy += itemH[i];
    }
    g.text(L"Hedges’ g, lab studies", {s.x + 20, iy + 4, 300, 17}, T(12, 400, P.text3));
  } else {
    struct Row { std::vector<const wchar_t*> keys; std::vector<const char*> icons; };
    const vector<Row> rows = {
        {{L"Space"}, {"i-play"}}, {{L"←", L"→"}, {"i-prev", "i-next"}}, {{L"↑", L"↓"}, {"i-vol"}}, {{L"M"}, {"i-mute"}},
        {{L"1", L"–", L"8"}, {"i-target"}}, {{L"B"}, {"i-phones"}}, {{L"V"}, {"i-video"}}, {{L"N"}, {"i-mini"}}, {{L"Ctrl", L"V"}, {"i-link"}}};
    float y = s.y + 56 + 8;
    for (const Row& r : rows) {
      float x = s.x + 20;
      for (const wchar_t* k : r.keys) {
        if (k[0] == L'–') { g.text(k, {x, y, 14, 26}, T(12, 400, P.text2, TX_CENTER)); x += 14 + 4; continue; }
        const TextStyle ks = T(12, 500, P.text2, TX_CENTER);
        const float w = std::max(26.f, g.measure(k, ks) + 14);
        g.strokeRound({x, y, w, 26}, 6, P.line2, 1);
        g.fill({x + 4, y + 25, w - 8, 1}, P.line2);
        g.text(k, {x, y, w, 25}, ks);
        x += w + 4;
      }
      float ix = s.x + 20 + 128;
      for (const char* ic : r.icons) {
        const bool solid = std::strcmp(ic, "i-play") == 0;
        g.icon(ic, ix + 10, y + 13, 20, P.text2, solid);
        ix += 28;
      }
      y += 26 + 12;
    }
  }
}

// ---------------------------------------------------------------------------------------------------
// The compact player
// ---------------------------------------------------------------------------------------------------

void Ui::paintMini() {
  const Palette& P = g.pal;
  const Model& m = app.model;
  const auto& stock = stockScenes();
  paintRing();
  paintTransport();
  for (int i = 0; i < NSCENES; i++) {
    const R r = L.scene[i];
    if (!visible(r)) continue;
    const bool cur = m.s.scene == stock[(size_t)i].id, hv = hot(H_SCENE, i);
    if (cur) {
      g.fillRound(r, 10, P.raised);
      g.strokeRound(r, 10, P.line, 1);
    } else if (hv) {
      g.fillRound(r, 10, P.hover);
    }
    g.icon(stock[(size_t)i].icon.c_str(), r.cx(), r.cy(), 20, cur ? P.accent : (hv ? P.text : P.text2));
  }
  if (visible(L.mute.inset(-4, -8))) {
    iconBtn(L.mute, app.muted || m.s.master == 0 ? "i-mute" : "i-vol", hot(H_MUTE), 18);
    slider(L.master, m.s.master);
  }
  iconBtn(L.miniBtn, "i-expand", hot(H_MINI), 18);
  paintSpectrum();
}
