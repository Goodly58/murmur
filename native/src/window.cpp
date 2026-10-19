#include "window.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <dxgiformat.h>

#include "install.h"
#include "jumplist.h"
#include "net.h"
#include "resolver.h"
#include "ui.h"

namespace {

constexpr UINT THUMB_PREV = 1001, THUMB_PLAY = 1002, THUMB_NEXT = 1003;
constexpr UINT WM_APP_STREAM = WM_APP + 1, WM_APP_POSTER = WM_APP + 2, WM_APP_FRESH = WM_APP + 3, WM_APP_SHOW = WM_APP + 4, WM_APP_EDITEND = WM_APP + 5, WM_APP_DEVLOST = WM_APP + 6;
enum : UINT_PTR { T_TICK = 1, T_SPEC, T_CLOSE, T_SPIN, T_TIP, T_SAVE, T_TOAST, T_SWEEP };
constexpr wchar_t CLASS_NAME[] = L"MurmurMainWindow";
constexpr float MIN_W = 940, MIN_H = 640;    // client size in DIPs
constexpr float MINI_W = 320, MINI_H = 484;  // the compact player's client size

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#define DWMWA_CAPTION_COLOR 35
#define DWMWA_TEXT_COLOR 36
#endif

string readFile(const wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  string out;
  char buf[8192];
  DWORD got = 0;
  while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
  CloseHandle(f);
  return out;
}

bool writeFileAtomic(const wstring& path, const string& data) {
  const wstring tmp = path + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  const bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size();
  CloseHandle(f);
  if (!ok) { DeleteFileW(tmp.c_str()); return false; }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool systemDark() {
  DWORD v = 1, sz = sizeof v;
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
  return v == 0;
}

COLORREF toCol(Color c) {
  return RGB((int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
}

Com<IWICBitmap> decodeImage(Gfx& g, const vector<uint8_t>& bytes) {
  Com<IWICBitmap> out;
  if (!g.wic || bytes.empty()) return out;
  Com<IWICStream> stream;
  if (FAILED(g.wic->CreateStream(stream.put())) || FAILED(stream->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size()))) return out;
  Com<IWICBitmapDecoder> dec;
  if (FAILED(g.wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put()))) return out;
  Com<IWICBitmapFrameDecode> fr;
  if (FAILED(dec->GetFrame(0, fr.put()))) return out;
  Com<IWICFormatConverter> conv;
  g.wic->CreateFormatConverter(conv.put());
  if (FAILED(conv->Initialize(fr.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return out;
  g.wic->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, out.put());
  return out;
}

struct PosterMsg {
  string id;
  vector<uint8_t> bytes;
  bool ok = false;
};

class MainWindow;
LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM);

class DropTarget : public IDropTarget {
 public:
  explicit DropTarget(MainWindow* w) : w_(w) {}
  virtual ~DropTarget() {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = (ULONG)InterlockedDecrement(&ref_);
    if (!r) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override;
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override { *eff = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
  HRESULT STDMETHODCALLTYPE DragLeave() override;
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) override;

 private:
  MainWindow* w_;
  LONG ref_ = 1;
  bool ok_ = false;
};

class MainWindow {
 public:
  HINSTANCE inst = nullptr;
  HWND hwnd = nullptr;
  Gfx gfx;
  Engine eng;
  Stream stream;
  Spectrum spec;
  std::unique_ptr<App> app;
  std::unique_ptr<Ui> ui;
  Com<ID2D1HwndRenderTarget> rt;
  float dpi = 96;
  bool dark = true, specAwake = false, minimized = false, tracking = false, softwareRT = false;
  string posterAsked;
  wstring statePath;
  int lastMinute = -1;
  wstring lastTitle;
  int lastTaskbar = -1;
  int64_t lastRefreshMs = 0;
  bool miniMode = false;
  HICON thumbIcons[4] = {};  // previous, play, pause, next
  int thumbKey = -1;

  // inline editor
  HWND edit = nullptr;
  HFONT editFont = nullptr;
  HBRUSH editBrush = nullptr;
  COLORREF editBrushColor = 0;
  WNDPROC oldEditProc = nullptr;
  std::function<void(bool, const wstring&)> editDone;
  bool editAccent = false;
  int editGen = 0;

  ITaskbarList3* taskbar = nullptr;
  UINT taskbarMsg = 0;
  DropTarget* drop = nullptr;

  bool init(HINSTANCE hi, const vector<wstring>& args, int showCmd);
  int run();
  LRESULT onMessage(UINT m, WPARAM w, LPARAM l);

  // pieces
  void createTarget();
  void onPaint();
  void onSize(int w, int h);
  void onDpi(UINT newDpi, const RECT* suggested);
  void onTimer(UINT_PTR id);
  void onAppChange();
  void onAppTick();
  void applyTheme();
  void rearmTick();
  void wakeSpectrum();
  void updateToastTimer();
  void updateChrome();
  void wantPoster();
  void saveState();
  void restorePlacement();
  void capturePlacement();
  void addThumbButtons();
  void updateThumbButtons();
  HICON makeThumbIcon(const char* name, bool solid);
  void freeThumbIcons();
  void setMini(bool on);
  void handleArgs(const vector<wstring>& args);
  void pasteClipboard();
  void pickFiles();
  void beginEdit(const R& r, const wstring& text, int maxLen, bool onAccent, std::function<void(bool, const wstring&)> done);
  void endEdit(bool ok);
  int popup(float x, float y, const vector<wstring>& items);
  void invalidateDip(const R& r);
  float zoom = 1;                           // the UI grows with the window once it is bigger than the layout needs
  float eff() const { return dpi * zoom; }  // pixels per 96 DIPs, zoom included
  float s() const { return eff() / 96.f; }
  void fit(int pxW, int pxH);               // recomputes the zoom and the UI's size from the client size
};

// ---------------------------------------------------------------------------------------------------
// Drop target
// ---------------------------------------------------------------------------------------------------

bool dataFiles(IDataObject* d, vector<wstring>& out) {
  FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM sm = {};
  if (FAILED(d->GetData(&f, &sm))) return false;
  HDROP h = (HDROP)GlobalLock(sm.hGlobal);
  if (h) {
    const UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; i++) {
      wchar_t path[MAX_PATH * 2];
      if (DragQueryFileW(h, i, path, MAX_PATH * 2)) out.push_back(path);
    }
    GlobalUnlock(sm.hGlobal);
  }
  ReleaseStgMedium(&sm);
  return !out.empty();
}

bool dataText(IDataObject* d, wstring& out) {
  static const UINT urlFmt = RegisterClipboardFormatW(L"UniformResourceLocatorW");
  for (UINT fmt : {urlFmt, (UINT)CF_UNICODETEXT}) {
    FORMATETC f = {(CLIPFORMAT)fmt, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm = {};
    if (FAILED(d->GetData(&f, &sm))) continue;
    const wchar_t* p = (const wchar_t*)GlobalLock(sm.hGlobal);
    if (p) out = p;
    GlobalUnlock(sm.hGlobal);
    ReleaseStgMedium(&sm);
    if (!out.empty()) return true;
  }
  return false;
}

HRESULT DropTarget::DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) {
  vector<wstring> files;
  wstring text;
  ok_ = dataFiles(d, files) || dataText(d, text);
  *eff = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  if (ok_) w_->ui->setDropping(true);
  return S_OK;
}

HRESULT DropTarget::DragLeave() {
  w_->ui->setDropping(false);
  return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) {
  w_->ui->setDropping(false);
  *eff = DROPEFFECT_COPY;
  vector<wstring> files;
  wstring text;
  if (dataFiles(d, files)) w_->app->addFiles(files);
  else if (dataText(d, text)) w_->app->pasteText(text);
  SetForegroundWindow(w_->hwnd);
  return S_OK;
}

// ---------------------------------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------------------------------

bool MainWindow::init(HINSTANCE hi, const vector<wstring>& args, int showCmd) {
  inst = hi;
  if (!gfx.init()) {
    MessageBoxW(nullptr, L"Direct2D could not start on this PC.", L"Murmur", MB_ICONERROR);
    return false;
  }
  softwareRT = _wgetenv(L"MURMUR_SOFTWARE") != nullptr;
  statePath = appDataDir() + L"\\state.json";

  app = std::make_unique<App>(eng, stream);
  app->model.load(readFile(statePath));
  stream.configure();
  eng.onOutputLost = [this] { if (hwnd) PostMessageW(hwnd, WM_APP_DEVLOST, 0, 0); };  // from the audio thread
  if (!eng.start(true)) app->showToast(L"No audio device found", L"", nullptr, 8000);
  taskbarMsg = RegisterWindowMessageW(L"TaskbarButtonCreated");

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof wc;
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
  wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = CLASS_NAME;
  RegisterClassExW(&wc);

  hwnd = CreateWindowExW(0, CLASS_NAME, L"Murmur", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, nullptr, nullptr, inst, this);
  if (!hwnd) return false;

  dark = systemDark();
  ui = std::make_unique<Ui>(*app, gfx, spec);
  ui->hooks.invalidateAll = [this] { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); };
  ui->hooks.invalidate = [this](const R& r) { invalidateDip(r); };
  ui->hooks.edit = [this](const R& r, const wstring& t, int n, bool acc, std::function<void(bool, const wstring&)> done) { beginEdit(r, t, n, acc, std::move(done)); };
  ui->hooks.openUrl = [](const wstring& u) { ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
  ui->hooks.pickFiles = [this] { pickFiles(); };
  ui->hooks.capture = [this](bool on) { if (on) SetCapture(hwnd); else if (GetCapture() == hwnd) ReleaseCapture(); };
  ui->hooks.toggleMini = [this] { setMini(!miniMode); };
  ui->hooks.menu = [this](float x, float y, const vector<wstring>& items) { return popup(x, y, items); };

  app->onChange = [this] { onAppChange(); };
  app->onTick = [this] { onAppTick(); };
  app->onSave = [this] { SetTimer(hwnd, T_SAVE, 300, nullptr); };
  app->onWake = [this] { wakeSpectrum(); };
  app->onNeedFresh = [this] {
    const int64_t now = wallMs();
    if (now - lastRefreshMs < 3600 * 1000) return;  // at most once an hour
    lastRefreshMs = now;
    startResolver(hwnd, WM_APP_FRESH, stream.ytdlpPath(), true);
  };
  stream.setSink([hw = hwnd](const StreamEvent& e) { PostMessageW(hw, WM_APP_STREAM, 0, (LPARAM) new StreamEvent(e)); });

  dpi = (float)GetDpiForWindow(hwnd);
  applyTheme();
  restorePlacement();
  app->init();
  if (app->model.s.mini) {
    app->model.s.mini = false;  // setMini(true) sets it again once the window is ready
    setMini(true);
  }

  if (SUCCEEDED(OleInitialize(nullptr))) {
    drop = new DropTarget(this);
    RegisterDragDrop(hwnd, drop);
  }
  handleArgs(args);
  startResolver(hwnd, WM_APP_FRESH, stream.ytdlpPath());
  {
    wchar_t self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    if (_wcsicmp(wstring(self, n).c_str(), installedExePath().c_str()) == 0) updateJumpList(installedExePath());
  }

  ShowWindow(hwnd, app->model.s.maximized ? SW_SHOWMAXIMIZED : (showCmd == SW_SHOWMINNOACTIVE ? SW_SHOWMINNOACTIVE : SW_SHOW));
  UpdateWindow(hwnd);
  SetTimer(hwnd, T_SWEEP, 30000, nullptr);
  return true;
}

int MainWindow::run() {
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return (int)msg.wParam;
}

void MainWindow::restorePlacement() {
  State& st = app->model.s;
  RECT work = {};
  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {};
  mi.cbSize = sizeof mi;
  bool placed = false;
  if (st.winX != INT32_MIN && st.winW > 0 && st.winH > 0) {
    const POINT pt = {st.winX + 60, st.winY + 20};
    HMONITOR saved = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (saved) { mon = saved; placed = true; }
  }
  GetMonitorInfoW(mon, &mi);
  work = mi.rcWork;
  UINT dx = 96, dy = 96;
  typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
  if (HMODULE sh = LoadLibraryW(L"shcore.dll")) {
    if (auto fn = (GetDpiForMonitorFn)(void*)GetProcAddress(sh, "GetDpiForMonitor")) fn(mon, 0, &dx, &dy);
    FreeLibrary(sh);
  }
  const float sc = dx / 96.f;
  int w = placed ? (int)std::lround(st.winW * sc) : (int)std::lround(1200 * sc);
  int h = placed ? (int)std::lround(st.winH * sc) : (int)std::lround(820 * sc);
  w = std::min<int>(w, (int)(work.right - work.left));
  h = std::min<int>(h, (int)(work.bottom - work.top));
  int x = placed ? st.winX : work.left + ((work.right - work.left) - w) / 2;
  int y = placed ? st.winY : work.top + ((work.bottom - work.top) - h) / 2;
  x = std::max<int>(work.left - 20, std::min<int>(x, (int)work.right - 120));
  y = std::max<int>(work.top, std::min<int>(y, (int)work.bottom - 80));
  SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  dpi = (float)GetDpiForWindow(hwnd);
}

// Where the window is, in the model: the full window's rectangle, or the compact player's corner.
void MainWindow::capturePlacement() {
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof wp;
  if (!GetWindowPlacement(hwnd, &wp)) return;
  State& st = app->model.s;
  const RECT& rc = wp.rcNormalPosition;
  if (miniMode) {
    st.miniX = rc.left;
    st.miniY = rc.top;
    return;
  }
  st.winX = rc.left;
  st.winY = rc.top;
  st.winW = (int)std::lround((rc.right - rc.left) * 96.0 / dpi);
  st.winH = (int)std::lround((rc.bottom - rc.top) * 96.0 / dpi);
  st.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
}

void MainWindow::saveState() {
  KillTimer(hwnd, T_SAVE);
  capturePlacement();
  writeFileAtomic(statePath, app->model.serialize());
}

// The compact player: a small fixed-size window that stays on top of the others, so the clock is always in
// sight while you work. Leaving it puts the full window back where it was.
void MainWindow::setMini(bool on) {
  if (on == miniMode || !hwnd) return;
  if (edit) endEdit(false);
  State& st = app->model.s;
  capturePlacement();  // the window we are leaving: its rectangle (full) or its corner (mini)
  if (on) {
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    miniMode = true;
    st.mini = true;
    ui->setMini(true);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(LONG_PTR)(WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    RECT rc = {0, 0, (LONG)std::lround(MINI_W * dpi / 96.f), (LONG)std::lround(MINI_H * dpi / 96.f)};
    AdjustWindowRectExForDpi(&rc, (DWORD)style, FALSE, 0, (UINT)dpi);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    // the lower right corner of the screen the window is on, unless it was left somewhere else last time
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const int margin = (int)std::lround(24 * dpi / 96.f);
    int x = (int)mi.rcWork.right - w - margin, y = (int)mi.rcWork.bottom - h - margin;
    if (st.miniX != INT32_MIN) {
      const POINT pt = {st.miniX + 40, st.miniY + 20};
      if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) { x = st.miniX; y = st.miniY; }
    }
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_NOACTIVATE);
  } else {
    miniMode = false;
    st.mini = false;
    ui->setMini(false);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    restorePlacement();
    if (st.maximized) ShowWindow(hwnd, SW_SHOWMAXIMIZED);
  }
  SetTimer(hwnd, T_SAVE, 300, nullptr);
  InvalidateRect(hwnd, nullptr, FALSE);
}

void MainWindow::handleArgs(const vector<wstring>& args) {
  Launch l = parseLaunch(args);
  mlog("launch: %zu args, scene=%s plan=%s play=%d mini=%d full=%d links=%zu files=%zu", args.size(), l.scene.c_str(), l.plan.c_str(), (int)l.play, (int)l.mini, (int)l.full, l.links.size(), l.files.size());
  vector<wstring> files;
  for (const wstring& f : l.files) if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) files.push_back(f);
  l.files = files;
  if (l.mini) setMini(true);
  else if (l.full) setMini(false);
  app->applyLaunch(l);
}

// ---------------------------------------------------------------------------------------------------
// Theme, painting and sizing
// ---------------------------------------------------------------------------------------------------

void MainWindow::applyTheme() {
  ui->setTheme(dark);
  const Palette& P = gfx.pal;
  const BOOL d = dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof d);
  const COLORREF cap = toCol(P.bg), border = toCol(P.line), txt = toCol(P.text2);
  DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof cap);
  DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof border);
  DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &txt, sizeof txt);
  InvalidateRect(hwnd, nullptr, FALSE);
}

