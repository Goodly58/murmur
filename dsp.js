/* Murmur DSP
   Pure functions that synthesise seamlessly looping noise textures into Float32Arrays.
   Loaded by the page as a classic script and by tools/calibrate.js via require().

   Why this is cheap: a sound is rendered once into a buffer, then the audio thread just
   loops it. Nothing runs per-sample in JavaScript while a sound plays. */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.MurmurDSP = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const TAU = Math.PI * 2;
  const TARGET_RMS = 0.25;

  /* Per-sound gain so that equal slider positions sound about equally loud (A-weighted).
     Filled in by tools/calibrate.js. */
  const LOUD = {
    white: 0.485, pink: 0.578, brown: 1.5, speech: 0.38,
    rain: 0.374, ocean: 0.546, wind: 0.455, fire: 0.561, cabin: 1.202
  };

  /* seconds = loop length, scale = sample rate as a fraction of the context rate.
     Low-frequency textures are rendered at half rate: half the RAM, same sound. */
  const SOUNDS = {
    white:  { seconds: 10, scale: 1 },
    pink:   { seconds: 12, scale: 1 },
    brown:  { seconds: 14, scale: 0.5 },
    speech: { seconds: 12, scale: 0.5 },
    rain:   { seconds: 20, scale: 1 },
    ocean:  { seconds: 36, scale: 0.5 },
    wind:   { seconds: 36, scale: 0.5 },
    fire:   { seconds: 20, scale: 0.667 },
    cabin:  { seconds: 16, scale: 0.5 }
  };

  function rngFrom(seed) {
    let a = seed >>> 0;
    return function () {
      a = (a + 0x6D2B79F5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  /* ---------- building blocks ---------- */

  function white(n, rng) {
    const x = new Float32Array(n);
    for (let i = 0; i < n; i++) x[i] = rng() * 2 - 1;
    return x;
  }

  /* Paul Kellet's refined pink filter: -3 dB/octave from ~10 Hz to Nyquist. */
  function pinkNoise(n, rng) {
    const x = new Float32Array(n);
    let b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (let i = 0; i < n; i++) {
      const w = rng() * 2 - 1;
      b0 = 0.99886 * b0 + w * 0.0555179;
      b1 = 0.99332 * b1 + w * 0.0750759;
      b2 = 0.96900 * b2 + w * 0.1538520;
      b3 = 0.86650 * b3 + w * 0.3104856;
      b4 = 0.55000 * b4 + w * 0.5329522;
      b5 = -0.7616 * b5 - w * 0.0168980;
      x[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
      b6 = w * 0.115926;
    }
    return x;
  }

  /* Leaky integrator: -6 dB/octave above ~8 Hz, then a 25 Hz high-pass so no energy is
     wasted below what speakers and headphones can reproduce. */
  function brownNoise(n, rng, fs) {
    const x = new Float32Array(n);
    const a = Math.exp(-TAU * 8 / fs);
    let y = 0;
    for (let i = 0; i < n; i++) { y = a * y + (rng() * 2 - 1); x[i] = y; }
    return highpass(x, 25, fs, 1);
  }

  /* One-pole filters, applied `order` times (6 dB/octave each). In place. */
  function lowpass(x, fc, fs, order) {
    const a = 1 - Math.exp(-TAU * fc / fs);
    for (let k = 0; k < (order || 1); k++) {
      let y = 0;
      for (let i = 0; i < x.length; i++) { y += a * (x[i] - y); x[i] = y; }
    }
    return x;
  }

  function highpass(x, fc, fs, order) {
    const a = 1 - Math.exp(-TAU * fc / fs);
    for (let k = 0; k < (order || 1); k++) {
      let y = 0;
      for (let i = 0; i < x.length; i++) { y += a * (x[i] - y); x[i] -= y; }
    }
    return x;
  }

  /* Zero mean, RMS 1, in place. */
  function unit(x) {
    let m = 0;
    for (let i = 0; i < x.length; i++) m += x[i];
    m /= x.length;
    let e = 0;
    for (let i = 0; i < x.length; i++) { x[i] -= m; e += x[i] * x[i]; }
    const g = 1 / Math.sqrt(e / x.length || 1);
    for (let i = 0; i < x.length; i++) x[i] *= g;
    return x;
  }

  /* x holds n + f samples. Equal-power crossfade of the last f samples onto the first f,
     so the returned n samples loop with no click and no level dip. */
  function loopify(x, n) {
    const f = x.length - n;
    const out = new Float32Array(n);
    out.set(x.subarray(0, n));
    for (let i = 0; i < f; i++) {
      const t = (i + 0.5) / f * Math.PI / 2;
      out[i] = x[i] * Math.sin(t) + x[n + i] * Math.cos(t);
    }
    return out;
  }

  /* Sum of sines with a whole number of cycles per buffer, so it tiles exactly.
     parts = [[cycles, amplitude], ...]. Scaled so the largest |value| is 1. */
  function periodic(n, parts, rng) {
    const y = new Float32Array(n);
    for (const p of parts) {
      const w = TAU * p[0] / n, ph = rng() * TAU, a = p[1];
      for (let i = 0; i < n; i++) y[i] += a * Math.sin(w * i + ph);
    }
    let peak = 1e-9;
    for (let i = 0; i < n; i++) peak = Math.max(peak, Math.abs(y[i]));
    for (let i = 0; i < n; i++) y[i] /= peak;
    return y;
  }

  const cyc = (dur, period) => Math.max(1, Math.round(dur / period));

  /* Shaped noise bed: source -> shape -> drop filter warm-up -> loopable. */
  function bed(n, fs, rng, source, shape) {
    const f = Math.round(0.6 * fs), w = Math.round(0.3 * fs);
    const x = source(n + f + w, rng, fs);
    if (shape) shape(x);
    return loopify(x.subarray(w), n);
  }

  /* ---------- the sounds (each returns n samples at RMS 1) ---------- */

  const renderers = {
    white: (n, fs, rng) => unit(bed(n, fs, rng, white)),
    pink: (n, fs, rng) => unit(bed(n, fs, rng, pinkNoise)),
    brown: (n, fs, rng) => unit(bed(n, fs, rng, brownNoise)),

    /* Pink noise confined to the speech band, which is what office sound-masking does. */
    speech: (n, fs, rng) => unit(bed(n, fs, rng, pinkNoise, x => {
      highpass(x, 250, fs, 2); lowpass(x, 4000, fs, 2);
    })),

    /* Steady hiss with a slow swell, plus randomly placed damped pings for drops. */
    rain(n, fs, rng) {
      const dur = n / fs;
      const hiss = unit(bed(n, fs, rng, white, x => { highpass(x, 1000, fs, 2); lowpass(x, 8500, fs, 2); }));
      const swell = periodic(n, [[cyc(dur, 9), 0.5], [cyc(dur, 4.3), 0.35], [cyc(dur, 2.1), 0.25]], rng);
      for (let i = 0; i < n; i++) hiss[i] *= 1 + 0.25 * swell[i];
      const drops = Math.round(70 * dur);
      for (let d = 0; d < drops; d++) {
        const t0 = (rng() * n) | 0;
        const tau = (0.0008 + rng() * 0.0025) * fs;
        const w = TAU * (1500 + rng() * 4500) / fs;
        const amp = 3.5 * Math.pow(rng(), 3);
        const len = Math.min(n, Math.ceil(tau * 6));
        for (let k = 0; k < len; k++) hiss[(t0 + k) % n] += amp * Math.exp(-k / tau) * Math.sin(w * k);
      }
      return unit(hiss);
    },

    /* Low rumble that breathes with the swell, and surf hiss that peaks a moment later. */
    ocean(n, fs, rng) {
      const dur = n / fs;
      const rumble = unit(bed(n, fs, rng, white, x => lowpass(x, 220, fs, 2)));
      const surf = unit(bed(n, fs, rng, white, x => { highpass(x, 600, fs, 1); lowpass(x, 4500, fs, 2); }));
      const raw = periodic(n, [[cyc(dur, 12), 1], [cyc(dur, 9), 0.8], [cyc(dur, 7.2), 0.55], [cyc(dur, 5.1), 0.3]], rng);
      let lo = Infinity, hi = -Infinity;
      for (let i = 0; i < n; i++) { lo = Math.min(lo, raw[i]); hi = Math.max(hi, raw[i]); }
      const swell = new Float32Array(n);
      for (let i = 0; i < n; i++) swell[i] = Math.pow((raw[i] - lo) / (hi - lo), 2.2);
      const lag = Math.round(0.9 * fs), out = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        const foam = swell[(i + n - lag) % n];
        out[i] = rumble[i] * (0.25 + 0.75 * Math.sqrt(swell[i])) + 1.2 * surf[i] * foam;
      }
      return unit(out);
    },

    /* White noise through a band-pass whose centre drifts, with slow gusts. */
    wind(n, fs, rng) {
      const dur = n / fs;
      const f = Math.round(0.6 * fs), w = Math.round(0.3 * fs), len = n + f + w;
      const src = white(len, rng);
      const drift = periodic(n, [[cyc(dur, 13), 1], [cyc(dur, 7.5), 0.6], [cyc(dur, 3.3), 0.25]], rng);
      const gust = periodic(n, [[cyc(dur, 18), 1], [cyc(dur, 10), 0.7], [cyc(dur, 4.6), 0.3]], rng);
      const out = new Float32Array(len);
      const q = 1 / 2.2;
      let low = 0, band = 0, F = 0, vol = 0;
      for (let i = 0; i < len; i++) {
        if ((i & 31) === 0) { // drift and gusts move slowly: update the control values every 32 samples
          const m = (((i - w) % n) + n) % n;
          F = 2 * Math.sin(Math.PI * 420 * Math.pow(2, 1.2 * drift[m]) / fs);
          vol = 0.35 + 0.65 * Math.pow(0.5 + 0.5 * gust[m], 1.6);
        }
        low += F * band;
        const high = src[i] - low - q * band;
        band += F * high;
        out[i] = band * vol;
      }
      return unit(loopify(out.subarray(w), n));
    },

    /* Dull roar plus sparse, heavy-tailed pops that come in short clusters. */
    fire(n, fs, rng) {
      const dur = n / fs;
      const roar = unit(bed(n, fs, rng, white, x => { lowpass(x, 700, fs, 2); highpass(x, 50, fs, 1); }));
      const flick = periodic(n, [[cyc(dur, 2.3), 1], [cyc(dur, 1.1), 0.6], [cyc(dur, 0.6), 0.3]], rng);
      const out = new Float32Array(n);
      for (let i = 0; i < n; i++) out[i] = 0.5 * roar[i] * (0.8 + 0.25 * flick[i]);
      const events = Math.round(11 * dur);
      for (let e = 0; e < events; e++) {
        const t0 = (rng() * n) | 0;
        const pops = 1 + ((rng() * 3) | 0);
        for (let p = 0; p < pops; p++) {
          const tp = t0 + Math.round(p * (0.012 + rng() * 0.05) * fs);
          const amp = 0.05 + 2.2 * Math.pow(rng(), 3);
          const len = Math.round((0.0012 + rng() * 0.004) * fs);
          const tau = len * 0.35;
          let prev = 0;
          for (let k = 0; k < len; k++) {
            const c = rng() * 2 - 1;
            out[(tp + k) % n] += amp * (c - prev) * 0.5 * Math.exp(-k / tau);
            prev = c;
          }
        }
      }
      return unit(out);
    },

    /* Low-passed pink noise with a faint engine hum. Hum frequencies are snapped to a whole
       number of cycles per loop so they tile exactly. */
    cabin(n, fs, rng) {
      const dur = n / fs;
      const body = unit(bed(n, fs, rng, pinkNoise, x => lowpass(x, 1100, fs, 2)));
      const beat = periodic(n, [[cyc(dur, 8), 0.3]], rng);
      const out = new Float32Array(n);
      for (const t of [[58, 0.20], [116, 0.12], [174.5, 0.07]]) {
        const w = TAU * Math.max(1, Math.round(t[0] * dur)) / n, ph = rng() * TAU;
        for (let i = 0; i < n; i++) out[i] += t[1] * Math.sin(w * i + ph) * (1 + 0.2 * beat[i]);
      }
      for (let i = 0; i < n; i++) out[i] += body[i];
      return unit(out);
    }
  };

  /* Renders one stereo loop. L and R come from different seeds, so the two ears get
     uncorrelated noise (a wide, outside-the-head sound instead of a mono blob). */
  function render(id, seed, baseRate) {
    const def = SOUNDS[id];
    const fs = Math.round(baseRate * def.scale);
    const n = Math.round(def.seconds * fs);
    const L = renderers[id](n, fs, rngFrom(seed));
    const R = renderers[id](n, fs, rngFrom(seed ^ 0x9E3779B9));
    for (let i = 0; i < n; i++) { L[i] *= TARGET_RMS; R[i] *= TARGET_RMS; }
    return { rate: fs, channels: [L, R] };
  }

  return { SOUNDS, LOUD, TARGET_RMS, render, rngFrom };
});
