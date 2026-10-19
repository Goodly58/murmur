#include "audio.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>

namespace {
constexpr double TAU = 6.283185307179586;
constexpr int CHUNK = 1024;
constexpr uint32_t SCOPE_SIZE = 16384;  // power of two
constexpr double BEAT_CARRIER_BIN = 200.0;
constexpr double BEAT_CARRIER_ISO = 240.0;

const GUID kFloatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// One cycle of a square wave with Lanczos-smoothed harmonics, mapped to 0..1: crisp on/off for the
// isochronic gate, without clicks or overshoot.
const vector<float>& pulseTable() {
  static const vector<float> t = [] {
    const int N = 2048;
    vector<double> v((size_t)N);
    double peak = 1e-9;
    for (int i = 0; i < N; i++) {
      double s = 0;
      for (int k = 1; k <= 7; k += 2) {
        const double x = 3.14159265358979323846 * k / 9.0;
        s += (4.0 / (3.14159265358979323846 * k)) * (std::sin(x) / x) * std::sin(TAU * k * i / N);
      }
      v[(size_t)i] = s;
      peak = std::max(peak, std::fabs(s));
    }
    vector<float> out((size_t)N);
    for (int i = 0; i < N; i++) out[(size_t)i] = (float)(0.5 + 0.5 * v[(size_t)i] / peak);
    return out;
  }();
  return t;
}

inline float softLimit(float x) {
  const float a = std::fabs(x);
  if (a <= 0.5f) return x;
  const float y = 0.5f + 0.5f * std::tanh((a - 0.5f) * 2.0f);
  return x < 0 ? -y : y;
}

class NotifClient : public IMMNotificationClient {
 public:
  NotifClient(std::atomic<bool>* flag, std::condition_variable* cv) : flag_(flag), cv_(cv) {}
  virtual ~NotifClient() = default;
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = (ULONG)InterlockedDecrement(&ref_);
    if (!r) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *ppv = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eMultimedia) { flag_->store(true); cv_->notify_all(); }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  LONG ref_ = 1;
  std::atomic<bool>* flag_;
  std::condition_variable* cv_;
};
}  // namespace

struct Engine::Shared {
  std::mutex mu;
  std::shared_ptr<const Loop> loops[NCH];
  bool loading[NCH] = {};
  int loopRate = 0;
};

struct Engine::Dev {
  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* dev = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  NotifClient* notif = nullptr;
  UINT32 bufFrames = 0;
  bool isFloat = true;
  bool started = false;
};

Engine::Engine() : sh_(std::make_shared<Shared>()), scope_(SCOPE_SIZE, 0.0f) {}

Engine::~Engine() { shutdown(); }

bool Engine::start(bool realDevice, int offlineRate) {
  realDevice_ = realDevice;
  quit_ = false;
  if (!realDevice) {
    rate_ = offlineRate;
    playing_ = true;
    return true;
  }
  thread_ = std::thread([this] { deviceLoop(); });
  return true;
}