void MainWindow::createTarget() {
  if (rt) return;
  RECT rc;
  GetClientRect(hwnd, &rc);
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      softwareRT ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE));
  const D2D1_HWND_RENDER_TARGET_PROPERTIES hp =
      D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU((UINT)(rc.right - rc.left), (UINT)(rc.bottom - rc.top)), D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS);
  if (FAILED(gfx.d2d->CreateHwndRenderTarget(props, hp, rt.put()))) rt.reset();
}

void MainWindow::onPaint() {
  PAINTSTRUCT ps;
  BeginPaint(hwnd, &ps);
  const bool had = (bool)rt;
  createTarget();
  if (rt) {
    const float k = 96.f / eff();
    RECT rc;
    GetClientRect(hwnd, &rc);
    R dirty = {ps.rcPaint.left * k, ps.rcPaint.top * k, (ps.rcPaint.right - ps.rcPaint.left) * k, (ps.rcPaint.bottom - ps.rcPaint.top) * k};
    if (!had) dirty = {0, 0, (rc.right - rc.left) * k, (rc.bottom - rc.top) * k};  // a fresh target starts empty
    rt->BeginDraw();
    ui->paint(rt.get(), eff(), dirty);
    const HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
      rt.reset();
      InvalidateRect(hwnd, nullptr, FALSE);
    }
  }
  EndPaint(hwnd, &ps);
}

