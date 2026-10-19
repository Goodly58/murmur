/* Audio engine: sound loops, beats, buses, chime, local music.
   Sounds are rendered once (dsp.js, in a worker) into looping buffers, so playback is
   BufferSource -> Gain with no per-sample JavaScript. The context is suspended whenever idle. */
import { CHANNELS } from './data.js';

const curve = v => Math.pow(Math.max(0, Math.min(100, v)) / 100, 1.5);
const chanGain = (id, v) => curve(v) * MurmurDSP.LOUD[id];
const toneGain = v => 0.3 * curve(v);

let ctx = null, g = null;
let running = false;          // audio is wanted (the play state)
let suspendTimer = 0;
let want = { levels: {}, beat: null };
let tau = 0.15;               // ramp speed for level changes: small = quick, large = slow crossfade
let masterVol = 0.7, fadeMul = 1, soundMul = 1, mediaMul = 1;
const live = new Map();       // channel id -> { src, gain, stopped }
const cache = new Map();      // channel id -> AudioBuffer (dropped 30 s after a channel stops)
let tone = null;

function ensure() {
  if (ctx) return ctx;
  const AC = window.AudioContext || window.webkitAudioContext;
  ctx = new AC({ latencyHint: 'playback' }); // bigger buffers, fewer wake-ups, less power
  const n = {
    soundBus: ctx.createGain(), mediaBus: ctx.createGain(), mix: ctx.createGain(),
    master: ctx.createGain(), fade: ctx.createGain(),
    limiter: ctx.createDynamicsCompressor(), analyser: ctx.createAnalyser()
  };
  n.limiter.threshold.value = -6;  // safety limiter: stacked sounds can never clip
  n.limiter.knee.value = 0;
  n.limiter.ratio.value = 20;
  n.limiter.attack.value = 0.003;
  n.limiter.release.value = 0.25;
  n.analyser.fftSize = 4096;
  n.analyser.smoothingTimeConstant = 0.78;
  n.analyser.minDecibels = -105;
  n.analyser.maxDecibels = -28;
  n.soundBus.connect(n.mix);
  n.mediaBus.connect(n.mix);
  n.mix.connect(n.analyser);       // the visualiser sees the mix before master volume
  n.mix.connect(n.master);
  n.master.connect(n.fade);
  n.fade.connect(n.limiter);
  n.limiter.connect(ctx.destination);
  g = n;
  applyBuses(true);
  return ctx;
}

function applyBuses(instant) {
  if (!ctx) return;
  const t = ctx.currentTime;
  const set = (param, v, k) => (instant ? (param.value = v) : param.setTargetAtTime(v, t, k));
  set(g.master.gain, Math.pow(masterVol, 1.5) * fadeMul, 0.12);
  set(g.soundBus.gain, soundMul, 0.12);
  set(g.mediaBus.gain, mediaMul, 0.12);
}

function wake() {
  ensure();
  clearTimeout(suspendTimer);
  if (ctx.state === 'suspended') ctx.resume().catch(() => {});
}

/* ---- rendering (off the main thread) ---- */

let worker = null, workerIdle = 0, token = 0;
const waiting = new Map();

function toBuffer(r) {
  const buf = ctx.createBuffer(2, r.channels[0].length, r.rate);
  buf.copyToChannel(r.channels[0], 0);
  buf.copyToChannel(r.channels[1], 1);
  return buf;
}

function render(id) {
  return new Promise(resolve => {
    const seed = (Math.random() * 4294967296) >>> 0;
    const rate = Math.min(ctx.sampleRate, 48000);
    const inline = () => resolve(toBuffer(MurmurDSP.render(id, seed, rate)));
    try {
      if (!worker) {
        worker = new Worker('worker.js');
        worker.onmessage = e => {
          const job = waiting.get(e.data.token);
          waiting.delete(e.data.token);
          if (job) job.resolve(toBuffer(e.data));
          clearTimeout(workerIdle);
          workerIdle = setTimeout(() => { if (!waiting.size && worker) { worker.terminate(); worker = null; } }, 15000);
        };
        worker.onerror = () => {
          const jobs = [...waiting.values()];
          waiting.clear();
          if (worker) worker.terminate();
          worker = null;
          for (const j of jobs) j.fallback();
        };
      }
      const t = ++token;
      waiting.set(t, { resolve: b => resolve(b), fallback: inline });
      clearTimeout(workerIdle);
      worker.postMessage({ id, seed, rate, token: t });
    } catch (e) {
      inline();
    }
  });
}

/* ---- channels ---- */