void Engine::shutdown() {
  quit_ = true;
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Engine::setMix(const Mix& levels, const BeatSpec& beat, double ramp) {
  {
    std::lock_guard<std::mutex> g(mu_);
    p_.want = levels;
    p_.beat = beat;
    p_.ramp = ramp;
  }
  for (int i = 0; i < NCH; i++) if (levels[(size_t)i] > 0) requestLoad(i);
  cv_.notify_all();
}

void Engine::setMaster(double v) { std::lock_guard<std::mutex> g(mu_); p_.master = clampd(v, 0, 1); }
void Engine::setSoundMul(double m) { std::lock_guard<std::mutex> g(mu_); p_.soundMul = clampd(m, 0, 1); }
void Engine::setFade(double f) { std::lock_guard<std::mutex> g(mu_); p_.fade = clampd(f, 0, 1); }

void Engine::play() {
  playing_ = true;
  cv_.notify_all();
}

void Engine::pause() { playing_ = false; }

string Engine::deviceName() const { return deviceName_; }
string Engine::lastError() const { return error_; }

void Engine::chime(int kind) {
  const double E5 = 659.25, B5 = 987.77, G5 = 783.99, E6 = 1318.5;
  std::lock_guard<std::mutex> g(noteMu_);
  auto note = [&](double f, double when, double len, double amp) { notes_.push_back({f, when, len, amp}); };
  if (kind == 0) { note(E5, 0, 2.2, 0.16); note(B5, 0.16, 2.2, 0.14); }
  else if (kind == 1) { note(B5, 0, 2.2, 0.15); note(G5, 0.2, 2.6, 0.14); }
  else { note(E5, 0, 2.4, 0.16); note(G5, 0.2, 2.4, 0.15); note(E6, 0.4, 3.0, 0.15); }
  cv_.notify_all();
}

void Engine::addNote(double freq, double whenSec, double lenSec, double amp) {
  const double sr = rate_.load();
  static const double parts[3][3] = {{1, 1, 1}, {2.76, 0.35, 0.5}, {5.4, 0.12, 0.25}};  // multiple, level, decay share
  for (auto& pt : parts) {
    Voice v;
    v.inc = TAU * freq * pt[0] / sr;
    v.amp = amp * pt[1];
    const double T = lenSec * pt[2] * sr;
    v.k = std::log(1e4) / T;
    v.attack = 0.01 * sr;
    v.delay = whenSec * sr;
    v.age = 0;
    v.phase = 0;
    v.alive = true;
    voices_.push_back(v);
  }
}

bool Engine::wantsAudio() const {
  {
    std::lock_guard<std::mutex> g(noteMu_);
    if (!notes_.empty()) return true;
  }
  std::lock_guard<std::mutex> g(mu_);
  if (p_.beat.mode != 0) return true;
  for (int v : p_.want) if (v > 0) return true;
  return lastActive_.load() > wallMs() - 1500;  // still fading out
}

void Engine::requestLoad(int id) {
  const int rate = rate_.load();
  {
    std::lock_guard<std::mutex> g(sh_->mu);
    if (sh_->loopRate != rate) {
      for (auto& l : sh_->loops) l.reset();
      sh_->loopRate = rate;
    }
    if (sh_->loops[id] || sh_->loading[id]) return;
    sh_->loading[id] = true;
  }
  const uint32_t seed = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() * 2654435761u) ^ (uint32_t)(id * 7919);
  std::thread([sh = sh_, id, rate, seed] {
    auto loop = renderLoop(id, seed, rate);
    std::lock_guard<std::mutex> g(sh->mu);
    if (sh->loopRate == rate) sh->loops[id] = loop;
    sh->loading[id] = false;
  }).detach();
}

