// UI self-tests: the real Ui and drawing code, driven by synthetic input around a fake clock.
#include "resolver.h"
#include "rig.h"
#include "tests.h"

namespace {

struct Pix { int r, g, b; };

bool render(Rig& r, float scale, vector<uint8_t>& px, UINT& pw, UINT& ph) {
  pw = (UINT)std::lround(r.ui->width() * scale);
  ph = (UINT)std::lround(r.ui->height() * scale);
  Com<IWICBitmap> bmp;
  r.g.wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, bmp.put());
  Com<ID2D1RenderTarget> rt;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
  if (FAILED(r.g.d2d->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put()))) return false;
  rt->BeginDraw();
  r.ui->paintAll(rt.get(), 96 * scale);
  if (FAILED(rt->EndDraw())) return false;
  px.assign((size_t)pw * ph * 4, 0);
  return SUCCEEDED(bmp->CopyPixels(nullptr, pw * 4, (UINT)px.size(), px.data()));
}

Pix at(const vector<uint8_t>& px, UINT pw, int x, int y) {
  const uint8_t* p = &px[((size_t)y * pw + (size_t)x) * 4];
  return {p[2], p[1], p[0]};
}

bool closeTo(Pix a, Pix b, int tol) {
  return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

string pixStr(Pix p) { return std::to_string(p.r) + "," + std::to_string(p.g) + "," + std::to_string(p.b); }

int countKind(Ui& ui, HitKind k) {
  int n = 0;
  for (auto& h : ui.hits()) if (h.k == k) n++;
  return n;
}

}  // namespace

