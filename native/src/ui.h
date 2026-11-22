// The window's contents: layout, painting and input. It draws with Direct2D into any render target (the
// window or an offscreen bitmap for snapshots) and talks to the App controller; it owns no Win32 window.
#pragma once
#include "app.h"
#include "gfx.h"

constexpr int KEYS_ROWS = 14;  // rows on the Keys sheet
constexpr float SHEET_EVIDENCE_H = 546, SHEET_KEYS_H = 64 + KEYS_ROWS * 38 + 4;  // modal sheet heights (DIPs)
constexpr float KEYS_LABEL_X = 212;  // where a row's words begin on the Keys sheet, after its keys and icons

// The task card and sheet (DIPs): the task, up to four steps under it and the row that adds one. The card has a heading of its own;
// the sheet already says "Task" at the top.
constexpr float TK_PAD = 14, TK_HEAD = 34, TK_TITLE = 38, TK_STEP = 30, TK_FOOT = 12;
constexpr float TK_ROWS_MAX = TK_TITLE + TASK_STEPS * TK_STEP + TK_FOOT;  // the tallest the rows get
constexpr float TK_BODY_MAX = TK_HEAD + TK_ROWS_MAX;                      // the tallest the card gets
constexpr float SHEET_TASK_TOP = 52, SHEET_TASK_MAX = SHEET_TASK_TOP + TK_ROWS_MAX;

// The compact player (DIPs). It is MINI_W wide and as tall as what it holds: the ring, the transport and the row of scenes,
// and under them, on the scenes that have tasks, the task with its steps (see Ui::miniHeight). The task's rows are a little
// tighter than the card's in the full window.
constexpr float MINI_W = 320;
constexpr float MT_TITLE = 32, MT_STEP = 27, MT_GAP = 10, MT_FOOT = 8, MINI_PAD = 12;

// The loop picker: a small panel under the loop button with the number of sets to run. The choices are 1 (no loop) to 6 and
// then "until stopped"; cell i holds loopChoice(i). It is anchored to the button, so its size is fixed here (DIPs).
constexpr int LOOP_CELLS = 7;
constexpr float LP_PAD = 14, LP_HEAD = 30, LP_CELL_W = 34, LP_CELL_H = 28, LP_SEG = 3, LP_FOOT = 34;
constexpr float LP_W = 2 * LP_PAD + 2 * LP_SEG + LOOP_CELLS * LP_CELL_W + (LOOP_CELLS - 1) * 2;
constexpr float LP_H = LP_HEAD + LP_CELL_H + 2 * LP_SEG + LP_FOOT;
inline int loopChoice(int cell) { return cell < LOOP_CELLS - 1 ? cell + 1 : SETS_FOREVER; }
inline int loopCell(int choice) {  // -1 for a count the cells do not have (--loop=8): the caption says it, no cell is lit
  return choice == SETS_FOREVER ? LOOP_CELLS - 1 : choice >= 1 && choice <= LOOP_CELLS - 1 ? choice - 1 : -1;
}

// The Spotify card in the side panel, in place of the video when the scene has a playlist (DIPs): the cover, what plays,
// and the lock that says whether Spotify gets to record it.
constexpr float SP_CARD_H = 112, SP_COVER = 88;

// The playlist picker: a sheet with a filter line and the listener's playlists under it.
constexpr float PK_W = 440, PK_HEAD = 52, PK_SEARCH = 36, PK_GAP = 10, PK_ROW = 44, PK_FOOT = 38;
constexpr int PK_ROWS_MAX = 9, PK_ROWS_MIN = 3;
constexpr size_t PK_FILTER_MAX = 60;

// The button that starts a finished plan again, inside the ring (DIPs).
constexpr float AGAIN_W = 124, AGAIN_H = 32;

// The Plugins sheet: a row for each plugin in the folder, and the button that opens that folder (DIPs).
constexpr float PG_W = 440, PG_HEAD = 56, PG_ROW = 58, PG_EMPTY = 64, PG_FOOT = 58;

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
  H_FILEPICK, H_FPREV, H_FNEXT, H_FCLEAR, H_LINKGO, H_SHEETCLOSE, H_SHEETBACK, H_EVLINK, H_TOASTACT, H_PBAR, H_SIDE, H_MINI, H_SETUP,
  H_LOOP,
  // the task. For the rows, i is 0 for the task and n for step n (so 1 to 4). H_TASK: i 0 is the button beside the plans, 1 the
  // line under the ring. H_TADD: i 0 writes the task, 1 adds a step.
  H_TASK, H_TCHECK, H_TTEXT, H_TDEL, H_TADD,
  H_LSET,   // a cell of the loop picker (i is the cell, see loopChoice)
  H_AGAIN,  // "Start again" in the ring of a finished plan
  H_SPOT,   // opens the playlist picker
  H_LOCK,   // private on or off, on the Spotify card
  H_PICK,   // a row of the playlist picker (i is its place in the filtered list)
  H_STATS,  // the focus statistics in the corner (only a tooltip)
  H_PLUGS,  // opens the Plugins sheet
  H_PLUG,   // a plugin's switch (i is its place in the list)
  H_PLUGSET,  // asks that plugin for its own settings window
  H_PLUGDIR   // opens the folder plugins are put in
};

