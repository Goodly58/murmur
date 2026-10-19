#include "gfx.h"

namespace {
struct IconShapeDef {
  const char* d;
  bool filled;
};
struct IconDef {
  const char* name;
  vector<IconShapeDef> shapes;
};
const IconDef ICON_DEFS[] = {
#include "icons_gen.inc"
#include "icons_extra.inc"
};
}  // namespace

Color mixColor(Color a, Color b, float t) {
  return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

Palette paletteFor(bool dark) {
  Palette p;
  p.dark = dark;
  if (dark) {
    p.bg = rgb(0x0c0e11); p.panel = rgb(0x12151a); p.raised = rgb(0x181c22); p.hover = rgb(0x1e2229);
    p.line = rgb(0x20252c); p.line2 = rgb(0x2d333c);
    p.text = rgb(0xeceef1); p.text2 = rgb(0xa4acb7); p.text3 = rgb(0x6d7581);
    p.accent = rgb(0xf2b563); p.accentInk = rgb(0x1b1305); p.accentDim = rgb(0xf2b563, .16f);
    p.calm = rgb(0x6cc0b1); p.danger = rgb(0xef7d72); p.ok = rgb(0x7fc596);
  } else {
    p.bg = rgb(0xf5f3ef); p.panel = rgb(0xfbfaf7); p.raised = rgb(0xffffff); p.hover = rgb(0xedeae4);
    p.line = rgb(0xe2ded6); p.line2 = rgb(0xd1ccc1);
    p.text = rgb(0x191b1f); p.text2 = rgb(0x555c67); p.text3 = rgb(0x868c96);
    p.accent = rgb(0xa45f0c); p.accentInk = rgb(0xffffff); p.accentDim = rgb(0xa45f0c, .12f);
    p.calm = rgb(0x2a7f71); p.danger = rgb(0xc2443a); p.ok = rgb(0x2f7d4a);
  }
  return p;
}

bool Gfx::init() {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)d2d.put()))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dw.put()))) return false;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)wic.put());

  // Inter if it is installed (the web app uses it too), otherwise the Windows UI font
  Com<IDWriteFontCollection> col;
  dw->GetSystemFontCollection(col.put(), FALSE);
  for (const wchar_t* f : {L"Inter", L"Segoe UI Variable Text", L"Segoe UI"}) {
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (col && SUCCEEDED(col->FindFamilyName(f, &idx, &exists)) && exists) { family = f; break; }
  }
  if (family.empty()) family = L"Segoe UI";

  D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
  d2d->CreateStrokeStyle(sp, nullptr, 0, round_.put());
  D2D1_STROKE_STYLE_PROPERTIES dp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0);
  const float dashes[2] = {3, 3};
  d2d->CreateStrokeStyle(dp, dashes, 2, dashed_.put());

  for (const IconDef& def : ICON_DEFS) {
    Icon ic;
    ic.name = def.name;
    for (const IconShapeDef& s : def.shapes) ic.shapes.push_back({parsePath(s.d), s.filled});
    icons_.push_back(std::move(ic));
  }
  return true;
}

void Gfx::bind(ID2D1RenderTarget* target, float dpi) {
  rt = target;
  scale = dpi / 96.f;
  rt->SetDpi(dpi, dpi);
  rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  brush_.reset();
  rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.put());
}

ID2D1SolidColorBrush* Gfx::brush(Color c) {
  brush_->SetColor(c);
  return brush_.get();
}

// ---------------------------------------------------------------------------------------------------
// SVG path data -> geometry. Arcs map straight onto Direct2D's own SVG-style arc segment.
// ---------------------------------------------------------------------------------------------------

