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

string fmt(const char* f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

// A thin stroke is rarely drawn at full strength, so colours of marks are told apart by hue rather than by exact value.
bool orangeish(Pix p) { return p.r - p.b > 40 && p.r >= p.g; }
bool tealish(Pix p) { return p.g - p.r > 25 && p.b - p.r > 15; }

int sceneIdx(const string& id) {
  const auto& s = stockScenes();
  for (size_t i = 0; i < s.size(); i++) if (s[i].id == id) return (int)i;
  return -1;
}

int countKind(Ui& ui, HitKind k) {
  int n = 0;
  for (auto& h : ui.hits()) if (h.k == k) n++;
  return n;
}

// The balance slider says which side is which: each end has a colour and a word of its own, and the thumb takes the
// colour of the side it leans to.
void testBalance(TestLog& t, Rig& r) {
  Ui& ui = *r.ui;
  App& app = *r.app;
  auto col = [](Color c) { return Pix{(int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255)}; };

  const BalanceWords v0 = balanceWords(0, true), v20 = balanceWords(20, true), v50 = balanceWords(50, true), v76 = balanceWords(76, true),
                     v100 = balanceWords(100, true), m20 = balanceWords(20, false);
  t.check("balance: the ends are named after what they control", v50.left == L"Video" && v50.right == L"Sounds" && m20.left == L"Music" && m20.right == L"Sounds");
  t.check("balance: it says which way it leans", v20.lean == L"More video" && v20.side == -1 && v76.lean == L"More sounds" && v76.side == 1 && v50.lean == L"Even" && v50.side == 0);
  t.check("balance: and when one side is all there is", v0.lean == L"Video only" && v100.lean == L"Sounds only" && m20.lean == L"More music");
  t.check("balance: a little either side of the middle still reads as even",
          balanceWords(48, true).side == 0 && balanceWords(52, true).side == 0 && balanceWords(47, true).side == -1 && balanceWords(53, true).side == 1);

  app.selectScene("focus");
  app.setTab("stream");
  app.stopAll();
  ui.setSize(1180, 760);
  ui.changed();
  const Hit b = ui.find(H_BLEND);
  t.check("balance: the tip names the two sides", b.k == H_BLEND && b.tip.find(L"left is the video") != wstring::npos && b.tip.find(L"right is the sounds") != wstring::npos, narrow(b.tip));
  const float cy = b.r.cy(), x0 = b.r.x + 7, x1 = b.r.r() - 7, mid = (x0 + x1) / 2, sx = b.r.x - 28, ex = b.r.r() + 28;
  auto thumbX = [&](int v) { return x0 + (x1 - x0) * v / 100.f; };
  auto hasColor = [&](const vector<uint8_t>& px, UINT pw, float xa, float ya, float xb, float yb, Pix want, int tol) {
    for (int y = (int)ya; y <= (int)yb; y++)
      for (int x = (int)xa; x <= (int)xb; x++) if (closeTo(at(px, pw, x, y), want, tol)) return true;
    return false;
  };

  for (const bool dark : {true, false}) {
    ui.setTheme(dark);
    const Palette P = paletteFor(dark);
    const string theme = dark ? "dark" : "light";
    vector<uint8_t> px;
    UINT pw = 0, ph = 0;

    app.setBlend(20);
    render(r, 1, px, pw, ph);
    t.check("balance (" + theme + "): leaning to the video, the thumb is the video's colour", closeTo(at(px, pw, (int)thumbX(20), (int)cy), col(P.accent), 4), pixStr(at(px, pw, (int)thumbX(20), (int)cy)));
    t.check("balance (" + theme + "): and a bar of that colour runs from the middle to it", closeTo(at(px, pw, (int)((thumbX(20) + mid) / 2), (int)cy), col(P.accent), 4));
    app.setBlend(80);
    render(r, 1, px, pw, ph);
    t.check("balance (" + theme + "): leaning to the sounds, the thumb is the sounds' colour", closeTo(at(px, pw, (int)thumbX(80), (int)cy), col(P.calm), 4), pixStr(at(px, pw, (int)thumbX(80), (int)cy)));
    t.check("balance (" + theme + "): and so is the bar", closeTo(at(px, pw, (int)((thumbX(80) + mid) / 2), (int)cy), col(P.calm), 4));
    app.setBlend(50);
    render(r, 1, px, pw, ph);
    t.check("balance (" + theme + "): in the middle the thumb is neutral and there is no bar",
            closeTo(at(px, pw, (int)thumbX(50), (int)cy), col(P.text), 4) && !closeTo(at(px, pw, (int)((x0 + mid) / 2), (int)cy), col(P.accent), 40) &&
                !closeTo(at(px, pw, (int)((mid + x1) / 2), (int)cy), col(P.calm), 40));
    const Pix leftTrack = at(px, pw, (int)((x0 + mid) / 2), (int)cy), rightTrack = at(px, pw, (int)((mid + x1) / 2), (int)cy);
    t.check("balance (" + theme + "): each half of the track is tinted with its side's colour",
            (dark ? leftTrack.r > leftTrack.b + 20 && rightTrack.g > rightTrack.r + 15 : leftTrack.r > leftTrack.b + 10 && rightTrack.g > rightTrack.r + 5),
            pixStr(leftTrack) + " | " + pixStr(rightTrack));
    t.check("balance (" + theme + "): the end icons carry the same two colours",
            hasColor(px, pw, sx, cy - 9, sx + 18, cy + 9, col(P.accent), 40) && !hasColor(px, pw, sx, cy - 9, sx + 18, cy + 9, col(P.calm), 40) &&
                hasColor(px, pw, ex - 18, cy - 9, ex, cy + 9, col(P.calm), 40) && !hasColor(px, pw, ex - 18, cy - 9, ex, cy + 9, col(P.accent), 40));
    t.check("balance (" + theme + "): and so do the words under them",
            hasColor(px, pw, sx, cy + 11, sx + 70, cy + 27, col(P.accent), 50) && !hasColor(px, pw, sx, cy + 11, sx + 70, cy + 27, col(P.calm), 50) &&
                hasColor(px, pw, ex - 70, cy + 11, ex, cy + 27, col(P.calm), 50) && !hasColor(px, pw, ex - 70, cy + 11, ex, cy + 27, col(P.accent), 50));
  }
  ui.setTheme(true);

  const Hit tile = ui.find(H_TILE, 0);
  t.check("balance: the sound tiles still start below the words and fit the window", tile.k == H_TILE && tile.r.y > cy + 27 + 10 && ui.find(H_TILE, 9).r.b() <= ui.height(),
          std::to_string(tile.r.y) + " vs " + std::to_string(cy + 27));
  app.setTab("files");
  ui.changed();
  t.check("balance: with your own music the tip says music", ui.find(H_BLEND).tip.find(L"left is your music") != wstring::npos, narrow(ui.find(H_BLEND).tip));
  app.setTab("stream");
  ui.changed();
  app.setBlend(50);
}

}  // namespace

void testUi(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init()) { t.check("ui: Direct2D is available", false); return; }
  Ui& ui = *r.ui;
  App& app = *r.app;

  // ---- layout
  t.check("ui: every scene, ten tiles, five bands", countKind(ui, H_SCENE) == NSCENES && countKind(ui, H_TILE) == 10 && countKind(ui, H_BAND) == 5);
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
  r.click(H_SCENE, sceneIdx("create"));
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
  testBalance(t, r);

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
  ui.key((int)('1' + sceneIdx("create")), false, false);
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

