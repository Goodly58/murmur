// `Murmur.exe --device-test [seconds]`: plays a quiet mix through the real default output, records it
// back with a WASAPI loopback capture, and reports what the process cost (CPU time, memory).
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <psapi.h>

#include "audio.h"
#include "common.h"
#include "crash.h"
#include "net.h"
#include "setup.h"
#include "tests.h"

// MinGW only forward-declares IAudioMeterInformation, so the one method needed is declared here.
struct IMeterX : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
};
static const GUID IID_IMeterX = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

namespace {

struct Capture {
  std::atomic<bool> stop{false};
  vector<float> samples;  // mono
  std::mutex mu;
  int rate = 0;
  string error;

  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* wf = nullptr;
    do {
      if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) { error = "enumerator"; break; }
      if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) { error = "no endpoint"; break; }
      if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) { error = "activate"; break; }
      if (FAILED(client->GetMixFormat(&wf))) { error = "mix format"; break; }
      if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, wf, nullptr))) { error = "init loopback"; break; }
      if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) { error = "capture service"; break; }
      rate = (int)wf->nSamplesPerSec;
      const int ch = wf->nChannels;
      client->Start();
      while (!stop) {
        UINT32 pkt = 0;
        cap->GetNextPacketSize(&pkt);
        while (pkt) {
          BYTE* data = nullptr;
          UINT32 frames = 0;
          DWORD flags = 0;
          if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
          std::lock_guard<std::mutex> g(mu);
          const float* f = (const float*)data;
          for (UINT32 i = 0; i < frames; i++) {
            float s = 0;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) for (int c = 0; c < std::min(ch, 2); c++) s += f[(size_t)i * (size_t)ch + (size_t)c];
            samples.push_back(s * 0.5f);
          }
          cap->ReleaseBuffer(frames);
          cap->GetNextPacketSize(&pkt);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      client->Stop();
    } while (false);
    if (wf) CoTaskMemFree(wf);
    if (cap) cap->Release();
    if (client) client->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    CoUninitialize();
  }

  double rmsBetween(size_t a, size_t b) {
    std::lock_guard<std::mutex> g(mu);
    b = std::min(b, samples.size());
    if (a >= b) return 0;
    double e = 0;
    for (size_t i = a; i < b; i++) e += (double)samples[i] * samples[i];
    return std::sqrt(e / (double)(b - a));
  }
  size_t size() { std::lock_guard<std::mutex> g(mu); return samples.size(); }
};

double cpuSeconds() {
  FILETIME c, e, k, u;
  GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  auto sec = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
  return sec(k) + sec(u);
}

}  // namespace

// `Murmur.exe --session-peaks [seconds]`: which programs are making sound right now (peak level per audio session).
int runSessionPeaks(int seconds) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* dev = nullptr;
  IAudioSessionManager2* mgr = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) ||
      FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) ||
      FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr))) {
    std::printf("cannot open the audio session list\n");
    return 1;
  }
  std::map<DWORD, float> peaks;
  for (int s = 0; s < seconds * 10; s++) {
    IAudioSessionEnumerator* list = nullptr;
    if (SUCCEEDED(mgr->GetSessionEnumerator(&list))) {
      int n = 0;
      list->GetCount(&n);
      for (int i = 0; i < n; i++) {
        IAudioSessionControl* ctl = nullptr;
        if (FAILED(list->GetSession(i, &ctl))) continue;
        IAudioSessionControl2* c2 = nullptr;
        IMeterX* meter = nullptr;
        if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)) && SUCCEEDED(ctl->QueryInterface(IID_IMeterX, (void**)&meter))) {
          DWORD pid = 0;
          c2->GetProcessId(&pid);
          float peak = 0;
          meter->GetPeakValue(&peak);
          peaks[pid] = std::max(peaks[pid], peak);
        }
        if (meter) meter->Release();
        if (c2) c2->Release();
        ctl->Release();
      }
      list->Release();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  for (auto& kv : peaks) {
    wchar_t name[MAX_PATH] = L"(system)";
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, kv.first)) {
      DWORD n = MAX_PATH;
      wchar_t full[MAX_PATH];
      if (QueryFullProcessImageNameW(p, 0, full, &n)) {
        const wchar_t* base = wcsrchr(full, L'\\');
        wcsncpy(name, base ? base + 1 : full, MAX_PATH - 1);
      }
      CloseHandle(p);
    }
    std::printf("  pid %5lu  %-24ls peak %.4f\n", (unsigned long)kv.first, name, kv.second);
  }
  mgr->Release();
  dev->Release();
  en->Release();
  return 0;
}

