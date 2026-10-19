// The window's contents: layout, painting and input. It draws with Direct2D into any render target (the
// window or an offscreen bitmap for snapshots) and talks to the App controller; it owns no Win32 window.
#pragma once
#include "app.h"
#include "gfx.h"

// Live spectrum of the mix on a log-frequency axis. Cheap on purpose: 15 fps, only repainted when the
// shape has moved enough to see, and it goes back to sleep once the shape settles.
struct Spectrum {
  static constexpr int POINTS = 96;
  static constexpr double FMIN = 30, FMAX = 16000;
  float vals[POINTS] = {0};   // smoothed target
  float shown[POINTS] = {0};  // what is currently painted
  vector<float> mags;         // smoothed FFT magnitudes
  int idle = 0;
  // Returns true when the picture changed enough to repaint. `live`: sound is playing. `asleep` is set when
  // the shape has settled and the caller can stop its timer.
  bool frame(const Engine& eng, bool live, bool* asleep);
  void markPainted() { std::memcpy(shown, vals, sizeof vals); }
  void setFlat(float v) { for (auto& x : vals) x = v; markPainted(); }
};

enum HitKind {
  H_NONE = 0, H_SCENE, H_PLAN, H_PREV, H_PLAY, H_NEXT, H_TILE, H_BAND, H_RESET, H_BEATMODE, H_MUTE, H_MASTER, H_BLEND,
  H_INFO, H_KEYS, H_CHIP, H_CHIPX, H_PADD, H_PUPD, H_TAB, H_LINK, H_VIDEO, H_WATCH, H_PLAYER, H_VPREV, H_VNEXT, H_VCLEAR,
  H_FILEPICK, H_FPREV, H_FNEXT, H_FCLEAR, H_LINKGO, H_SHEETCLOSE, H_SHEETBACK, H_EVLINK, H_TOASTACT, H_PBAR, H_SIDE
};

struct Hit {
  HitKind k = H_NONE;
  int i = 0;
  R r;
  wstring tip;
  bool operator==(const Hit& o) const { return k == o.k && i == o.i; }
  bool operator!=(const Hit& o) const { return !(*this == o); }
};

struct UiHooks {
  std::function<void()> invalidateAll;
  std::function<void(const R&)> invalidate;
  // Starts an inline text editor over `r`. `done(true, text)` on Enter or when focus leaves, `done(false, ...)` on Esc.
  std::function<void(const R& r, const wstring& text, int maxLen, bool onAccent, std::function<void(bool, const wstring&)> done)> edit;
  std::function<void(const wstring& url)> openUrl;
  std::function<void()> pickFiles;
  std::function<void(bool)> capture;
  // Shows a pop-up menu at a point (DIPs); returns the chosen index or -1.
  std::function<int(float x, float y, const vector<wstring>& items)> menu;
};

class Ui {
 public:
  Ui(App& app, Gfx& gfx, Spectrum& spec);

  App& app;
  Gfx& g;
  Spectrum& spec;
  UiHooks hooks;

  void setSize(float wDip, float hDip) { W_ = wDip; H_ = hDip; layoutDirty_ = true; }
  void setTheme(bool dark) { g.pal = paletteFor(dark); }
  float width() const { return W_; }
  float height() const { return H_; }

  // The caller has already called BeginDraw. `dirty` is the region to refresh (DIPs).
  void paint(ID2D1RenderTarget* rt, float dpi, const R& dirty);
  void changed() { layoutDirty_ = true; }  // state changed: the layout is rebuilt before the next paint or hit test

  // pointer
  void mouseMove(float x, float y, bool leftDown);
  void mouseDown(float x, float y);
  void mouseUp(float x, float y);
  void doubleClick(float x, float y);
  void rightClick(float x, float y);
  void wheel(float x, float y, int delta);
  void mouseLeave();
  bool key(int vk, bool ctrl, bool shift);  // returns true if handled
  int cursorAt(float x, float y);           // 0 arrow, 1 hand, 2 vertical resize
  bool dragActive() const { return dragging_ || sliderDrag_ != H_NONE; }

  // timing helpers for the window
  bool wantsSpinner() const { return app.streamState == SS_LOADING && app.model.s.streamOn && app.model.s.tab == "stream"; }
  void tipTimer();                    // the hover delay elapsed
  bool tipPending() const { return tipPending_; }
  void clearTip();

  // images
  void setPoster(const string& id, Com<IWICBitmap> bmp, bool failed);
  string posterIdFor() const;         // the video id whose thumbnail should be on screen ("" none)
  bool hasPoster(const string& id) const { return poster_.id == id && (poster_.wic || poster_.failed); }

