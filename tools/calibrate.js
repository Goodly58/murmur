/* Checks the noise renderers and prints the LOUD table for dsp.js.
   Run: node tools/calibrate.js
   Reports per sound: RMS/peak, spectral slope, loop-seam level, render time, and the
   A-weighted gain that makes equal slider positions sound about equally loud. */
'use strict';
const D = require('../dsp.js');

const BASE = 48000;
const NFFT = 16384;

function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const a = i + k, b = a + len / 2;
        const tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
        const nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
      }
    }
  }
}

/* One-sided Welch PSD, normalised so the bins sum to the mean square. */
function welch(x) {
  const w = new Float64Array(NFFT);
  let U = 0;
  for (let i = 0; i < NFFT; i++) { w[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / NFFT); U += w[i] * w[i]; }
  const psd = new Float64Array(NFFT / 2 + 1);
  let segs = 0;
  for (let s = 0; s + NFFT <= x.length; s += NFFT / 2) {
    const re = new Float64Array(NFFT), im = new Float64Array(NFFT);
    for (let i = 0; i < NFFT; i++) re[i] = x[s + i] * w[i];
    fft(re, im);
    for (let k = 0; k <= NFFT / 2; k++) {
      const p = (re[k] * re[k] + im[k] * im[k]) / (NFFT * U);
      psd[k] += (k === 0 || k === NFFT / 2) ? p : 2 * p;
    }
    segs++;
  }
  for (let k = 0; k < psd.length; k++) psd[k] /= segs;
  return psd;
}

function aWeightPower(f) {
  if (f <= 0) return 0;
  const f2 = f * f;
  const ra = (12194 ** 2 * f2 * f2) /
    ((f2 + 20.6 ** 2) * Math.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194 ** 2));
  return (ra * 1.2589) ** 2; // +2 dB so 1 kHz is 0 dB
}

function rms(x) { let e = 0; for (let i = 0; i < x.length; i++) e += x[i] * x[i]; return Math.sqrt(e / x.length); }

const rows = [];
for (const id of Object.keys(D.SOUNDS)) {
  const t0 = process.hrtime.bigint();
  const r = D.render(id, 12345, BASE);
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  const fs = r.rate, L = r.channels[0], R = r.channels[1], n = L.length;

  let mean = 0, peak = 0;
  for (let i = 0; i < n; i++) { mean += L[i]; peak = Math.max(peak, Math.abs(L[i])); }
  mean /= n;

  const psd = welch(L), df = fs / NFFT;
  // slope in dB/octave of power density, 100 Hz .. min(8 kHz, 0.8 Nyquist)
  const hiF = Math.min(8000, fs * 0.4);
  let sx = 0, sy = 0, sxx = 0, sxy = 0, m = 0;
  for (let k = Math.ceil(100 / df); k * df < hiF; k++) {
    const x = Math.log2(k * df), y = 10 * Math.log10(psd[k] / df + 1e-30);
    sx += x; sy += y; sxx += x * x; sxy += x * y; m++;
  }
  const slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);

  let aw = 0;
  for (let k = 1; k < psd.length; k++) aw += psd[k] * aWeightPower(k * df);
  aw = Math.sqrt(aw);

  // loop seam: level in a 60 ms window centred on the join, relative to the whole loop
  const two = new Float32Array(2 * n); two.set(L); two.set(L, n);
  const half = Math.round(0.03 * fs);
  const seam = rms(two.subarray(n - half, n + half)) / rms(L);
  let dsum = 0; for (let i = 1; i < n; i++) dsum += (L[i] - L[i - 1]) ** 2;
  const jump = Math.abs(L[0] - L[n - 1]) / Math.sqrt(dsum / (n - 1));

  let corr = 0; for (let i = 0; i < n; i++) corr += L[i] * R[i];
  corr /= n * rms(L) * rms(R);

  rows.push({ id, fs, n, rms: rms(L), mean, crest: peak / rms(L), slope, aw, seam, jump, corr, ms });
}

const maxRatio = Math.max(...rows.map(r => 1 / r.aw)); // loudest-needed sound sets the ceiling
const CEIL = 1.5;                                        // largest allowed per-sound gain
const T = CEIL / maxRatio;
console.log('id       rate   sec  rms    mean      crest slope  seam  jump  LRcorr  ms    gain');
const loud = {};
for (const r of rows) {
  const g = T / r.aw;
  loud[r.id] = +g.toFixed(3);
  console.log(
    r.id.padEnd(8), String(r.fs).padStart(5), (r.n / r.fs).toFixed(0).padStart(4),
    r.rms.toFixed(3), r.mean.toExponential(1).padStart(8), r.crest.toFixed(1).padStart(6),
    r.slope.toFixed(1).padStart(6), r.seam.toFixed(2).padStart(5), r.jump.toFixed(2).padStart(5),
    r.corr.toFixed(3).padStart(7), r.ms.toFixed(0).padStart(5), loud[r.id].toFixed(3).padStart(7));
}
console.log('\nLOUD =', JSON.stringify(loud));
