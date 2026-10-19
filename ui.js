/* Murmur UI: wires the scenes, session ring, mix tiles, media panel and spectrum to the engine. */
import { CHANNELS, BANDS, PLANS, SCENES, BREAK_MIX } from './data.js';
import { engine } from './engine.js';
import { Session } from './session.js';
import { video, parseYouTube, norm, keyOf } from './video.js';
import { createSpectrum, FMIN, FMAX } from './spectrum.js';

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const ic = (id, cls = '') => `<svg class="ic${cls ? ' ' + cls : ''}" aria-hidden="true"><use href="#${id}"/></svg>`;
const RING = 2 * Math.PI * 150;

/* ---------------- state ---------------- */

const KEY = 'murmur.v2';

function loadState() {
  const d = { scene: 'focus', plans: {}, mixes: {}, beats: {}, beatsMode: 'binaural', master: 70, blend: 50,
    videoOn: true, custom: null, tab: 'video', wide: false, log: {} };
  let s = null;
  try { s = JSON.parse(localStorage.getItem(KEY) || 'null'); } catch (e) { /* storage unavailable */ }
  if (!s || typeof s !== 'object') return d;
  const num = (v, lo, hi, def) => (v !== null && v !== '' && Number.isFinite(+v) ? clamp(+v, lo, hi) : def);
  if (SCENES.some(x => x.id === s.scene)) d.scene = s.scene;
  for (const sc of SCENES) {
    if (s.plans && sc.plans.includes(s.plans[sc.id])) d.plans[sc.id] = s.plans[sc.id];
    const m = s.mixes && s.mixes[sc.id];
    if (m && typeof m === 'object') {
      const mix = {};
      for (const c of CHANNELS) { const v = num(m[c.id], 0, 100, 0); if (v) mix[c.id] = v; }
      d.mixes[sc.id] = mix;
    }
    const b = s.beats && s.beats[sc.id];
    if (b && BANDS.some(x => x.id === b.band)) d.beats[sc.id] = { band: b.band, level: num(b.level, 0, 100, 20), on: !!b.on };
  }
  if (['off', 'binaural', 'isochronic'].includes(s.beatsMode)) d.beatsMode = s.beatsMode;
  d.master = num(s.master, 0, 100, 70);
  d.blend = num(s.blend, 0, 100, 50);
  d.videoOn = s.videoOn !== false;
  d.tab = s.tab === 'files' ? 'files' : 'video';
  d.wide = !!s.wide;
  if (s.custom && /^[\w-]{11}$/.test(s.custom.id || '')) d.custom = { id: s.custom.id };
  else if (s.custom && /^[\w-]+$/.test(s.custom.list || '')) d.custom = { list: s.custom.list };
  if (s.log && typeof s.log === 'object') {
    for (const k of Object.keys(s.log).filter(k => /^\d{4}-\d{2}-\d{2}$/.test(k)).sort().slice(-60)) d.log[k] = Math.max(0, +s.log[k] || 0);
  }
  return d;
}

const S = loadState();
let saveTimer = 0;
function writeNow() {
  clearTimeout(saveTimer);
  try { localStorage.setItem(KEY, JSON.stringify(S)); } catch (e) { /* storage unavailable */ }
}
function save() { clearTimeout(saveTimer); saveTimer = setTimeout(writeNow, 300); }
window.addEventListener('pagehide', writeNow);

let running = false;
let muted = false;
let lastFade = 1;
let closing = 0;
let vIndex = 0, vTried = 0;
const lastLevel = {};

