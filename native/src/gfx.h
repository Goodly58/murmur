// Drawing layer: Direct2D + DirectWrite helpers, the colour palette and the icon set.
// Everything is laid out in DIPs (1 DIP = 1 CSS pixel at 100% scale); the render target's DPI does the scaling.
#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wincodec.h>

#include "common.h"

template <class T>
class Com {
 public:
  Com() = default;
  Com(T* p) : p_(p) {}
  Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
  Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  Com& operator=(const Com& o) { if (this != &o) { Com t(o); std::swap(p_, t.p_); } return *this; }
  Com& operator=(Com&& o) noexcept { if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
  ~Com() { reset(); }
  void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  T** put() { reset(); return &p_; }
  explicit operator bool() const { return p_ != nullptr; }
 private:
  T* p_ = nullptr;
};

struct R {
  float x = 0, y = 0, w = 0, h = 0;
  float r() const { return x + w; }
  float b() const { return y + h; }
  float cx() const { return x + w / 2; }
  float cy() const { return y + h / 2; }
  bool has(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  R inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
  R inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
  bool hits(const R& o) const { return x < o.r() && o.x < r() && y < o.b() && o.y < b(); }
  D2D1_RECT_F d() const { return D2D1::RectF(x, y, x + w, y + h); }
};
inline R rectFrom(float x, float y, float w, float h) { return {x, y, w, h}; }

using Color = D2D1_COLOR_F;
inline Color rgb(uint32_t hex, float a = 1) {
  return D2D1::ColorF(((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a);
}
inline Color withAlpha(Color c, float a) { c.a = a; return c; }
Color mixColor(Color a, Color b, float t);  // t=0 -> a

struct Palette {
  bool dark = true;
  Color bg, panel, raised, hover, line, line2, text, text2, text3, accent, accentInk, accentDim, calm, danger, ok;
};
Palette paletteFor(bool dark);

enum TextAlign { TX_LEFT = 0, TX_CENTER = 1, TX_RIGHT = 2 };

struct TextStyle {
  float size = 14;
  int weight = 400;
  Color color = D2D1::ColorF(1, 1, 1);
  int align = TX_LEFT;
  float spacing = 0;     // letter spacing in em
  bool tabular = false;  // tabular figures
  bool ellipsis = false; // trim with an ellipsis when it does not fit
  bool wrapNone = true;
};

class Gfx {
 public:
  bool init();
  void bind(ID2D1RenderTarget* rt, float dpi);   // call at the start of every paint
  ID2D1RenderTarget* rt = nullptr;
  float scale = 1;                              // device pixels per DIP

  Com<ID2D1Factory> d2d;
  Com<IDWriteFactory> dw;
  Com<IWICImagingFactory> wic;
  wstring family;
  Palette pal;

  // shapes
  void fill(const R& r, Color c);
  void fillRound(const R& r, float rad, Color c);
  void strokeRound(const R& r, float rad, Color c, float w = 1, bool dashed = false);  // stroke drawn inside the rect
  void fillCircle(float cx, float cy, float rad, Color c);
  void strokeCircle(float cx, float cy, float rad, Color c, float w);
  void line(float x1, float y1, float x2, float y2, Color c, float w = 1, bool roundCap = false);
  // Clockwise arc with round caps; angles in degrees, 0 = three o'clock, -90 = twelve.
  void arc(float cx, float cy, float rad, float startDeg, float sweepDeg, Color c, float w);
  float snap(float v) const { return std::round(v * scale) / scale; }
  R snap(const R& r) const { return {snap(r.x), snap(r.y), snap(r.r()) - snap(r.x), snap(r.b()) - snap(r.y)}; }
  void pushClip(const R& r) { rt->PushAxisAlignedClip(r.d(), D2D1_ANTIALIAS_MODE_ALIASED); }
  void popClip() { rt->PopAxisAlignedClip(); }
  void pushRoundClip(const R& r, float rad);
  void popLayer() { rt->PopLayer(); }

  // text
  float measure(const wstring& s, const TextStyle& st);
  void text(const wstring& s, const R& box, const TextStyle& st);  // vertically centred in box
  // Draws with the text's baseline on baselineY (left aligned at x); returns the width. For mixing sizes on one line.
  float textAt(const wstring& s, float x, float baselineY, const TextStyle& st);
  // icons: name from icons_gen.inc, drawn in a size x size square centred at (cx, cy)
  void icon(const char* name, float cx, float cy, float size, Color c, bool solid = false, float stroke = 1.6f);

  // gradients
  void verticalGradient(const R& r, Color top, Color bottom, ID2D1Geometry* clip = nullptr);

  ID2D1SolidColorBrush* brush(Color c);

 private:
  struct Shape { Com<ID2D1PathGeometry> g; bool filled; };
  struct Icon { string name; vector<Shape> shapes; };
  Com<ID2D1PathGeometry> parsePath(const char* d);
  const Icon* findIcon(const char* name);
  IDWriteTextFormat* format(float size, int weight);
  vector<Icon> icons_;
  std::map<uint32_t, Com<IDWriteTextFormat>> formats_;
  Com<ID2D1SolidColorBrush> brush_;
  Com<ID2D1StrokeStyle> round_, dashed_;
};
