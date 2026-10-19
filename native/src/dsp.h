// Sound synthesis: each sound is rendered once into a seamlessly looping stereo buffer.
// A port of the web app's dsp.js; the algorithms and constants are the same.
#pragma once
#include "common.h"
#include "data.h"

struct Loop {
  vector<float> ch[2];
  int rate = 0;
  int frames() const { return (int)ch[0].size(); }
};

struct SoundSpec {
  double seconds;  // loop length
  double scale;    // sample rate as a fraction of the output rate (low-frequency sounds need less)
};
extern const SoundSpec SOUNDS[NCH];

// Per-sound gain that makes equal slider positions about equally loud (A-weighted).
extern const double LOUD[NCH];
constexpr double TARGET_RMS = 0.25;

// Renders sound `id` (index into CHANNELS). The left and right channels use different seeds, so the
// two ears get uncorrelated noise.
std::shared_ptr<Loop> renderLoop(int id, uint32_t seed, int baseRate);