  // modal panels
  void openSheet(int which) { sheet_ = which; layoutDirty_ = true; invalidateAll(); }
  int sheet() const { return sheet_; }
  void setDropping(bool d) { if (dropping_ != d) { dropping_ = d; invalidateAll(); } }
  bool linkOpen() const { return linkOpen_; }

  // used by the window, tests and snapshots
  const vector<Hit>& hits() { ensureLayout(); return hits_; }
  Hit find(HitKind k, int i = 0);
  float spinnerAngle = 0;
  float sideScroll() const { return sideScroll_; }
  R specRect() { ensureLayout(); return L.spec; }
  R ringRect() { ensureLayout(); return L.ring; }
  R statsRect() { ensureLayout(); return L.stats; }
  R playerRect() { ensureLayout(); return L.player; }
  R transportRect() { ensureLayout(); return L.transport; }
  void paintAll(ID2D1RenderTarget* rt, float dpi) { paint(rt, dpi, R{0, 0, W_, H_}); }
  void startRename(const string& presetId);

 private:
  struct Layout {
    R top, presets, rail, stage, side, ring, spec, stats, transport;
    R beatSeg, beatBtn[3], master, mute, info, keys;
    R pClip, pAdd, pUpd;
    vector<R> chip;
    vector<float> chipText;   // width available for each chip's name
    R scene[7];
    R planSeg;
    vector<R> plan;
    vector<string> planIds;
    R prev, play, next;
    R tabsSeg, tab[2], link, power, watch, player, vbar, vPrev, vTitle, vNext, vClear;
    R linkForm, linkInput, linkGo;
    R dropzone, tracks, fbar;
    R blendRow, blendSlider, blendL, blendR;
    R mixLabel, reset;
    R tile[10], band[5];
    float ringPx = 0, sideContentH = 0, pTotal = 0, specH = 0, playD = 68;
    bool playerNone = false;
    int fileRows = 0;
  } L;

  void ensureLayout() { if (layoutDirty_) build(); }
  void build();
  void add(HitKind k, int i, const R& r, const wstring& tip = L"");
  Hit hitAt(float x, float y);
  bool hot(HitKind k, int i = 0) const { return hover_.k == k && hover_.i == i; }

  // painting
  void paintTop();
  void paintPresets();
  void paintRail();
  void paintStage();
  void paintRing();
  void paintSpectrum();
  void paintSide();
  void paintPlayer();
  void paintFiles();
  void paintMix();
  void paintOverlays();
  void paintSheet();
  void seg(const R& r);
  void segButton(const R& r, bool selected, bool hovered);
  void iconBtn(const R& r, const char* icon, bool hovered, float size = 20, float alpha = 1, bool tintAccent = false);
  void slider(const R& r, int value);
  void shadow(const R& r, float rad, float alpha);
  void tag(float x, float y, const wstring& text, int tone, float* outW);
  bool visible(const R& r) const { return r.hits(dirty_); }
  wstring timeText(const SessionSnap& s) const;

  // actions
  void activate(const Hit& h);
  void setSlider(HitKind k, float x);
  void startLinkEdit();
  void scrollPresets(float delta);
  void ensureChipVisible();
  void invalidateAll() { if (hooks.invalidateAll) hooks.invalidateAll(); }
  void invalidate(const R& r) { if (hooks.invalidate) hooks.invalidate(r); }

  float W_ = 1180, H_ = 760;
  bool layoutDirty_ = true;
  vector<Hit> hits_;
  R dirty_;
  Hit hover_, press_;
  float pressY_ = 0, dragStartVal_ = 0;
  bool dragging_ = false;
  HitKind sliderDrag_ = H_NONE;
  int sheet_ = 0;  // 0 none, 1 evidence, 2 keys
  bool dropping_ = false, linkOpen_ = false;
  float sideScroll_ = 0, presetScroll_ = 0;
  bool tipPending_ = false, tipShown_ = false;
  wstring tipText_;
  R tipRect_;
  string lastScene_;
  struct Poster {
    string id;
    Com<IWICBitmap> wic;
    Com<ID2D1Bitmap> d2d;
    ID2D1RenderTarget* owner = nullptr;
    bool failed = false;
  } poster_;
  ID2D1Bitmap* posterBitmap();
  string editing_;  // preset id being renamed ("" none)
};

wstring fmtClock(int64_t ms, bool up);