namespace {
struct PathReader {
  const char* p;
  void skip() { while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') p++; }
  bool number(float& out) {
    skip();
    char* end = nullptr;
    const double v = strtod(p, &end);
    if (end == p) return false;
    p = end;
    out = (float)v;
    return true;
  }
  bool flag(bool& out) {
    skip();
    if (*p != '0' && *p != '1') return false;
    out = *p == '1';
    p++;
    return true;
  }
  bool command(char& c) {
    skip();
    if (!*p) return false;
    if (std::isalpha((unsigned char)*p)) { c = *p++; return true; }
    return false;
  }
  bool moreNumbers() {
    skip();
    return *p == '-' || *p == '+' || *p == '.' || std::isdigit((unsigned char)*p);
  }
};
}  // namespace

Com<ID2D1PathGeometry> Gfx::parsePath(const char* d) {
  Com<ID2D1PathGeometry> geo;
  if (FAILED(d2d->CreatePathGeometry(geo.put()))) return geo;
  Com<ID2D1GeometrySink> sink;
  if (FAILED(geo->Open(sink.put()))) return geo;
  sink->SetFillMode(D2D1_FILL_MODE_WINDING);

  PathReader rd{d};
  D2D1_POINT_2F cur = {0, 0}, start = {0, 0}, lastCtl = {0, 0};
  char cmd = 0, prev = 0;
  bool open = false;
  auto begin = [&](D2D1_POINT_2F pt) {
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED);
    open = true;
    start = pt;
  };
  auto ensure = [&] { if (!open) begin(cur); };

  char c;
  while (true) {
    if (rd.command(c)) cmd = c;
    else if (!rd.moreNumbers() || !cmd) break;  // implicit repeat of the previous command, or the end
    const bool rel = std::islower((unsigned char)cmd) != 0;
    const char up = (char)std::toupper((unsigned char)cmd);
    float a[7];
    auto abs = [&](float x, float y) { return D2D1::Point2F(rel ? cur.x + x : x, rel ? cur.y + y : y); };
    bool ok = true;
    switch (up) {
      case 'M': {
        ok = rd.number(a[0]) && rd.number(a[1]);
        if (!ok) break;
        cur = abs(a[0], a[1]);
        begin(cur);
        cmd = rel ? 'l' : 'L';  // further pairs are line-tos
        break;
      }
      case 'L': ok = rd.number(a[0]) && rd.number(a[1]); if (!ok) break; ensure(); cur = abs(a[0], a[1]); sink->AddLine(cur); break;
      case 'H': ok = rd.number(a[0]); if (!ok) break; ensure(); cur.x = rel ? cur.x + a[0] : a[0]; sink->AddLine(cur); break;
      case 'V': ok = rd.number(a[0]); if (!ok) break; ensure(); cur.y = rel ? cur.y + a[0] : a[0]; sink->AddLine(cur); break;
      case 'C': {
        for (int i = 0; i < 6 && ok; i++) ok = rd.number(a[i]);
        if (!ok) break;
        ensure();
        const D2D1_POINT_2F c1 = abs(a[0], a[1]), c2 = abs(a[2], a[3]), e = abs(a[4], a[5]);
        sink->AddBezier(D2D1::BezierSegment(c1, c2, e));
        lastCtl = c2;
        cur = e;
        break;
      }
      case 'S': {
        for (int i = 0; i < 4 && ok; i++) ok = rd.number(a[i]);
        if (!ok) break;
        ensure();
        const bool afterCubic = prev == 'C' || prev == 'S';
        const D2D1_POINT_2F c1 = afterCubic ? D2D1::Point2F(2 * cur.x - lastCtl.x, 2 * cur.y - lastCtl.y) : cur;
        const D2D1_POINT_2F c2 = abs(a[0], a[1]), e = abs(a[2], a[3]);
        sink->AddBezier(D2D1::BezierSegment(c1, c2, e));
        lastCtl = c2;
        cur = e;
        break;
      }
      case 'Q': {
        for (int i = 0; i < 4 && ok; i++) ok = rd.number(a[i]);
        if (!ok) break;
        ensure();
        const D2D1_POINT_2F q = abs(a[0], a[1]), e = abs(a[2], a[3]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(q, e));
        lastCtl = q;
        cur = e;
        break;
      }
      case 'T': {
        ok = rd.number(a[0]) && rd.number(a[1]);
        if (!ok) break;
        ensure();
        const bool afterQuad = prev == 'Q' || prev == 'T';
        const D2D1_POINT_2F q = afterQuad ? D2D1::Point2F(2 * cur.x - lastCtl.x, 2 * cur.y - lastCtl.y) : cur;
        const D2D1_POINT_2F e = abs(a[0], a[1]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(q, e));
        lastCtl = q;
        cur = e;
        break;
      }
      case 'A': {
        bool large = false, sweep = false;
        ok = rd.number(a[0]) && rd.number(a[1]) && rd.number(a[2]) && rd.flag(large) && rd.flag(sweep) && rd.number(a[3]) && rd.number(a[4]);
        if (!ok) break;
        ensure();
        const D2D1_POINT_2F e = abs(a[3], a[4]);
        if (a[0] == 0 || a[1] == 0) sink->AddLine(e);
        else sink->AddArc(D2D1::ArcSegment(e, D2D1::SizeF(std::fabs(a[0]), std::fabs(a[1])), a[2],
                                           sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                           large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        cur = e;
        break;
      }
      case 'Z':
        if (open) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; }
        cur = start;
        break;
      default: ok = false;
    }
    if (!ok) break;
    prev = up;
    if (up == 'Z') cmd = 0;  // numbers after Z are not allowed
  }
  if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  return geo;
}

const Gfx::Icon* Gfx::findIcon(const char* name) {
  for (auto& i : icons_) if (i.name == name) return &i;
  return nullptr;
}

void Gfx::icon(const char* name, float cx, float cy, float size, Color c, bool solid, float stroke) {
  const Icon* ic = findIcon(name);
  if (!ic) return;
  D2D1_MATRIX_3X2_F old;
  rt->GetTransform(&old);
  const float k = size / 24.f;
  rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(cx - size / 2, cy - size / 2) * old);
  ID2D1SolidColorBrush* b = brush(c);
  for (const Shape& s : ic->shapes) {
    if (!s.g) continue;
    if (solid || s.filled) rt->FillGeometry(s.g.get(), b);
    if (!solid) rt->DrawGeometry(s.g.get(), b, stroke, round_.get());
  }
  rt->SetTransform(old);
}