void testUi(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init()) { t.check("ui: Direct2D is available", false); return; }
  Ui& ui = *r.ui;
  App& app = *r.app;

  // ---- layout
  t.check("ui: seven scenes, ten tiles, five bands", countKind(ui, H_SCENE) == 7 && countKind(ui, H_TILE) == 10 && countKind(ui, H_BAND) == 5);
  t.check("ui: Focus offers three plans", countKind(ui, H_PLAN) == 3);
  t.check("ui: the three beat modes and the volume controls", countKind(ui, H_BEATMODE) == 3 && countKind(ui, H_MUTE) == 1 && countKind(ui, H_MASTER) == 1);
  t.check("ui: nothing to update or reset yet", countKind(ui, H_PUPD) == 0 && countKind(ui, H_RESET) == 0);
  for (auto sz : {std::pair<float, float>{940, 640}, {1180, 760}, {1600, 900}, {2560, 1300}}) {
    ui.setSize(sz.first, sz.second);
    bool inside = true;
    for (auto& h : ui.hits()) if (h.r.x < -1 || h.r.y < -1 || h.r.r() > sz.first + 1 || h.r.b() > sz.second + 1 || h.r.w <= 0 || h.r.h <= 0) inside = false;
    const Hit play = ui.find(H_PLAY);
    t.check("ui: layout holds together at " + std::to_string((int)sz.first) + "x" + std::to_string((int)sz.second),
            inside && play.k == H_PLAY && play.r.b() < ui.specRect().y + 1 && ui.ringRect().b() < play.r.y && ui.ringRect().x > 232 && ui.ringRect().r() < sz.first - 396 + 1);
  }
  ui.setSize(1180, 760);

  // ---- transport and scenes by pointer
  r.click(H_SCENE, 2);
  t.check("ui: clicking a scene starts it", app.model.s.scene == "create" && app.running);
  r.click(H_PLAY);
  t.check("ui: the play button pauses", !app.running);
  r.click(H_PLAY);
  t.check("ui: and plays again", app.running);
  r.click(H_PLAN, 1);
  t.check("ui: a plan button switches the plan", app.model.planId() == "p50");
  r.click(H_PLAN, 0);

  // ---- tiles: tap toggles, drag sets the level
  app.selectScene("focus");
  const int brown = channelIndex("brown"), rain = channelIndex("rain");
  const int before = app.model.mixOf()[(size_t)brown];
  r.click(H_TILE, brown);
  t.check("ui: tapping a sound switches it off", before > 0 && app.model.mixOf()[(size_t)brown] == 0);
  r.click(H_TILE, brown);
  t.check("ui: and on again at its last level", app.model.mixOf()[(size_t)brown] == before, std::to_string(app.model.mixOf()[(size_t)brown]));
  {
    const Hit tile = ui.find(H_TILE, rain);
    const float x = tile.r.cx(), y = tile.r.cy();
    ui.mouseMove(x, y, false);
    ui.mouseDown(x, y);
    ui.mouseMove(x, y - 2, true);
    t.check("ui: a small wobble is still a tap", app.model.mixOf()[(size_t)rain] == 0);
    ui.mouseMove(x, y - 30, true);
    ui.mouseMove(x, y - 36, true);
    ui.mouseUp(x, y - 36);
    const int want = (int)std::lround(clampd(36.0 / tile.r.h * 100, 0, 100));
    t.check("ui: dragging up raises the level in proportion", std::abs(app.model.mixOf()[(size_t)rain] - want) <= 1, std::to_string(app.model.mixOf()[(size_t)rain]) + " want " + std::to_string(want));
    ui.mouseDown(x, y);
    ui.mouseMove(x, y + 400, true);
    ui.mouseUp(x, y + 400);
    t.check("ui: dragging far down stops at zero", app.model.mixOf()[(size_t)rain] == 0);
    ui.mouseDown(x, y);
    ui.mouseMove(x, y - 400, true);
    ui.mouseUp(x, y - 400);
    t.check("ui: and far up at one hundred", app.model.mixOf()[(size_t)rain] == 100);
    app.resetMix();
  }
  {
    const Hit tile = ui.find(H_TILE, brown);
    const int cur = app.model.mixOf()[(size_t)brown];
    ui.wheel(tile.r.cx(), tile.r.cy(), 120);
    t.check("ui: the wheel over a tile nudges its level by five", app.model.mixOf()[(size_t)brown] == cur + 5);
    app.resetMix();
  }
  r.click(H_BAND, 4);
  t.check("ui: a band button picks the beat frequency", app.model.beatOf().band == 4 && app.model.beatOf().on);
  r.click(H_BEATMODE, 2);
  t.check("ui: the beat mode buttons set the mode", app.model.s.beatsMode == 2);
  r.click(H_BEATMODE, 0);
  t.check("ui: off silences the beat", app.model.s.beatsMode == 0 && app.effective().beat.mode == 0);
  r.click(H_BEATMODE, 1);
  app.resetMix();

  // ---- sliders
  {
    const Hit m = ui.find(H_MASTER);
    const float x25 = m.r.x + 7 + (m.r.w - 14) * .25f;
    ui.mouseMove(x25, m.r.cy(), false);
    ui.mouseDown(x25, m.r.cy());
    t.check("ui: pressing on the volume track jumps there", std::abs(app.model.s.master - 25) <= 1, std::to_string(app.model.s.master));
    ui.mouseMove(m.r.r() + 80, m.r.cy(), true);
    t.check("ui: dragging past the end stops at the maximum", app.model.s.master == 100);
    ui.mouseMove(m.r.x - 80, m.r.cy(), true);
    ui.mouseUp(m.r.x - 80, m.r.cy());
    t.check("ui: and at the minimum", app.model.s.master == 0);
    app.setMaster(70);
    const Hit b = ui.find(H_BLEND);
    ui.mouseDown(b.r.x + 7 + (b.r.w - 14) * .8f, b.r.cy());
    ui.mouseUp(b.r.x + 7 + (b.r.w - 14) * .8f, b.r.cy());
    t.check("ui: the balance slider moves toward the sounds", std::abs(app.model.s.blend - 80) <= 1, std::to_string(app.model.s.blend));
    app.setBlend(50);
  }

  // ---- presets
  app.selectScene("read");
  r.click(H_TILE, rain);
  r.edits.clear();
  r.click(H_PADD);
  t.check("ui: Save mix makes a preset and opens its name for editing", app.model.s.presets.size() == 1 && r.edits.size() == 1 && (bool)r.lastEditDone);
  r.lastEditDone(true, L"  Rainy desk ");
  t.check("ui: the name is trimmed and kept", app.model.s.presets[0].name == L"Rainy desk");
  t.check("ui: the chip is on the bar and active", ui.find(H_CHIP, 0).k == H_CHIP && app.model.s.scene == app.model.s.presets[0].id);
  r.click(H_PADD);
  r.lastEditDone(false, L"ignored");
  t.check("ui: Escape keeps the suggested name", app.model.s.presets.size() == 2 && app.model.s.presets[1].name != L"ignored");
  {
    const Hit chip = ui.find(H_CHIP, 0);
    r.edits.clear();
    ui.doubleClick(chip.r.cx(), chip.r.cy());
    t.check("ui: double-clicking a chip renames it", r.edits.size() == 1 && r.edits[0].second == L"Rainy desk");
    r.lastEditDone(true, L"Exam week");
    t.check("ui: the new name sticks", app.model.s.presets[0].name == L"Exam week");
  }
  r.click(H_SCENE, 0);
  r.click(H_CHIP, 0);
  t.check("ui: clicking a chip loads the preset", app.model.s.scene == app.model.s.presets[0].id);
  r.click(H_TILE, brown);
  t.check("ui: tweaking shows the update button", countKind(ui, H_PUPD) == 1 && countKind(ui, H_RESET) == 1);
  r.click(H_PUPD);
  t.check("ui: updating saves the tweak into the preset", app.model.s.presets[0].mix[(size_t)brown] > 0 && countKind(ui, H_PUPD) == 0);
  r.click(H_CHIPX, 1);
  t.check("ui: the cross on a chip deletes it and offers undo", app.model.s.presets.size() == 1 && app.toast.active() && countKind(ui, H_TOASTACT) == 1);
  r.click(H_TOASTACT);
  t.check("ui: Undo brings the preset back", app.model.s.presets.size() == 2);
  ui.hooks.menu = [](float, float, const vector<wstring>&) { return 1; };
  {
    const Hit chip = ui.find(H_CHIP, 1);
    ui.rightClick(chip.r.cx(), chip.r.cy());
    t.check("ui: the context menu's Delete removes a preset", app.model.s.presets.size() == 1);
  }

  // ---- sheets
  r.click(H_INFO);
  t.check("ui: the info button opens the evidence", ui.sheet() == 1 && countKind(ui, H_EVLINK) == 4 && countKind(ui, H_SCENE) == 0);
  wstring opened;
  ui.hooks.openUrl = [&](const wstring& u) { opened = u; };
  r.click(H_EVLINK, 0);
  t.check("ui: a study link opens in the browser", opened.find(L"ncbi.nlm.nih.gov") != wstring::npos);
  ui.key(VK_ESCAPE, false, false);
  t.check("ui: Escape closes the sheet", ui.sheet() == 0);
  r.click(H_KEYS);
  t.check("ui: the keys button opens the shortcut sheet", ui.sheet() == 2);
  ui.mouseMove(5, 5, false);
  ui.mouseDown(5, 5);
  ui.mouseUp(5, 5);
  t.check("ui: clicking outside closes it", ui.sheet() == 0);

  // ---- keyboard
  app.selectScene("focus");
  ui.key(VK_SPACE, false, false);
  t.check("ui: Space pauses", !app.running);
  ui.key(VK_SPACE, false, false);
  t.check("ui: and plays", app.running);
  ui.key('3', false, false);
  t.check("ui: a digit picks the scene", app.model.s.scene == "create");
  const int mode = app.model.s.beatsMode;
  ui.key('B', false, false);
  t.check("ui: B cycles the beat mode", app.model.s.beatsMode == (mode + 1) % 3);
  ui.key('B', false, false);
  ui.key('B', false, false);
  ui.key('M', false, false);
  t.check("ui: M mutes", app.muted);
  ui.key('M', false, false);
  app.setMaster(60);
  ui.key(VK_UP, false, false);
  t.check("ui: Up raises the volume by five", app.model.s.master == 65);
  ui.key(VK_DOWN, false, false);
  ui.key(VK_DOWN, false, false);
  t.check("ui: Down lowers it", app.model.s.master == 55);
  t.check("ui: Ctrl combinations are left to the window", !ui.key('V', true, false));
  ui.key(VK_OEM_2, false, true);
  t.check("ui: a question mark opens the shortcuts", ui.sheet() == 2);
  ui.key(VK_RETURN, false, false);
  t.check("ui: Enter closes a sheet", ui.sheet() == 0);

  // ---- stream panel
  app.selectScene("focus");
  t.check("ui: a thumbnail is wanted for the stream", ui.posterIdFor() == "rFZHOHl-L8A", ui.posterIdFor());
  r.click(H_LINK);
  t.check("ui: the link button opens the link field", ui.linkOpen() && (bool)r.lastEditDone);
  r.lastEditDone(true, L"https://www.youtube.com/watch?v=dQw4w9WgXcQ");
  t.check("ui: submitting a link pins that stream", app.model.s.custom == "dQw4w9WgXcQ" && !ui.linkOpen());
  t.check("ui: the thumbnail follows it", ui.posterIdFor() == "dQw4w9WgXcQ");
  r.click(H_VCLEAR);
  t.check("ui: the cross goes back to the scene's streams", app.model.s.custom.empty());
  r.click(H_VNEXT);
  t.check("ui: the arrows step through streams", app.wantedSpec() == "5yx6BWlEVcY", app.wantedSpec());
  app.streamState = SS_ERROR;
  ui.changed();
  const int loads = (int)std::count_if(r.st.commands.begin(), r.st.commands.end(), [](const string& c) { return c.compare(0, 4, "load") == 0; });
  r.click(H_PLAYER);
  const int loads2 = (int)std::count_if(r.st.commands.begin(), r.st.commands.end(), [](const string& c) { return c.compare(0, 4, "load") == 0; });
  t.check("ui: clicking the thumbnail after an error tries again", loads2 == loads + 1 && app.streamState == SS_LOADING);
  r.click(H_VIDEO);
  t.check("ui: the power button turns the stream off", !app.model.s.streamOn && countKind(ui, H_PLAYER) == 0 && ui.posterIdFor().empty());
  r.click(H_VIDEO);
  app.selectScene("mask");
  t.check("ui: scenes without a stream show no player controls", app.streamState == SS_NONE && countKind(ui, H_PLAYER) == 0 && countKind(ui, H_VNEXT) == 0);
  app.selectScene("focus");

  // ---- music files
  int picked = 0;
  ui.hooks.pickFiles = [&] { picked++; };
  r.click(H_TAB, 1);
  t.check("ui: the music tab shows the drop zone", app.model.s.tab == "files" && countKind(ui, H_FILEPICK) == 1);
  r.click(H_FILEPICK);
  t.check("ui: clicking it opens the file picker", picked == 1);
  app.addFiles({L"C:\\m\\a.mp3", L"C:\\m\\b.mp3", L"C:\\m\\c.flac"});
  t.check("ui: with files there are track controls instead", countKind(ui, H_FILEPICK) == 0 && countKind(ui, H_FNEXT) == 1 && countKind(ui, H_FCLEAR) == 1);
  r.click(H_FNEXT);
  t.check("ui: next goes to the next track", std::count(r.st.commands.begin(), r.st.commands.end(), "{\"command\":[\"playlist-next\"]}") == 1);
  r.click(H_FCLEAR);
  t.check("ui: clear empties the list", app.files.empty() && countKind(ui, H_FILEPICK) == 1);
  r.click(H_TAB, 0);

  // ---- breaks lock the mix
  app.selectScene("focus");
  r.advance(1000);
  r.advance(25 * 60000 + 500);
  t.check("ui: during a break the tiles and bands cannot be touched", app.session.snapshot().kind == PH_BREAK && countKind(ui, H_TILE) == 0 && countKind(ui, H_BAND) == 0);
  r.advance(5 * 60000);
  t.check("ui: and come back with the next focus phase", countKind(ui, H_TILE) == 10);

  // ---- hover and tooltips
  {
    const Hit h = ui.find(H_SCENE, 3);
    ui.mouseMove(h.r.cx(), h.r.cy(), false);
    t.check("ui: hovering a scene starts the tooltip delay", ui.tipPending());
    ui.tipTimer();
    t.check("ui: the delay ends and the tip is shown", !ui.tipPending());
    ui.mouseLeave();
    t.check("ui: the cursor is a hand over buttons and a resize arrow over tiles", ui.cursorAt(h.r.cx(), h.r.cy()) == 1 && ui.cursorAt(ui.find(H_TILE, 0).r.cx(), ui.find(H_TILE, 0).r.cy()) == 2 && ui.cursorAt(5, 700) == 0);
  }

  // ---- the side panel scrolls when the window is short
  ui.setSize(1100, 640);
  {
    const Hit before2 = ui.find(H_BAND, 0);
    ui.wheel(1000, 400, -120);
    ui.wheel(1000, 400, -120);
    const Hit after = ui.find(H_BAND, 0);
    t.check("ui: a short window scrolls the side panel until the bands come into view", ui.sideScroll() > 0 && before2.k == H_NONE && after.k == H_BAND, std::to_string(ui.sideScroll()));
  }
  ui.setSize(1180, 760);
  ui.wheel(1000, 400, 1200);

  // ---- drawing: real pixels
  {
    app.selectScene("focus");
    app.stopAll();
    ui.mouseLeave();
    ui.changed();
    vector<uint8_t> px;
    UINT pw = 0, ph = 0;
    const bool ok = render(r, 1.0f, px, pw, ph);
    t.check("ui: the window renders offscreen", ok && pw == 1180 && ph == 760);
    if (ok) {
      const Palette P = paletteFor(true);
      auto col = [](Color c) { return Pix{(int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255)}; };
      t.check("ui: the background is the dark theme's", closeTo(at(px, pw, 6, 400), col(P.bg), 1), pixStr(at(px, pw, 6, 400)));
      const Hit play = ui.find(H_PLAY);
      t.check("ui: the play button is amber", closeTo(at(px, pw, (int)(play.r.cx() - 20), (int)play.r.cy()), col(P.accent), 3), pixStr(at(px, pw, (int)(play.r.cx() - 20), (int)play.r.cy())));
      const Hit tile = ui.find(H_TILE, brown);
      const Pix fill = at(px, pw, (int)(tile.r.x + 8), (int)(tile.r.b() - 8));
      t.check("ui: a lit tile is filled from the bottom", closeTo(fill, Pix{59, 52, 45}, 6), pixStr(fill));
      const Hit off = ui.find(H_TILE, channelIndex("fire"));
      t.check("ui: an unused tile is plain", closeTo(at(px, pw, (int)(off.r.x + 8), (int)(off.r.b() - 8)), col(P.raised), 2));
      const Hit sc = ui.find(H_SCENE, 0);
      t.check("ui: the current scene has its accent bar", closeTo(at(px, pw, 1, (int)sc.r.cy()), col(P.accent), 3), pixStr(at(px, pw, 1, (int)sc.r.cy())));
      // a quarter of the way round the ring the progress arc is amber; the opposite side is only the track
      r.advance(1000);
      app.startAll();
      r.advance(6 * 60000 + 15000);  // 6:15 of 25:00 gone
      ui.changed();
      render(r, 1.0f, px, pw, ph);
      const R ring = ui.ringRect();
      const float rad = ring.w / 2 * (150.f / 160.f), cx = ring.cx(), cy = ring.cy();
      const Pix onArc = at(px, pw, (int)std::lround(cx + rad), (int)std::lround(cy));  // three o'clock: 25% of the circle
      const Pix offArc = at(px, pw, (int)std::lround(cx - rad), (int)std::lround(cy));
      t.check("ui: the ring's progress arc is drawn up to the elapsed time", closeTo(onArc, col(P.accent), 40) && !closeTo(offArc, col(P.accent), 40), pixStr(onArc) + " vs " + pixStr(offArc));
      // light theme and a high-DPI render
      ui.setTheme(false);
      const Palette L = paletteFor(false);
      render(r, 1.5f, px, pw, ph);
      t.check("ui: the light theme and 150% scale render", pw == 1770 && closeTo(at(px, pw, 9, 600), col(L.bg), 1), pixStr(at(px, pw, 9, 600)));
      const Hit play2 = ui.find(H_PLAY);
      t.check("ui: scaled, the play button is still where the layout says", closeTo(at(px, pw, (int)((play2.r.cx() - 20) * 1.5f), (int)(play2.r.cy() * 1.5f)), col(L.accent), 3));
      ui.setTheme(true);
    }
  }
}

