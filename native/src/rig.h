// A window-less rig: the real App, Ui and drawing code around an offline audio engine, a recording stream
// and a fake clock. Used by the snapshot renderer and by the UI self-tests.
#pragma once
#include <dxgiformat.h>

#include "ui.h"

struct Rig {
  Gfx g;
  Engine eng;
  Stream st;
  Spectrum spec;
  std::unique_ptr<App> app;
  std::unique_ptr<Ui> ui;
  int64_t clock = 5000000;
  time_t today = 1760000000;
  vector<std::pair<R, wstring>> edits;                                   // inline editors the Ui asked for
  std::function<void(bool, const wstring&)> lastEditDone;
  int invalidations = 0;

  bool init(float w = 1180, float h = 760, bool dark = true) {
    if (!g.init()) return false;
    eng.start(false, 48000);
    st.setDryRun(true);
    app = std::make_unique<App>(eng, st);
    app->session.setClock([this] { return clock; });
    app->model.nowFn = [this] { return today; };
    app->init();
    ui = std::make_unique<Ui>(*app, g, spec);
    ui->setTheme(dark);
    ui->setSize(w, h);
    ui->hooks.invalidateAll = [this] { invalidations++; };
    ui->hooks.invalidate = [this](const R&) { invalidations++; };
    ui->hooks.edit = [this](const R& r, const wstring& t, int, bool, std::function<void(bool, const wstring&)> done) {
      edits.push_back({r, t});
      lastEditDone = std::move(done);
    };
    app->onChange = [this] { ui->changed(); };
    app->onTick = [this] { ui->changed(); };
    return true;
  }
  void advance(int64_t ms) { clock += ms; app->tick(); }
  void click(const Hit& h) {
    ui->mouseMove(h.r.cx(), h.r.cy(), false);
    ui->mouseDown(h.r.cx(), h.r.cy());
    ui->mouseUp(h.r.cx(), h.r.cy());
  }
  void click(HitKind k, int i = 0) { click(ui->find(k, i)); }
};

bool savePng(Gfx& g, IWICBitmap* bmp, const wstring& path);
int runSnapshot(int argc, wchar_t** argv);
int runMakeIcon(const wstring& outPath);