// The loop button beside the plans, the R key, the mark in the ring (also in the compact player) and the Keys sheet.
void testLoopUi(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init()) { t.check("loop ui: the rig starts", false); return; }
  Ui& ui = *r.ui;
  App& app = *r.app;
  auto tipOf = [&] { return narrow(ui.find(H_LOOP).tip); };

  app.selectScene("focus");
  app.stopAll();
  ui.setSize(1180, 760);
  ui.changed();
  const Hit lp = ui.find(H_LOOP), first = ui.find(H_PLAN, 0), last = ui.find(H_PLAN, 2);
  t.check("loop ui: Focus has the loop button right after its three plans", lp.k == H_LOOP && last.k == H_PLAN && lp.r.x > last.r.r() && lp.r.x - last.r.r() < 16 && std::fabs(lp.r.cy() - last.r.cy()) < 2,
          std::to_string(lp.r.x) + " after " + std::to_string(last.r.r()));
  const float mid = ui.ringRect().cx();
  t.check("loop ui: the plans are centred over the ring", std::fabs(((first.r.x - 3) + (last.r.r() + 3)) / 2 - mid) < 1.5f,
          std::to_string(((first.r.x - 3) + (last.r.r() + 3)) / 2) + " vs " + std::to_string(mid));
  const Hit tb = ui.find(H_TASK, 0);  // at this width the task button takes the place after it, instead of the card beside the ring
  t.check("loop ui: the task button follows the loop button", tb.k == H_TASK && tb.r.x > lp.r.r() && tb.r.x - lp.r.r() < 16 && std::fabs(tb.r.cy() - lp.r.cy()) < 2);
  t.check("loop ui: its tip says what it does", tipOf().find("Loop the rounds") != string::npos && tipOf().find("how many sets") != string::npos && tipOf().find("(R)") != string::npos, tipOf());

  // it is there only while the chosen plan has rounds to loop: not on the ∞ plan, not on a timer, not on a scene of timers
  for (const char* id : {"sprint", "read", "create"}) {
    app.selectScene(id);
    app.stopAll();
    ui.changed();
    t.check(string("loop ui: ") + id + " has the button on its plan with rounds", ui.find(H_LOOP).k == H_LOOP);
  }
  for (const char* id : {"mask", "white"}) {
    app.selectScene(id);
    app.stopAll();
    ui.changed();
    t.check(string("loop ui: ") + id + " starts on the ∞ plan, so there is no button", app.model.planId() == "free" && ui.find(H_LOOP).k == H_NONE);
    app.selectPlan("p25");
    ui.changed();
    t.check(string("loop ui: ") + id + " has it once a plan with rounds is chosen", ui.find(H_LOOP).k == H_LOOP);
    app.selectPlan("free");
  }
  for (const char* id : {"calm", "sleep"}) {
    app.selectScene(id);
    app.stopAll();
    ui.changed();
    t.check(string("loop ui: ") + id + " has none, its plans are timers and ∞", ui.find(H_LOOP).k == H_NONE);
  }
  app.selectScene("focus");
  app.stopAll();
  ui.changed();

  // changing plans does not move them: the plans stay where they are while the button comes and goes
  {
    const float x25 = ui.find(H_PLAN, 0).r.x;
    app.selectPlan("free");
    ui.changed();
    const float xFree = ui.find(H_PLAN, 0).r.x;
    t.check("loop ui: on the ∞ plan the button is gone", ui.find(H_LOOP).k == H_NONE);
    t.check("loop ui: and the plans did not move", std::fabs(x25 - xFree) < 0.01f && ui.find(H_TASK, 0).k == H_TASK, std::to_string(x25) + " vs " + std::to_string(xFree));
    t.check("loop ui: the task button then sits right after the plans", ui.find(H_TASK, 0).r.x - ui.find(H_PLAN, 2).r.r() < 16 + 3, std::to_string(ui.find(H_TASK, 0).r.x - ui.find(H_PLAN, 2).r.r()));
    app.selectPlan("p50");
    ui.changed();
    t.check("loop ui: nor on the other plan with rounds", std::fabs(ui.find(H_PLAN, 0).r.x - x25) < 0.01f && ui.find(H_LOOP).k == H_LOOP);
    app.selectPlan("p25");
    ui.changed();
    t.check("loop ui: nor with the loop on", (app.setLoop(true), ui.changed(), std::fabs(ui.find(H_PLAN, 0).r.x - x25) < 0.01f));
    app.setLoop(false);
    ui.changed();
  }

  // pointer and key: a click opens the picker, R still turns it on and off
  r.click(H_LOOP);
  t.check("loop ui: a click opens the picker and changes nothing yet", ui.sheet() == 4 && !app.model.s.repeat && countKind(ui, H_LSET) == LOOP_CELLS);
  ui.key(VK_ESCAPE, false, false);
  t.check("loop ui: Esc closes it", ui.sheet() == 0 && !app.model.s.repeat);
  ui.key('R', false, false);
  t.check("loop ui: the R key turns it on", app.model.s.repeat && app.session.loop() && ui.sheet() == 0);
  t.check("loop ui: and the tip changes to match", tipOf().find("Looping until you stop it") != string::npos && tipOf().find("R turns it off") != string::npos, tipOf());
  ui.key('R', false, false);
  t.check("loop ui: and off", !app.model.s.repeat && tipOf().find("Loop the rounds") != string::npos);
  app.setLoop(false);
  ui.changed();

  // the mark in the ring: found by drawing the ring with the loop off and on and looking at what changed
  auto ringDiff = [&](int* n, R* box, Pix* strongest) {
    const bool was = app.model.s.repeat;
    vector<uint8_t> a, b;
    UINT pw = 0, ph = 0;
    app.setLoop(false);
    render(r, 1, a, pw, ph);
    app.setLoop(true);
    render(r, 1, b, pw, ph);
    app.setLoop(was);
    const R rr = ui.ringRect();
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1, cnt = 0, bestD = 0;
    for (int y = (int)rr.y; y < (int)rr.b(); y++)
      for (int x = (int)rr.x; x < (int)rr.r(); x++) {
        const Pix p = at(a, pw, x, y), q = at(b, pw, x, y);
        const int d = std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b);
        if (d <= 9) continue;
        cnt++;
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
        if (d > bestD) { bestD = d; *strongest = q; }
      }
    *n = cnt;
    *box = {(float)x0, (float)y0, (float)(x1 - x0 + 1), (float)(y1 - y0 + 1)};
  };
  {
    app.selectScene("focus");
    app.stopAll();
    app.selectPlan("p25");
    ui.changed();
    int n = 0;
    R box;
    Pix strong{0, 0, 0};
    ringDiff(&n, &box, &strong);
    t.check("loop ui: a small repeat mark appears in the ring when the loop is on", n > 15 && box.w <= 20 && box.h <= 20, fmt("%d pixels, %.0fx%.0f", n, box.w, box.h));
    t.check("loop ui: it sits right of the dots, below the middle", box.x > ui.ringRect().cx() && box.y > ui.ringRect().cy(),
            fmt("at %.0f,%.0f in a ring centred at %.0f,%.0f", box.x, box.y, ui.ringRect().cx(), ui.ringRect().cy()));
    t.check("loop ui: during a round it is the accent colour", orangeish(strong) && !tealish(strong), pixStr(strong));
    app.startAll();
    r.advance(25 * 60000 + 1000);
    ui.changed();
    ringDiff(&n, &box, &strong);
    t.check("loop ui: during a break it turns the break colour", n > 15 && tealish(strong) && !orangeish(strong), pixStr(strong));
    app.stopAll();
    app.selectPlan("free");
    ui.changed();
    ringDiff(&n, &box, &strong);
    t.check("loop ui: with no rounds there is no mark", n == 0, std::to_string(n));
  }

  // the compact player has no button but shows the mark
  {
    app.selectScene("focus");
    app.stopAll();
    app.selectPlan("p25");
    ui.setMini(true);
    ui.setSize(320, 484);
    ui.changed();
    int n = 0;
    R box;
    Pix strong{0, 0, 0};
    ringDiff(&n, &box, &strong);
    t.check("loop ui: the compact player has no button", ui.find(H_LOOP).k == H_NONE);
    t.check("loop ui: but it shows the mark", n > 15 && box.w <= 20 && box.h <= 20, fmt("%d pixels, %.0fx%.0f", n, box.w, box.h));
    ui.setMini(false);
    ui.setSize(1180, 760);
  }

  // in the narrowest window the buttons are still inside the stage: the plans give way to them, but only as far as they must
  ui.setSize(940, 640);
  ui.changed();
  {
    const Hit b = ui.find(H_LOOP), f = ui.find(H_PLAN, 0), tk = ui.find(H_TASK, 0);
    const float centred = ui.ringRect().cx() - ((ui.find(H_PLAN, 2).r.r() + 3) - (f.r.x - 3)) / 2;  // where the segment's left edge would be
    t.check("loop ui: at the smallest window size the buttons fit in the stage", b.k == H_LOOP && tk.k == H_TASK && f.r.x - 3 >= 232 && tk.r.r() <= 940 - 396 - 12,
            fmt("%.0f to %.0f of 232 to 544", f.r.x - 3, tk.r.r()));
    t.check("loop ui: and the plans moved left to make room, by less than the buttons are wide", centred - (f.r.x - 3) > 1 && centred - (f.r.x - 3) < 34 + 8 + 34 + 8,
            fmt("%.1f", centred - (f.r.x - 3)));
    app.selectPlan("free");
    ui.changed();
    const Hit f2 = ui.find(H_PLAN, 0);
    t.check("loop ui: on the ∞ plan nothing needs room, so the plans are centred again", std::fabs(((f2.r.x - 3) + (ui.find(H_PLAN, 2).r.r() + 3)) / 2 - ui.ringRect().cx()) < 1.5f);
    app.selectPlan("p25");
  }
  ui.setSize(1180, 760);

  // the Keys sheet has a row for it, and room for every row
  bool hasR = false;
  for (const KeyRow& row : keyRows()) {
    bool key = false, icon = false;
    for (const wchar_t* k : row.keys) key = key || wcscmp(k, L"R") == 0;
    for (const char* ic : row.icons) icon = icon || std::strcmp(ic, "i-repeat") == 0;
    hasR = hasR || (key && icon);
  }
  t.check("loop ui: the Keys sheet lists R with the repeat icon", hasR && (int)keyRows().size() == KEYS_ROWS, std::to_string(keyRows().size()));
  t.check("loop ui: and the sheet is tall enough for its last row", 64 + (KEYS_ROWS - 1) * 38 + 26 <= SHEET_KEYS_H, std::to_string(SHEET_KEYS_H));
}