async function startChannel(id) {
  if (live.has(id)) return;
  const entry = { src: null, gain: null, stopped: false };
  live.set(id, entry);
  wake();
  let buf = cache.get(id);
  if (!buf) { buf = await render(id); cache.set(id, buf); }
  if (entry.stopped) return;
  const gain = ctx.createGain();
  gain.gain.value = 0;
  gain.connect(g.soundBus);
  const src = ctx.createBufferSource();
  src.buffer = buf;
  src.loop = true;
  src.connect(gain);
  src.start(0, Math.random() * buf.duration);
  entry.src = src;
  entry.gain = gain;
  gain.gain.setTargetAtTime(chanGain(id, want.levels[id] || 0), ctx.currentTime, tau);
}

function stopChannel(id) {
  const entry = live.get(id);
  if (!entry) return;
  live.delete(id);
  entry.stopped = true;
  if (entry.gain) {
    entry.gain.gain.setTargetAtTime(0, ctx.currentTime, Math.max(0.08, tau * 0.6));
    setTimeout(() => { try { entry.src.stop(); } catch (e) {} entry.src.disconnect(); entry.gain.disconnect(); }, Math.max(700, tau * 4000));
  }
  setTimeout(() => { if (!live.has(id)) cache.delete(id); }, 30000);
  idleCheck();
}

/* ---- beats ---- */

function pulseWave() {
  // Square wave with Lanczos-smoothed harmonics: crisp on/off without clicks or overshoot.
  const K = 7, real = new Float32Array(K + 1), imag = new Float32Array(K + 1);
  for (let k = 1; k <= K; k += 2) {
    const x = Math.PI * k / (K + 2);
    imag[k] = (4 / (Math.PI * k)) * (Math.sin(x) / x);
  }
  return ctx.createPeriodicWave(real, imag);
}

function startTone(mode) {
  wake();
  const out = ctx.createGain();
  out.gain.value = 0;
  out.connect(g.soundBus);
  if (mode === 'binaural') {
    const l = ctx.createOscillator(), r = ctx.createOscillator(), m = ctx.createChannelMerger(2);
    l.connect(m, 0, 0);
    r.connect(m, 0, 1);
    m.connect(out);
    l.start(); r.start();
    tone = { mode, out, l, r, nodes: [l, r, m] };
  } else {
    const osc = ctx.createOscillator(), am = ctx.createGain(), lfo = ctx.createOscillator(), depth = ctx.createGain();
    am.gain.value = 0.5;      // gain = 0.5 + 0.5 * pulse, swinging between 0 and 1
    depth.gain.value = 0.5;
    lfo.setPeriodicWave(pulseWave());
    lfo.connect(depth);
    depth.connect(am.gain);
    osc.connect(am);
    am.connect(out);
    osc.start(); lfo.start();
    tone = { mode, out, osc, lfo, nodes: [osc, lfo, am, depth] };
  }
}

function stopTone() {
  if (!tone) return;
  const t = tone;
  tone = null;
  t.out.gain.setTargetAtTime(0, ctx.currentTime, 0.08);
  setTimeout(() => {
    for (const n of t.nodes) { try { if (n.stop) n.stop(); } catch (e) {} n.disconnect(); }
    t.out.disconnect();
  }, 700);
  idleCheck();
}

function reconcileTone() {
  const b = want.beat;
  if (!b) { stopTone(); return; }
  if (tone && tone.mode !== b.mode) stopTone();
  if (!tone) startTone(b.mode);
  const now = ctx.currentTime, carrier = b.mode === 'binaural' ? 200 : 240;
  if (tone.mode === 'binaural') {
    tone.l.frequency.setTargetAtTime(carrier - b.hz / 2, now, 0.05);
    tone.r.frequency.setTargetAtTime(carrier + b.hz / 2, now, 0.05);
  } else {
    tone.osc.frequency.setTargetAtTime(carrier, now, 0.05);
    tone.lfo.frequency.setTargetAtTime(b.hz, now, 0.05);
  }
  tone.out.gain.setTargetAtTime(toneGain(b.level), now, tau);
}

/* ---- reconcile what is wanted with what is playing ---- */

function reconcile() {
  if (!running) return;
  for (const c of CHANNELS) {
    const v = want.levels[c.id] || 0, e = live.get(c.id);
    if (v > 0) {
      if (!e) startChannel(c.id);
      else if (e.gain) e.gain.gain.setTargetAtTime(chanGain(c.id, v), ctx.currentTime, tau);
    } else if (e) stopChannel(c.id);
  }
  reconcileTone();
}

function idleCheck() {
  clearTimeout(suspendTimer);
  suspendTimer = setTimeout(() => {
    const musicOn = music.el && !music.el.paused;
    if (ctx && ctx.state === 'running' && !live.size && !tone && !musicOn) ctx.suspend();
  }, 3000);
}