const scene = () => SCENES.find(s => s.id === S.scene);
const planId = () => S.plans[S.scene] || scene().plans[0];
const mixOf = () => S.mixes[S.scene] || scene().mix;
const beatOf = () => S.beats[S.scene] || (scene().beat ? { ...scene().beat, on: true } : { band: 'alpha', level: 20, on: false });
const bandOf = id => BANDS.find(b => b.id === id);
const dayKey = (d = new Date()) => `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;

/* ---------------- session ---------------- */

const session = new Session({
  onTick: snap => { paintStage(snap); applyFade(snap.fade); },
  onPhase: snap => {
    engine.chime(snap.kind === 'break' ? 'break' : 'start');
    applyAudio(2.5);
    paintStage(snap);
  },
  onDone: () => { engine.chime('done'); finish(); },
  onFocus: ms => {
    const k = dayKey();
    S.log[k] = (S.log[k] || 0) + ms / 60000;
    if (++focusTicks % 30 === 0) save(); // not every second; pagehide writes the rest
  }
});
let focusTicks = 0;

/* What should be audible right now: the scene's mix, or the break mix during breaks. */
function effective() {
  const snap = session.snapshot();
  if (snap.done) return { levels: {}, beat: null };
  if (snap.kind === 'break') return { levels: BREAK_MIX, beat: null };
  const b = beatOf();
  const beat = S.beatsMode !== 'off' && b.on && b.level > 0 ? { mode: S.beatsMode, hz: bandOf(b.band).hz, level: b.level } : null;
  return { levels: mixOf(), beat };
}

function applyAudio(ramp) {
  const e = effective();
  engine.setMix(e.levels, e.beat, ramp);
  paintMix();
  spectrum.start();            // wake the spectrum; it sleeps again once the shape settles
}

function volumes() {
  const m = muted ? 0 : S.master / 100, b = S.blend / 100;
  return { m, sound: Math.pow(clamp(2 * b, 0, 1), 1.5), media: Math.pow(clamp(2 * (1 - b), 0, 1), 1.5) };
}

function applyVideoVolume() {
  const v = volumes();
  video.setVolume(Math.round(100 * Math.pow(v.m, 1.5) * v.media * lastFade));
}

function applyVolumes() {
  const v = volumes();
  engine.setMaster(v.m);
  engine.setBlend(v.sound, v.media);
  applyVideoVolume();
  spectrum.start();
}

function applyFade(f) {
  if (closing || Math.abs(f - lastFade) < 0.002) return;
  lastFade = f;
  engine.setFade(f);
  applyVideoVolume();
}

/* ---------------- play / stop ---------------- */

async function startAll(ramp = 0.15) {
  clearInterval(closing);
  closing = 0;
  lastFade = 1;
  engine.setFade(1);
  const fresh = !running;
  running = true;
  session.start();
  applyVolumes();
  applyAudio(ramp);
  paintTransport();
  await engine.play();
  if (S.tab === 'files') engine.music.play();
  else { engine.music.pause(); syncVideo(true); }
  spectrum.start();
  const snap = session.snapshot();
  if (fresh && snap.cycle && snap.kind === 'focus' && snap.elapsed < 1500) engine.chime('start');
}

function stopAll() {
  running = false;
  session.pause();
  engine.pause();
  video.pause();
  engine.music.pause();
  paintTransport();
  document.title = 'Murmur';
}

/* Plan finished: fade everything out over four seconds, then stop. */
function finish() {
  clearInterval(closing);
  const t0 = performance.now();
  closing = setInterval(() => {
    const k = Math.min(1, (performance.now() - t0) / 4000);
    lastFade = 1 - k;
    engine.setFade(lastFade);
    applyVideoVolume();
    if (k >= 1) {
      clearInterval(closing);
      closing = 0;
      stopAll();
      lastFade = 1;
      engine.setFade(1);
      applyVideoVolume();
    }
  }, 120);
}

function togglePlay() { if (running) stopAll(); else startAll(); }

/* ---------------- scenes and plans ---------------- */

function selectScene(id) {
  if (!SCENES.some(s => s.id === id) || (id === S.scene && running)) return;
  S.scene = id;
  vIndex = 0;
  vTried = 0;
  session.setPlan(planId());
  renderPlans();
  paintScenes();
  paintPlayer();
  paintStage(session.snapshot());
  startAll(running ? 1.4 : 0.15);
  save();
}

function selectPlan(id) {
  if (!scene().plans.includes(id)) return;
  S.plans[S.scene] = id;
  session.setPlan(id);
  renderPlans();
  paintStage(session.snapshot());
  if (running) { session.start(); applyAudio(1.2); }
  save();
}

/* ---------------- mix ---------------- */

function setLevel(id, v) {
  if (id === 'beats') {
    S.beats[S.scene] = { ...beatOf(), level: v, on: v > 0 };
  } else {
    const m = { ...mixOf() };
    if (v > 0) m[id] = v; else delete m[id];
    S.mixes[S.scene] = m;
  }
  if (!running) startAll(); else applyAudio(0.12);
  save();
}

function toggleTile(id) {
  if (id === 'beats') {
    if (S.beatsMode === 'off') S.beatsMode = 'binaural';
    const b = beatOf();
    S.beats[S.scene] = { ...b, on: !b.on, level: b.level || 20 };
    paintBeatsMode();
    if (!running) startAll(); else applyAudio(0.12);
    save();
    return;
  }
  const v = mixOf()[id] || 0;
  if (v > 0) lastLevel[id] = v;
  setLevel(id, v > 0 ? 0 : (lastLevel[id] || 35));
}

function resetMix() {
  delete S.mixes[S.scene];
  delete S.beats[S.scene];
  if (running) applyAudio(0.4); else paintMix();
  save();
}

function setBeatsMode(mode) {
  S.beatsMode = mode;
  paintBeatsMode();
  if (running) applyAudio(0.3); else paintMix();
  save();
}

/* ---------------- video and music ---------------- */

function wanted() {
  if (!S.videoOn) return null;
  if (S.custom) return S.custom;
  const l = scene().videos;
  return l.length ? l[vIndex % l.length] : null;
}

function setVState(s) { $('#player').dataset.state = s; }

function posterFor(w) {
  const e = norm(w);
  return e && e.id ? `https://i.ytimg.com/vi/${e.id}/mqdefault.jpg` : '';
}