// The picker under the loop button: its cells, what a click or a key does, how it looks, and the count in the button and the ring.
void testLoopPickerUi(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init(1602, 707)) { t.check("picker ui: the rig starts", false); return; }
  Ui& ui = *r.ui;
  App& app = *r.app;
  auto col = [](Color c) { return Pix{(int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255)}; };
  auto tipOf = [&] { return narrow(ui.find(H_LOOP).tip); };
  auto hasColor = [&](const vector<uint8_t>& px, UINT pw, const R& box, Pix want, int tol) {
    for (int y = (int)box.y; y < (int)box.b(); y++)
      for (int x = (int)box.x; x < (int)box.r(); x++) if (closeTo(at(px, pw, x, y), want, tol)) return true;
    return false;
  };
  auto differs = [&](const vector<uint8_t>& a, const vector<uint8_t>& b, UINT pw, const R& box) {  // how many pixels of the box are not the same
    int n = 0;
    for (int y = (int)box.y; y < (int)box.b(); y++)
      for (int x = (int)box.x; x < (int)box.r(); x++) {
        const Pix p = at(a, pw, x, y), q = at(b, pw, x, y);
        if (std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b) > 9) n++;
      }
    return n;
  };
  auto choose = [&](int cell) {  // the way a person does it: open the picker, click a cell
    r.click(H_LOOP);
    r.click(H_LSET, cell);
  };

  app.selectScene("focus");
  app.selectPlan("p25");
  app.stopAll();
  ui.changed();
  const Hit lb = ui.find(H_LOOP);
  r.click(H_LOOP);
  t.check("picker ui: the loop button opens it", ui.sheet() == 4);
  vector<Hit> cs;
  for (int i = 0; i < LOOP_CELLS; i++) cs.push_back(ui.find(H_LSET, i));
  bool ordered = true, same = true;
  for (int i = 0; i < LOOP_CELLS; i++) {
    ordered = ordered && cs[(size_t)i].k == H_LSET && (i == 0 || cs[(size_t)i].r.x > cs[(size_t)i - 1].r.r());
    same = same && std::fabs(cs[(size_t)i].r.w - cs[0].r.w) < .01f && std::fabs(cs[(size_t)i].r.y - cs[0].r.y) < .01f;
  }
  t.check("picker ui: seven cells, in order and the same size: 1 to 6 and until stopped", LOOP_CELLS == 7 && countKind(ui, H_LSET) == 7 && ordered && same);
  t.check("picker ui: they hang under the loop button, centred on it", cs[0].r.y > lb.r.b() && cs[0].r.y - lb.r.b() < 60 && std::fabs((cs[0].r.x + cs[6].r.r()) / 2 - lb.r.cx()) < 1.5f,
          fmt("%.1f vs %.1f", (cs[0].r.x + cs[6].r.r()) / 2, lb.r.cx()));
  t.check("picker ui: while it is open nothing behind it can be clicked", (int)ui.hits().size() == 2 + LOOP_CELLS && countKind(ui, H_PLAN) == 0 && countKind(ui, H_PLAY) == 0 && countKind(ui, H_LOOP) == 0);

  // what the cells do
  for (int i = 0; i < LOOP_CELLS - 1; i++) {
    app.setLoop(false);
    app.model.s.sets = SETS_FOREVER;
    ui.openSheet(0);
    choose(i);
    const bool ok = i == 0 ? !app.model.s.repeat && app.model.s.sets == SETS_FOREVER : app.model.s.repeat && app.model.s.sets == i + 1 && app.session.sets() == i + 1 && app.session.loop();
    t.check("picker ui: the cell for " + std::to_string(i + 1) + (i == 0 ? " is no loop" : " sets turns the loop on with that count") + ", and closes the picker", ok && ui.sheet() == 0,
            fmt("repeat %d, sets %d, sheet %d", app.model.s.repeat, app.model.s.sets, ui.sheet()));
  }
  choose(LOOP_CELLS - 1);
  t.check("picker ui: the last cell is until stopped", app.model.s.repeat && app.model.s.sets == SETS_FOREVER && ui.sheet() == 0 && tipOf().find("until you stop it") != string::npos, tipOf());
  choose(3);
  t.check("picker ui: and the button's tip names the count", app.model.s.sets == 4 && tipOf() == "Looping 4 sets: click to change (R turns it off)", tipOf());
  choose(0);
  t.check("picker ui: the first cell turns it off and the count waits for the next time", !app.model.s.repeat && app.model.s.sets == 4);
  ui.key('R', false, false);
  t.check("picker ui: R then brings the loop back with that count", app.model.s.repeat && app.model.s.sets == 4 && app.session.sets() == 4);

  // leaving it
  r.click(H_LOOP);
  const Hit panel = ui.find(H_SIDE);
  ui.mouseMove(panel.r.x + 20, panel.r.y + 10, false);
  ui.mouseDown(panel.r.x + 20, panel.r.y + 10);
  ui.mouseUp(panel.r.x + 20, panel.r.y + 10);
  t.check("picker ui: a click on the panel itself, between the cells, leaves it open", ui.sheet() == 4 && app.model.s.sets == 4);
  ui.mouseMove(40, 600, false);
  ui.mouseDown(40, 600);
  ui.mouseUp(40, 600);
  t.check("picker ui: a click anywhere else closes it and changes nothing", ui.sheet() == 0 && app.model.s.repeat && app.model.s.sets == 4);
  r.click(H_LOOP);
  ui.key(VK_ESCAPE, false, false);
  t.check("picker ui: Esc closes it", ui.sheet() == 0 && app.model.s.sets == 4);
  r.click(H_LOOP);
  ui.key(VK_RETURN, false, false);
  t.check("picker ui: Enter closes it", ui.sheet() == 0 && app.model.s.sets == 4);

  // the keys while it is open
  app.setLoop(false);
  app.model.s.sets = SETS_FOREVER;
  r.click(H_LOOP);
  ui.key('3', false, false);
  t.check("picker ui: a digit picks that many sets and closes it", ui.sheet() == 0 && app.model.s.repeat && app.model.s.sets == 3);
  r.click(H_LOOP);
  ui.key(VK_RIGHT, false, false);
  t.check("picker ui: the right arrow steps to the next cell and the picker stays", ui.sheet() == 4 && app.model.s.sets == 4 && app.setsChoice() == 4);
  ui.key(VK_LEFT, false, false);
  ui.key(VK_LEFT, false, false);
  t.check("picker ui: the left arrow steps back", app.setsChoice() == 2 && ui.sheet() == 4);
  ui.key(VK_LEFT, false, false);
  t.check("picker ui: down to one, which is no loop", !app.model.s.repeat && app.setsChoice() == 1);
  ui.key(VK_LEFT, false, false);
  t.check("picker ui: and no further", !app.model.s.repeat && ui.sheet() == 4);
  for (int i = 0; i < 9; i++) ui.key(VK_RIGHT, false, false);
  t.check("picker ui: the right arrow ends at until stopped", app.model.s.repeat && app.model.s.sets == SETS_FOREVER && ui.sheet() == 4);
  ui.key(VK_RIGHT, false, false);
  t.check("picker ui: and no further", app.model.s.repeat && app.model.s.sets == SETS_FOREVER);
  ui.key('R', false, false);
  t.check("picker ui: R turns the loop off, with the picker still open", !app.model.s.repeat && ui.sheet() == 4);
  ui.key('R', false, false);
  const bool running = app.running;
  const bool eaten = ui.key(VK_SPACE, false, false) && ui.key('7', false, false) && ui.key('T', false, false) && ui.key('M', false, false);
  t.check("picker ui: other keys are swallowed, the page behind is not touched", eaten && ui.sheet() == 4 && app.running == running && app.model.s.repeat && app.model.s.sets == SETS_FOREVER && !app.muted);
  ui.key(VK_ESCAPE, false, false);

  // the words under the cells
  app.selectPlan("p25");
  t.check("picker ui: the words say the sets and the time they take", ui.loopCaption(4) == L"4 sets · 7 h 55 min" && ui.loopCaption(2) == L"2 sets · 3 h 55 min" && ui.loopCaption(6) == L"6 sets · 11 h 55 min",
          narrow(ui.loopCaption(4)));
  t.check("picker ui: one is once, with no loop", ui.loopCaption(1) == L"Once, no loop · 1 h 55 min", narrow(ui.loopCaption(1)));
  t.check("picker ui: and no count goes on until you stop it", ui.loopCaption(SETS_FOREVER) == L"Until you stop it");
  app.selectPlan("p50");
  t.check("picker ui: they follow the plan", ui.loopCaption(3) == L"3 sets · 8 h 50 min" && ui.loopCaption(1) == L"Once, no loop · 2 h 50 min", narrow(ui.loopCaption(3)));
  app.selectPlan("p25");
  t.check("picker ui: time in words", fmtSpan(475 * 60000) == L"7 h 55 min" && fmtSpan(120 * 60000) == L"2 h" && fmtSpan(45 * 60000) == L"45 min" && fmtSpan(61 * 60000) == L"1 h 1 min" &&
                                      fmtSpan(0) == L"0 min" && fmtSpan(29999) == L"0 min" && fmtSpan(30000) == L"1 min" && fmtSpan(-5) == L"0 min");

  // a plan change underneath closes it
  r.click(H_LOOP);
  app.selectPlan("free");
  ui.hits();
  t.check("picker ui: if the plan changes to one with nothing to loop, it goes", ui.sheet() == 0 && countKind(ui, H_LSET) == 0);
  app.selectPlan("p25");

  // how it looks, in both themes
  app.setLoop(false);
  app.model.s.sets = SETS_FOREVER;
  for (const bool dark : {true, false}) {
    ui.setTheme(dark);
    const Palette P = paletteFor(dark);
    const string theme = dark ? "dark" : "light";
    vector<uint8_t> shut, forever, counted, open, hover;
    UINT pw = 0, ph = 0;
    app.setLoop(false);
    ui.openSheet(0);
    render(r, 1, shut, pw, ph);
    const Hit b = ui.find(H_LOOP);
    const R icon = {b.r.cx() - 9, b.r.cy() - 9, 18, 18}, middle = {b.r.cx() - 5, b.r.cy() - 4, 10, 8};
    auto orange = [&](const vector<uint8_t>& px, const R& box) {
      int best = -255;
      for (int y = (int)box.y; y < (int)box.b(); y++)
        for (int x = (int)box.x; x < (int)box.r(); x++) { const Pix p = at(px, pw, x, y); best = std::max(best, p.r - p.b); }
      return best;
    };
    app.chooseSets(SETS_FOREVER);
    render(r, 1, forever, pw, ph);
    app.chooseSets(4);
    render(r, 1, counted, pw, ph);
    t.check("picker ui (" + theme + "): off, the loop icon is grey", orange(shut, icon) < 20, std::to_string(orange(shut, icon)));
    t.check("picker ui (" + theme + "): on, it is the accent colour", orange(forever, icon) > 120 && orange(counted, icon) > 120, std::to_string(orange(forever, icon)));
    t.check("picker ui (" + theme + "): with a count the number is drawn inside the icon, and with none the inside is empty",
            differs(forever, counted, pw, middle) > 6 && closeTo(at(forever, pw, (int)b.r.cx(), (int)b.r.cy()), col(P.raised), 3), fmt("%d differ", differs(forever, counted, pw, middle)));

    app.chooseSets(4);
    ui.openSheet(4);
    render(r, 1, open, pw, ph);
    const Hit panel2 = ui.find(H_SIDE);
    t.check("picker ui (" + theme + "): the panel is drawn in the panel colour", closeTo(at(open, pw, (int)panel2.r.cx(), (int)panel2.r.y + 3), col(P.panel), 3), pixStr(at(open, pw, (int)panel2.r.cx(), (int)panel2.r.y + 3)));
    t.check("picker ui (" + theme + "): the page behind is not dimmed", closeTo(at(open, pw, 40, 540), at(counted, pw, 40, 540), 0) && closeTo(at(open, pw, 1100, 300), at(counted, pw, 1100, 300), 0),
            pixStr(at(open, pw, 40, 540)) + " vs " + pixStr(at(counted, pw, 40, 540)) + " | " + pixStr(at(open, pw, 1100, 300)) + " vs " + pixStr(at(counted, pw, 1100, 300)));
    t.check("picker ui (" + theme + "): the chosen cell is lit in the accent, the others are not", orange(open, ui.find(H_LSET, 3).r) > 100 && orange(open, ui.find(H_LSET, 1).r) < 40 && orange(open, ui.find(H_LSET, 6).r) < 40);
    app.setLoop(false);
    vector<uint8_t> none;
    render(r, 1, none, pw, ph);
    t.check("picker ui (" + theme + "): with the loop off the first cell is the chosen one, in plain text", orange(none, ui.find(H_LSET, 0).r) < 40 && hasColor(none, pw, ui.find(H_LSET, 0).r, col(P.raised), 3));
    app.chooseSets(4);
    render(r, 1, open, pw, ph);  // the baseline for the pointer: four chosen, the pointer away
    const Hit c5 = ui.find(H_LSET, 5);
    ui.mouseMove(c5.r.cx(), c5.r.cy(), false);
    render(r, 1, hover, pw, ph);
    const R words = {panel2.r.x + 10, panel2.r.b() - 30, panel2.r.w - 20, 22};
    t.check("picker ui (" + theme + "): pointing at a cell changes the words, and the cell lights up", differs(open, hover, pw, words) > 20 && differs(open, hover, pw, c5.r) > 20,
            fmt("%d, %d", differs(open, hover, pw, words), differs(open, hover, pw, c5.r)));
    app.chooseSets(8);  // a count the cells do not have (from --loop=8): none of them is lit, and the words say it
    render(r, 1, none, pw, ph);
    bool anyLit = false;
    for (int i = 0; i < LOOP_CELLS; i++) anyLit = anyLit || orange(none, ui.find(H_LSET, i).r) > 100;
    t.check("picker ui (" + theme + "): a count the cells do not have lights none of them, and the words say it", !anyLit && ui.loopCaption(8) == L"8 sets · 15 h 55 min", narrow(ui.loopCaption(8)));
    app.chooseSets(4);
    ui.mouseLeave();
    ui.openSheet(0);
  }
  ui.setTheme(true);

  // the count in the ring: which set this is, next to the repeat mark
  {
    vector<uint8_t> plain, counted;
    UINT pw = 0, ph = 0;
    app.chooseSets(SETS_FOREVER);
    render(r, 1, plain, pw, ph);
    app.chooseSets(4);
    render(r, 1, counted, pw, ph);
    const R ring = ui.ringRect();
    int x0 = 1 << 20, x1 = -1;
    for (int y = (int)ring.y; y < (int)ring.b(); y++)
      for (int x = (int)ring.x; x < (int)ring.r(); x++) {
        const Pix p = at(plain, pw, x, y), q = at(counted, pw, x, y);
        if (std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b) > 9) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
      }
    t.check("picker ui: with a count the ring says which set it is, to the right of the repeat mark", x1 > 0 && x1 - x0 + 1 >= 14 && x0 > ring.cx(), fmt("%d to %d", x0, x1));
  }

  // in the narrowest window it stays over the stage
  ui.setSize(940, 640);
  app.selectPlan("p25");
  ui.changed();
  r.click(H_LOOP);
  {
    const Hit panel3 = ui.find(H_SIDE);
    t.check("picker ui: at the smallest window size it is still all over the stage", ui.sheet() == 4 && panel3.r.x >= 232 && panel3.r.r() <= 940 - 396 && panel3.r.b() < 640,
            fmt("%.0f to %.0f", panel3.r.x, panel3.r.r()));
    r.click(H_LSET, 2);
    t.check("picker ui: and works there", app.model.s.sets == 3 && ui.sheet() == 0);
  }
  ui.setSize(1602, 707);
  app.setLoop(false);
}