void MainWindow::onSize(int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (rt) rt->Resize(D2D1::SizeU((UINT)w, (UINT)h));
  fit(w, h);
  InvalidateRect(hwnd, nullptr, FALSE);
}

void MainWindow::fit(int w, int h) {
  const float wd = w * 96.f / dpi, hd = h * 96.f / dpi;
  const double z = miniMode ? 1.0 : std::min(wd / 1280.0, hd / 820.0);
  zoom = (float)(std::round(clampd(z, 1.0, 1.75) * 20.0) / 20.0);  // steps of 5%
  ui->setSize(w * 96.f / eff(), h * 96.f / eff());
}

void MainWindow::onDpi(UINT newDpi, const RECT* r) {
  dpi = (float)newDpi;
  freeThumbIcons();
  thumbKey = -1;
  if (r) SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
  RECT rc;
  GetClientRect(hwnd, &rc);
  fit(rc.right - rc.left, rc.bottom - rc.top);
  InvalidateRect(hwnd, nullptr, FALSE);
}

void MainWindow::invalidateDip(const R& r) {
  if (!hwnd || r.w <= 0 || r.h <= 0) return;
  const float k = s();
  RECT rc = {(LONG)std::floor(r.x * k) - 1, (LONG)std::floor(r.y * k) - 1, (LONG)std::ceil(r.r() * k) + 1, (LONG)std::ceil(r.b() * k) + 1};
  InvalidateRect(hwnd, &rc, FALSE);
}