function paintPlayer() {
  const w = wanted(), list = scene().videos, custom = !!S.custom;
  $('#media').classList.toggle('video-off', !S.videoOn);
  $('#vPower').setAttribute('aria-pressed', String(S.videoOn));
  $('#vClear').hidden = !custom;
  $('#vPrev').hidden = $('#vNext').hidden = custom || list.length < 2;
  const img = $('#posterImg'), src = w ? posterFor(w) : '';
  if (src) { if (img.getAttribute('src') !== src) img.src = src; img.hidden = false; } else { img.removeAttribute('src'); img.hidden = true; }
  const st = $('#player').dataset.state;
  if (!w) setVState('none');
  else if (st === 'none') setVState('idle');
  if (!w || (video.active && video.key !== keyOf(norm(w)))) $('#vTitle').textContent = '';
}

async function syncVideo(play) {
  const w = wanted();
  if (!S.videoOn || !w) video.destroy();
  paintPlayer();
  if (!S.videoOn || !w || S.tab !== 'video' || !play) return;
  if (video.key !== keyOf(norm(w))) { setVState('loading'); $('#vTitle').textContent = ''; }
  await video.load(w, $('#ytHost'), { controls: !!S.custom });
  applyVideoVolume();
}

video.cb.playing = title => {
  setVState('playing');
  // titles are full of emoji; drop them for a cleaner line
  $('#vTitle').textContent = title.replace(/[\p{Extended_Pictographic}️‍]/gu, '').replace(/\s{2,}/g, ' ').trim();
  vTried = 0;
  applyVideoVolume();
};

video.cb.fail = reason => {
  const n = scene().videos.length;
  if (reason === 'offline' || S.custom || !n || ++vTried >= n) { setVState('error'); return; }
  vIndex++;
  syncVideo(true);
};