// "Start again" in the ring, and the restart arrow on the big button, once a plan is over.
void testAgainUi(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init(1602, 707)) { t.check("again ui: the rig starts", false); return; }
  Ui& ui = *r.ui;
  App& app = *r.app;
  Model& m = app.model;
  auto col = [](Color c) { return Pix{(int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255)}; };
  auto hasColor = [&](const vector<uint8_t>& px, UINT pw, const R& box, Pix want, int tol) {
    for (int y = (int)box.y; y < (int)box.b(); y++)
      for (int x = (int)box.x; x < (int)box.r(); x++) if (closeTo(at(px, pw, x, y), want, tol)) return true;
    return false;
  };
  const int64_t MIN = 60000;
  auto finish = [&](const char* scene, const char* plan) {  // plays the plan to its end
    app.stopAll();
    app.selectScene(scene);
    app.selectPlan(plan);
    app.setLoop(false);
    app.stopAll();
    m.s.task = Task();
    ui.changed();
    app.startAll();
    r.advance(1000);
    r.advance(app.session.snapshot().duration == INF_MS ? 0 : planSetMs(plan));
    ui.changed();
  };

  app.selectScene("focus");
  app.selectPlan("p25");
  app.stopAll();
  ui.changed();
  t.check("again ui: nothing to start again before the plan is over", countKind(ui, H_AGAIN) == 0 && ui.find(H_PLAY).tip == L"Play / pause (Space)");

  finish("focus", "p25");
  t.check("again ui: when the plan ends, the plan is over", app.session.done() && app.closing);
  const Hit ag = ui.find(H_AGAIN);
  const R ring = ui.ringRect();
  t.check("again ui: the ring offers Start again, inside it, under its middle", ag.k == H_AGAIN && std::fabs(ag.r.cx() - ring.cx()) < 1.5f && ag.r.y > ring.cy() && ag.r.b() < ring.b() - 40 && ag.r.w == AGAIN_W && ag.r.h == AGAIN_H,
          fmt("%.0f,%.0f %.0fx%.0f in a ring from %.0f to %.0f", ag.r.x, ag.r.y, ag.r.w, ag.r.h, ring.y, ring.b()));
  t.check("again ui: its tip, and the big button's, say so", narrow(ag.tip).find("Start again") != string::npos && ui.find(H_PLAY).tip == L"Start again (Space)", narrow(ui.find(H_PLAY).tip));
  t.check("again ui: skip is off, as it was, and there is no line of task under the ring to share the place", countKind(ui, H_NEXT) == 0 && ui.find(H_TASK, 1).k == H_NONE);

  // it works while the sound is still fading out, and after
  r.click(H_AGAIN);
  SessionSnap s = app.session.snapshot();
  t.check("again ui: a click starts the plan again from the first round, and the fade is cancelled", !app.session.done() && app.session.running() && app.running && !app.closing && s.round == 1 && s.lap == 0 && s.elapsed < 5000);
  t.check("again ui: the button is gone once it runs", countKind(ui, H_AGAIN) == 0 && ui.find(H_PLAY).tip == L"Play / pause (Space)");
  finish("focus", "p25");
  bool over = false;
  for (int i = 0; i < 60 && !over; i++) over = app.closeTick(wallMs() + 5000 + i * 120);
  ui.changed();
  t.check("again ui: after the fade-out it is still there, with nothing playing", over && !app.running && app.session.done() && ui.find(H_AGAIN).k == H_AGAIN);
  r.click(H_AGAIN);
  t.check("again ui: and starts it from there", app.running && !app.session.done() && app.session.snapshot().round == 1);
  finish("focus", "p25");
  r.click(H_PLAY);
  t.check("again ui: the big play button does the same, rather than pausing", app.running && !app.session.done() && app.session.running() && !app.closing);

  // the same with the loop and its count: the sets start over too
  app.selectPlan("p25");
  app.chooseSets(2);
  app.stopAll();
  app.startAll();
  r.advance(1000);
  r.advance(2 * 115 * MIN + 5 * MIN);
  ui.changed();
  t.check("again ui: two sets over", app.session.done() && app.session.snapshot().lap == 1 && ui.find(H_AGAIN).k == H_AGAIN);
  r.click(H_AGAIN);
  t.check("again ui: it starts the first of two again", !app.session.done() && app.session.snapshot().lap == 0 && app.session.snapshot().sets == 2);
  app.setLoop(false);

  // a plan with no dots (a timer) has it too, and the layout gives room to it
  finish("calm", "t10");
  ui.changed();
  const Hit ag2 = ui.find(H_AGAIN);
  t.check("again ui: a finished timer offers it as well", app.session.done() && ag2.k == H_AGAIN && ag2.r.y > ui.ringRect().cy(), fmt("%.0f vs %.0f", ag2.r.y, ui.ringRect().cy()));
  r.click(H_AGAIN);
  t.check("again ui: and it starts the timer again", !app.session.done() && app.session.snapshot().elapsed < 5000 && app.session.planId() == "t10");

  // the colours, in both themes: the pill is the accent, its words the ink of the accent, and the play button's middle is empty
  for (const bool dark : {true, false}) {
    ui.setTheme(dark);
    const Palette P = paletteFor(dark);
    const string theme = dark ? "dark" : "light";
    vector<uint8_t> px, going;
    UINT pw = 0, ph = 0;
    app.stopAll();
    app.selectScene("focus");
    app.selectPlan("p25");
    app.stopAll();
    ui.changed();
    render(r, 1, going, pw, ph);
    const Hit play = ui.find(H_PLAY);
    const Pix triangle = at(going, pw, (int)play.r.cx(), (int)play.r.cy());
    finish("focus", "p25");
    ui.mouseLeave();  // the pointer from the last click would light the button
    render(r, 1, px, pw, ph);
    const Hit a = ui.find(H_AGAIN), pl = ui.find(H_PLAY);
    t.check("again ui (" + theme + "): the button is the accent colour", closeTo(at(px, pw, (int)a.r.x + 8, (int)a.r.cy()), col(P.accent), 3), pixStr(at(px, pw, (int)a.r.x + 8, (int)a.r.cy())));
    t.check("again ui (" + theme + "): its icon and words are drawn in the ink colour", hasColor(px, pw, a.r.inset(6, 4), col(P.accentInk), 40));
    // the play triangle fills the middle of the button; the restart arrow is a ring with an empty middle and its right side
    // 7.5 px out from the centre, where neither the triangle nor the pause bars (which the button shows while the sound fades) reach
    const Pix ringSide = at(px, pw, (int)(pl.r.cx() + 7.5f), (int)pl.r.cy());
    t.check("again ui (" + theme + "): the big button has the restart arrow: an empty middle and a ring around it, not the triangle or the bars",
            closeTo(triangle, col(P.accentInk), 40) && closeTo(at(px, pw, (int)pl.r.cx(), (int)pl.r.cy()), col(P.accent), 4) && closeTo(ringSide, col(P.accentInk), 70),
            pixStr(triangle) + " | " + pixStr(at(px, pw, (int)pl.r.cx(), (int)pl.r.cy())) + " | " + pixStr(ringSide));
    app.startAll();
    app.stopAll();
  }
  ui.setTheme(true);

  // without a card the ring says the task in a line; once the plan is over the button has that place, and the task button still opens the sheet
  ui.setSize(1180, 760);
  finish("focus", "p25");
  app.taskSet(L"Review notes");
  ui.changed();
  t.check("again ui: in a narrower window the ring has the button and not the task line", ui.find(H_AGAIN).k == H_AGAIN && ui.find(H_TASK, 1).k == H_NONE && ui.find(H_TASK, 0).k == H_TASK);
  app.startAll();
  ui.changed();
  t.check("again ui: while it runs the task line is back and the button is gone", ui.find(H_AGAIN).k == H_NONE && ui.find(H_TASK, 1).k == H_TASK);
  ui.setSize(1602, 707);

  // the compact player: the button when the ring is big enough, the arrow on the big button always
  app.selectScene("focus");
  app.selectPlan("p25");
  finish("focus", "p25");
  ui.setMini(true);
  ui.setSize(320, 484);
  ui.changed();
  t.check("again ui: the compact player has it too", ui.find(H_AGAIN).k == H_AGAIN && ui.find(H_PLAY).tip == L"Start again (Space)");
  r.click(H_AGAIN);
  t.check("again ui: and it works there", !app.session.done() && app.running);
  finish("focus", "p25");
  ui.setSize(260, 484);
  ui.changed();
  t.check("again ui: in a ring too small for it the button is left out, and the big button still restarts", ui.find(H_AGAIN).k == H_NONE && ui.find(H_PLAY).tip == L"Start again (Space)" && ui.ringRect().w < 180,
          fmt("ring %.0f", ui.ringRect().w));
  r.click(H_PLAY);
  t.check("again ui: so it can", !app.session.done() && app.running);
  ui.setMini(false);
  ui.setSize(1602, 707);

  // the smallest window
  ui.setSize(940, 640);
  finish("focus", "p25");
  ui.changed();
  {
    const Hit a3 = ui.find(H_AGAIN);
    t.check("again ui: at the smallest window size the button still fits in the ring", a3.k == H_AGAIN && a3.r.cx() > ui.ringRect().x && a3.r.b() < ui.ringRect().b());
  }
  ui.setSize(1602, 707);
}