// ---------------------------------------------------------------------------------------------------
// Timers and app events
// ---------------------------------------------------------------------------------------------------

void MainWindow::rearmTick() {
  if (app->running && app->session.running() && !app->session.done()) {
    SetTimer(hwnd, T_TICK, (UINT)clampd((double)app->msUntilTick(), 15, 1000), nullptr);
  } else {
    KillTimer(hwnd, T_TICK);
  }
}

void MainWindow::wakeSpectrum() {
  spec.idle = 0;
  if (!specAwake && !minimized) {
    specAwake = true;
    SetTimer(hwnd, T_SPEC, 66, nullptr);
  }
}

void MainWindow::updateToastTimer() {
  if (app->toast.active()) {
    const int64_t left = app->toast.until - wallMs();
    SetTimer(hwnd, T_TOAST, (UINT)std::max<int64_t>(50, left), nullptr);
  } else {
    KillTimer(hwnd, T_TOAST);
  }
}

void MainWindow::onAppChange() {
  ui->changed();
  InvalidateRect(hwnd, nullptr, FALSE);
  rearmTick();
  if (app->closing) SetTimer(hwnd, T_CLOSE, 120, nullptr);
  updateToastTimer();
  if (ui->wantsSpinner() && !minimized) SetTimer(hwnd, T_SPIN, 33, nullptr);
  else KillTimer(hwnd, T_SPIN);
  wantPoster();
  updateChrome();
}

void MainWindow::onAppTick() {
  ui->changed();
  if (!minimized) {
    invalidateDip(ui->ringRect().inset(-10));
    const int minute = (int)std::floor(app->model.todayMinutes());
    if (minute != lastMinute) {
      lastMinute = minute;
      invalidateDip(ui->statsRect());
    }
  }
  updateChrome();  // the title and taskbar progress show the clock while minimized
}

// ---------------------------------------------------------------------------------------------------
// Taskbar preview buttons: previous / play-pause / next under the thumbnail when you hover the taskbar icon.
// The glyphs are our own icons, drawn once into small bitmaps.
// ---------------------------------------------------------------------------------------------------