bool Engine::waitSoundsReady(int timeoutMs) {
  const int64_t end = wallMs() + timeoutMs;
  while (wallMs() < end) {
    bool all = true;
    {
      std::lock_guard<std::mutex> g(mu_);
      std::lock_guard<std::mutex> g2(sh_->mu);
      for (int i = 0; i < NCH; i++) if (p_.want[(size_t)i] > 0 && !sh_->loops[i]) all = false;
    }
    if (all) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

void Engine::sweep() {
  const int64_t now = wallMs();
  std::lock_guard<std::mutex> g(mu_);
  std::lock_guard<std::mutex> g2(sh_->mu);
  for (int i = 0; i < NCH; i++) {
    if (p_.want[(size_t)i] > 0) lastUse_[i] = now;
    else if (sh_->loops[i] && lastUse_[i] && now - lastUse_[i] > 30000) { sh_->loops[i].reset(); lastUse_[i] = 0; }
  }
}

size_t Engine::loopBytes() const {
  size_t b = 0;
  std::lock_guard<std::mutex> g(sh_->mu);
  for (auto& l : sh_->loops) if (l) b += (l->ch[0].size() + l->ch[1].size()) * sizeof(float);
  return b;
}

void Engine::copyScope(float* dst, int n) const {
  const uint32_t pos = scopePos_.load(std::memory_order_acquire);
  for (int k = 0; k < n; k++) dst[k] = scope_[(pos - (uint32_t)n + (uint32_t)k) & (SCOPE_SIZE - 1)];
}

// ---------------------------------------------------------------------------------------------------
// Mixing
// ---------------------------------------------------------------------------------------------------

void Engine::renderBlock(float* out, int frames) {
  Params p;
  std::shared_ptr<const Loop> lp[NCH];
  {
    std::lock_guard<std::mutex> g(mu_);
    p = p_;
  }
  {
    std::lock_guard<std::mutex> g(sh_->mu);
    for (int i = 0; i < NCH; i++) lp[i] = sh_->loops[i];
  }
  {
    std::lock_guard<std::mutex> g(noteMu_);
    for (auto& n : notes_) addNote(n.freq, n.when, n.len, n.amp);
    notes_.clear();
  }
  const double sr = rate_.load();
  const bool playing = playing_.load();
  bool busy = false;

  while (frames > 0) {
    const int n = std::min(frames, CHUNK);
    float mixL[CHUNK], mixR[CHUNK];
    std::memset(mixL, 0, sizeof(float) * (size_t)n);
    std::memset(mixR, 0, sizeof(float) * (size_t)n);

    for (int i = 0; i < NCH; i++) {
      Chan& c = chans_[i];
      if (c.loop != lp[i]) {
        c.loop = lp[i];
        if (c.loop) c.pos = (double)(((uint64_t)wallMs() * 2654435761ull + (uint64_t)i * 40503ull) % (uint64_t)c.loop->frames());
      }
      const double lvl = p.want[(size_t)i] / 100.0;
      const double target = (c.loop && lvl > 0) ? std::pow(lvl, 1.5) * LOUD[i] : 0.0;
      if (!c.loop || (target == 0 && c.gain < 1e-6)) { c.gain = 0; continue; }
      busy = true;
      const double tau = target < c.gain ? std::max(0.08, p.ramp * 0.6) : std::max(0.005, p.ramp);
      const double a = 1 - std::exp(-1.0 / (tau * sr));
      const Loop& L = *c.loop;
      const int len = L.frames();
      const double step = L.rate / sr;
      const float* l0 = L.ch[0].data();
      const float* r0 = L.ch[1].data();
      double pos = c.pos, g = c.gain;
      for (int f = 0; f < n; f++) {
        g += (target - g) * a;
        const int i0 = (int)pos;
        const float fr = (float)(pos - i0);
        int i1 = i0 + 1;
        if (i1 >= len) i1 = 0;
        mixL[f] += (l0[i0] + (l0[i1] - l0[i0]) * fr) * (float)g;
        mixR[f] += (r0[i0] + (r0[i1] - r0[i0]) * fr) * (float)g;
        pos += step;
        if (pos >= len) pos -= len;
      }
      c.pos = pos;
      c.gain = g;
    }

    // beats: binaural (a tone per ear) or isochronic (one tone gated at the beat rate)
    {
      const bool on = p.beat.mode != 0;
      const double ampT = on ? 0.3 * std::pow(p.beat.level / 100.0, 1.5) : 0.0;
      if (on || bAmp_ > 1e-6) {
        busy = true;
        const double carrier = p.beat.mode == 2 ? BEAT_CARRIER_ISO : BEAT_CARRIER_BIN;
        const double f0t = p.beat.mode == 2 ? carrier : carrier - p.beat.hz / 2;
        const double f1t = p.beat.mode == 2 ? carrier : carrier + p.beat.hz / 2;
        const double aA = 1 - std::exp(-1.0 / (std::max(0.01, p.ramp) * sr));
        const double aF = 1 - std::exp(-1.0 / (0.05 * sr));
        const vector<float>& pt = pulseTable();
        for (int f = 0; f < n; f++) {
          bAmp_ += (ampT - bAmp_) * aA;
          bF0_ += (f0t - bF0_) * aF;
          bF1_ += (f1t - bF1_) * aF;
          bLfo_ += (p.beat.hz - bLfo_) * aF;
          bPh0_ += TAU * bF0_ / sr;
          bPh1_ += TAU * bF1_ / sr;
          bLfoPh_ += bLfo_ / sr;
          if (bPh0_ > TAU) bPh0_ -= TAU;
          if (bPh1_ > TAU) bPh1_ -= TAU;
          if (bLfoPh_ >= 1) bLfoPh_ -= 1;
          if (p.beat.mode == 2 || (!on && bF0_ == bF1_)) {
            const float gate = pt[(size_t)(bLfoPh_ * (double)pt.size()) % pt.size()];
            const float s = (float)(bAmp_ * std::sin(bPh0_)) * gate;
            mixL[f] += s;
            mixR[f] += s;
          } else {
            mixL[f] += (float)(bAmp_ * std::sin(bPh0_));
            mixR[f] += (float)(bAmp_ * std::sin(bPh1_));
          }
        }
      }
    }

    // balance with the stream, scope tap (before master volume), master, chimes, limiter
    const double aS = 1 - std::exp(-1.0 / (0.12 * sr));
    const double aM = 1 - std::exp(-1.0 / (0.12 * sr));
    const double aP = 1 - std::exp(-1.0 / (0.05 * sr));
    const double masterT = std::pow(p.master, 1.5) * p.fade;
    const uint32_t sp = scopePos_.load(std::memory_order_relaxed);
    for (int f = 0; f < n; f++) {
      sm_ += (p.soundMul - sm_) * aS;
      fd_ += ((playing ? 1.0 : 0.0) - fd_) * aP;
      mg_ += (masterT - mg_) * aM;
      float l = mixL[f] * (float)sm_, r = mixR[f] * (float)sm_;
      scope_[(sp + (uint32_t)f) & (SCOPE_SIZE - 1)] = 0.5f * (l + r);
      float ch = 0;
      for (auto& v : voices_) {
        if (!v.alive) continue;
        if (v.delay > 0) { v.delay -= 1; continue; }
        const double env = v.age < v.attack ? v.age / v.attack : std::exp(-(v.age - v.attack) * v.k);
        ch += (float)(v.amp * env * std::sin(v.phase));
        v.phase += v.inc;
        if (v.phase > TAU) v.phase -= TAU;
        v.age += 1;
        if (env < 1e-4 && v.age > v.attack) v.alive = false;
      }
      const float g = (float)(mg_ * fd_);
      out[2 * f] = softLimit((l + ch) * g);
      out[2 * f + 1] = softLimit((r + ch) * g);
    }
    scopePos_.store(sp + (uint32_t)n, std::memory_order_release);
    out += 2 * n;
    frames -= n;
  }
  for (size_t k = 0; k < voices_.size();) {
    if (!voices_[k].alive) voices_.erase(voices_.begin() + (long)k);
    else { busy = true; k++; }
  }
  if (busy) lastActive_ = wallMs();
}

// ---------------------------------------------------------------------------------------------------
// WASAPI device thread
// ---------------------------------------------------------------------------------------------------

bool Engine::openDevice() {
  Dev& d = *dev_;
  HRESULT hr;
  if (!d.en) {
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&d.en);
    if (FAILED(hr)) { error_ = "no device enumerator"; return false; }
    d.notif = new NotifClient(&deviceDirty_, &cv_);
    d.en->RegisterEndpointNotificationCallback(d.notif);
  }
  hr = d.en->GetDefaultAudioEndpoint(eRender, eMultimedia, &d.dev);
  if (FAILED(hr)) { error_ = "no audio output device"; return false; }
  hr = d.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&d.client);
  if (FAILED(hr)) { error_ = "cannot open the audio output"; closeDevice(); return false; }

  WAVEFORMATEX* mix = nullptr;
  hr = d.client->GetMixFormat(&mix);
  if (FAILED(hr) || !mix) { error_ = "cannot read the output format"; closeDevice(); return false; }
  const int rate = (int)mix->nSamplesPerSec;
  CoTaskMemFree(mix);

  WAVEFORMATEXTENSIBLE wf = {};
  wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wf.Format.nChannels = 2;
  wf.Format.nSamplesPerSec = (DWORD)rate;
  wf.Format.wBitsPerSample = 32;
  wf.Format.nBlockAlign = 8;
  wf.Format.nAvgBytesPerSec = (DWORD)rate * 8;
  wf.Format.cbSize = 22;
  wf.Samples.wValidBitsPerSample = 32;
  wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  wf.SubFormat = kFloatSubtype;
  const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &wf.Format, nullptr);
  d.isFloat = true;
  if (FAILED(hr)) {  // very rare: fall back to 16-bit
    d.client->Release();
    d.client = nullptr;
    hr = d.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&d.client);
    if (SUCCEEDED(hr)) {
      WAVEFORMATEX w16 = {};
      w16.wFormatTag = WAVE_FORMAT_PCM;
      w16.nChannels = 2;
      w16.nSamplesPerSec = (DWORD)rate;
      w16.wBitsPerSample = 16;
      w16.nBlockAlign = 4;
      w16.nAvgBytesPerSec = (DWORD)rate * 4;
      hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &w16, nullptr);
      d.isFloat = false;
    }
  }
  if (FAILED(hr)) { error_ = "the audio output refused our format"; closeDevice(); return false; }
  hr = d.client->GetBufferSize(&d.bufFrames);
  if (FAILED(hr)) { error_ = "no audio buffer"; closeDevice(); return false; }
  hr = d.client->GetService(__uuidof(IAudioRenderClient), (void**)&d.render);
  if (FAILED(hr)) { error_ = "no render client"; closeDevice(); return false; }

  if (rate != rate_.load()) {
    rate_ = rate;
    std::lock_guard<std::mutex> g(sh_->mu);
    for (auto& l : sh_->loops) l.reset();
    sh_->loopRate = rate;
  }
  {
    Mix want;
    { std::lock_guard<std::mutex> g(mu_); want = p_.want; }
    for (int i = 0; i < NCH; i++) if (want[(size_t)i] > 0) requestLoad(i);
  }
  error_.clear();
  return true;
}