// ---------------------------------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------------------------------

void Gfx::fill(const R& r, Color c) { rt->FillRectangle(r.d(), brush(c)); }

void Gfx::fillRound(const R& r, float rad, Color c) {
  rt->FillRoundedRectangle(D2D1::RoundedRect(r.d(), rad, rad), brush(c));
}

void Gfx::strokeRound(const R& r, float rad, Color c, float w, bool dashed) {
  const R in = r.inset(w / 2);
  rt->DrawRoundedRectangle(D2D1::RoundedRect(in.d(), std::max(0.f, rad - w / 2), std::max(0.f, rad - w / 2)), brush(c), w, dashed ? dashed_.get() : nullptr);
}

void Gfx::fillCircle(float cx, float cy, float rad, Color c) {
  rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), brush(c));
}

void Gfx::strokeCircle(float cx, float cy, float rad, Color c, float w) {
  rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), brush(c), w);
}

void Gfx::line(float x1, float y1, float x2, float y2, Color c, float w, bool roundCap) {
  rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush(c), w, roundCap ? round_.get() : nullptr);
}

void Gfx::arc(float cx, float cy, float rad, float startDeg, float sweepDeg, Color c, float w) {
  if (sweepDeg <= 0.05f) return;
  if (sweepDeg >= 359.9f) { strokeCircle(cx, cy, rad, c, w); return; }
  const float a0 = startDeg * 3.14159265f / 180.f, a1 = (startDeg + sweepDeg) * 3.14159265f / 180.f;
  Com<ID2D1PathGeometry> geo;
  if (FAILED(d2d->CreatePathGeometry(geo.put()))) return;
  Com<ID2D1GeometrySink> sink;
  if (FAILED(geo->Open(sink.put()))) return;
  sink->BeginFigure(D2D1::Point2F(cx + rad * std::cos(a0), cy + rad * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + rad * std::cos(a1), cy + rad * std::sin(a1)), D2D1::SizeF(rad, rad), 0,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE, sweepDeg > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  rt->DrawGeometry(geo.get(), brush(c), w, round_.get());
}

void Gfx::pushRoundClip(const R& r, float rad) {
  Com<ID2D1RoundedRectangleGeometry> g;
  d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r.d(), rad, rad), g.put());
  rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.get()), nullptr);
}