// `Murmur.exe --loopback [seconds]`: prints the level of whatever is playing on the default output, once a second.
// Handy for checking that the real window really makes sound.
int runLoopback(int seconds) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Capture cap;
  std::thread capThread([&] { cap.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  for (int i = 0; i < seconds; i++) {
    const size_t a = cap.size();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::printf("  second %d: output level %.5f\n", i + 1, cap.rmsBetween(a, cap.size()));
  }
  if (!cap.error.empty()) std::printf("  capture error: %s\n", cap.error.c_str());
  cap.stop = true;
  capThread.join();
  return 0;
}

// `Murmur.exe --lost-test`: plays a very quiet mix on the real output, then pretends the output was unplugged and
// checks that the engine stops by itself and reports it (the same path a real unplug takes).
int runLostTest() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Engine e;
  std::atomic<int> lost{0};
  e.onOutputLost = [&] { lost++; };
  e.start(true);
  Mix m = emptyMix();
  m[(size_t)channelIndex("brown")] = 30;
  e.setMaster(0.05);
  e.setMix(m, BeatSpec{0, 10, 0}, 0.1);
  e.play();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  const bool playingBefore = e.playing();
  e.simulateOutputLost();
  for (int i = 0; i < 40 && lost.load() == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const bool stopped = !e.playing();
  std::printf("lost test: playing before %d, callbacks %d, playing after %d\n", (int)playingBefore, lost.load(), (int)!stopped);
  const bool ok = playingBefore && lost.load() == 1 && stopped;
  e.shutdown();
  std::printf("%s\n", ok ? "OUTPUT-LOST OK: the engine stopped and reported it" : "OUTPUT-LOST TEST FAILED");
  return ok ? 0 : 1;
}

// `Murmur.exe --tools-test <folder>`: runs the real download of mpv and yt-dlp (the player's Set up button) into a
// scratch folder, with MURMUR_TOOLS pointed there so nothing else on the PC is touched, then starts both once.
int runToolsTest(const wstring& dir) {
  SetEnvironmentVariableW(L"MURMUR_TOOLS", dir.c_str());
  SetEnvironmentVariableW(L"MURMUR_DATA", dir.c_str());  // its log lines stay in the scratch folder too
  std::printf("tools folder: %s\n", narrow(toolsDir()).c_str());
  wstring lastStep;
  int lastPct = -10;
  const auto t0 = std::chrono::steady_clock::now();
  const wstring error = installTools(TOOL_MPV | TOOL_YTDLP, [&](const wstring& step, double fraction) {
    const int pct = (int)(fraction * 100);
    if (step == lastStep && pct < lastPct + 10) return;
    lastStep = step;
    lastPct = pct;
    std::printf("  %3d%%  %s\n", pct, narrow(step).c_str());
  }, realToolIO());
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!error.empty()) {
    std::printf("TOOLS TEST FAILED after %.0f s: %s\n", secs, narrow(error).c_str());
    return 1;
  }
  bool ok = true;
  for (const wchar_t* rel : {L"\\mpv\\mpv.exe", L"\\yt-dlp.exe"}) {
    const wstring exe = toolsDir() + rel;
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    const bool there = GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fa) != 0;
    string out;
    int code = -1;
    if (there) runCapture(L"\"" + exe + L"\" --version", out, 30000, &code);
    out = out.substr(0, out.find('\n'));
    std::printf("  %s: %s, %.1f MB, --version -> %d: %s\n", narrow(exe.substr(toolsDir().size() + 1)).c_str(), there ? "present" : "MISSING",
                there ? (double)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) / 1048576.0 : 0.0, code, out.c_str());
    ok = ok && there && code == 0;
  }
  const bool leftover = GetFileAttributesW((toolsDir() + L"\\.part").c_str()) != INVALID_FILE_ATTRIBUTES;
  std::printf("  working folder cleaned up: %s\n", leftover ? "NO" : "yes");
  ok = ok && !leftover;
  std::printf("%s after %.0f s\n", ok ? "TOOLS TEST OK" : "TOOLS TEST FAILED", secs);
  return ok ? 0 : 1;
}