// The task card beside the ring, the Task sheet in a narrow window, the line in the ring, the T key and the editors. There is
// ONE task for the whole plan, so there is nothing to switch between: no chips, no rounds.
void testTasksUi(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init()) { t.check("task ui: the rig starts", false); return; }
  Ui& ui = *r.ui;
  App& app = *r.app;
  Model& m = app.model;
  auto has = [&](HitKind k, int i = 0) { return ui.find(k, i).k == k; };
  auto taskHits = [&] {  // every hit that belongs to the task
    int n = 0;
    for (HitKind k : {H_TASK, H_TCHECK, H_TTEXT, H_TDEL, H_TADD}) n += countKind(ui, k);
    return n;
  };
  // the editor's callback may start another editor and so replace itself: always call a copy
  auto finish = [&](int how, const wstring& text) {
    auto d = r.lastEditDone;
    if (d) d(how, text);
  };
  auto reset = [&](const char* scene = "focus") {
    app.stopAll();
    app.selectScene(scene);
    app.stopAll();
    app.selectPlan(m.scene().plans[0]);
    m.s.task = Task();
    app.toast = Toast();
    r.edits.clear();
    r.lastEditDone = nullptr;
    ui.openSheet(0);
    ui.changed();
  };
  auto cover = [&] {  // the box around everything the card or sheet registered
    R b;
    bool any = false;
    for (const Hit& h : ui.hits()) {
      if (h.k != H_TCHECK && h.k != H_TTEXT && h.k != H_TDEL && h.k != H_TADD) continue;
      if (!any) b = h.r;
      else { const float x1 = std::max(b.r(), h.r.r()), y1 = std::max(b.b(), h.r.b()); b.x = std::min(b.x, h.r.x); b.y = std::min(b.y, h.r.y); b.w = x1 - b.x; b.h = y1 - b.y; }
      any = true;
    }
    return b;
  };
  auto fullTask = [&] {  // the task with all four steps
    m.s.task = Task();
    m.setTask(L"Chapter 3 problems");
    for (const wchar_t* s : {L"Read the examples", L"Question 1 to 5", L"Question 6 to 10", L"Check the answers"}) m.addStep(s);
    ui.changed();
  };

  // ---- a wide window: the card beside the ring
  reset();
  ui.setSize(1602, 707);
  ui.changed();
  t.check("task ui: a wide window has the card beside the ring, with a field for the task", has(H_TADD, 0) && !has(H_TASK, 0) && ui.find(H_TADD, 0).r.x >= ui.ringRect().r(),
          fmt("field at %.0f, ring ends at %.0f", ui.find(H_TADD, 0).r.x, ui.ringRect().r()));
  t.check("task ui: and nothing else yet", !has(H_TTEXT, 0) && !has(H_TCHECK, 0) && !has(H_TDEL, 0) && !has(H_TADD, 1) && taskHits() == 1);
  t.check("task ui: its tip invites you to write the task", narrow(ui.find(H_TADD, 0).tip) == "Write the task (T)", narrow(ui.find(H_TADD, 0).tip));
  const R fieldPlan = ui.find(H_TADD, 0).r;
  for (const char* plan : {"p50", "free", "p25"}) {
    app.selectPlan(plan);
    ui.changed();
    const R f = ui.find(H_TADD, 0).r;
    t.check(string("task ui: the same field on ") + plan + ": the task is for the plan, not for a round", taskHits() == 1 && std::fabs(f.x - fieldPlan.x) < .01f && std::fabs(f.y - fieldPlan.y) < .01f && std::fabs(f.w - fieldPlan.w) < .01f);
  }

  // writing the task: Enter carries on from the task through the steps, up to four
  r.click(H_TADD, 0);
  t.check("task ui: a click on the field opens an editor over it", r.edits.size() == 1 && (bool)r.lastEditDone);
  const R field = ui.find(H_TADD, 0).r;
  t.check("task ui: the editor is inside the field", r.edits[0].second.empty() && r.edits[0].first.x >= field.x && r.edits[0].first.r() <= field.r() && std::fabs(r.edits[0].first.cy() - field.cy()) < 1.5f);
  finish(EDIT_ENTER, L"  Chapter 3   problems ");
  t.check("task ui: Enter sets the task, cleaned", m.s.task.text == L"Chapter 3 problems", narrow(m.s.task.text));
  t.check("task ui: and goes straight on to the first step", r.edits.size() == 2 && r.edits[1].second.empty());
  t.check("task ui: that editor is on the row below the task", r.edits[1].first.cy() > ui.find(H_TTEXT, 0).r.cy() + 20 && !has(H_TADD, 0) && has(H_TADD, 1));
  finish(EDIT_ENTER, L"Read the examples");
  finish(EDIT_ENTER, L"Question 1 to 5");
  t.check("task ui: each Enter adds a step and asks for the next", m.s.task.steps.size() == 2 && r.edits.size() == 4);
  finish(EDIT_ENTER, L"Question 6 to 10");
  finish(EDIT_ENTER, L"Check the answers");
  t.check("task ui: the fourth step ends it, there is no room for a fifth", m.s.task.steps.size() == 4 && r.edits.size() == 5 && !has(H_TADD, 1) && has(H_TTEXT, 4));
  t.check("task ui: every part has its tick, text and cross", has(H_TCHECK, 0) && has(H_TDEL, 0) && has(H_TCHECK, 4) && has(H_TTEXT, 4) && has(H_TDEL, 4) && !has(H_TCHECK, 5));
  t.check("task ui: and the steps sit under the task, in order", ui.find(H_TTEXT, 0).r.y < ui.find(H_TTEXT, 1).r.y && ui.find(H_TTEXT, 3).r.y < ui.find(H_TTEXT, 4).r.y && ui.find(H_TTEXT, 1).r.x > ui.find(H_TTEXT, 0).r.x);

  // the chain stops where it should
  m.s.task = Task();
  r.edits.clear();
  ui.changed();
  r.click(H_TADD, 0);
  finish(EDIT_BLUR, L"Blurred task");
  t.check("task ui: moving away keeps the text but does not carry on", m.s.task.text == L"Blurred task" && r.edits.size() == 1);
  r.click(H_TADD, 1);
  finish(EDIT_ENTER, L"   ");
  t.check("task ui: Enter on nothing ends the chain and adds nothing", m.s.task.steps.empty() && r.edits.size() == 2);
  r.click(H_TADD, 1);
  finish(EDIT_CANCEL, L"never kept");
  t.check("task ui: Esc adds nothing and does not carry on", m.s.task.steps.empty() && r.edits.size() == 3);
  r.click(H_TADD, 1);
  finish(EDIT_BLUR, L"Blurred step");
  t.check("task ui: a step kept by moving away is added, without carrying on", m.s.task.steps.size() == 1 && r.edits.size() == 4);
  r.click(H_TTEXT, 0);
  t.check("task ui: a click on the task edits it, showing what is there", r.edits.size() == 5 && r.edits.back().second == L"Blurred task");
  finish(EDIT_ENTER, L"Renamed");
  t.check("task ui: Enter on a task that was already there just changes it", m.s.task.text == L"Renamed" && r.edits.size() == 5);
  r.click(H_TTEXT, 0);
  finish(EDIT_CANCEL, L"zzz");
  t.check("task ui: Esc leaves it as it was", m.s.task.text == L"Renamed" && r.edits.size() == 6);
  r.click(H_TTEXT, 0);
  finish(EDIT_ENTER, L"   ");
  t.check("task ui: an emptied task is not a deleted one", m.s.task.text == L"Renamed");
  r.click(H_TTEXT, 1);
  t.check("task ui: a step opens with its own words", r.edits.back().second == L"Blurred step");
  finish(EDIT_ENTER, L"Reworded");
  t.check("task ui: and Enter rewords it without carrying on", m.s.task.steps[0].text == L"Reworded" && r.edits.size() == 8);

  // ticking and deleting
  fullTask();
  const size_t editsBefore = r.edits.size();
  r.click(H_TCHECK, 1);
  t.check("task ui: a click on a step's box ticks it", m.s.task.steps[0].done && m.s.task.stepsDone() == 1 && !m.s.task.finished());
  r.click(H_TCHECK, 1);
  t.check("task ui: and again unticks it", !m.s.task.steps[0].done);
  r.click(H_TCHECK, 0);
  t.check("task ui: the task's box ticks the lot", m.s.task.finished());
  r.click(H_TCHECK, 0);
  t.check("task ui: and unticks the lot", m.s.task.stepsDone() == 0);
  r.click(H_TDEL, 2);
  t.check("task ui: a step's cross removes it", m.s.task.steps.size() == 3 && m.s.task.steps[1].text == L"Question 6 to 10" && has(H_TADD, 1));
  r.click(H_TDEL, 0);
  t.check("task ui: the task's cross deletes it, steps and all, and offers an undo", m.s.task.empty() && app.toast.active() && has(H_TOASTACT) && has(H_TADD, 0));
  r.click(H_TOASTACT);
  t.check("task ui: the undo brings the whole task back", m.s.task.text == L"Chapter 3 problems" && m.s.task.steps.size() == 3 && !app.toast.active());
  t.check("task ui: none of that opened an editor", r.edits.size() == editsBefore);

  // the T key writes the next thing
  reset();
  ui.setSize(1602, 707);
  ui.changed();
  t.check("task ui: T with no task asks for it", ui.key('T', false, false) && r.edits.size() == 1 && r.edits[0].second.empty());
  finish(EDIT_ENTER, L"Write the essay");
  t.check("task ui: and then for its steps", r.edits.size() == 2);
  finish(EDIT_CANCEL, L"");
  t.check("task ui: Esc stops there", m.s.task.text == L"Write the essay" && m.s.task.steps.empty() && r.edits.size() == 2);
  ui.key('T', false, false);
  t.check("task ui: T again adds a step", r.edits.size() == 3 && r.edits[2].second.empty() && r.edits[2].first.cy() > ui.find(H_TTEXT, 0).r.cy());
  finish(EDIT_BLUR, L"Outline");
  for (const wchar_t* s : {L"Draft", L"Edit", L"Submit"}) m.addStep(s);
  ui.changed();
  r.edits.clear();
  ui.key('T', false, false);
  t.check("task ui: with four steps it edits the task instead", r.edits.size() == 1 && r.edits[0].second == L"Write the essay");
  finish(EDIT_CANCEL, L"");

  // nothing overlaps: the card, the ring, the plans, the transport, the spectrum and the side panel
  const std::pair<float, float> wide[] = {{1602, 707}, {1500, 720}, {1700, 800}, {1920, 1080}};
  for (const auto& sz : wide) {
    ui.setSize(sz.first, sz.second);
    ui.changed();
    const string at = fmt("%.0fx%.0f", sz.first, sz.second);
    if (!has(H_TADD, 1)) { m.s.task.steps.pop_back(); ui.changed(); }  // three steps and the add row: the tallest card
    const R box = cover();
    const float sideX = ui.find(H_TAB, 0).r.x - 20;  // the side panel starts 20 left of its first tab
    const R ring = ui.ringRect(), tr = ui.transportRect(), sp = ui.specRect(), seg = ui.find(H_PLAN, 0).r;
    t.check("task ui: at " + at + " it has a card", !has(H_TASK, 0) && box.w > 200 && box.h > 100, fmt("%.0f x %.0f", box.w, box.h));
    t.check("task ui: at " + at + " the card clears the ring, the transport and the spectrum", !box.hits(ring) && !box.hits(tr) && !box.hits(sp), fmt("card %.0f,%.0f %.0fx%.0f", box.x, box.y, box.w, box.h));
    t.check("task ui: at " + at + " it stays between the ring and the side panel, under the plans", box.x >= ring.r() && box.r() <= sideX && box.y >= seg.y, fmt("%.0f to %.0f, panel at %.0f", box.x, box.r(), sideX));
  }
  // and the card keeps its place as steps come and go: it is as tall as what is in it, from the same top
  ui.setSize(1602, 707);
  m.s.task = Task();
  ui.changed();
  const float cardTop = ui.find(H_TADD, 0).r.y;
  m.setTask(L"Chapter 3");
  ui.changed();
  const float titleTop = ui.find(H_TTEXT, 0).r.y;
  for (const wchar_t* s : {L"a", L"b", L"c", L"d"}) m.addStep(s);
  ui.changed();
  t.check("task ui: the card does not move as steps are added", std::fabs(ui.find(H_TTEXT, 0).r.y - titleTop) < .01f && std::fabs(titleTop - (cardTop - 3)) < 1.f && ui.find(H_TTEXT, 4).r.b() < ui.ringRect().b() + 60,
          fmt("%.1f %.1f %.1f", cardTop, titleTop, ui.find(H_TTEXT, 0).r.y));

  // ---- a narrow window: a button and a sheet
  reset();
  ui.setSize(1180, 760);
  ui.changed();
  t.check("task ui: a narrow window has a button instead of the card", has(H_TASK, 0) && !has(H_TADD, 0) && !has(H_TTEXT, 0));
  t.check("task ui: its tip invites you to write the task", narrow(ui.find(H_TASK, 0).tip) == "Write the task (T)", narrow(ui.find(H_TASK, 0).tip));
  t.check("task ui: the button comes right after the loop button", ui.find(H_TASK, 0).r.x > ui.find(H_LOOP).r.r() && ui.find(H_TASK, 0).r.x - ui.find(H_LOOP).r.r() < 16 && std::fabs(ui.find(H_TASK, 0).r.cy() - ui.find(H_LOOP).r.cy()) < 2);
  t.check("task ui: there is no line in the ring while there is no task", !has(H_TASK, 1));
  r.click(H_TASK, 0);
  t.check("task ui: the button opens the Task sheet", ui.sheet() == 3 && has(H_SHEETCLOSE) && has(H_TADD, 0));
  {
    const Hit s = ui.find(H_SIDE);
    const R box = cover();
    t.check("task ui: the sheet holds everything", s.k == H_SIDE && box.x >= s.r.x && box.r() <= s.r.r() && box.y >= s.r.y && box.b() <= s.r.b(), fmt("rows %.0f..%.0f in a sheet %.0f..%.0f", box.y, box.b(), s.r.y, s.r.b()));
    t.check("task ui: the rows start under the sheet's own heading, with no second one of their own", std::fabs(ui.find(H_TADD, 0).r.y - (s.r.y + SHEET_TASK_TOP + 3)) < .5f, fmt("%.1f vs %.1f", ui.find(H_TADD, 0).r.y, s.r.y + SHEET_TASK_TOP));
  }
  r.click(H_TADD, 0);
  t.check("task ui: writing works there too", r.edits.size() == 1);
  finish(EDIT_ENTER, L"Chapter 3 problems");
  t.check("task ui: the chain carries on inside the sheet", ui.sheet() == 3 && m.s.task.text == L"Chapter 3 problems" && r.edits.size() == 2 && has(H_TADD, 1));
  const float h0 = ui.find(H_SIDE).r.b() - ui.find(H_SIDE).r.y, top0 = ui.find(H_SIDE).r.y;
  finish(EDIT_ENTER, L"Read the examples");
  finish(EDIT_CANCEL, L"");
  t.check("task ui: the sheet grows downwards with the steps, its top stays", ui.find(H_SIDE).r.b() - ui.find(H_SIDE).r.y > h0 + 20 && std::fabs(ui.find(H_SIDE).r.y - top0) < 0.5f);
  t.check("task ui: a click outside, or Esc, closes it", (ui.key(VK_ESCAPE, false, false), ui.sheet() == 0));
  t.check("task ui: with a task there is a line in the ring that opens the sheet", has(H_TASK, 1) && ui.find(H_TASK, 1).r.cy() > ui.ringRect().cy() && ui.find(H_TASK, 1).tip == L"Chapter 3 problems");
  t.check("task ui: and the button's tip is now about the task, not about writing one", narrow(ui.find(H_TASK, 0).tip) == "The task (T)", narrow(ui.find(H_TASK, 0).tip));
  r.click(H_TASK, 1);
  t.check("task ui: clicking it opens the sheet", ui.sheet() == 3);
  ui.key(VK_ESCAPE, false, false);
  ui.key('T', false, false);
  t.check("task ui: T there opens the sheet and starts on the next step", ui.sheet() == 3 && r.edits.back().second.empty() && r.edits.size() == 4);
  finish(EDIT_CANCEL, L"");
  t.check("task ui: T with the sheet open carries on writing", (ui.key('T', false, false), r.edits.size() == 5));
  finish(EDIT_CANCEL, L"");
  ui.key(VK_RETURN, false, false);
  t.check("task ui: Enter closes the sheet", ui.sheet() == 0);

  // the smallest window still has room for the button
  ui.setSize(940, 640);
  ui.changed();
  {
    const Hit b = ui.find(H_TASK, 0), f = ui.find(H_PLAN, 0);
    t.check("task ui: at the smallest window size the group, button included, fits in the stage", b.k == H_TASK && f.r.x - 3 >= 232 && b.r.r() <= 940 - 396, fmt("%.0f to %.0f of 232 to 544", f.r.x - 3, b.r.r()));
  }
  ui.setSize(1180, 760);

  // ---- the scenes without tasks
  const std::pair<float, float> both[] = {{1602, 707}, {1180, 760}};
  for (const char* id : {"calm", "sleep"}) {
    reset(id);
    for (const auto& sz : both) {
      ui.setSize(sz.first, sz.second);
      ui.changed();
      t.check(string("task ui: ") + id + " at " + fmt("%.0f", sz.first) + " has no card, button or line", taskHits() == 0);
    }
    r.edits.clear();
    app.toast = Toast();
    t.check(string("task ui: T in ") + id + " says why instead", ui.key('T', false, false) && r.edits.empty() && ui.sheet() == 0 && app.toast.active() && app.toast.msg.find(L"focus scenes") != wstring::npos, narrow(app.toast.msg));
  }
  reset();
  ui.setSize(1180, 760);
  ui.openSheet(3);
  app.selectScene("calm");
  ui.changed();
  const int leftOver = taskHits();  // this lays the window out again, which is what closes the sheet
  t.check("task ui: if the scene changes under an open Task sheet, the sheet goes", ui.sheet() == 0 && leftOver == 0);
  reset();

  // ---- how it looks: the tick boxes, in both themes, in the card and in the sheet
  for (const bool dark : {true, false}) {
    ui.setTheme(dark);
    const string theme = dark ? "dark" : "light";
    for (const bool sheet : {false, true}) {
      reset();
      ui.setSize(sheet ? 1180 : 1602, sheet ? 760 : 707);
      m.setTask(L"Chapter 3 problems");
      m.addStep(L"Read the examples");
      m.addStep(L"Question 1 to 5");
      m.tickStep(0);
      if (sheet) ui.openSheet(3);
      ui.changed();
      vector<uint8_t> px;
      UINT pw = 0, ph = 0;
      render(r, 1, px, pw, ph);
      auto boxPix = [&](int part) {  // the left edge of the tick box, in the middle of its height
        const R b = ui.find(H_TCHECK, part).r.inset(5);
        return at(px, pw, (int)(b.x + 2.5f), (int)b.cy());
      };
      const string where = sheet ? "in the sheet" : "in the card";
      if (!sheet) {  // the card says what it is in the strip above the task; the sheet has its own heading instead
        const Palette P = paletteFor(dark);
        const Pix panel = {(int)std::lround(P.panel.r * 255), (int)std::lround(P.panel.g * 255), (int)std::lround(P.panel.b * 255)};
        const R tk = ui.find(H_TCHECK, 0).r;
        int ink = 0;
        for (int y = (int)(tk.y + 5 - TK_HEAD + 8); y < (int)(tk.y + 5 - 6); y++)
          for (int x = (int)(tk.x + 5); x < (int)(tk.x + 5 + 70); x++) if (!closeTo(at(px, pw, x, y), panel, 14)) ink++;
        t.check("task ui (" + theme + "): the card has its TASK heading above the task", ink > 20, std::to_string(ink));
      }
      t.check("task ui (" + theme + "): " + where + ", a ticked step has a filled accent box", orangeish(boxPix(1)), pixStr(boxPix(1)));
      t.check("task ui (" + theme + "): " + where + ", an unticked one is an empty box", !orangeish(boxPix(2)), pixStr(boxPix(2)));
      t.check("task ui (" + theme + "): " + where + ", the task's box shows it is part way", !orangeish(boxPix(0)));
      m.tickStep(1);
      ui.changed();
      render(r, 1, px, pw, ph);
      t.check("task ui (" + theme + "): " + where + ", with every step ticked the task's box is filled too", orangeish(boxPix(0)) && orangeish(boxPix(2)), pixStr(boxPix(0)));
    }
  }
  ui.setTheme(true);

  // the task is the big thing and the steps are smaller: the same letters measure wider as the task than as a step
  {
    reset();
    ui.setSize(1602, 707);
    m.setTask(L"Chapter 3 problems");
    m.addStep(L"Chapter 3 problems");
    ui.changed();
    const TextStyle big = ui.taskStyle(true, false), small = ui.taskStyle(false, false);
    t.check("task ui: the task is set larger and heavier than its steps", big.size > small.size && big.weight > small.weight &&
                                                                          r.g.measure(L"Chapter 3 problems", big) > r.g.measure(L"Chapter 3 problems", small) + 8);
    t.check("task ui: and its row is taller", ui.find(H_TTEXT, 0).r.h > ui.find(H_TTEXT, 1).r.h);
  }

  // ---- the line in the ring, and the ring with a card
  reset();
  ui.setSize(1180, 760);
  ui.changed();
  {
    vector<uint8_t> a, b;
    UINT pw = 0, ph = 0;
    render(r, 1, a, pw, ph);
    m.setTask(L"Chapter 3 problems");
    ui.changed();
    render(r, 1, b, pw, ph);
    const R rr = ui.ringRect();
    int inside = 0, below = 0, outside = 0;
    for (int y = 0; y < (int)ph; y++)
      for (int x = 0; x < (int)pw; x++) {
        const Pix p = at(a, pw, x, y), q = at(b, pw, x, y);
        if (std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b) <= 9) continue;
        if (x >= rr.x && x < rr.r() && y >= rr.y && y < rr.b()) { inside++; if (y > rr.cy()) below++; }
        else if (!(ui.find(H_TASK, 0).r.has((float)x, (float)y))) outside++;  // the button lights up too
      }
    t.check("task ui: the task appears as a line in the ring, below the middle", inside > 100 && below == inside, fmt("%d pixels, %d below the middle", inside, below));
    t.check("task ui: and changes nothing else on screen", outside == 0, std::to_string(outside));
    // it stays inside the circle: every changed pixel is within the tick marks
    const float cx = rr.cx(), cy = rr.cy(), rad = 137 * ui.ringRect().w / 320.f;
    int beyond = 0;
    for (int y = (int)rr.y; y < (int)rr.b(); y++)
      for (int x = (int)rr.x; x < (int)rr.r(); x++) {
        const Pix p = at(a, pw, x, y), q = at(b, pw, x, y);
        if (std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b) > 9 && std::hypot(x - cx, y - cy) > rad) beyond++;
      }
    t.check("task ui: and stays inside the tick marks", beyond == 0, std::to_string(beyond));
    // the same task, with a long title, is cut rather than spilling out
    m.s.task.text = wstring(60, L'W');
    ui.changed();
    render(r, 1, b, pw, ph);
    beyond = 0;
    for (int y = (int)rr.y; y < (int)rr.b(); y++)
      for (int x = (int)rr.x; x < (int)rr.r(); x++) {
        const Pix p = at(a, pw, x, y), q = at(b, pw, x, y);
        if (std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b) > 9 && std::hypot(x - cx, y - cy) > rad) beyond++;
      }
    t.check("task ui: sixty wide letters are cut to fit, not spilled", beyond == 0, std::to_string(beyond));
  }
  // with the card beside the ring the ring keeps to its own business
  reset();
  ui.setSize(1602, 707);
  ui.changed();
  {
    vector<uint8_t> a, b;
    UINT pw = 0, ph = 0;
    render(r, 1, a, pw, ph);
    m.setTask(L"Chapter 3 problems");
    m.addStep(L"Read the examples");
    ui.changed();
    render(r, 1, b, pw, ph);
    const R rr = ui.ringRect().inset(-2);
    int inRing = 0, inCard = 0;
    const R box = cover();
    for (int y = 0; y < (int)ph; y++)
      for (int x = 0; x < (int)pw; x++) {
        const Pix p = at(a, pw, x, y), q = at(b, pw, x, y);
        if (std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b) <= 9) continue;
        if (rr.has((float)x, (float)y)) inRing++;
        else if (box.inset(-10).has((float)x, (float)y)) inCard++;
      }
    t.check("task ui: with the card there is no second copy of the task in the ring", inRing == 0, std::to_string(inRing));
    t.check("task ui: the card draws the task", inCard > 200, std::to_string(inCard));
  }

  // ---- the compact player: the task and its steps under the scenes, to tick and to edit; the ring has no copy of it
  reset();
  m.setTask(L"Chapter 3 problems");
  {
    vector<uint8_t> a, b;
    UINT pw = 0, ph = 0;
    ui.setMini(true);
    ui.changed();
    const float tall = ui.miniHeight();
    ui.setSize(MINI_W, tall);
    render(r, 1, b, pw, ph);
    const float scenesEnd = ui.find(H_SCENE, 0).r.b();
    const Hit titleRow = ui.find(H_TTEXT, 0);
    m.s.task = Task();
    ui.changed();
    ui.setSize(MINI_W, tall);  // the same size, so the two pictures can be compared
    render(r, 1, a, pw, ph);
    const R rr = ui.ringRect();
    int inRing = 0, below = 0;
    for (int y = 0; y < (int)ph; y++)
      for (int x = 0; x < (int)pw; x++) {
        const Pix p = at(a, pw, x, y), q = at(b, pw, x, y);
        if (std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b) <= 9) continue;
        if (rr.has((float)x, (float)y)) inRing++;
        else if ((float)y >= scenesEnd) below++;
      }
    t.check("task ui: the compact player has no copy of the task in the ring", inRing == 0, std::to_string(inRing));
    t.check("task ui: it writes the task under the scenes", below > 150 && titleRow.k == H_TTEXT && titleRow.r.y >= scenesEnd && titleRow.r.r() <= MINI_W, std::to_string(below));
    m.setTask(L"Chapter 3 problems");
    m.addStep(L"Question 1");
    m.addStep(L"Question 2");
    ui.changed();
    ui.setSize(MINI_W, ui.miniHeight());
    t.check("task ui: with a row for the task, one for each step and one that adds a step",
            ui.find(H_TTEXT, 0).k == H_TTEXT && ui.find(H_TCHECK, 0).k == H_TCHECK && ui.find(H_TCHECK, 1).k == H_TCHECK && ui.find(H_TCHECK, 2).k == H_TCHECK && ui.find(H_TDEL, 2).k == H_TDEL &&
                ui.find(H_TADD, 1).k == H_TADD && ui.find(H_TASK, 1).k == H_NONE && ui.find(H_TTEXT, 1).r.y > ui.find(H_TTEXT, 0).r.y);
    r.click(H_TCHECK, 1);
    t.check("task ui: a step is ticked there", m.s.task.steps[0].done && !m.s.task.steps[1].done);
    r.edits.clear();
    r.click(H_TTEXT, 2);
    t.check("task ui: a click on a step opens it for editing, in its own row",
            r.edits.size() == 1 && r.edits[0].second == L"Question 2" && r.edits[0].first.y >= scenesEnd && r.edits[0].first.r() <= MINI_W);
    auto done = r.lastEditDone;
    done(EDIT_BLUR, L"Question 2, both parts");
    t.check("task ui: and what is typed is kept", m.s.task.steps[1].text == L"Question 2, both parts");
    r.edits.clear();
    r.click(H_TTEXT, 0);
    done = r.lastEditDone;
    done(EDIT_BLUR, L"Chapter 3, all of it");
    t.check("task ui: the task itself is edited the same way", r.edits.size() == 1 && m.s.task.text == L"Chapter 3, all of it");
    r.edits.clear();
    t.check("task ui: T writes the next step there too, with no sheet", ui.key('T', false, false) && r.edits.size() == 1 && r.edits[0].second.empty() && ui.sheet() == 0);
    done = r.lastEditDone;
    done(EDIT_ENTER, L"Question 3");
    t.check("task ui: Enter keeps it and goes on to the one after", m.s.task.steps.size() == 3 && r.edits.size() == 2);
    done = r.lastEditDone;
    done(EDIT_CANCEL, L"");
    r.click(H_TDEL, 3);
    t.check("task ui: a step is deleted there", m.s.task.steps.size() == 2);
    r.click(H_TCHECK, 0);
    t.check("task ui: ticking the task ticks every step", m.s.task.finished());
    r.click(H_TDEL, 0);
    t.check("task ui: and the task can be deleted, with an Undo", m.s.task.empty() && app.toast.action == L"Undo" && ui.find(H_TADD, 0).k == H_TADD);
    app.toast = Toast();
    ui.setMini(false);
    ui.setSize(1180, 760);
  }

  // ---- the same task through every round and break: nothing to switch, nothing changes
  reset();
  ui.setSize(1602, 707);
  fullTask();
  app.startAll();
  {
    r.advance(500);
    ui.changed();
    const R first = ui.find(H_TTEXT, 0).r;
    bool same = true;
    int looked = 0;
    for (int span : {25, 5, 25, 5, 25, 5, 25}) {  // each phase of 25·5, rounds and breaks
      ui.changed();
      same = same && has(H_TTEXT, 0) && has(H_TTEXT, 4) && narrow(m.s.task.text) == "Chapter 3 problems" && std::fabs(ui.find(H_TTEXT, 0).r.y - first.y) < .01f && std::fabs(ui.find(H_TTEXT, 0).r.x - first.x) < .01f;
      looked++;
      r.advance((int64_t)span * 60000);
    }
    t.check("task ui: the card is the same in all seven phases of 25·5, rounds and breaks alike", same && looked == 7 && app.session.done(), fmt("%d phases, done %d", looked, app.session.done()));
    t.check("task ui: and when the plan is over", has(H_TTEXT, 0) && m.s.task.steps.size() == 4 && !m.s.task.finished());
  }
  app.stopAll();
  ui.setSize(1180, 760);
  reset();
  m.setTask(L"Chapter 3");
  app.startAll();
  ui.changed();
  {
    const wstring tipRound = ui.find(H_TASK, 1).tip;
    r.advance(25 * 60000 + 1000);  // into the first break
    ui.changed();
    t.check("task ui: the line in the ring says the same in a break, without a word about the next round", has(H_TASK, 1) && ui.find(H_TASK, 1).tip == tipRound && tipRound == L"Chapter 3", narrow(ui.find(H_TASK, 1).tip));
  }
  app.stopAll();

  // ---- the Keys sheet
  bool hasT = false;
  for (const KeyRow& row : keyRows()) {
    bool key = false, icon = false;
    for (const wchar_t* k : row.keys) key = key || wcscmp(k, L"T") == 0;
    for (const char* ic : row.icons) icon = icon || std::strcmp(ic, "i-task") == 0;
    hasT = hasT || (key && icon);
  }
  t.check("task ui: the Keys sheet lists T with the task icon", hasT && (int)keyRows().size() == KEYS_ROWS, std::to_string(keyRows().size()));
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
      "-OekvEFm1lo\tis_live\tgentle rain ambience  cozy sound to chill to\n"
      "DWcJFNfaw9c\twas_live\tlofi hip hop radio - beats to sleep/chill to\n"
      "5qap5aO4i9A\twas_live\tlofi hip hop radio - beats to relax/study to\n"
      "garbage line without tabs\n"
      "short\tis_live\tbad id\n";
  const auto entries = parseFlatList(sample);
  t.check("resolver: lines are parsed and bad ones skipped", entries.size() == 12, std::to_string(entries.size()));
  const auto m = matchStreams(entries);
  t.check("resolver: Focus finds the current lofi radio first", m.count("focus") && m.at("focus")[0] == "rFZHOHl-L8A");
  t.check("resolver: Focus keeps sleep and halloween streams out", m.count("focus") && std::find(m.at("focus").begin(), m.at("focus").end(), "JD-kMIpDfnY") == m.at("focus").end() &&
                                                                    std::find(m.at("focus").begin(), m.at("focus").end(), "1-LpQekNa9g") == m.at("focus").end());
  t.check("resolver: Sleep finds the deep sleep stream", m.count("sleep") && m.at("sleep")[0] == "nI725iVsyoQ");
  t.check("resolver: Read gets the classical radio and Calm the fireplace", m.count("read") && m.at("read")[0] == "jXAEIWcGXwE" && m.count("calm") && m.at("calm")[0] == "q_4KI-ChIIs");
  const SceneDef* calm = findStock("calm");
  t.check("resolver: Calm adds the rain ambience after the fireplace", m.count("calm") && m.at("calm").size() == 2 && m.at("calm")[1] == "-OekvEFm1lo");
  t.check("resolver: Calm has a title hint for each stream, in the same order, and no longer lists the dead one",
          calm && calm->streams.size() == 3 && calm->hintInclude.size() == calm->streams.size() && calm->streams[1] == "-OekvEFm1lo" &&
              calm->hintInclude[1] == "gentle rain ambience" && std::find(calm->streams.begin(), calm->streams.end(), "JK0pBKoK5FQ") == calm->streams.end());
  t.check("resolver: ended streams are not offered", !m.count("create") || std::find(m.at("create").begin(), m.at("create").end(), "DWcJFNfaw9c") == m.at("create").end());
  t.check("resolver: scenes with no stream get nothing", !m.count("mask") && !m.count("white"));

  // the cache must not outlive the hints that produced it, or a retired stream keeps coming back
  std::map<string, vector<string>> back;
  int64_t when = 0;
  const string saved = cacheText(m, 1700000000);
  t.check("resolver: the cache round-trips", parseCacheText(saved, back, &when) && when == 1700000000 && back == m);
  string other = saved;
  const size_t sigAt = other.find(hintSignature());
  if (sigAt != string::npos) other[sigAt] = other[sigAt] == '0' ? '1' : '0';
  std::map<string, vector<string>> none;
  t.check("resolver: a cache written under other hints is ignored", sigAt != string::npos && !parseCacheText(other, none, &when) && none.empty());
  t.check("resolver: a cache from before there was a signature is ignored",
          !parseCacheText("{\"at\": 1700000000, \"fresh\": {\"sprint\": [\"iODXbOA6T3o\"]}}", none, &when) && none.empty());
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
  ui.setSize(MINI_W, ui.miniHeight());
  t.check("mini: every scene, transport, mute, volume and the way back",
          countKind(ui, H_SCENE) == NSCENES && countKind(ui, H_PLAY) == 1 && countKind(ui, H_PREV) == 1 && countKind(ui, H_NEXT) == 1 && countKind(ui, H_MUTE) == 1 &&
              countKind(ui, H_MASTER) == 1 && countKind(ui, H_MINI) == 1);
  t.check("mini: no mix tiles, bands, plans or presets", countKind(ui, H_TILE) == 0 && countKind(ui, H_BAND) == 0 && countKind(ui, H_PLAN) == 0 && countKind(ui, H_CHIP) == 0);
  auto fits = [&] {  // every clickable thing is inside a window of the size the player asks for
    ui.setSize(MINI_W, ui.miniHeight());
    for (auto& h : ui.hits()) if (h.r.x < 0 || h.r.y < 0 || h.r.r() > MINI_W + 1 || h.r.b() > ui.miniHeight() + 1) return false;
    return true;
  };
  t.check("mini: everything fits the compact window", fits());
  const Hit play = ui.find(H_PLAY);
  t.check("mini: the ring and transport sit above the scene row, and there is no spectrum",
          ui.ringRect().b() < play.r.y && play.r.b() < ui.find(H_SCENE, 0).r.y && ui.specRect().w == 0 && ui.specRect().h == 0);

  // it is as tall as what it holds: the task and its steps go under the scenes, where the spectrum was
  {
    const float scenesEnd = ui.find(H_SCENE, 0).r.b();
    const float bare = ui.miniHeight();
    t.check("mini: with no task there is the field that asks for one, and nothing under it",
            ui.find(H_TADD, 0).k == H_TADD && ui.find(H_TADD, 0).r.y >= scenesEnd && bare == scenesEnd + MT_GAP + MT_TITLE + MT_FOOT, std::to_string(bare));
    app.taskSet(L"Read chapter 4");
    t.check("mini: a task makes room for the row that adds its first step", ui.miniHeight() == bare + MT_STEP && fits() && ui.find(H_TADD, 1).k == H_TADD);
    app.taskAddStep(L"Sections 1 and 2");
    app.taskAddStep(L"Section 3");
    app.taskAddStep(L"The exercises");
    t.check("mini: every step makes it one row taller", ui.miniHeight() == bare + 4 * MT_STEP && fits(), std::to_string(ui.miniHeight()));
    const float rows3 = ui.miniHeight();
    app.taskAddStep(L"Check the answers");
    t.check("mini: the fourth step takes the place of the row that adds one, so it is no taller", ui.miniHeight() == rows3 && fits() && ui.find(H_TADD, 1).k == H_NONE && ui.find(H_TTEXT, 4).k == H_TTEXT);
    float lowest = 0;
    for (auto& h : ui.hits()) if (h.k == H_TTEXT || h.k == H_TDEL) lowest = std::max(lowest, h.r.b());
    t.check("mini: the last row ends just above the window's edge", lowest <= ui.miniHeight() - MT_FOOT + 0.5f && lowest >= ui.miniHeight() - MT_FOOT - MT_STEP, std::to_string(lowest));
    app.taskRemoveStep(0);
    t.check("mini: taking a step away makes room for the adding row again, at the same height", ui.miniHeight() == rows3 && ui.find(H_TADD, 1).k == H_TADD);
    app.selectScene("calm");
    app.stopAll();
    t.check("mini: a scene without tasks ends under the scenes", ui.miniHeight() == scenesEnd + MINI_PAD && fits() && ui.find(H_TTEXT, 0).k == H_NONE && ui.find(H_TADD, 0).k == H_NONE, std::to_string(ui.miniHeight()));
    r.edits.clear();
    app.toast = Toast();
    ui.key('T', false, false);
    t.check("mini: and T says why there is nothing to write there", r.edits.empty() && app.toast.active());
    app.toast = Toast();
    app.model.s.task = Task();
    app.selectScene("focus");
    app.stopAll();
    ui.changed();
    ui.setSize(MINI_W, ui.miniHeight());
  }
  {
    Model saved;
    saved.s.miniX = 1200;
    saved.s.miniY = 300;
    saved.s.miniH = 571;
    Model back, old;
    back.load(saved.serialize());
    old.load(R"({"mini":{"on":true,"x":5,"y":6}})");
    t.check("mini: how tall its window was is remembered with where it was", back.s.miniX == 1200 && back.s.miniY == 300 && back.s.miniH == 571 && old.s.miniX == 5 && old.s.miniH == 0);
  }

  r.click(H_SCENE, sceneIdx("mask"));
  t.check("mini: a scene button starts that scene", app.model.s.scene == "mask" && app.running);
  r.click(H_PLAY);
  t.check("mini: the play button pauses", !app.running);
  {
    const Hit m = ui.find(H_MASTER), ex = ui.find(H_MINI), mute = ui.find(H_MUTE);
    t.check("mini: the volume stands upright under the expand button, with mute at its foot",
            ui.masterVertical() && m.r.h > 2 * m.r.w && m.r.y >= ex.r.b() && std::fabs(m.r.cx() - ex.r.cx()) < 1 && mute.r.y >= m.r.b() && std::fabs(mute.r.cx() - ex.r.cx()) < 1);
    const R ring = ui.ringRect();
    const float reach = ring.w / 2 * (150.f / 160.f) + 4;  // the ring's own line, and a little air
    auto clear = [&](const R& q) {  // no corner of it comes inside the ring
      for (float cx : {q.x, q.r()}) for (float cy : {q.y, q.b()}) if (std::hypot(cx - ring.cx(), cy - ring.cy()) < reach) return false;
      return true;
    };
    t.check("mini: neither touches the ring", clear(m.r) && clear(mute.r));
    const float y = m.r.b() - 7 - (m.r.h - 14) * .25f;
    ui.mouseMove(m.r.cx(), y, false);
    ui.mouseDown(m.r.cx(), y);
    ui.mouseUp(m.r.cx(), y);
    t.check("mini: a click a quarter of the way up sets a quarter of the volume", std::abs(app.model.s.master - 25) <= 2, std::to_string(app.model.s.master));
    ui.mouseDown(m.r.cx(), y);
    ui.mouseMove(m.r.cx() + 40, m.r.y - 30, true);  // dragged up past its top, and sideways: only the height counts
    const int top = app.model.s.master;
    ui.mouseMove(m.r.cx(), m.r.b() + 30, true);
    const int bottom = app.model.s.master;
    ui.mouseUp(m.r.cx(), m.r.b() + 30);
    t.check("mini: dragging up is louder, down is quieter", top == 100 && bottom == 0, std::to_string(top) + " " + std::to_string(bottom));
    ui.wheel(m.r.cx(), m.r.cy(), 120 * 2);
    t.check("mini: the wheel turns it too", app.model.s.master == 10, std::to_string(app.model.s.master));
    app.setMaster(70);
  }
  r.click(H_MINI);
  t.check("mini: the expand button asks to go back", toggled == 3);

  // drawing
  app.selectScene("focus");
  r.advance(1000);
  r.advance(6 * 60000 + 15000);
  ui.changed();
  ui.setSize(MINI_W, ui.miniHeight());
  vector<uint8_t> px;
  UINT pw = 0, ph = 0;
  const bool ok = render(r, 1.0f, px, pw, ph);
  t.check("mini: the compact window renders", ok && pw == 320 && ph == (UINT)std::lround(ui.miniHeight()));
  if (ok) {
    const Palette P = paletteFor(true);
    auto col = [](Color c) { return Pix{(int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255)}; };
    const R vol = ui.find(H_MASTER).r;
    const Pix low = at(px, pw, (int)vol.cx(), (int)vol.b() - 3), high = at(px, pw, (int)vol.cx(), (int)vol.y + 2);
    t.check("mini: the volume is filled from the bottom up to its level", closeTo(low, col(P.accent), 30) && !closeTo(high, col(P.accent), 60), pixStr(low) + " / " + pixStr(high));
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
  t.check("mini: leaving it brings the full layout back", countKind(ui, H_TILE) == 10 && countKind(ui, H_SCENE) == NSCENES && ui.find(H_MINI).k == H_MINI);
  t.check("mini: with its volume lying flat and its spectrum", !ui.masterVertical() && ui.find(H_MASTER).r.w > 2 * ui.find(H_MASTER).r.h && ui.specRect().w > 0);
}