struct Hit {
  HitKind k = H_NONE;
  int i = 0;
  R r;
  wstring tip;
  bool operator==(const Hit& o) const { return k == o.k && i == o.i; }
  bool operator!=(const Hit& o) const { return !(*this == o); }
};

// The words that explain the balance slider: what each end is, and which way the thumb is leaning.
struct BalanceWords {
  wstring left, right, lean;  // "Video", "Sounds", "More sounds"
  int side = 0;               // -1 leaning to the stream (or your music), 0 in the middle, 1 leaning to the sounds
};
BalanceWords balanceWords(int blend, bool streamTab);

// A row of the Keys sheet: the keys to press, the icons for what they do, and the same in a few words.
struct KeyRow {
  vector<const wchar_t*> keys;
  vector<const char*> icons;
  const wchar_t* label;
};
const vector<KeyRow>& keyRows();

// How an inline editor ended. Enter and moving away both keep the text; only Enter means "and carry on".
enum EditEnd { EDIT_CANCEL = 0, EDIT_BLUR = 1, EDIT_ENTER = 2 };

struct UiHooks {
  std::function<void()> invalidateAll;
  std::function<void(const R&)> invalidate;
  // Starts an inline text editor over `r`. `done(how, text)` runs once the editor is gone, so it may start another.
  std::function<void(const R& r, const wstring& text, int maxLen, bool onAccent, std::function<void(int how, const wstring&)> done)> edit;
  std::function<void(const wstring& url)> openUrl;
  std::function<void()> pickFiles;
  std::function<void(bool)> capture;
  // Shows a pop-up menu at a point (DIPs); returns the chosen index or -1.
  std::function<int(float x, float y, const vector<wstring>& items)> menu;
  std::function<void()> toggleMini;  // switch between the full window and the compact player
  std::function<void()> loadPlaylists;  // the picker opened: read the listener's playlists and hand them to setPlaylists
};

// A row of the playlist picker.
struct PickRow {
  enum Kind { None, Link, List } kind = List;  // "no playlist" (back to the stream), the link typed into the filter, a playlist
  int index = -1;                              // List: which one of the playlists
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
  bool character(wchar_t c);                // typed text: the playlist picker's filter takes it. True if it did
  void pasteInto(const wstring& text);      // Ctrl+V while the picker is open: a link is used at once, other text filters
  int cursorAt(float x, float y);           // 0 arrow, 1 hand, 2 vertical resize
  bool dragActive() const { return dragging_ || sliderDrag_ != H_NONE; }

  // timing helpers for the window
  bool wantsSpinner() const { return app.streamState == SS_LOADING && app.model.s.streamOn && app.model.s.tab == "stream"; }
  void tipTimer();                    // the hover delay elapsed
  bool tipPending() const { return tipPending_; }
  void clearTip();

  // images
  void setPoster(const string& id, Com<IWICBitmap> bmp, bool failed);
  // The picture that should be on screen: a video id (its thumbnail), or "s:" and the URL of a Spotify cover ("" none).
  string posterIdFor() const;

  // the playlist picker
  void setPlaylists(vector<SpotifyList> lists);
  const vector<PickRow>& pickRows() { ensureLayout(); return pick_; }
  const wstring& pickFilter() const { return pickFilter_; }
  int pickSelected() const { return pickSel_; }
  bool spotifyCard() const;           // the side panel shows the Spotify card, not the video
  bool shownPrivate() const;          // what the card's lock says: how it plays now, or how it will
  bool hasPoster(const string& id) const { return poster_.id == id && (poster_.wic || poster_.failed); }

  // modal panels
  void openSheet(int which);
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
  void setMini(bool on);
  bool mini() const { return mini_; }
  float miniHeight() const;  // how tall the compact player is with the task it holds now
  bool masterVertical() { ensureLayout(); return L.masterVert; }
  wstring loopCaption(int choice) const;  // what a choice of the loop picker comes to: the sets and the time they take
  TextStyle taskStyle(bool title, bool done) const;  // how the task (large) and its steps (small) are set, ticked or not