function stepVideo(dir) {
  const n = scene().videos.length;
  if (S.custom || n < 2) return;
  vIndex = (vIndex + dir + n) % n;
  vTried = 0;
  if (!running) startAll(); else syncVideo(true);
}

function loadCustom(spec) {
  S.custom = spec;
  S.videoOn = true;
  S.tab = 'video';
  vTried = 0;
  paintTabs();
  if (!running) startAll(); else syncVideo(true);
  save();
}

function setTab(tab) {
  if (tab === S.tab) return;
  S.tab = tab;
  paintTabs();
  if (tab === 'files') { video.pause(); if (running) engine.music.play(); }
  else { engine.music.pause(); if (running) syncVideo(true); }
  save();
}

const isAudio = f => f.type.startsWith('audio/') || /\.(mp3|m4a|aac|wav|flac|ogg|oga|opus|weba)$/i.test(f.name);

function addFiles(files) {
  const list = [...files].filter(isAudio);
  if (!list.length) return;
  engine.music.add(list);
  S.tab = 'files';
  paintTabs();
  video.pause();
  if (!running) startAll(); else engine.music.play();
}

engine.music.onchange = () => {
  const m = engine.music;
  $('#tracks').replaceChildren(...m.tracks.map((t, i) => {
    const li = document.createElement('li');
    li.textContent = t.name;
    if (i === m.index) li.className = 'cur';
    return li;
  }));
  $('#tracks').hidden = m.tracks.length < 2;   // one file needs no list; the bar below names it
  $('#fBar').hidden = !m.tracks.length;
  $('#fNow').textContent = m.index >= 0 ? m.tracks[m.index].name : '';
  $('#filePick').hidden = m.tracks.length > 0;
  spectrum.start();
};

/* ---------------- painting ---------------- */

const tiles = {};
let tickKey = '', lastPaint = '';

function fmtTime(ms, up) {
  if (!Number.isFinite(ms)) return '--:--';
  const s = Math.max(0, up ? Math.floor(ms / 1000) : Math.ceil(ms / 1000));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60;
  const p = n => String(n).padStart(2, '0');
  return h ? `${h}:${p(m)}:${p(x)}` : `${p(m)}:${p(x)}`;
}

function renderTicks(snap) {
  const key = `${snap.planId}:${snap.index}`;
  if (key === tickKey) return;
  tickKey = key;
  const n = snap.duration === Infinity ? 60 : clamp(Math.ceil(snap.duration / 60000), 1, 60);
  let out = '';
  for (let i = 0; i < n; i++) {
    const a = (-90 + (i / n) * 360) * Math.PI / 180, c = Math.cos(a), s = Math.sin(a);
    out += `<line x1="${(160 + 137 * c).toFixed(2)}" y1="${(160 + 137 * s).toFixed(2)}" x2="${(160 + 143 * c).toFixed(2)}" y2="${(160 + 143 * s).toFixed(2)}"/>`;
  }
  $('#ticks').innerHTML = out;
  $('#ticks').dataset.n = n;
}

function paintStage(snap) {
  const ring = $('#ring');
  const kind = snap.done ? 'done' : snap.kind;
  ring.dataset.kind = kind;
  const label = snap.done ? 'Done' : snap.kind === 'break' ? 'Break' : scene().name;
  const free = snap.kind === 'free';
  const time = fmtTime(snap.done ? 0 : free ? snap.elapsed : snap.remaining, free);
  $('#time').textContent = time;
  $('#phase').textContent = label;

  renderTicks(snap);
  const prog = snap.done ? 1 : clamp(snap.progress, 0, 1);
  ring.dataset.zero = String(prog < 0.0005);
  $('#ringProg').style.strokeDashoffset = (RING * (1 - prog)).toFixed(2);

  const n = +$('#ticks').dataset.n, lit = Math.floor(prog * n + 1e-6);
  $$('#ticks line').forEach((l, i) => l.classList.toggle('on', i < lit));

  const dots = $('#dots');
  const sig = `${snap.rounds}:${snap.round}:${kind}`;
  if (sig !== lastPaint) {
    lastPaint = sig;
    if (snap.cycle) {
      const doneCount = snap.done ? snap.rounds : snap.kind === 'break' ? snap.round : snap.round - 1;
      dots.innerHTML = Array.from({ length: snap.rounds }, (_, i) =>
        `<i class="${i < doneCount ? 'done' : (i === snap.round - 1 && snap.kind === 'focus' ? 'cur' : '')}"></i>`).join('');
    } else dots.innerHTML = '';
  }
  $('#next').disabled = !snap.cycle || snap.done;
  document.title = running ? `${time} · ${label} — Murmur` : 'Murmur';
  paintStats();
}