// `Murmur.exe --crash-test`: writes through a null pointer, so the crash handler has something to report.
int runCrashTest() {
  if (!GetEnvironmentVariableW(L"MURMUR_DATA", nullptr, 0)) crashLogTo(L"");  // a deliberate crash must not leave a note in the real folder
  volatile int* nothing = nullptr;
  *nothing = 1;
  return 0;
}

// `Murmur.exe --update-test <folder>`: the daily yt-dlp update against the real GitHub. The folder must already hold an
// older yt-dlp.exe; it is both the tools folder and the data folder for the run, so nothing else on the PC is touched.
int runUpdateTest(const wstring& dir) {
  SetEnvironmentVariableW(L"MURMUR_TOOLS", dir.c_str());
  SetEnvironmentVariableW(L"MURMUR_DATA", dir.c_str());
  const wstring exe = toolsDir() + L"\\yt-dlp.exe";
  auto version = [&]() -> string {
    string out;
    int code = -1;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return "MISSING";
    if (!runCapture(L"\"" + exe + L"\" --version", out, 30000, &code) || code != 0) return "does not run";
    return out.substr(0, out.find_first_of("\r\n"));
  };
  const string before = version();
  std::printf("yt-dlp before: %s\n", before.c_str());
  const auto t0 = std::chrono::steady_clock::now();
  const bool asked = refreshYtdlp(exe, realToolIO(), wallMs() / 1000, true);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const string after = version();
  std::printf("yt-dlp after:  %s   (asked to update: %s, %.1f s)\n", after.c_str(), asked ? "yes" : "no", secs);
  const bool stamped = GetFileAttributesW((dir + L"\\yt-dlp.checked").c_str()) != INVALID_FILE_ATTRIBUTES;
  std::printf("  check remembered: %s\n", stamped ? "yes" : "NO");
  string log;
  if (FILE* f = _wfopen((dir + L"\\murmur.log").c_str(), L"rb")) {
    char b[1024];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) log.append(b, n);
    fclose(f);
  }
  while (!log.empty() && (log.back() == '\n' || log.back() == '\r')) log.pop_back();
  const size_t nl = log.rfind('\n');
  std::printf("  log: %s\n", log.substr(nl == string::npos ? 0 : nl + 1).c_str());
  const bool ok = asked && stamped && after != "MISSING" && after != "does not run";
  std::printf("%s\n", ok ? "UPDATE TEST OK" : "UPDATE TEST FAILED");
  return ok ? 0 : 1;
}

int runDeviceTest(int seconds) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Capture cap;
  std::thread capThread([&] { cap.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  const size_t baseEnd = cap.size();
  const double base = cap.rmsBetween(0, baseEnd);

  Engine e;
  e.start(true);
  Mix m = emptyMix();
  m[(size_t)channelIndex("brown")] = 45;
  m[(size_t)channelIndex("pink")] = 18;
  e.setMaster(0.14);  // quiet on purpose
  e.setMix(m, BeatSpec{1, 16, 22}, 0.15);
  const double cpu0 = cpuSeconds();
  const int64_t t0 = wallMs();
  e.play();
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  const double cpu1 = cpuSeconds();
  const int64_t t1 = wallMs();
  const size_t end = cap.size();
  const double played = cap.rmsBetween(baseEnd + (size_t)(cap.rate * 1.5), end);

  PROCESS_MEMORY_COUNTERS_EX pm = {};
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
  std::printf("device test: %d Hz output, loops %.1f MB\n", e.sampleRate(), e.loopBytes() / 1048576.0);
  std::printf("  loopback level before: %.5f   while playing: %.5f   (expected about 0.01)\n", base, played);
  std::printf("  process CPU while playing: %.2f%% of one core over %.1f s\n", 100.0 * (cpu1 - cpu0) / ((t1 - t0) / 1000.0), (t1 - t0) / 1000.0);
  std::printf("  working set %.1f MB, private %.1f MB\n", pm.WorkingSetSize / 1048576.0, pm.PrivateUsage / 1048576.0);
  if (!e.lastError().empty()) std::printf("  engine error: %s\n", e.lastError().c_str());
  if (!cap.error.empty()) std::printf("  capture error: %s\n", cap.error.c_str());

  e.pause();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const bool ok = played > base + 0.003 && e.lastError().empty();
  cap.stop = true;
  capThread.join();
  e.shutdown();
  std::printf("%s\n", ok ? "DEVICE OK: audio reached the output" : "DEVICE TEST FAILED");
  return ok ? 0 : 1;
}
