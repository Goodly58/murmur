// `Murmur.exe --device-test [seconds]`: plays a quiet mix through the real default output, records it
// back with a WASAPI loopback capture, and reports what the process cost (CPU time, memory).
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <psapi.h>

#include "audio.h"
#include "common.h"
#include "tests.h"

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