/* ---- local music ---- */

const music = {
  tracks: [], index: -1, el: null, node: null, onchange: null,
  _el() {
    if (this.el) return this.el;
    const a = new Audio();
    a.preload = 'auto';
    a.addEventListener('ended', () => this.step(1, true));
    a.addEventListener('error', () => this.step(1, true));
    this.el = a;
    return a;
  },
  _route() {
    if (this.node) return;
    ensure();
    this.node = ctx.createMediaElementSource(this._el());
    this.node.connect(g.mediaBus);
  },
  _emit() { if (this.onchange) this.onchange(); },
  add(files) {
    for (const f of files) this.tracks.push({ name: f.name.replace(/\.[^.]+$/, ''), url: URL.createObjectURL(f) });
    if (this.index < 0) this._load(0);
    this._emit();
  },
  _load(i) {
    this.index = Math.max(0, Math.min(i, this.tracks.length - 1));
    this._el().src = this.tracks[this.index].url;
  },
  async play() {
    if (!this.tracks.length) return;
    this._route();
    wake();
    this._el().play().catch(() => {});
    this._emit();
  },
  pause() { if (this.el) this.el.pause(); this._emit(); },
  step(dir, autoplay) {
    if (!this.tracks.length) return;
    const wasPlaying = autoplay || (this.el && !this.el.paused);
    this._load((this.index + dir + this.tracks.length) % this.tracks.length);
    this._emit();
    if (wasPlaying && running) this.play();
  },
  clear() {
    this.pause();
    for (const t of this.tracks) URL.revokeObjectURL(t.url);
    this.tracks = [];
    this.index = -1;
    if (this.el) this.el.removeAttribute('src');
    this._emit();
  }
};

/* ---- public API ---- */

function note(t, freq, when, len, amp) {
  // One bell strike: three inharmonic partials with exponential decay.
  for (const [mult, a, decay] of [[1, 1, 1], [2.76, 0.35, 0.5], [5.4, 0.12, 0.25]]) {
    const o = ctx.createOscillator(), e = ctx.createGain();
    o.frequency.value = freq * mult;
    e.gain.setValueAtTime(0.0001, when);
    e.gain.exponentialRampToValueAtTime(amp * a, when + 0.01);
    e.gain.exponentialRampToValueAtTime(0.0001, when + len * decay);
    o.connect(e);
    e.connect(g.master);
    o.start(when);
    o.stop(when + len * decay + 0.05);
    o.onended = () => { o.disconnect(); e.disconnect(); };
  }
}

export const engine = {
  get ctx() { return ctx; },
  get analyser() { return g ? g.analyser : null; },
  get running() { return running; },
  music,

  async play() {
    running = true;
    clearTimeout(suspendTimer);
    ensure();
    if (ctx.state !== 'running') { try { await ctx.resume(); } catch (e) {} }
    g.fade.gain.cancelScheduledValues(ctx.currentTime);
    g.fade.gain.setTargetAtTime(1, ctx.currentTime, 0.15);
    reconcile();
  },

  pause() {
    running = false;
    if (!ctx) return;
    g.fade.gain.cancelScheduledValues(ctx.currentTime);
    g.fade.gain.setTargetAtTime(0, ctx.currentTime, 0.05);
    clearTimeout(suspendTimer);
    suspendTimer = setTimeout(() => { if (!running && ctx.state === 'running') ctx.suspend(); }, 450);
  },

  /* levels: {channelId: 0..100}; beat: {mode:'binaural'|'isochronic', hz, level} or null. */
  setMix(levels, beat, ramp) {
    want = { levels, beat };
    tau = ramp == null ? 0.15 : ramp;
    reconcile();
  },

  setMaster(v01) { masterVol = v01; applyBuses(); },
  setBlend(sounds, media) { soundMul = sounds; mediaMul = media; applyBuses(); },
  setFade(f) { fadeMul = f; applyBuses(); },

  chime(kind) {
    if (!ctx || ctx.state !== 'running') return;
    const t = ctx.currentTime + 0.05;
    const E5 = 659.25, B5 = 987.77, G5 = 783.99, E6 = 1318.5;
    if (kind === 'start') { note(t, E5, t, 2.2, 0.16); note(t, B5, t + 0.16, 2.2, 0.14); }
    else if (kind === 'break') { note(t, B5, t, 2.2, 0.15); note(t, G5, t + 0.2, 2.6, 0.14); }
    else if (kind === 'done') { note(t, E5, t, 2.4, 0.16); note(t, G5, t + 0.2, 2.4, 0.15); note(t, E6, t + 0.4, 3, 0.15); }
  }
};