HICON MainWindow::makeThumbIcon(const char* name, bool solid) {
  const int n = std::max(16, GetSystemMetricsForDpi(SM_CXSMICON, (UINT)dpi));
  Com<IWICBitmap> bmp;
  if (!gfx.wic || FAILED(gfx.wic->CreateBitmap((UINT)n, (UINT)n, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, bmp.put()))) return nullptr;
  Com<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props =
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  if (FAILED(gfx.d2d->CreateWicBitmapRenderTarget(bmp.get(), props, target.put()))) return nullptr;
  DWORD light = 0;
  DWORD sz = sizeof light;
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &sz);
  gfx.bind(target.get(), 96);
  target->BeginDraw();
  target->Clear(D2D1::ColorF(0, 0, 0, 0));
  gfx.icon(name, n / 2.f, n / 2.f, (float)n, light ? rgb(0x202124) : rgb(0xf3f4f6), solid, 2.0f);
  const bool ok = SUCCEEDED(target->EndDraw());
  if (!ok) return nullptr;

  vector<uint32_t> px((size_t)n * n);
  if (FAILED(bmp->CopyPixels(nullptr, (UINT)n * 4, (UINT)px.size() * 4, (BYTE*)px.data()))) return nullptr;
  for (uint32_t& v : px) {  // premultiplied -> straight alpha, which icons expect
    const uint32_t a = v >> 24;
    if (a && a < 255) {
      const uint32_t r = std::min<uint32_t>(255, ((v >> 16) & 255) * 255 / a), g2 = std::min<uint32_t>(255, ((v >> 8) & 255) * 255 / a), b = std::min<uint32_t>(255, (v & 255) * 255 / a);
      v = (a << 24) | (r << 16) | (g2 << 8) | b;
    }
  }
  BITMAPV5HEADER bi = {};
  bi.bV5Size = sizeof bi;
  bi.bV5Width = n;
  bi.bV5Height = -n;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!color || !bits) return nullptr;
  std::memcpy(bits, px.data(), px.size() * 4);
  HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
  ICONINFO ii = {};
  ii.fIcon = TRUE;
  ii.hbmMask = mask;
  ii.hbmColor = color;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color);
  DeleteObject(mask);
  return icon;
}

void MainWindow::freeThumbIcons() {
  for (HICON& h : thumbIcons) {
    if (h) DestroyIcon(h);
    h = nullptr;
  }
}

void MainWindow::addThumbButtons() {
  if (!taskbar || !hwnd) return;
  if (!thumbIcons[0]) {
    thumbIcons[0] = makeThumbIcon("i-prev", false);
    thumbIcons[1] = makeThumbIcon("i-play", true);
    thumbIcons[2] = makeThumbIcon("i-pause", true);
    thumbIcons[3] = makeThumbIcon("i-next", false);
  }
  THUMBBUTTON b[3] = {};
  const UINT ids[3] = {THUMB_PREV, THUMB_PLAY, THUMB_NEXT};
  const wchar_t* tips[3] = {L"Restart phase", L"Play", L"Skip phase"};
  for (int i = 0; i < 3; i++) {
    b[i].dwMask = (THUMBBUTTONMASK)(THB_ICON | THB_TOOLTIP | THB_FLAGS);
    b[i].iId = ids[i];
    b[i].hIcon = thumbIcons[i == 0 ? 0 : i == 1 ? 1 : 3];
    wcsncpy(b[i].szTip, tips[i], 259);
    b[i].dwFlags = THBF_ENABLED;
  }
  const HRESULT hr = taskbar->ThumbBarAddButtons(hwnd, 3, b);
  if (FAILED(hr)) mlog("taskbar preview buttons failed: 0x%08lx", (unsigned long)hr);
  thumbKey = -1;
}

void MainWindow::updateThumbButtons() {
  if (!taskbar || !thumbIcons[0]) return;
  const SessionSnap s = app->session.snapshot();
  const bool next = s.cycle && !s.done;
  const int key = (app->running ? 1 : 0) | (next ? 2 : 0);
  if (key == thumbKey) return;
  thumbKey = key;
  THUMBBUTTON b[3] = {};
  const UINT ids[3] = {THUMB_PREV, THUMB_PLAY, THUMB_NEXT};
  const wchar_t* tips[3] = {L"Restart phase", app->running ? L"Pause" : L"Play", L"Skip phase"};
  for (int i = 0; i < 3; i++) {
    b[i].dwMask = (THUMBBUTTONMASK)(THB_ICON | THB_TOOLTIP | THB_FLAGS);
    b[i].iId = ids[i];
    b[i].hIcon = i == 0 ? thumbIcons[0] : i == 1 ? thumbIcons[app->running ? 2 : 1] : thumbIcons[3];
    wcsncpy(b[i].szTip, tips[i], 259);
    b[i].dwFlags = (i == 2 && !next) ? THBF_DISABLED : THBF_ENABLED;
  }
  taskbar->ThumbBarUpdateButtons(hwnd, 3, b);
}

void MainWindow::updateChrome() {
  const SessionSnap s = app->session.snapshot();
  wstring title = L"Murmur";
  if (app->running && !s.done) {
    const bool free = s.kind == PH_FREE;
    const wstring label = s.kind == PH_BREAK ? wstring(L"Break") : app->model.scene().name;
    title = fmtClock(free ? s.elapsed : s.remaining, free) + L" · " + label + L" — Murmur";
  }
  if (title != lastTitle) {
    lastTitle = title;
    SetWindowTextW(hwnd, title.c_str());
  }
  updateThumbButtons();
  if (taskbar) {
    int state = TBPF_NOPROGRESS;
    ULONGLONG done = 0;
    if (app->running && !s.done && s.duration < INF_MS) { state = TBPF_NORMAL; done = (ULONGLONG)std::lround(clampd(s.progress, 0, 1) * 1000); }
    else if (!app->running && s.duration < INF_MS && s.elapsed > 0 && !s.done) { state = TBPF_PAUSED; done = (ULONGLONG)std::lround(clampd(s.progress, 0, 1) * 1000); }
    const int key = state * 10000 + (int)done;
    if (key != lastTaskbar) {
      lastTaskbar = key;
      taskbar->SetProgressState(hwnd, (TBPFLAG)state);
      if (state != TBPF_NOPROGRESS) taskbar->SetProgressValue(hwnd, done, 1000);
    }
  }
}