void testResolver(TestLog& t) {
  const string sample =
      "1-LpQekNa9g\tis_live\t24/7 halloween ambience  spooky lofi to get chills to\n"
      "nI725iVsyoQ\tis_live\t24/7 deep sleep music  calm ambient to sleep & dream to\n"
      "3PFJ9SETS4M\tis_live\tlofi house radio   lounge music to vibe/chill to\n"
      "rFZHOHl-L8A\tis_live\tlofi hip hop radio  beats to relax/study to\n"
      "VAlMDl00mYY\tis_live\tsleep lofi radio  relaxing music to sleep/dream to\n"
      "CwPCy1GLS38\tis_live\tsad lofi radio  beats for rainy days\n"
      "JD-kMIpDfnY\tis_live\tlofi hip hop radio  beats to sleep/chill to\r\n"
      "jXAEIWcGXwE\tis_live\tclassical music radio  relaxing songs to read/study to\n"
      "q_4KI-ChIIs\tis_live\tfireplace ambience  cozy sound to chill to\n"
      "DWcJFNfaw9c\twas_live\tlofi hip hop radio - beats to sleep/chill to\n"
      "5qap5aO4i9A\twas_live\tlofi hip hop radio - beats to relax/study to\n"
      "garbage line without tabs\n"
      "short\tis_live\tbad id\n";
  const auto entries = parseFlatList(sample);
  t.check("resolver: lines are parsed and bad ones skipped", entries.size() == 11, std::to_string(entries.size()));
  const auto m = matchStreams(entries);
  t.check("resolver: Focus finds the current lofi radio first", m.count("focus") && m.at("focus")[0] == "rFZHOHl-L8A");
  t.check("resolver: Focus keeps sleep and halloween streams out", m.count("focus") && std::find(m.at("focus").begin(), m.at("focus").end(), "JD-kMIpDfnY") == m.at("focus").end() &&
                                                                    std::find(m.at("focus").begin(), m.at("focus").end(), "1-LpQekNa9g") == m.at("focus").end());
  t.check("resolver: Sleep finds the deep sleep stream", m.count("sleep") && m.at("sleep")[0] == "nI725iVsyoQ");
  t.check("resolver: Read gets the classical radio and Calm the fireplace", m.count("read") && m.at("read")[0] == "jXAEIWcGXwE" && m.count("calm") && m.at("calm")[0] == "q_4KI-ChIIs");
  t.check("resolver: ended streams are not offered", !m.count("create") || std::find(m.at("create").begin(), m.at("create").end(), "DWcJFNfaw9c") == m.at("create").end());
  t.check("resolver: scenes with no stream get nothing", !m.count("mask") && !m.count("white"));
}

