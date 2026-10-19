#include "dsp.h"

const SoundSpec SOUNDS[NCH] = {
    {14, 0.5},    // brown
    {12, 1.0},    // pink
    {10, 1.0},    // white
    {12, 0.5},    // speech mask
    {20, 1.0},    // rain
    {36, 0.5},    // ocean
    {36, 0.5},    // wind
    {20, 0.667},  // fire
    {16, 0.5},    // cabin
};

const double LOUD[NCH] = {1.5, 0.578, 0.485, 0.38, 0.374, 0.546, 0.455, 0.561, 1.202};

namespace {

constexpr double TAU = 6.283185307179586;
using Vec = vector<double>;

struct Rng {
  uint32_t a;
  explicit Rng(uint32_t seed) : a(seed) {}
  double next() {
    a += 0x6D2B79F5u;
    uint32_t t = a;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return (double)(t ^ (t >> 14)) / 4294967296.0;
  }
};

Vec whiteNoise(int n, Rng& rng, double) {
  Vec x((size_t)n);
  for (int i = 0; i < n; i++) x[(size_t)i] = rng.next() * 2 - 1;
  return x;
}

// Paul Kellet's refined pink filter: -3 dB/octave from ~10 Hz to Nyquist.
Vec pinkNoise(int n, Rng& rng, double) {
  Vec x((size_t)n);
  double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
  for (int i = 0; i < n; i++) {
    double w = rng.next() * 2 - 1;
    b0 = 0.99886 * b0 + w * 0.0555179;
    b1 = 0.99332 * b1 + w * 0.0750759;
    b2 = 0.96900 * b2 + w * 0.1538520;
    b3 = 0.86650 * b3 + w * 0.3104856;
    b4 = 0.55000 * b4 + w * 0.5329522;
    b5 = -0.7616 * b5 - w * 0.0168980;
    x[(size_t)i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
    b6 = w * 0.115926;
  }
  return x;
}

void lowpass(Vec& x, double fc, double fs, int order = 1) {
  const double a = 1 - std::exp(-TAU * fc / fs);
  for (int k = 0; k < order; k++) {
    double y = 0;
    for (double& v : x) { y += a * (v - y); v = y; }
  }
}

void highpass(Vec& x, double fc, double fs, int order = 1) {
  const double a = 1 - std::exp(-TAU * fc / fs);
  for (int k = 0; k < order; k++) {
    double y = 0;
    for (double& v : x) { y += a * (v - y); v -= y; }
  }
}

// Leaky integrator: -6 dB/octave above ~8 Hz, then a 25 Hz high-pass so no energy sits below what
// speakers and headphones can reproduce.
Vec brownNoise(int n, Rng& rng, double fs) {
  Vec x((size_t)n);
  const double a = std::exp(-TAU * 8 / fs);
  double y = 0;
  for (int i = 0; i < n; i++) { y = a * y + (rng.next() * 2 - 1); x[(size_t)i] = y; }
  highpass(x, 25, fs, 1);
  return x;
}

void unit(Vec& x) {
  double m = 0;
  for (double v : x) m += v;
  m /= (double)x.size();
  double e = 0;
  for (double& v : x) { v -= m; e += v * v; }
  double g = 1.0 / std::sqrt(e / (double)x.size());
  if (!(e > 0)) g = 1;
  for (double& v : x) v *= g;
}

// x holds n + f samples starting at `start`. Equal-power crossfade of the last f samples onto the
// first f, so the n returned samples loop without a click or a level dip.
Vec loopify(const double* x, int total, int n) {
  const int f = total - n;
  Vec out(x, x + n);
  for (int i = 0; i < f; i++) {
    const double t = (i + 0.5) / f * 3.14159265358979323846 / 2;
    out[(size_t)i] = x[i] * std::sin(t) + x[n + i] * std::cos(t);
  }
  return out;
}

// Sum of sines with a whole number of cycles per buffer, so it tiles exactly. Scaled to max |v| = 1.
Vec periodic(int n, const vector<std::pair<int, double>>& parts, Rng& rng) {
  Vec y((size_t)n, 0.0);
  for (auto& p : parts) {
    const double w = TAU * p.first / n, ph = rng.next() * TAU, a = p.second;
    for (int i = 0; i < n; i++) y[(size_t)i] += a * std::sin(w * i + ph);
  }
  double peak = 1e-9;
  for (double v : y) peak = std::max(peak, std::fabs(v));
  for (double& v : y) v /= peak;
  return y;
}

int cyc(double dur, double period) { return std::max(1, (int)std::llround(dur / period)); }

using Source = Vec (*)(int, Rng&, double);
using Shape = std::function<void(Vec&)>;

// source -> shape -> drop the filter warm-up -> loopable
Vec bed(int n, double fs, Rng& rng, Source source, const Shape& shape = nullptr) {
  const int f = (int)std::llround(0.6 * fs), w = (int)std::llround(0.3 * fs);
  Vec x = source(n + f + w, rng, fs);
  if (shape) shape(x);
  return loopify(x.data() + w, n + f, n);
}

Vec rWhite(int n, double fs, Rng& rng) { Vec x = bed(n, fs, rng, whiteNoise); unit(x); return x; }
Vec rPink(int n, double fs, Rng& rng) { Vec x = bed(n, fs, rng, pinkNoise); unit(x); return x; }
Vec rBrown(int n, double fs, Rng& rng) { Vec x = bed(n, fs, rng, brownNoise); unit(x); return x; }

// Pink noise confined to the speech band, which is what office sound masking does.
Vec rSpeech(int n, double fs, Rng& rng) {
  Vec x = bed(n, fs, rng, pinkNoise, [&](Vec& v) { highpass(v, 250, fs, 2); lowpass(v, 4000, fs, 2); });
  unit(x);
  return x;
}

// Steady hiss with a slow swell, plus randomly placed damped pings for drops.
Vec rRain(int n, double fs, Rng& rng) {
  const double dur = n / fs;
  Vec hiss = bed(n, fs, rng, whiteNoise, [&](Vec& v) { highpass(v, 1000, fs, 2); lowpass(v, 8500, fs, 2); });
  unit(hiss);
  Vec swell = periodic(n, {{cyc(dur, 9), 0.5}, {cyc(dur, 4.3), 0.35}, {cyc(dur, 2.1), 0.25}}, rng);
  for (int i = 0; i < n; i++) hiss[(size_t)i] *= 1 + 0.25 * swell[(size_t)i];
  const int drops = (int)std::llround(70 * dur);
  for (int d = 0; d < drops; d++) {
    const int t0 = (int)(rng.next() * n);
    const double tau = (0.0008 + rng.next() * 0.0025) * fs;
    const double w = TAU * (1500 + rng.next() * 4500) / fs;
    const double amp = 3.5 * std::pow(rng.next(), 3);
    const int len = std::min(n, (int)std::ceil(tau * 6));
    for (int k = 0; k < len; k++) hiss[(size_t)((t0 + k) % n)] += amp * std::exp(-k / tau) * std::sin(w * k);
  }
  unit(hiss);
  return hiss;
}

// Low rumble that breathes with the swell, and surf hiss that peaks a moment later.
Vec rOcean(int n, double fs, Rng& rng) {
  const double dur = n / fs;
  Vec rumble = bed(n, fs, rng, whiteNoise, [&](Vec& v) { lowpass(v, 220, fs, 2); });
  unit(rumble);
  Vec surf = bed(n, fs, rng, whiteNoise, [&](Vec& v) { highpass(v, 600, fs, 1); lowpass(v, 4500, fs, 2); });
  unit(surf);
  Vec raw = periodic(n, {{cyc(dur, 12), 1.0}, {cyc(dur, 9), 0.8}, {cyc(dur, 7.2), 0.55}, {cyc(dur, 5.1), 0.3}}, rng);
  double lo = 1e30, hi = -1e30;
  for (double v : raw) { lo = std::min(lo, v); hi = std::max(hi, v); }
  Vec swell((size_t)n);
  for (int i = 0; i < n; i++) swell[(size_t)i] = std::pow((raw[(size_t)i] - lo) / (hi - lo), 2.2);
  const int lag = (int)std::llround(0.9 * fs);
  Vec out((size_t)n);
  for (int i = 0; i < n; i++) {
    const double foam = swell[(size_t)((i + n - lag) % n)];
    out[(size_t)i] = rumble[(size_t)i] * (0.25 + 0.75 * std::sqrt(swell[(size_t)i])) + 1.2 * surf[(size_t)i] * foam;
  }
  unit(out);
  return out;
}

// White noise through a band-pass whose centre drifts, with slow gusts.
Vec rWind(int n, double fs, Rng& rng) {
  const double dur = n / fs;
  const int f = (int)std::llround(0.6 * fs), w = (int)std::llround(0.3 * fs), len = n + f + w;
  Vec src = whiteNoise(len, rng, fs);
  Vec drift = periodic(n, {{cyc(dur, 13), 1.0}, {cyc(dur, 7.5), 0.6}, {cyc(dur, 3.3), 0.25}}, rng);
  Vec gust = periodic(n, {{cyc(dur, 18), 1.0}, {cyc(dur, 10), 0.7}, {cyc(dur, 4.6), 0.3}}, rng);
  Vec out((size_t)len);
  const double q = 1 / 2.2;
  double low = 0, band = 0, F = 0, vol = 0;
  for (int i = 0; i < len; i++) {
    if ((i & 31) == 0) {  // drift and gusts move slowly: refresh the control values every 32 samples
      const int m = (((i - w) % n) + n) % n;
      F = 2 * std::sin(3.14159265358979323846 * 420 * std::pow(2.0, 1.2 * drift[(size_t)m]) / fs);
      vol = 0.35 + 0.65 * std::pow(0.5 + 0.5 * gust[(size_t)m], 1.6);
    }
    low += F * band;
    const double high = src[(size_t)i] - low - q * band;
    band += F * high;
    out[(size_t)i] = band * vol;
  }
  Vec x = loopify(out.data() + w, n + f, n);
  unit(x);
  return x;
}

// Dull roar plus sparse, heavy-tailed pops that come in short clusters.
Vec rFire(int n, double fs, Rng& rng) {
  const double dur = n / fs;
  Vec roar = bed(n, fs, rng, whiteNoise, [&](Vec& v) { lowpass(v, 700, fs, 2); highpass(v, 50, fs, 1); });
  unit(roar);
  Vec flick = periodic(n, {{cyc(dur, 2.3), 1.0}, {cyc(dur, 1.1), 0.6}, {cyc(dur, 0.6), 0.3}}, rng);
  Vec out((size_t)n);
  for (int i = 0; i < n; i++) out[(size_t)i] = 0.5 * roar[(size_t)i] * (0.8 + 0.25 * flick[(size_t)i]);
  const int events = (int)std::llround(11 * dur);
  for (int e = 0; e < events; e++) {
    const int t0 = (int)(rng.next() * n);
    const int pops = 1 + (int)(rng.next() * 3);
    for (int p = 0; p < pops; p++) {
      const int tp = t0 + (int)std::llround(p * (0.012 + rng.next() * 0.05) * fs);
      const double amp = 0.05 + 2.2 * std::pow(rng.next(), 3);
      const int len = (int)std::llround((0.0012 + rng.next() * 0.004) * fs);
      const double tau = len * 0.35;
      double prev = 0;
      for (int k = 0; k < len; k++) {
        const double c = rng.next() * 2 - 1;
        out[(size_t)((tp + k) % n)] += amp * (c - prev) * 0.5 * std::exp(-k / tau);
        prev = c;
      }
    }
  }
  unit(out);
  return out;
}

// Low-passed pink noise with a faint engine hum. Hum frequencies are snapped to a whole number of
// cycles per loop so they tile exactly.
Vec rCabin(int n, double fs, Rng& rng) {
  const double dur = n / fs;
  Vec body = bed(n, fs, rng, pinkNoise, [&](Vec& v) { lowpass(v, 1100, fs, 2); });
  unit(body);
  Vec beat = periodic(n, {{cyc(dur, 8), 0.3}}, rng);
  Vec out((size_t)n, 0.0);
  const double tones[3][2] = {{58, 0.20}, {116, 0.12}, {174.5, 0.07}};
  for (auto& t : tones) {
    const double w = TAU * std::max(1LL, std::llround(t[0] * dur)) / n, ph = rng.next() * TAU;
    for (int i = 0; i < n; i++) out[(size_t)i] += t[1] * std::sin(w * i + ph) * (1 + 0.2 * beat[(size_t)i]);
  }
  for (int i = 0; i < n; i++) out[(size_t)i] += body[(size_t)i];
  unit(out);
  return out;
}

Vec renderOne(int id, int n, double fs, Rng& rng) {
  switch (id) {
    case 0: return rBrown(n, fs, rng);
    case 1: return rPink(n, fs, rng);
    case 2: return rWhite(n, fs, rng);
    case 3: return rSpeech(n, fs, rng);
    case 4: return rRain(n, fs, rng);
    case 5: return rOcean(n, fs, rng);
    case 6: return rWind(n, fs, rng);
    case 7: return rFire(n, fs, rng);
    default: return rCabin(n, fs, rng);
  }
}

}  // namespace

std::shared_ptr<Loop> renderLoop(int id, uint32_t seed, int baseRate) {
  const SoundSpec& sp = SOUNDS[id];
  const int fs = (int)std::llround(baseRate * sp.scale);
  const int n = (int)std::llround(sp.seconds * fs);
  auto loop = std::make_shared<Loop>();
  loop->rate = fs;
  Rng rl(seed), rr(seed ^ 0x9E3779B9u);
  Vec L = renderOne(id, n, fs, rl), R = renderOne(id, n, fs, rr);
  for (int c = 0; c < 2; c++) {
    const Vec& v = c == 0 ? L : R;
    loop->ch[c].resize((size_t)n);
    for (int i = 0; i < n; i++) loop->ch[c][(size_t)i] = (float)(v[(size_t)i] * TARGET_RMS);
  }
  return loop;
}