// ---------------------------------------------------------------------------------------------------
// The Sprint scene: fast, intense work that is not mentally heavy.
// ---------------------------------------------------------------------------------------------------

void testSprint(TestLog& t) {
  const SceneDef* sp = findStock("sprint");
  t.check("sprint: a stock scene, second in the rail", sp && stockScenes()[1].id == "sprint" && (int)stockScenes().size() == NSCENES);
  if (!sp) return;
  t.check("sprint: a light noise bed and beta beats", sp->study && sp->mix[(size_t)channelIndex("pink")] == 14 && sp->mix[(size_t)channelIndex("brown")] == 8 &&
                                                         sp->beat.on && sp->beat.band == bandIndex("beta") && sp->beat.level == 24);
  t.check("sprint: 15 minute rounds first, then 25, then open", sp->plans == vector<string>({"p15", "p25", "free"}));
  const PlanDef* p = findPlan("p15");
  t.check("sprint: four 15 minute rounds with 3 minute breaks",
          p && p->phases.size() == 7 && p->phases[0].kind == PH_FOCUS && p->phases[0].ms == 15 * 60000 && p->phases[1].kind == PH_BREAK && p->phases[1].ms == 3 * 60000);
  bool ids = sp->streams.size() == 6;
  for (auto& id : sp->streams) if (id.size() != 11) ids = false;
  t.check("sprint: six streams, ranked: the instrumental house study radio, techno, synthwave, drum and bass, game study radio, lofi house",
          ids && sp->streams[0] == "Ihm9OQWmibA" && sp->streams[1] == "UYOb37KRFqk" && sp->streams[2] == "4xDzrJKXOOY" && sp->streams[5] == "3PFJ9SETS4M");
  t.check("sprint: the vocal deep house stream is gone, from the list and from the title hints",
          std::find(sp->streams.begin(), sp->streams.end(), "8BzYJsT1FhA") == sp->streams.end() &&
              std::find(sp->hintInclude.begin(), sp->hintInclude.end(), "luxury vocal house") == sp->hintInclude.end());
  t.check("sprint: each stream has a title hint of its own, in the same order", sp->hintInclude.size() == sp->streams.size() && sp->hintInclude[0] == "smooth study music");

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init()) { t.check("sprint: the rig starts", false); return; }
  App& app = *r.app;
  r.ui->key('2', false, false);
  t.check("sprint: the 2 key starts it on the 15 minute plan", app.model.s.scene == "sprint" && app.model.planId() == "p15" && app.running && app.session.planId() == "p15");
  t.check("sprint: it begins with the instrumental house study radio", app.wantedSpec() == "Ihm9OQWmibA", app.wantedSpec());
  app.stepStream(1);
  t.check("sprint: next goes to the minimal techno", app.wantedSpec() == "UYOb37KRFqk", app.wantedSpec());
  app.stepStream(-1);
  t.check("sprint: and previous comes back to the house radio", app.wantedSpec() == "Ihm9OQWmibA", app.wantedSpec());
  r.advance(1000);
  const auto e = app.effective();
  t.check("sprint: beta beats at 16 Hz are on", e.beat.mode == 1 && e.beat.hz == 16 && e.beat.level == 24);
  r.advance(15 * 60000);
  t.check("sprint: after 15 minutes comes the short break", app.session.snapshot().kind == PH_BREAK && app.effective().levels == BREAK_MIX);
  t.check("sprint: it counts as study time", app.model.todayMinutes() > 14, std::to_string(app.model.todayMinutes()));

  vector<StreamEntry> entries = {
      {"AAAAAAAAAAA", "is_live", L"synthwave radio - beats to chill/game to"},
      {"BBBBBBBBBBB", "is_live", L"LIVE 24/7 | Deep House 2026-2027 Luxury Vocal House, Chill Grooves & Night Drive Vibes"},
      {"CCCCCCCCCCC", "is_live", L"lofi house radio   lounge music to vibe/chill to"},
      {"DDDDDDDDDDD", "was_live", L"Liquid Drum & Bass Radio 24/7"},
      {"EEEEEEEEEEE", "is_live", L"Baroque Beats Radio 24/7"},
      {"FFFFFFFFFFF", "is_live", L"Chill House · Smooth Study Music · 24/7 Live Radio"},
  };
  const auto m = matchStreams(entries);
  const auto it = m.find("sprint");
  t.check("sprint: fresh streams keep the ranking (house study radio under its new name, synthwave, lofi house), skip ended ones, and pick up neither baroque radios nor the vocal deep house",
          it != m.end() && it->second.size() == 3 && it->second[0] == "FFFFFFFFFFF" && it->second[1] == "AAAAAAAAAAA" && it->second[2] == "CCCCCCCCCCC");
}