// ---------------------------------------------------------------------------------------------------
// Launch requests, the compact player and losing the sound output.
// ---------------------------------------------------------------------------------------------------

void testLaunch(TestLog& t) {
  Launch l = parseLaunch({L"--scene", L"sleep", L"--plan", L"t60", L"--play"});
  t.check("launch: scene, plan and play are read", l.scene == "sleep" && l.plan == "t60" && l.play && !l.mini);
  l = parseLaunch({L"--scene=calm", L"--plan=t20", L"--mini"});
  t.check("launch: the --name=value form works", l.scene == "calm" && l.plan == "t20" && l.mini);
  l = parseLaunch({L"https://youtu.be/dQw4w9WgXcQ", L"C:/m/a.MP3", L"notes.txt", L"--bogus", L"--full"});
  t.check("launch: links, audio files and --full are found, the rest is ignored", l.links.size() == 1 && l.files.size() == 1 && l.full && l.scene.empty());
  l = parseLaunch({L"--scene"});
  t.check("launch: a flag without a value is harmless", l.scene.empty());

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init()) { t.check("launch: the rig starts", false); return; }
  App& app = *r.app;
  Launch go;
  go.scene = "sleep";
  go.plan = "t60";
  go.play = true;
  app.applyLaunch(go);
  t.check("launch: --scene sleep --plan t60 --play starts that session", app.model.s.scene == "sleep" && app.model.planId() == "t60" && app.session.planId() == "t60" && app.running);
  r.advance(5000);
  t.check("launch: the session is running", app.session.snapshot().elapsed >= 4000);
  app.applyLaunch(go);
  t.check("launch: asking again starts the timer afresh, like picking the plan again", app.session.snapshot().elapsed < 2000 && app.running);
  app.stopAll();
  Launch pick;
  pick.scene = "read";
  app.applyLaunch(pick);
  t.check("launch: a scene without --play only selects it", app.model.s.scene == "read" && !app.running);
  Launch bad;
  bad.scene = "nope";
  bad.plan = "zzz";
  app.applyLaunch(bad);
  t.check("launch: unknown names change nothing", app.model.s.scene == "read" && app.model.planId() == "p50");
  Launch link;
  link.links = {L"https://youtu.be/dQw4w9WgXcQ"};
  app.applyLaunch(link);
  t.check("launch: a link pins that stream and starts it", app.model.s.custom == "dQw4w9WgXcQ" && app.running);

  // the sound output goes away
  int lost = 0;
  r.eng.onOutputLost = [&] { lost++; app.outputLost(); };
  r.eng.simulateOutputLost();
  t.check("output: losing the device stops playback and says why",
          lost == 1 && !app.running && !r.eng.playing() && app.toast.active() && app.toast.msg.find(L"removed") != wstring::npos);
  app.toast = Toast();
  app.outputLost();
  t.check("output: when nothing was playing there is nothing to say", !app.toast.active());

  // state of the compact player
  Model m;
  m.s.mini = true;
  m.s.miniX = 1200;
  m.s.miniY = 700;
  Model back;
  back.load(m.serialize());
  t.check("model: the compact player's state is kept", back.s.mini && back.s.miniX == 1200 && back.s.miniY == 700);
  Model fresh;
  fresh.load("{}");
  t.check("model: and defaults to the full window", !fresh.s.mini && fresh.s.miniX == INT32_MIN);
}