void Gfx::verticalGradient(const R& r, Color top, Color bottom, ID2D1Geometry* clip) {
  D2D1_GRADIENT_STOP stops[2] = {{0, top}, {1, bottom}};
  Com<ID2D1GradientStopCollection> sc;
  if (FAILED(rt->CreateGradientStopCollection(stops, 2, sc.put()))) return;
  Com<ID2D1LinearGradientBrush> gb;
  if (FAILED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.y), D2D1::Point2F(0, r.b())), sc.get(), gb.put()))) return;
  if (clip) rt->FillGeometry(clip, gb.get());
  else rt->FillRectangle(r.d(), gb.get());
}

// ---------------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------------

IDWriteTextFormat* Gfx::format(float size, int weight) {
  const uint32_t key = ((uint32_t)std::lround(size * 10) << 12) | (uint32_t)weight;
  auto it = formats_.find(key);
  if (it != formats_.end()) return it->second.get();
  Com<IDWriteTextFormat> f;
  if (FAILED(dw->CreateTextFormat(family.c_str(), nullptr, (DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", f.put()))) return nullptr;
  f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  IDWriteTextFormat* raw = f.get();
  formats_[key] = std::move(f);
  return raw;
}

static Com<IDWriteTextLayout> makeLayout(Gfx& g, IDWriteTextFormat* fmt, const wstring& s, float w, float h, const TextStyle& st) {
  Com<IDWriteTextLayout> l;
  if (!fmt || FAILED(g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, std::max(1.f, w), std::max(1.f, h), l.put()))) return l;
  const DWRITE_TEXT_RANGE all = {0, (UINT32)s.size()};
  if (st.spacing != 0) {
    Com<IDWriteTextLayout1> l1;
    if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)l1.put()))) l1->SetCharacterSpacing(0, st.spacing * st.size, 0, all);
  }
  if (st.tabular) {
    Com<IDWriteTypography> t;
    if (SUCCEEDED(g.dw->CreateTypography(t.put()))) {
      DWRITE_FONT_FEATURE f = {DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
      t->AddFontFeature(f);
      l->SetTypography(t.get(), all);
    }
  }
  return l;
}

float Gfx::measure(const wstring& s, const TextStyle& st) {
  if (s.empty()) return 0;
  Com<IDWriteTextLayout> l = makeLayout(*this, format(st.size, st.weight), s, 4000, st.size * 2, st);
  if (!l) return 0;
  DWRITE_TEXT_METRICS m;
  l->GetMetrics(&m);
  return m.widthIncludingTrailingWhitespace;
}

float Gfx::textAt(const wstring& s, float x, float baselineY, const TextStyle& st) {
  if (s.empty()) return 0;
  Com<IDWriteTextLayout> l = makeLayout(*this, format(st.size, st.weight), s, 4000, st.size * 3, st);
  if (!l) return 0;
  l->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  DWRITE_LINE_METRICS lm = {};
  UINT32 n = 0;
  l->GetLineMetrics(&lm, 1, &n);
  DWRITE_TEXT_METRICS tm;
  l->GetMetrics(&tm);
  rt->DrawTextLayout(D2D1::Point2F(x, baselineY - lm.baseline), l.get(), brush(st.color), D2D1_DRAW_TEXT_OPTIONS_NONE);
  return tm.widthIncludingTrailingWhitespace;
}

void Gfx::text(const wstring& s, const R& box, const TextStyle& st) {
  if (s.empty() || box.w <= 0 || box.h <= 0) return;
  Com<IDWriteTextLayout> l = makeLayout(*this, format(st.size, st.weight), s, box.w, box.h, st);
  if (!l) return;
  l->SetTextAlignment(st.align == TX_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : st.align == TX_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
  if (st.ellipsis) {
    Com<IDWriteInlineObject> sign;
    if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(l.get(), sign.put()))) {
      DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      l->SetTrimming(&t, sign.get());
    }
  }
  rt->DrawTextLayout(D2D1::Point2F(box.x, box.y), l.get(), brush(st.color), D2D1_DRAW_TEXT_OPTIONS_NONE);
}