// ---------------------------------------------------------------------------------------------------
// The player's Set up button, for a PC without mpv or yt-dlp.
// ---------------------------------------------------------------------------------------------------

void testSetupUi(TestLog& t) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init()) { t.check("setup ui: the rig starts", false); return; }
  Ui& ui = *r.ui;
  App& app = *r.app;
  Stream& st = r.st;
  const Palette P = paletteFor(true);
  auto col = [](Color c) { return Pix{(int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255)}; };
  app.selectScene("focus");
  app.stopAll();
  t.check("setup ui: no button while both tools are there", countKind(ui, H_SETUP) == 0);

  st.configure(L"", L"");
  ui.changed();
  const Hit b = ui.find(H_SETUP);
  const R pl = ui.playerRect();
  t.check("setup ui: with neither tool the player offers the download in its middle",
          b.k == H_SETUP && b.r.x >= pl.x && b.r.r() <= pl.r() && b.r.y > pl.y + pl.h * 0.3f && b.r.b() < pl.b() - 20 && std::fabs(b.r.cx() - pl.cx()) < 1);
  vector<uint8_t> px;
  UINT pw = 0, ph = 0;
  render(r, 1, px, pw, ph);
  t.check("setup ui: the button is drawn in the accent colour", closeTo(at(px, pw, (int)(b.r.x + 12), (int)b.r.cy()), col(P.accent), 6), pixStr(at(px, pw, (int)(b.r.x + 12), (int)b.r.cy())));
  t.check("setup ui: and the player behind it is dimmed to black", closeTo(at(px, pw, (int)(pl.x + 14), (int)(pl.b() - 14)), Pix{0, 0, 0}, 4));

  int asked = 0;
  app.startSetup = [&](int need) { asked = need; };
  r.click(H_SETUP);
  t.check("setup ui: clicking it starts the download for both tools", asked == (TOOL_MPV | TOOL_YTDLP) && app.setupState == SETUP_RUNNING);
  t.check("setup ui: while it runs the button gives way to a progress bar", ui.find(H_SETUP).k == H_NONE);
  app.onSetupProgress(L"Downloading mpv", 0.5);
  render(r, 1, px, pw, ph);
  {
    const float bw = std::min(196.f, pl.w - 32), trackY = pl.y + std::round(pl.h * 0.42f) + 28 + 2, x0 = pl.cx() - bw / 2;
    const Pix filled = at(px, pw, (int)(x0 + bw * 0.25f), (int)trackY), empty = at(px, pw, (int)(x0 + bw * 0.75f), (int)trackY);
    t.check("setup ui: the bar is filled to the progress and empty beyond it", closeTo(filled, col(P.accent), 6) && !closeTo(empty, col(P.accent), 60), pixStr(filled) + " / " + pixStr(empty));
  }
  app.onSetupDone(false, L"Couldn't reach GitHub.");
  const Hit again = ui.find(H_SETUP);
  t.check("setup ui: after a failure the button comes back to try again", again.k == H_SETUP && again.tip == L"Try the download again");
  asked = 0;
  r.click(H_PLAYER);
  t.check("setup ui: clicking anywhere on the player does the same", asked == (TOOL_MPV | TOOL_YTDLP) && app.setupState == SETUP_RUNNING);
  app.onSetupDone(false, L"x");

  st.configure(L"mpv.exe", L"");
  ui.changed();
  t.check("setup ui: with mpv but no yt-dlp the stream tab still offers it", ui.find(H_SETUP).k == H_SETUP);
  app.setTab("files");
  t.check("setup ui: your own music needs only mpv, so no button there", countKind(ui, H_SETUP) == 0);
  app.setTab("stream");
  st.configure(L"", L"");
  app.selectScene("mask");
  t.check("setup ui: scenes without a stream have no player and no button", countKind(ui, H_SETUP) == 0 && countKind(ui, H_PLAYER) == 0);
  app.selectScene("focus");
  st.configure(L"mpv.exe", L"yt-dlp.exe");
  ui.changed();
  t.check("setup ui: once the tools are there the button is gone and the player is as before", countKind(ui, H_SETUP) == 0 && countKind(ui, H_PLAYER) == 1);
}
