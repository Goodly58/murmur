/* Live spectrum of the mix on a log-frequency axis. It is the one thing on the page that animates,
   so it is kept cheap: 15 fps, a frame is only painted when the shape moved enough to see, it runs
   only while there is sound to show, and the browser stops calling it when the tab is hidden. */

export const FMIN = 30, FMAX = 16000;
export const stats = { frames: 0, draws: 0 };
const POINTS = 96, FPS = 15, EPS = 0.01;

export function createSpectrum(canvas, getAnalyser, isLive) {
  const c2 = canvas.getContext('2d');
  const vals = new Float32Array(POINTS);   // smoothed target
  const shown = new Float32Array(POINTS);  // what is currently painted
  let w = 1, h = 1, raf = 0, bins = null, buf = null, binKey = '', grad = null, idle = 0;
  let accent = [242, 181, 99], line = 'rgba(255,255,255,.06)';
  const reduce = matchMedia('(prefers-reduced-motion: reduce)');

  function palette() {
    const s = getComputedStyle(document.documentElement);
    const hex = (s.getPropertyValue('--accent').trim() || '#f2b563').replace('#', '');
    accent = [0, 2, 4].map(i => parseInt(hex.slice(i, i + 2), 16));
    line = s.getPropertyValue('--line').trim() || line;
    grad = null;
  }

  function resize() {
    const r = canvas.getBoundingClientRect();
    const dpr = Math.min(1.25, window.devicePixelRatio || 1); // fewer pixels to paint; the curve stays smooth
    w = Math.max(1, Math.round(r.width));
    h = Math.max(1, Math.round(r.height));
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
    c2.setTransform(dpr, 0, 0, dpr, 0, 0);
    palette();
    draw();
  }

  function buildBins(an) {
    const key = an.context.sampleRate + ':' + an.fftSize;
    if (key === binKey) return;
    binKey = key;
    const hz = an.context.sampleRate / an.fftSize;
    bins = [];
    for (let i = 0; i < POINTS; i++) {
      bins.push([FMIN * Math.pow(FMAX / FMIN, i / POINTS) / hz, FMIN * Math.pow(FMAX / FMIN, (i + 1) / POINTS) / hz]);
    }
    buf = new Uint8Array(an.frequencyBinCount);
  }

  function sample(i) {
    const a = bins[i][0], b = bins[i][1];
    if (b - a < 1) {                       // narrower than one FFT bin: interpolate
      const c = (a + b) / 2, k = Math.floor(c), t = c - k;
      return buf[k] * (1 - t) + buf[k + 1] * t;
    }
    let s = 0, n = 0;
    for (let k = Math.floor(a); k <= Math.floor(b) && k < buf.length; k++) { s += buf[k]; n++; }
    return n ? s / n : 0;
  }

  function draw() {
    c2.clearRect(0, 0, w, h);
    c2.strokeStyle = line;
    c2.lineWidth = 1;
    for (let k = 1; k <= 3; k++) {
      const y = Math.round(h - (h * k) / 4) + 0.5;
      c2.beginPath(); c2.moveTo(0, y); c2.lineTo(w, y); c2.stroke();
    }
    const xs = new Float32Array(POINTS), ys = new Float32Array(POINTS);
    for (let i = 0; i < POINTS; i++) {
      shown[i] = vals[i];
      xs[i] = (i / (POINTS - 1)) * w;
      ys[i] = h - 2 - Math.pow(vals[i], 1.35) * (h - 8);
    }
    const p = new Path2D();
    p.moveTo(xs[0], ys[0]);
    for (let i = 1; i < POINTS - 1; i++) p.quadraticCurveTo(xs[i], ys[i], (xs[i] + xs[i + 1]) / 2, (ys[i] + ys[i + 1]) / 2);
    p.lineTo(xs[POINTS - 1], ys[POINTS - 1]);
    const f = new Path2D(p);
    f.lineTo(w, h); f.lineTo(0, h); f.closePath();
    if (!grad) {
      grad = c2.createLinearGradient(0, 0, 0, h);
      grad.addColorStop(0, `rgba(${accent}, .34)`);
      grad.addColorStop(1, `rgba(${accent}, 0)`);
    }
    c2.fillStyle = grad;
    c2.fill(f);
    c2.strokeStyle = `rgba(${accent}, .95)`;
    c2.lineWidth = 1.5;
    c2.lineJoin = 'round';
    c2.stroke(p);
  }

  /* A timer instead of requestAnimationFrame: a pending rAF makes the browser produce a frame
     every 16 ms even when the callback paints nothing, and that alone costs more than the drawing. */
  function frame() {
    raf = setTimeout(frame, 1000 / FPS);
    if (document.hidden) return;
    stats.frames++;
    const an = getAnalyser(), live = an && isLive();
    let peak = 0, moved = 0;
    if (live) { buildBins(an); an.getByteFrequencyData(buf); }
    for (let i = 0; i < POINTS; i++) {
      vals[i] = live ? vals[i] + (sample(i) / 255 - vals[i]) * 0.45 : vals[i] * 0.82;
      peak = Math.max(peak, vals[i]);
      moved += Math.abs(vals[i] - shown[i]);
    }
    moved /= POINTS;                      // average change: single jittery bins do not keep the loop awake
    if (moved > EPS) { draw(); stats.draws++; idle = 0; }
    else if (++idle > FPS * 2) stop();    // settled: keep the last frame, stop the loop
    if (!live && peak < 0.004) { draw(); stop(); } // faded out: nothing left to show
  }

  /* Wakes the loop. Call it whenever the mix changes; it puts itself back to sleep once the shape settles. */
  function start() {
    idle = 0;
    if (reduce.matches) { draw(); return; }
    if (raf) return;
    raf = setTimeout(frame, 0);
  }

  function stop() { clearTimeout(raf); raf = 0; }

  new ResizeObserver(resize).observe(canvas);
  matchMedia('(prefers-color-scheme: light)').addEventListener('change', () => { palette(); draw(); });
  document.addEventListener('visibilitychange', () => { if (!document.hidden) start(); });
  resize();
  return { start, stop, resize };
}
