// Audio engine: mixes the looping sounds, beats and chimes, and plays them through WASAPI.
// The same mixing code runs offline for tests and for `--render`.
#pragma once
#include "common.h"
#include "data.h"
#include "dsp.h"
#include "thread.h"

struct BeatSpec {
  int mode = 0;     // 0 off, 1 binaural (headphones), 2 isochronic (speakers)
  double hz = 10;   // beat frequency
  int level = 0;    // 0..100
};

class Engine {
 public:
  Engine();
  ~Engine();

  // realDevice=false: no output thread; the caller pulls audio with renderBlock().
  bool start(bool realDevice, int offlineRate = 48000);
  void shutdown();

  // Which sounds and beats should be audible. `ramp` is the fade time constant in seconds.
  void setMix(const Mix& levels, const BeatSpec& beat, double ramp);
  void setMaster(double v01);    // slider position 0..1 (the loudness curve is applied here)
  void setSoundMul(double m);    // video/sound balance, 0..1
  void setFade(double f);        // 1 normally; ramps to 0 near the end of a timer
  void play();
  void pause();
  void chime(int kind);          // 0 start, 1 break, 2 done
  bool playing() const { return playing_.load(); }
  int sampleRate() const { return rate_.load(); }
  string deviceName() const;
  string lastError() const;

  // Called on a worker thread when the output we were playing on goes away (headphones unplugged, device
  // disabled). The engine stops by itself first, so nothing blips out of the speakers; the app shows why.
  std::function<void()> onOutputLost;
  void simulateOutputLost();  // for tests

  void renderBlock(float* out, int frames);        // interleaved stereo
  bool waitSoundsReady(int timeoutMs);             // blocks until every wanted loop exists
  void copyScope(float* dst, int n) const;         // newest n mono samples of the pre-master mix
  void sweep();                                    // frees loops that have been silent for 30 s
  size_t loopBytes() const;

 private:
  struct Shared;
  struct Chan {
    std::shared_ptr<const Loop> loop;
    double pos = 0, gain = 0;
  };
  struct Voice {
    double phase = 0, inc = 0, amp = 0, k = 0, delay = 0, age = 0, attack = 0;
    bool alive = false;
  };
  struct Params {
    Mix want = emptyMix();
    BeatSpec beat;
    double ramp = 0.15, master = 0.7, soundMul = 1, fade = 1;
  };
  struct Note {
    double freq, when, len, amp;
  };

  void deviceLoop();
  bool openDevice();
  void closeDevice();
  void requestLoad(int id);
  bool wantsAudio() const;
  void addNote(double freq, double whenSec, double lenSec, double amp);

  std::shared_ptr<Shared> sh_;
  mutable std::mutex mu_;
  mutable std::mutex noteMu_;
  vector<Note> notes_;  // chimes waiting for the audio thread
  Params p_;
  std::atomic<bool> playing_{false}, quit_{false}, realDevice_{false}, lost_{false};
  std::atomic<int> rate_{48000};
  std::atomic<int64_t> lastActive_{0};
  std::atomic<uint32_t> seed_{0};  // 0: random noise and starting points, as in the app; else fixed (offline engines)
  Thread thread_;

  // audio-thread state
  Chan chans_[NCH];
  vector<Voice> voices_;
  double mg_ = 0, sm_ = 1, fd_ = 1, bAmp_ = 0, bF0_ = 200, bF1_ = 200, bPh0_ = 0, bPh1_ = 0, bLfo_ = 10, bLfoPh_ = 0;
  vector<float> scope_;
  std::atomic<uint32_t> scopePos_{0};
  int64_t lastUse_[NCH] = {0};

  // device state (touched only by the device thread)
  struct Dev;
  std::unique_ptr<Dev> dev_;
  std::condition_variable cv_;
  std::mutex cvMu_;
  std::atomic<bool> deviceDirty_{false};
  string error_, deviceName_;
};
