// `murmur --snapshot out.png [--size WxH] [--scale 1.25] [--light] [--do action]...`
// Renders the real UI offscreen so layouts can be checked without a window.
#include <shlwapi.h>

#include "rig.h"

bool savePng(Gfx& g, IWICBitmap* bmp, const wstring& path) {
  Com<IWICStream> stream;
  if (!g.wic || FAILED(g.wic->CreateStream(stream.put())) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  Com<IWICBitmapEncoder> enc;
  if (FAILED(g.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) || FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
  Com<IWICBitmapFrameEncode> frame;
  Com<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(frame.put(), props.put())) || FAILED(frame->Initialize(props.get()))) return false;
  UINT w = 0, h = 0;
  bmp->GetSize(&w, &h);
  frame->SetSize(w, h);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  frame->SetPixelFormat(&fmt);
  if (FAILED(frame->WriteSource(bmp, nullptr))) return false;
  return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

namespace {
HitKind kindByName(const string& n) {
  static const std::map<string, HitKind> m = {
      {"scene", H_SCENE}, {"plan", H_PLAN}, {"prev", H_PREV}, {"play", H_PLAY}, {"next", H_NEXT}, {"tile", H_TILE}, {"band", H_BAND},
      {"reset", H_RESET}, {"beatmode", H_BEATMODE}, {"mute", H_MUTE}, {"master", H_MASTER}, {"blend", H_BLEND}, {"info", H_INFO},
      {"keys", H_KEYS}, {"chip", H_CHIP}, {"chipx", H_CHIPX}, {"padd", H_PADD}, {"pupd", H_PUPD}, {"tab", H_TAB}, {"link", H_LINK},
      {"video", H_VIDEO}, {"watch", H_WATCH}, {"player", H_PLAYER}, {"vprev", H_VPREV}, {"vnext", H_VNEXT}, {"vclear", H_VCLEAR},
      {"filepick", H_FILEPICK}, {"toastact", H_TOASTACT}, {"mini", H_MINI}, {"setup", H_SETUP}};
  auto it = m.find(n);
  return it == m.end() ? H_NONE : it->second;
}

vector<string> split(const string& s, char c) {
  vector<string> out;
  size_t a = 0;
  for (;;) {
    const size_t b = s.find(c, a);
    out.push_back(s.substr(a, b == string::npos ? string::npos : b - a));
    if (b == string::npos) break;
    a = b + 1;
  }
  return out;
}

void makeDemoPresets(Rig& r) {
  App& a = *r.app;
  auto make = [&](const char* scene, std::initializer_list<std::pair<const char*, int>> mix, const wchar_t* name) {
    a.model.setScene(scene);
    a.session.setPlan(a.model.planId());
    a.model.resetMix();
    for (auto& kv : mix) a.model.setLevel(channelIndex(kv.first), kv.second);
    a.presetAdd();
    a.presetRename(a.model.s.scene, name);
  };
  make("focus", {{"brown", 40}, {"rain", 30}}, L"Rainy desk");
  make("read", {{"pink", 30}, {"fire", 20}}, L"Exam week");
  make("sleep", {{"ocean", 44}, {"brown", 20}}, L"Night owl");
}

bool decodeFile(Gfx& g, const wstring& path, Com<IWICBitmap>& out) {
  Com<IWICBitmapDecoder> dec;
  if (FAILED(g.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.put()))) return false;
  Com<IWICBitmapFrameDecode> fr;
  if (FAILED(dec->GetFrame(0, fr.put()))) return false;
  Com<IWICFormatConverter> conv;
  g.wic->CreateFormatConverter(conv.put());
  if (FAILED(conv->Initialize(fr.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  return SUCCEEDED(g.wic->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, out.put()));
}

void act(Rig& r, const string& a) {
  App& app = *r.app;
  Ui& ui = *r.ui;
  const vector<string> p = split(a, ':');
  const string& c = p[0];
  if (c == "pick") { app.model.setScene(p[1]); app.session.setPlan(app.model.planId()); ui.changed(); }
  else if (c == "scene") app.selectScene(p[1]);
  else if (c == "mini") ui.setMini(p.size() < 2 || p[1] != "off");
  else if (c == "play") app.startAll();
  else if (c == "pause") app.stopAll();
  else if (c == "advance") r.advance((int64_t)(std::stod(p[1]) * 1000));
  else if (c == "plan") app.selectPlan(p[1]);
  else if (c == "beats") app.setBeatsMode(std::stoi(p[1]));
  else if (c == "band") app.setBand(std::stoi(p[1]));
  else if (c == "tile") app.setLevel(std::stoi(p[1]), std::stoi(p[2]));
  else if (c == "master") app.setMaster(std::stoi(p[1]));
  else if (c == "blend") app.setBlend(std::stoi(p[1]));
  else if (c == "mute") app.toggleMute();
  else if (c == "presets") makeDemoPresets(r);
  else if (c == "tab") app.setTab(p[1]);
  else if (c == "files") {
    vector<wstring> f;
    for (int i = 0; i < std::stoi(p[1]); i++) f.push_back(L"C:\\Music\\Track " + std::to_wstring(i + 1) + L" - Ambient Study.mp3");
    app.addFiles(f);
    app.fileIndex = std::min(1, (int)f.size() - 1);
  }
  else if (c == "sheet") ui.openSheet(std::stoi(p[1]));
  else if (c == "toast") app.showToast(widen(p.size() > 1 ? p[1] : "Preset deleted"), p.size() > 2 ? widen(p[2]) : L"", [] {}, 600000);
  else if (c == "hover") {
    ui.changed();
    const Hit h = ui.find(kindByName(p[1]), p.size() > 2 ? std::stoi(p[2]) : 0);
    if (h.k != H_NONE) { ui.mouseMove(h.r.cx(), h.r.cy(), false); ui.tipTimer(); }
    else std::fprintf(stderr, "hover: no such hit %s\n", a.c_str());
  }
  else if (c == "stream") {
    app.streamState = p[1] == "loading" ? SS_LOADING : p[1] == "error" ? SS_ERROR : p[1] == "playing" ? SS_PLAYING : p[1] == "none" ? SS_NONE : SS_IDLE;
    ui.changed();
  }
  else if (c == "title") { app.streamTitle = widen(p[1]); ui.changed(); }
  else if (c == "tools") { app.stream.configure(p[1] == "none" ? L"" : L"mpv.exe", L""); ui.changed(); }  // tools:none, or tools:mpv (only yt-dlp missing)
  else if (c == "setup") {  // setup:running:42:Downloading mpv, or setup:failed:Couldn't reach GitHub. Check your connection.
    app.setupState = p[1] == "running" ? SETUP_RUNNING : p[1] == "failed" ? SETUP_FAILED : SETUP_IDLE;
    app.setupFraction = p[1] == "running" && p.size() > 2 ? std::stod(p[2]) / 100 : 0;
    app.setupStep = p[1] == "running" && p.size() > 3 ? widen(p[3]) : L"";
    app.setupError = p[1] == "failed" && p.size() > 2 ? widen(p[2]) : L"";
    ui.changed();
  }
  else if (c == "link") r.click(H_LINK);
  else if (c == "scroll") { for (int i = 0; i < std::stoi(p[1]); i++) ui.wheel(1100, 400, -120); }
  else if (c == "poster") {
    Com<IWICBitmap> bmp;
    const string id = ui.posterIdFor();
    if (!id.empty() && decodeFile(r.g, widen(p[1] + (p.size() > 2 ? ":" + p[2] : "")), bmp)) ui.setPoster(id, bmp, false);
    else std::fprintf(stderr, "poster: no id or cannot decode\n");
  }
  else if (c == "spectrum") {
    // let the engine mix for a while so the analyser has real sound to look at
    app.startAll();
    r.eng.waitSoundsReady(8000);
    vector<float> buf(2 * 4800);
    bool asleep = false;
    for (int i = 0; i < 60; i++) {
      for (int k = 0; k < 4; k++) r.eng.renderBlock(buf.data(), 4800);
      r.spec.frame(r.eng, true, &asleep);
    }
  }
  else std::fprintf(stderr, "unknown action %s\n", a.c_str());
}
}  // namespace

int runSnapshot(int argc, wchar_t** argv) {
  wstring out = L"snapshot.png";
  float w = 1180, h = 760, scale = 1;
  bool dark = true;
  vector<string> actions;
  for (int i = 1; i < argc; i++) {
    const wstring a = argv[i];
    if (a == L"--snapshot" && i + 1 < argc) out = argv[++i];
    else if (a == L"--size" && i + 1 < argc) { const string s = narrow(argv[++i]); const size_t x = s.find('x'); w = std::stof(s.substr(0, x)); h = std::stof(s.substr(x + 1)); }
    else if (a == L"--scale" && i + 1 < argc) scale = std::stof(narrow(argv[++i]));
    else if (a == L"--light") dark = false;
    else if (a == L"--do" && i + 1 < argc) actions.push_back(narrow(argv[++i]));
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Rig r;
  if (!r.init(w, h, dark)) { std::fprintf(stderr, "Direct2D is not available\n"); return 2; }
  for (const string& a : actions) act(r, a);

  // `--hits`: list every clickable thing with its centre (DIPs), for scripting the real window
  for (int i = 1; i < argc; i++) {
    if (wstring(argv[i]) != L"--hits") continue;
    static const char* names[] = {"none", "scene", "plan", "prev", "play", "next", "tile", "band", "reset", "beatmode", "mute", "master", "blend",
                                  "info", "keys", "chip", "chipx", "padd", "pupd", "tab", "link", "video", "watch", "player", "vprev", "vnext", "vclear",
                                  "filepick", "fprev", "fnext", "fclear", "linkgo", "sheetclose", "sheetback", "evlink", "toastact", "pbar", "side", "mini", "setup"};
    for (const Hit& h : r.ui->hits()) std::printf("%s %d %.0f %.0f\n", names[h.k], h.i, h.r.cx(), h.r.cy());
  }

  const UINT pw = (UINT)std::lround(w * scale), ph = (UINT)std::lround(h * scale);
  Com<IWICBitmap> bmp;
  r.g.wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, bmp.put());
  Com<ID2D1RenderTarget> rt;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
  if (FAILED(r.g.d2d->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put()))) { std::fprintf(stderr, "cannot create render target\n"); return 2; }
  rt->BeginDraw();
  r.ui->paintAll(rt.get(), 96 * scale);
  if (FAILED(rt->EndDraw())) { std::fprintf(stderr, "draw failed\n"); return 2; }
  if (!savePng(r.g, bmp.get(), out)) { std::fprintf(stderr, "cannot save %s\n", narrow(out).c_str()); return 2; }
  std::printf("wrote %s (%ux%u)\n", narrow(out).c_str(), pw, ph);
  return 0;
}