void MainWindow::onTimer(UINT_PTR id) {
  switch (id) {
    case T_TICK:
      app->tick();
      rearmTick();
      break;
    case T_SPEC: {
      bool asleep = false;
      const bool live = app->running && eng.playing();
      if (spec.frame(eng, live, &asleep)) invalidateDip(ui->specRect());
      if (asleep) { KillTimer(hwnd, T_SPEC); specAwake = false; }
      break;
    }
    case T_CLOSE:
      if (app->closeTick(wallMs())) KillTimer(hwnd, T_CLOSE);
      break;
    case T_SPIN:
      ui->spinnerAngle = std::fmod(ui->spinnerAngle + 14.f, 360.f);
      invalidateDip(ui->playerRect());
      if (!ui->wantsSpinner()) KillTimer(hwnd, T_SPIN);
      break;
    case T_TIP:
      KillTimer(hwnd, T_TIP);
      ui->tipTimer();
      break;
    case T_SAVE: saveState(); break;
    case T_TOAST:
      KillTimer(hwnd, T_TOAST);
      app->expireToast(wallMs());
      break;
    case T_SWEEP:
      eng.sweep();
      break;
    default: break;
  }
}

void MainWindow::wantPoster() {
  const string id = ui->posterIdFor();
  if (id == posterAsked) return;
  posterAsked = id;
  if (id.empty() || ui->hasPoster(id)) return;
  const wstring dir = appDataDir() + L"\\posters";
  const wstring path = dir + L"\\" + widen(id) + L".jpg";
  const string cached = readFile(path);
  if (!cached.empty()) {
    Com<IWICBitmap> bmp = decodeImage(gfx, vector<uint8_t>(cached.begin(), cached.end()));
    if (bmp) { ui->setPoster(id, bmp, false); return; }
  }
  std::thread([hw = hwnd, id, dir, path] {
    auto* m = new PosterMsg;
    m->id = id;
    m->ok = httpsGet(L"i.ytimg.com", L"/vi/" + widen(id) + L"/mqdefault.jpg", m->bytes, 6000);
    if (m->ok) {
      CreateDirectoryW(dir.c_str(), nullptr);
      writeFileAtomic(path, string(m->bytes.begin(), m->bytes.end()));
    }
    if (!PostMessageW(hw, WM_APP_POSTER, 0, (LPARAM)m)) delete m;
  }).detach();
}

// ---------------------------------------------------------------------------------------------------
// Clipboard, dialogs, menus and the inline editor
// ---------------------------------------------------------------------------------------------------

void MainWindow::pasteClipboard() {
  if (!OpenClipboard(hwnd)) return;
  wstring text;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) { text = p; GlobalUnlock(h); }
  }
  CloseClipboard();
  if (!text.empty()) app->pasteText(text);
}

void MainWindow::pickFiles() {
  static wchar_t buf[32768];
  buf[0] = 0;
  OPENFILENAMEW o = {};
  o.lStructSize = sizeof o;
  o.hwndOwner = hwnd;
  o.lpstrFilter = L"Audio\0*.mp3;*.m4a;*.aac;*.wav;*.flac;*.ogg;*.opus;*.wma;*.mka\0All files\0*.*\0";
  o.lpstrFile = buf;
  o.nMaxFile = 32768;
  o.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&o)) return;
  vector<wstring> files;
  const wchar_t* p = buf;
  const wstring first = p;
  p += first.size() + 1;
  if (!*p) files.push_back(first);  // a single file: the buffer holds its full path
  else {
    while (*p) {
      const wstring name = p;
      files.push_back(first + L"\\" + name);
      p += name.size() + 1;
    }
  }
  app->addFiles(files);
}

int MainWindow::popup(float x, float y, const vector<wstring>& items) {
  HMENU m = CreatePopupMenu();
  for (size_t i = 0; i < items.size(); i++) AppendMenuW(m, MF_STRING, i + 1, items[i].c_str());
  POINT pt = {(LONG)std::lround(x * s()), (LONG)std::lround(y * s())};
  ClientToScreen(hwnd, &pt);
  const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(m);
  return cmd - 1;
}

void MainWindow::beginEdit(const R& r, const wstring& text, int maxLen, bool onAccent, std::function<void(bool, const wstring&)> done) {
  if (edit) endEdit(true);
  const int fontPx = (int)std::lround(12.5f * s());
  editFont = CreateFontW(-fontPx, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, gfx.family.c_str());
  const int h = (int)std::lround(fontPx * 1.35f);
  const int x = (int)std::lround(r.x * s()), w = (int)std::lround(r.w * s());
  const int y = (int)std::lround(r.cy() * s() - h / 2.0);
  editGen++;
  editAccent = onAccent;
  editDone = std::move(done);
  edit = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, x, y, w, h, hwnd, (HMENU)(UINT_PTR)1001, inst, nullptr);
  if (!edit) { editDone = nullptr; return; }
  SendMessageW(edit, WM_SETFONT, (WPARAM)editFont, TRUE);
  SendMessageW(edit, EM_SETLIMITTEXT, (WPARAM)maxLen, 0);
  SetWindowLongPtrW(edit, GWLP_USERDATA, (LONG_PTR)this);
  oldEditProc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)editProc);
  SetFocus(edit);
  SendMessageW(edit, EM_SETSEL, 0, -1);
}