 private:
  struct Layout {
    R top, presets, rail, stage, side, ring, spec, stats, transport;
    R beatSeg, beatBtn[3], master, mute, info, keys, miniBtn;
    R pClip, pAdd, pUpd;
    vector<R> chip;
    vector<float> chipText;   // width available for each chip's name
    R scene[NSCENES];
    R planSeg, loopBtn;  // loopBtn: empty unless the chosen plan has rounds to loop (an ∞ plan or a timer has none)
    R taskBtn;           // opens the Task sheet; empty when the card is beside the ring instead, or the scene has no tasks
    R taskCard;          // the card beside the ring; empty when the window is too narrow for it
    vector<R> plan;
    vector<string> planIds;
    R prev, play, next;
    R tabsSeg, tab[2], link, power, watch, player, setup, vbar, vPrev, vTitle, vNext, vClear;
    R spot, cover, lock;  // the Spotify button beside the link, and on the card its cover and its lock
    R plugBtn;            // in the top bar: opens the Plugins sheet
    R linkForm, linkInput, linkGo;
    R dropzone, tracks, fbar;
    R blendRow, blendSlider, blendL, blendR, blendCap;  // blendCap: the words under the slider
    R mixLabel, reset;
    R tile[10], band[5];
    float ringPx = 0, sideContentH = 0, pTotal = 0, specH = 0, playD = 68;
    bool masterVert = false;  // the volume slider stands upright (the compact player's, under its expand button)
    bool playerNone = false;
    int fileRows = 0;
  } L;

  void ensureLayout() { if (layoutDirty_) build(); }
  void build();
  void buildMini();

  // The task is laid out once, as rows, and the same rows serve the card beside the ring and the Task sheet.
  enum { TR_TITLE, TR_STEP, TR_EMPTY, TR_ADD };
  static constexpr int NEW_STEP = TASK_STEPS + 1;  // the part number of a step that is being written
  struct TaskRow {
    int kind = TR_TITLE, part = 0;  // part: 0 is the task, n is step n
    R row, box, text, del;          // the whole row, its tick box, where the text goes (and the editor), the delete cross
  };
  struct TaskLayout {
    bool on = false;                // there is a task to show (not on Calm and Sleep)
    R card, label;                  // the card's outline and its heading; empty in the sheet
    vector<TaskRow> rows;
  } tv;
  float taskSheetH_ = SHEET_TASK_MAX;
  int taskEdit_ = -1;  // the part being edited (painting leaves its text to the editor)
  bool taskPinned() const { return L.taskCard.w > 0; }
  // Lays the task out as rows and returns the bottom edge. `card`: with the card's padding and heading.
  float buildTask(float x, float y, float w, bool card, float titleH = TK_TITLE, float stepH = TK_STEP, float foot = TK_FOOT);
  void addTaskHits();
  void paintTask();
  void editTask(int part);
  void taskNext();
  R sheetRect() const;
  R loopSeg() const;
  R loopCellRect(int i) const;
  struct RingGeo {  // the ring's centre block: the clock (or the tick when done), the phase name, the dots
    float ts, topH, y0, dotsY;
    R again;        // "Start again" under them when the plan is over and the ring has room (empty otherwise)
  };
  RingGeo ringGeo(const SessionSnap& s) const;
  R ringLine(const SessionSnap& s) const;          // where the task line goes under the dots (empty when there is no room)
  void paintAgain(const R& r);
  void paintLoopMenu();
  void paintPlugins();
  R plugRow(int i) const;     // the Plugins sheet: a plugin's row, its switch, its settings button, and the folder button
  R plugSwitch(int i) const;
  R plugGear(int i) const;
  R plugFolder() const;
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
  void paintSpotify();
  void paintPicker();
  void paintSetup();
  void paintFiles();
  void paintMix();
  void paintBalance();
  void paintOverlays();
  void paintMini();
  void paintTransport();
  void paintSheet();
  void seg(const R& r);
  void segButton(const R& r, bool selected, bool hovered);
  void iconBtn(const R& r, const char* icon, bool hovered, float size = 20, float alpha = 1, bool tintAccent = false);
  void slider(const R& r, int value);
  void vslider(const R& r, int value);  // the same, upright: full at the top
  void shadow(const R& r, float rad, float alpha);
  void tag(float x, float y, const wstring& text, int tone, float* outW);
  bool visible(const R& r) const { return r.hits(dirty_); }
  wstring timeText(const SessionSnap& s) const;

  // actions
  void activate(const Hit& h);
  void setSlider(HitKind k, float x, float y);
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
  int sheet_ = 0;  // 0 none, 1 evidence, 2 keys, 3 task, 4 the loop picker (a small panel, not a full sheet), 5 the playlist picker, 6 the plugins
  bool dropping_ = false, linkOpen_ = false, mini_ = false;
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

  // the playlist picker (sheet 5)
  vector<SpotifyList> lists_;
  bool listsLoaded_ = false;
  wstring pickFilter_;
  vector<PickRow> pick_;       // what the filter leaves, in order
  int pickSel_ = 0;            // the row the keyboard is on
  float pickScroll_ = 0;
  string pickUri_, pickScene_; // the scene and its playlist when the picker opened: it closes when either changes under it
  bool pickKeyChar_ = false;   // the key that opened the picker has its typed letter still on the way
  bool pickAuto_ = false;      // nothing was typed or moved yet: the keyboard sits on the playlist the scene has
  void refilter();
  void pickRow(int i);
  void pickMove(int d);
  int pickRowsShown() const;
  R pickSearchRect() const;
  R pickListRect() const;
};

wstring fmtClock(int64_t ms, bool up);
wstring fmtSpan(int64_t ms);  // a stretch of time in words: "45 min", "2 h", "7 h 55 min"