void Engine::closeDevice() {
  Dev& d = *dev_;
  if (d.client && d.started) d.client->Stop();
  d.started = false;
  if (d.render) { d.render->Release(); d.render = nullptr; }
  if (d.client) { d.client->Release(); d.client = nullptr; }
  if (d.dev) { d.dev->Release(); d.dev = nullptr; }
}

void Engine::deviceLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD taskIdx = 0;
  HANDLE mm = AvSetMmThreadCharacteristicsW(L"Audio", &taskIdx);
  dev_.reset(new Dev);
  vector<float> tmp;

  auto fill = [&](UINT32 frames) -> bool {
    Dev& d = *dev_;
    BYTE* buf = nullptr;
    if (FAILED(d.render->GetBuffer(frames, &buf)) || !buf) return false;
    if (d.isFloat) {
      renderBlock((float*)buf, (int)frames);
    } else {
      tmp.resize((size_t)frames * 2);
      renderBlock(tmp.data(), (int)frames);
      int16_t* o = (int16_t*)buf;
      for (size_t k = 0; k < tmp.size(); k++) o[k] = (int16_t)std::lrintf(clampd(tmp[k], -1, 1) * 32767.0);
    }
    d.render->ReleaseBuffer(frames, 0);
    return true;
  };

  while (!quit_) {
    {
      std::unique_lock<std::mutex> lk(cvMu_);
      cv_.wait_for(lk, std::chrono::milliseconds(250), [&] { return quit_.load() || (playing_.load() && wantsAudio()); });
    }
    if (quit_) break;
    if (!(playing_ && wantsAudio())) continue;
    if (!openDevice()) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }

    Dev& d = *dev_;
    const UINT32 target = (UINT32)(rate_.load() * 0.12);
    bool ok = fill(std::min(target, d.bufFrames));
    ok = ok && SUCCEEDED(d.client->Start());
    d.started = ok;
    deviceDirty_ = false;
    int64_t pausedAt = 0;
    while (ok && !quit_) {
      if (deviceDirty_.exchange(false)) break;  // the default output changed: reopen on the new one
      UINT32 pad = 0;
      if (FAILED(d.client->GetCurrentPadding(&pad))) break;
      if (pad < target && !fill(target - pad)) break;
      if (!playing_.load()) {
        if (!pausedAt) pausedAt = wallMs();
        if (wallMs() - pausedAt > 180) break;  // fade finished: release the device while paused
      } else {
        pausedAt = 0;
        if (!wantsAudio()) break;  // silent for a while: let the device sleep
      }
      std::unique_lock<std::mutex> lk(cvMu_);
      cv_.wait_for(lk, std::chrono::milliseconds(25), [&] { return quit_.load(); });
    }
    closeDevice();
  }
  closeDevice();
  if (dev_->en) {
    if (dev_->notif) { dev_->en->UnregisterEndpointNotificationCallback(dev_->notif); dev_->notif->Release(); }
    dev_->en->Release();
  }
  dev_.reset();
  if (mm) AvRevertMmThreadCharacteristics(mm);
  CoUninitialize();
}