void MainWindow::endEdit(bool ok) {
  if (!edit) return;
  wchar_t buf[512] = {0};
  GetWindowTextW(edit, buf, 512);
  const wstring text = buf;
  HWND e = edit;
  edit = nullptr;
  auto done = std::move(editDone);
  editDone = nullptr;
  DestroyWindow(e);
  if (editFont) { DeleteObject(editFont); editFont = nullptr; }
  if (GetFocus() != hwnd) SetFocus(hwnd);
  if (done) done(ok, text);
  InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  auto* self = (MainWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
  switch (m) {
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
      if (w == VK_RETURN) { PostMessageW(self->hwnd, WM_APP_EDITEND, 1, self->editGen); return 0; }
      if (w == VK_ESCAPE) { PostMessageW(self->hwnd, WM_APP_EDITEND, 0, self->editGen); return 0; }
      if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
      break;
    case WM_CHAR:
      if (w == VK_RETURN || w == VK_ESCAPE) return 0;
      break;
    case WM_KILLFOCUS: PostMessageW(self->hwnd, WM_APP_EDITEND, 1, self->editGen); break;
    default: break;
  }
  return CallWindowProcW(self->oldEditProc, h, m, w, l);
}

// ---------------------------------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------------------------------

LRESULT MainWindow::onMessage(UINT m, WPARAM w, LPARAM l) {
  if (m == taskbarMsg && taskbarMsg) {
    if (!taskbar && SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, (void**)&taskbar))) taskbar->HrInit();
    lastTaskbar = -1;
    thumbKey = -1;
    addThumbButtons();
    updateChrome();
    return 0;
  }
  const float k = 96.f / eff();
  switch (m) {
    case WM_PAINT: onPaint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DISPLAYCHANGE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_SIZE:
      minimized = w == SIZE_MINIMIZED;
      if (minimized) { KillTimer(hwnd, T_SPEC); KillTimer(hwnd, T_SPIN); specAwake = false; }
      else {
        onSize(LOWORD(l), HIWORD(l));
        if (app->running) wakeSpectrum();
        if (ui->wantsSpinner()) SetTimer(hwnd, T_SPIN, 33, nullptr);
      }
      return 0;
    case WM_EXITSIZEMOVE: SetTimer(hwnd, T_SAVE, 300, nullptr); return 0;
    case WM_DPICHANGED: onDpi(HIWORD(w), (const RECT*)l); return 0;
    case WM_GETMINMAXINFO: {
      auto* mi = (MINMAXINFO*)l;
      if (miniMode) {
        RECT mr = {0, 0, (LONG)std::lround(MINI_W * dpi / 96.f), (LONG)std::lround(MINI_H * dpi / 96.f)};
        AdjustWindowRectExForDpi(&mr, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE, 0, (UINT)dpi);
        mi->ptMinTrackSize.x = mi->ptMaxTrackSize.x = mi->ptMaxSize.x = mr.right - mr.left;
        mi->ptMinTrackSize.y = mi->ptMaxTrackSize.y = mi->ptMaxSize.y = mr.bottom - mr.top;
        return 0;
      }
      RECT rc = {0, 0, (LONG)std::lround(MIN_W * dpi / 96.f), (LONG)std::lround(MIN_H * dpi / 96.f)};
      AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)dpi);
      mi->ptMinTrackSize.x = rc.right - rc.left;
      mi->ptMinTrackSize.y = rc.bottom - rc.top;
      return 0;
    }
    case WM_SETTINGCHANGE:
      if (l && wcscmp((const wchar_t*)l, L"ImmersiveColorSet") == 0) {
        const bool d = systemDark();
        if (d != dark) { dark = d; applyTheme(); }
      }
      return 0;
    case WM_TIMER: onTimer((UINT_PTR)w); return 0;
    case WM_MOUSEMOVE: {
      if (!tracking) {
        TRACKMOUSEEVENT te = {sizeof te, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&te);
        tracking = true;
      }
      ui->mouseMove(GET_X_LPARAM(l) * k, GET_Y_LPARAM(l) * k, (w & MK_LBUTTON) != 0);
      if (ui->tipPending()) SetTimer(hwnd, T_TIP, 450, nullptr);
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking = false;
      KillTimer(hwnd, T_TIP);
      ui->mouseLeave();
      return 0;
    case WM_LBUTTONDOWN:
      if (GetFocus() != hwnd) SetFocus(hwnd);
      ui->mouseDown(GET_X_LPARAM(l) * k, GET_Y_LPARAM(l) * k);
      return 0;
    case WM_LBUTTONUP: ui->mouseUp(GET_X_LPARAM(l) * k, GET_Y_LPARAM(l) * k); return 0;
    case WM_LBUTTONDBLCLK: ui->doubleClick(GET_X_LPARAM(l) * k, GET_Y_LPARAM(l) * k); return 0;
    case WM_RBUTTONUP: ui->rightClick(GET_X_LPARAM(l) * k, GET_Y_LPARAM(l) * k); return 0;
    case WM_CAPTURECHANGED: ui->mouseUp(-1000, -1000); return 0;
    case WM_MOUSEWHEEL: {
      POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      ScreenToClient(hwnd, &pt);
      ui->wheel(pt.x * k, pt.y * k, GET_WHEEL_DELTA_WPARAM(w));
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        const int c = ui->cursorAt(pt.x * k, pt.y * k);
        SetCursor(LoadCursorW(nullptr, c == 1 ? IDC_HAND : c == 2 ? IDC_SIZENS : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_KEYDOWN: {
      const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, repeat = (l & (1 << 30)) != 0;
      if (ctrl && w == 'V') { pasteClipboard(); return 0; }
      if (ctrl && w == 'O') { pickFiles(); return 0; }
      if (repeat && (w == VK_SPACE || w == 'M' || w == 'B' || w == 'V' || w == VK_LEFT || w == VK_RIGHT)) return 0;
      if (ui->key((int)w, ctrl, shift)) return 0;
      break;
    }
    case WM_APPCOMMAND:
      switch (GET_APPCOMMAND_LPARAM(l)) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: app->togglePlay(); return TRUE;
        case APPCOMMAND_MEDIA_PLAY: if (!app->running) app->startAll(); return TRUE;
        case APPCOMMAND_MEDIA_PAUSE: case APPCOMMAND_MEDIA_STOP: if (app->running) app->stopAll(); return TRUE;
        case APPCOMMAND_MEDIA_NEXTTRACK: app->nextPhase(); return TRUE;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: app->prevPhase(); return TRUE;
        default: break;
      }
      break;
    case WM_CTLCOLOREDIT: {
      HDC dc = (HDC)w;
      const Palette& P = gfx.pal;
      const COLORREF bg = toCol(editAccent ? mixColor(P.panel, withAlpha(P.accent, 1), P.accentDim.a) : P.panel);
      SetTextColor(dc, toCol(P.text));
      SetBkColor(dc, bg);
      if (!editBrush || editBrushColor != bg) {
        if (editBrush) DeleteObject(editBrush);
        editBrush = CreateSolidBrush(bg);
        editBrushColor = bg;
      }
      return (LRESULT)editBrush;
    }
    case WM_COMMAND:
      if (HIWORD(w) == THBN_CLICKED) {
        switch (LOWORD(w)) {
          case THUMB_PREV: app->prevPhase(); break;
          case THUMB_PLAY: app->togglePlay(); break;
          case THUMB_NEXT: { const SessionSnap s = app->session.snapshot(); if (s.cycle && !s.done) app->nextPhase(); break; }
          default: break;
        }
      }
      return 0;
    case WM_APP_DEVLOST:
      app->outputLost();
      return 0;
    case WM_APP_EDITEND:
      if ((int)l == editGen) endEdit(w != 0);
      return 0;
    case WM_APP_STREAM: {
      std::unique_ptr<StreamEvent> e((StreamEvent*)l);
      app->onStreamEvent(*e);
      return 0;
    }
    case WM_APP_POSTER: {
      std::unique_ptr<PosterMsg> p((PosterMsg*)l);
      Com<IWICBitmap> bmp = p->ok ? decodeImage(gfx, p->bytes) : Com<IWICBitmap>();
      ui->setPoster(p->id, bmp, !bmp);
      return 0;
    }
    case WM_APP_FRESH: {
      std::unique_ptr<std::map<string, vector<string>>> f((std::map<string, vector<string>>*)l);
      app->fresh = *f;
      onAppChange();
      return 0;
    }
    case WM_APP_SHOW:
      if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
      SetForegroundWindow(hwnd);
      return 0;
    case WM_COPYDATA: {
      auto* cd = (COPYDATASTRUCT*)l;
      if (cd && cd->dwData == 1 && cd->lpData) {
        wstring all((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        while (!all.empty() && all.back() == L'\0') all.pop_back();  // the sender counts the terminator
        vector<wstring> args;
        size_t a = 0;
        for (;;) {
          const size_t b = all.find(L'\n', a);
          args.push_back(all.substr(a, b == wstring::npos ? wstring::npos : b - a));
          if (b == wstring::npos) break;
          a = b + 1;
        }
        handleArgs(args);
        SendMessageW(hwnd, WM_APP_SHOW, 0, 0);
      }
      return TRUE;
    }
    case WM_CLOSE:
      if (edit) endEdit(false);
      saveState();
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (drop) { RevokeDragDrop(hwnd); drop->Release(); drop = nullptr; OleUninitialize(); }
      for (UINT_PTR id : {T_TICK, T_SPEC, T_CLOSE, T_SPIN, T_TIP, T_SAVE, T_TOAST, T_SWEEP}) KillTimer(hwnd, id);
      app->stopAll();
      stream.shutdown();
      eng.shutdown();
      freeThumbIcons();
      if (taskbar) { taskbar->Release(); taskbar = nullptr; }
      if (editBrush) { DeleteObject(editBrush); editBrush = nullptr; }
      hwnd = nullptr;
      PostQuitMessage(0);
      return 0;
    default: break;
  }
  return DefWindowProcW(hwnd, m, w, l);
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  MainWindow* self;
  if (m == WM_NCCREATE) {
    self = (MainWindow*)((CREATESTRUCTW*)l)->lpCreateParams;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    self->hwnd = h;
  } else {
    self = (MainWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
  }
  if (!self || !self->ui) {
    // before the UI exists only the default handling is safe
    return DefWindowProcW(h, m, w, l);
  }
  return self->onMessage(m, w, l);
}

}  // namespace

int runApp(HINSTANCE inst, const vector<wstring>& args, int showCmd) {
  SetCurrentProcessExplicitAppUserModelID(L"Murmur.App");

  // one window: a second launch hands its arguments to the first and leaves
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Murmur.SingleInstance");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(CLASS_NAME, nullptr)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(other, &pid);
      AllowSetForegroundWindow(pid);
      wstring joined;
      for (size_t i = 0; i < args.size(); i++) joined += (i ? L"\n" : L"") + args[i];
      if (!joined.empty()) {
        COPYDATASTRUCT cd = {1, (DWORD)((joined.size() + 1) * sizeof(wchar_t)), (PVOID)joined.c_str()};
        SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, nullptr);
      } else {
        SendMessageTimeoutW(other, WM_APP_SHOW, 0, 0, SMTO_ABORTIFHUNG, 3000, nullptr);
      }
      return 0;
    }
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  int code = 1;
  {
    MainWindow w;
    if (w.init(inst, args, showCmd)) code = w.run();
  }
  CoUninitialize();
  if (mutex) CloseHandle(mutex);
  return code;
}