function paintTransport() {
  $('#play').setAttribute('aria-pressed', String(running));
}

function paintScenes() {
  for (const b of $$('#scenes .scene')) b.setAttribute('aria-current', String(b.dataset.scene === S.scene));
}

function renderPlans() {
  const cur = planId();
  $('#plans').innerHTML = scene().plans.map(p =>
    `<button type="button" role="radio" data-plan="${p}" aria-checked="${p === cur}">${PLANS[p].label}</button>`).join('');
}

function paintMix() {
  const snap = session.snapshot(), brk = snap.kind === 'break';
  const lv = brk ? BREAK_MIX : mixOf();
  for (const c of CHANNELS) {
    const v = lv[c.id] || 0, t = tiles[c.id];
    t.style.setProperty('--lv', v / 100);
    t.dataset.on = String(v > 0);
    t.setAttribute('aria-valuenow', v);
    $('.val', t).textContent = v;
  }
  const b = beatOf(), band = bandOf(b.band), bt = tiles.beats;
  const on = b.on && S.beatsMode !== 'off';
  bt.style.setProperty('--lv', b.level / 100);
  bt.dataset.on = String(on);
  bt.dataset.off = String(S.beatsMode === 'off');
  bt.setAttribute('aria-valuenow', b.level);
  $('.sym', bt).textContent = band.sym;
  $('.nm', bt).textContent = `${band.hz} Hz`;
  $('.val', bt).textContent = b.level;
  for (const el of $$('#bands .band')) el.setAttribute('aria-checked', String(el.dataset.band === b.band));
  $('#mix').dataset.beats = on ? 'on' : 'off';
  $('#mix').classList.toggle('locked', brk);
  $('#mixReset').hidden = !(S.mixes[S.scene] || S.beats[S.scene]);
}

function paintBeatsMode() {
  for (const b of $$('#beatsMode button')) b.setAttribute('aria-checked', String(b.dataset.mode === S.beatsMode));
}

function paintTabs() {
  for (const b of $$('#mediaTabs button')) b.setAttribute('aria-selected', String(b.dataset.tab === S.tab));
  $('#paneVideo').hidden = S.tab !== 'video';
  $('#paneFiles').hidden = S.tab !== 'files';
  $('#vLink').hidden = S.tab !== 'video';
  $('#vPower').hidden = S.tab !== 'video';
  if (S.tab !== 'video') $('#linkForm').hidden = true;
}

function paintVolume() {
  const m = $('#master'), bl = $('#blend');
  m.value = S.master;
  bl.value = S.blend;
  m.style.setProperty('--p', `${S.master}%`);
  bl.style.setProperty('--p', `${S.blend}%`);
  $('#mute use').setAttribute('href', muted || S.master === 0 ? '#i-mute' : '#i-vol');
}

function paintWide() {
  $('#app').classList.toggle('wide', S.wide);
  $('#vWide use').setAttribute('href', S.wide ? '#i-shrink' : '#i-expand');
}

