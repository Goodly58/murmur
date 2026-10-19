// `murmur --make-icon out.ico`: the app icon, drawn with the same code as the UI, as PNG frames in an .ico.
#include "rig.h"

int runMakeIcon(const wstring& outPath) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Gfx g;
  if (!g.init()) { std::fprintf(stderr, "Direct2D is not available\n"); return 2; }
  const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
  vector<vector<uint8_t>> pngs;
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const wstring tmpFile = wstring(tmp) + L"murmur_icon_frame.png";
  for (int n : sizes) {
    Com<IWICBitmap> bmp;
    g.wic->CreateBitmap((UINT)n, (UINT)n, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, bmp.put());
    Com<ID2D1RenderTarget> rt;
    const D2D1_RENDER_TARGET_PROPERTIES props =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(g.d2d->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put()))) return 2;
    g.bind(rt.get(), 96);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    const float s = (float)n, rad = s * 112.f / 512.f;
    g.fillRound({0, 0, s, s}, rad, rgb(0x0c0e11));
    g.strokeRound({0, 0, s, s}, rad, rgb(0xffffff, .07f), std::max(1.f, s / 128.f));
    g.icon("logo", s / 2, s / 2, 24.f * 17.2f / 512.f * s, rgb(0xf2b563), false, n <= 32 ? 2.2f : 1.5f);
    rt->EndDraw();
    if (!savePng(g, bmp.get(), tmpFile)) return 2;
    HANDLE f = CreateFileW(tmpFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 2;
    vector<uint8_t> bytes(GetFileSize(f, nullptr));
    DWORD got = 0;
    ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr);
    CloseHandle(f);
    pngs.push_back(std::move(bytes));
  }
  DeleteFileW(tmpFile.c_str());

  vector<uint8_t> ico;
  auto put16 = [&](uint16_t v) { ico.push_back((uint8_t)(v & 255)); ico.push_back((uint8_t)(v >> 8)); };
  auto put32 = [&](uint32_t v) { put16((uint16_t)(v & 0xFFFF)); put16((uint16_t)(v >> 16)); };
  put16(0);
  put16(1);
  put16((uint16_t)pngs.size());
  uint32_t offset = 6 + 16 * (uint32_t)pngs.size();
  for (size_t i = 0; i < pngs.size(); i++) {
    const int n = sizes[i];
    ico.push_back(n >= 256 ? 0 : (uint8_t)n);
    ico.push_back(n >= 256 ? 0 : (uint8_t)n);
    ico.push_back(0);
    ico.push_back(0);
    put16(1);
    put16(32);
    put32((uint32_t)pngs[i].size());
    put32(offset);
    offset += (uint32_t)pngs[i].size();
  }
  for (auto& p : pngs) ico.insert(ico.end(), p.begin(), p.end());
  HANDLE o = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (o == INVALID_HANDLE_VALUE) return 2;
  DWORD wrote = 0;
  WriteFile(o, ico.data(), (DWORD)ico.size(), &wrote, nullptr);
  CloseHandle(o);
  std::printf("wrote %s (%zu sizes, %zu bytes)\n", narrow(outPath).c_str(), pngs.size(), ico.size());
  return 0;
}