void testMini(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init()) { t.check("mini: the rig starts", false); return; }
  Ui& ui = *r.ui;
  App& app = *r.app;
  int toggled = 0;
  ui.hooks.toggleMini = [&] { toggled++; };

  t.check("mini: the full window has a mini-player button", ui.find(H_MINI).k == H_MINI);
  r.click(H_MINI);
  t.check("mini: clicking it asks the window to switch", toggled == 1);
  ui.key('N', false, false);
  t.check("mini: so does the N key", toggled == 2);

  ui.setMini(true);
  ui.setSize(320, 484);
  t.check("mini: seven scenes, transport, mute, volume and the way back",
          countKind(ui, H_SCENE) == 7 && countKind(ui, H_PLAY) == 1 && countKind(ui, H_PREV) == 1 && countKind(ui, H_NEXT) == 1 && countKind(ui, H_MUTE) == 1 &&
              countKind(ui, H_MASTER) == 1 && countKind(ui, H_MINI) == 1);
  t.check("mini: no mix tiles, bands, plans or presets", countKind(ui, H_TILE) == 0 && countKind(ui, H_BAND) == 0 && countKind(ui, H_PLAN) == 0 && countKind(ui, H_CHIP) == 0);
  bool inside = true;
  for (auto& h : ui.hits()) if (h.r.x < 0 || h.r.y < 0 || h.r.r() > 321 || h.r.b() > 485) inside = false;
  t.check("mini: everything fits the compact window", inside);
  const Hit play = ui.find(H_PLAY);
  t.check("mini: the ring and transport sit above the scene row and the spectrum strip",
          ui.ringRect().b() < play.r.y && play.r.b() < ui.find(H_SCENE, 0).r.y && ui.find(H_SCENE, 0).r.b() < ui.specRect().y && ui.specRect().b() <= 485);

  r.click(H_SCENE, 3);
  t.check("mini: a scene button starts that scene", app.model.s.scene == "mask" && app.running);
  r.click(H_PLAY);
  t.check("mini: the play button pauses", !app.running);
  {
    const Hit m = ui.find(H_MASTER);
    const float x = m.r.x + 7 + (m.r.w - 14) * .25f;
    ui.mouseMove(x, m.r.cy(), false);
    ui.mouseDown(x, m.r.cy());
    ui.mouseUp(x, m.r.cy());
    t.check("mini: the volume slider works", std::abs(app.model.s.master - 25) <= 2, std::to_string(app.model.s.master));
    app.setMaster(70);
  }
  r.click(H_MINI);
  t.check("mini: the expand button asks to go back", toggled == 3);

  // drawing
  app.selectScene("focus");
  r.advance(1000);
  r.advance(6 * 60000 + 15000);
  ui.changed();
  vector<uint8_t> px;
  UINT pw = 0, ph = 0;
  const bool ok = render(r, 1.0f, px, pw, ph);
  t.check("mini: the compact window renders", ok && pw == 320 && ph == 484);
  if (ok) {
    const Palette P = paletteFor(true);
    auto col = [](Color c) { return Pix{(int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255)}; };
    t.check("mini: dark background", closeTo(at(px, pw, 3, 3), col(P.bg), 1), pixStr(at(px, pw, 3, 3)));
    const Hit pl = ui.find(H_PLAY);
    t.check("mini: amber play button", closeTo(at(px, pw, (int)(pl.r.cx() - 20), (int)pl.r.cy()), col(P.accent), 3), pixStr(at(px, pw, (int)(pl.r.cx() - 20), (int)pl.r.cy())));
    const R ring = ui.ringRect();
    const float rad = ring.w / 2 * (150.f / 160.f);
    const Pix onArc = at(px, pw, (int)std::lround(ring.cx() + rad), (int)std::lround(ring.cy()));
    t.check("mini: the progress arc is drawn", closeTo(onArc, col(P.accent), 40), pixStr(onArc));
    const Hit cur = ui.find(H_SCENE, 0);
    t.check("mini: the current scene is highlighted", closeTo(at(px, pw, (int)(cur.r.x + 3), (int)cur.r.cy()), col(P.raised), 6), pixStr(at(px, pw, (int)(cur.r.x + 3), (int)cur.r.cy())));
  }

  ui.setMini(false);
  ui.setSize(1180, 760);
  t.check("mini: leaving it brings the full layout back", countKind(ui, H_TILE) == 10 && countKind(ui, H_SCENE) == 7 && ui.find(H_MINI).k == H_MINI);
}