let statMin = -1;
function paintStats() {
  const today = Math.floor(S.log[dayKey()] || 0);
  if (today === statMin) return;
  statMin = today;
  const days = [];
  for (let i = 6; i >= 0; i--) { const d = new Date(); d.setDate(d.getDate() - i); days.push(S.log[dayKey(d)] || 0); }
  const max = Math.max(30, ...days);
  $('#statBars').innerHTML = days.map(v => `<i style="--h:${Math.max(4, (v / max) * 100).toFixed(1)}%"></i>`).join('');
  $('#statToday').textContent = today >= 60 ? `${Math.floor(today / 60)}h ${today % 60}m` : `${today}m`;
}

/* ---------------- build ---------------- */

function build() {
  $('#scenes').innerHTML = SCENES.map((s, i) =>
    `<button type="button" class="scene" data-scene="${s.id}" aria-current="false" title="${s.name} (${i + 1})">${ic(s.icon)}<span>${s.name}</span><kbd>${i + 1}</kbd></button>`).join('');

  const tile = (id, label, tip, inner) =>
    `<div class="tile" role="slider" tabindex="0" data-ch="${id}" data-on="false" title="${tip}" aria-label="${label}" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0"><span class="fill"></span><span class="val"></span>${inner}</div>`;
  $('#tiles').innerHTML =
    CHANNELS.map(c => tile(c.id, c.tip, c.tip, `${ic(c.icon)}<span class="nm">${c.name}</span>`)).join('') +
    tile('beats', 'Beats', 'Beats: tap to switch, drag for level', '<span class="sym">β</span><span class="nm">16 Hz</span>');
  for (const t of $$('#tiles .tile')) tiles[t.dataset.ch] = t;

  $('#bands').innerHTML = BANDS.map(b =>
    `<button type="button" class="band" role="radio" data-band="${b.id}" aria-checked="false"><b>${b.sym}</b><small>${b.hz}</small></button>`).join('');

  const span = Math.log(FMAX / FMIN), at = f => ((Math.log(f / FMIN) / span) * 100).toFixed(2);
  let axis = '';
  for (const f of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) axis += `<i style="left:${at(f)}%"></i>`;
  for (const [f, t] of [[100, '100'], [1000, '1k'], [10000, '10k']]) axis += `<span style="left:${at(f)}%">${t}</span>`;
  $('#axis').innerHTML = axis;
}

/* ---------------- events ---------------- */

function bindTiles() {
  const root = $('#tiles');
  let drag = null;
  const levelOf = id => (id === 'beats' ? beatOf().level : mixOf()[id] || 0);

  root.addEventListener('pointerdown', e => {
    const t = e.target.closest('.tile');
    if (!t || e.button > 0) return;
    drag = { t, id: t.dataset.ch, y: e.clientY, h: t.getBoundingClientRect().height, start: levelOf(t.dataset.ch), moved: false };
    t.setPointerCapture(e.pointerId);
  });
  root.addEventListener('pointermove', e => {
    if (!drag) return;
    const dy = drag.y - e.clientY;
    if (!drag.moved && Math.abs(dy) < 5) return;
    drag.moved = true;
    drag.t.classList.add('drag');
    setLevel(drag.id, Math.round(clamp(drag.start + (dy / drag.h) * 100, 0, 100)));
  });
  root.addEventListener('pointerup', () => {
    if (!drag) return;
    const d = drag;
    drag = null;
    d.t.classList.remove('drag');
    if (!d.moved) toggleTile(d.id);
  });
  root.addEventListener('pointercancel', () => {
    if (drag) { drag.t.classList.remove('drag'); drag = null; }
  });
  root.addEventListener('keydown', e => {
    const t = e.target.closest('.tile');
    if (!t) return;
    const id = t.dataset.ch, v = levelOf(id);
    const step = { ArrowUp: 5, ArrowRight: 5, ArrowDown: -5, ArrowLeft: -5 }[e.key];
    if (step) { e.preventDefault(); e.stopPropagation(); setLevel(id, clamp(v + step, 0, 100)); }
    else if (e.key === 'Enter') { e.preventDefault(); toggleTile(id); }
  });
}

function bindRange(sel, get, set) {
  const el = $(sel);
  el.addEventListener('input', () => { set(+el.value); paintVolume(); save(); });
  el.value = get();
}

function openSheet(id) { const d = $(id); if (!d.open) d.showModal(); }

function bind() {
  $('#scenes').addEventListener('click', e => { const b = e.target.closest('.scene'); if (b) selectScene(b.dataset.scene); });
  $('#plans').addEventListener('click', e => { const b = e.target.closest('button'); if (b) selectPlan(b.dataset.plan); });
  $('#play').addEventListener('click', togglePlay);
  $('#prev').addEventListener('click', () => { session.restartPhase(); if (!running) startAll(); });
  $('#next').addEventListener('click', () => { session.skip(); if (!running && !session.snapshot().done) startAll(); });

  bindTiles();
  $('#bands').addEventListener('click', e => {
    const b = e.target.closest('.band');
    if (!b) return;
    S.beats[S.scene] = { ...beatOf(), band: b.dataset.band, on: true, level: beatOf().level || 20 };
    if (S.beatsMode === 'off') { S.beatsMode = 'binaural'; paintBeatsMode(); }
    if (!running) startAll(); else applyAudio(0.2);
    save();
  });
  $('#mixReset').addEventListener('click', resetMix);
  $('#beatsMode').addEventListener('click', e => { const b = e.target.closest('button'); if (b) setBeatsMode(b.dataset.mode); });

  bindRange('#master', () => S.master, v => { S.master = v; muted = false; applyVolumes(); });
  bindRange('#blend', () => S.blend, v => { S.blend = v; applyVolumes(); });
  $('#mute').addEventListener('click', toggleMute);

  $('#mediaTabs').addEventListener('click', e => { const b = e.target.closest('button'); if (b) setTab(b.dataset.tab); });
  $('#poster').addEventListener('click', () => startAll());
  $('#posterImg').addEventListener('error', e => { e.target.hidden = true; }); // offline or blocked: no broken-image glyph
  $('#vPrev').addEventListener('click', () => stepVideo(-1));
  $('#vNext').addEventListener('click', () => stepVideo(1));
  $('#vClear').addEventListener('click', () => { S.custom = null; vTried = 0; paintPlayer(); if (running) syncVideo(true); save(); });
  $('#vPower').addEventListener('click', () => { S.videoOn = !S.videoOn; syncVideo(running); save(); });
  $('#vWide').addEventListener('click', () => { S.wide = !S.wide; paintWide(); save(); });
  $('#vLink').addEventListener('click', () => {
    const f = $('#linkForm');
    f.hidden = !f.hidden;
    if (!f.hidden) $('#linkInput').focus();
  });
  $('#linkForm').addEventListener('submit', e => {
    e.preventDefault();
    const spec = parseYouTube($('#linkInput').value);
    $('#linkInput').setAttribute('aria-invalid', String(!spec));
    if (!spec) return;
    $('#linkInput').value = '';
    $('#linkForm').hidden = true;
    loadCustom(spec);
  });

  $('#filePick').addEventListener('click', () => $('#fileInput').click());
  $('#fileInput').addEventListener('change', e => { addFiles(e.target.files); e.target.value = ''; });
  $('#fPrev').addEventListener('click', () => engine.music.step(-1));
  $('#fNext').addEventListener('click', () => engine.music.step(1));
  $('#fClear').addEventListener('click', () => engine.music.clear());

  $('#btnScience').addEventListener('click', () => openSheet('#sheetScience'));
  $('#btnKeys').addEventListener('click', () => openSheet('#sheetKeys'));
  for (const d of $$('dialog.sheet')) {
    d.addEventListener('click', e => { if (e.target === d || e.target.closest('[data-close]')) d.close(); });
  }

  // paste a YouTube link anywhere
  document.addEventListener('paste', e => {
    const t = e.target;
    if (t.closest && t.closest('input, textarea')) return;
    const spec = parseYouTube(e.clipboardData && e.clipboardData.getData('text'));
    if (spec) { e.preventDefault(); loadCustom(spec); }
  });

  // drop audio files anywhere
  let depth = 0;
  const hasFiles = e => e.dataTransfer && [...e.dataTransfer.types].includes('Files');
  addEventListener('dragenter', e => { if (hasFiles(e)) { depth++; document.body.classList.add('dragging'); e.preventDefault(); } });
  addEventListener('dragover', e => { if (hasFiles(e)) e.preventDefault(); });
  addEventListener('dragleave', () => { depth = Math.max(0, depth - 1); if (!depth) document.body.classList.remove('dragging'); });
  addEventListener('drop', e => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    depth = 0;
    document.body.classList.remove('dragging');
    addFiles(e.dataTransfer.files);
  });

  document.addEventListener('keydown', onKey);
  document.addEventListener('keyup', e => { if (e.code === 'Space' && !typing(e.target)) e.preventDefault(); });
}

function toggleMute() { muted = !muted; applyVolumes(); paintVolume(); }

const typing = t => !!t && ((t.tagName === 'INPUT' && t.type !== 'range') || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT');

function onKey(e) {
  if (e.ctrlKey || e.metaKey || e.altKey || typing(e.target) || document.querySelector('dialog[open]')) return;
  const onRange = e.target.tagName === 'INPUT' && e.target.type === 'range';
  switch (e.key) {
    case ' ': e.preventDefault(); togglePlay(); break;
    case 'ArrowLeft': if (!onRange) { e.preventDefault(); $('#prev').click(); } break;
    case 'ArrowRight': if (!onRange) { e.preventDefault(); if (!$('#next').disabled) $('#next').click(); } break;
    case 'ArrowUp': case 'ArrowDown':
      if (!onRange) { e.preventDefault(); S.master = clamp(S.master + (e.key === 'ArrowUp' ? 5 : -5), 0, 100); muted = false; applyVolumes(); paintVolume(); save(); }
      break;
    case 'm': case 'M': toggleMute(); break;
    case 'b': case 'B': setBeatsMode({ off: 'binaural', binaural: 'isochronic', isochronic: 'off' }[S.beatsMode]); break;
    case 'v': case 'V': S.videoOn = !S.videoOn; syncVideo(running); save(); break;
    case '?': openSheet('#sheetKeys'); break;
    default:
      if (/^[1-7]$/.test(e.key)) selectScene(SCENES[+e.key - 1].id);
  }
}

/* ---------------- install + offline ---------------- */

let installEvent = null;
addEventListener('beforeinstallprompt', e => { e.preventDefault(); installEvent = e; $('#btnInstall').hidden = false; });
addEventListener('appinstalled', () => { installEvent = null; $('#btnInstall').hidden = true; });
$('#btnInstall').addEventListener('click', async () => {
  if (!installEvent) return;
  installEvent.prompt();
  try { await installEvent.userChoice; } catch (e) { /* dismissed */ }
  installEvent = null;
  $('#btnInstall').hidden = true;
});
if ('serviceWorker' in navigator && location.protocol.startsWith('http')) {
  addEventListener('load', () => navigator.serviceWorker.register('sw.js').catch(() => {}));
}

/* ---------------- start ---------------- */

const spectrum = createSpectrum($('#spectrum'), () => engine.analyser, () => running && !!engine.ctx && engine.ctx.state === 'running');

build();
session.setPlan(planId());
renderPlans();
paintScenes();
paintBeatsMode();
paintTabs();
paintVolume();
paintWide();
paintTransport();
paintMix();
paintPlayer();
paintStage(session.snapshot());
bind();
engine.music.onchange();
