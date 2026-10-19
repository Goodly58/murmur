/* YouTube wrapper. The IFrame API is loaded only when a video is first wanted, and the player is
   destroyed when video is switched off, so an idle page holds no player at all.
   A start that does not reach "playing" within 20 s counts as a failure, which lets the UI skip to
   the next candidate (live-stream ids rotate when a stream restarts). */

const ID = /^[\w-]{11}$/;

export function parseYouTube(input) {
  const s = String(input || '').trim();
  if (ID.test(s)) return { id: s };
  try {
    const u = new URL(/^https?:\/\//i.test(s) ? s : 'https://' + s);
    const host = u.hostname.replace(/^(www|m)\./, '');
    if (!/^(youtube\.com|youtube-nocookie\.com|youtu\.be)$/.test(host)) return null;
    let v = u.searchParams.get('v');
    if (host === 'youtu.be') v = u.pathname.split('/')[1];
    const m = u.pathname.match(/^\/(?:embed|live|shorts|v)\/([\w-]{11})/);
    if (m) v = m[1];
    const list = u.searchParams.get('list');
    if (v && ID.test(v)) return { id: v };
    if (list && /^[\w-]+$/.test(list)) return { list };
  } catch (e) { /* not a URL */ }
  return null;
}

/* Accepts 'abcdefghijk' or {id} / {list}. */
export function norm(entry) {
  if (!entry) return null;
  return typeof entry === 'string' ? { id: entry } : entry;
}

export function keyOf(e) { return e.id ? 'id:' + e.id : 'list:' + e.list; }

let apiPromise = null, player = null, ready = false, controls = false, current = '', failTimer = 0, host = null;
export const cb = { playing: null, fail: null };

function api() {
  if (apiPromise) return apiPromise;
  apiPromise = new Promise((resolve, reject) => {
    if (window.YT && window.YT.Player) return resolve();
    window.onYouTubeIframeAPIReady = resolve;
    const s = document.createElement('script');
    s.src = 'https://www.youtube.com/iframe_api';
    s.async = true;
    s.onerror = () => { apiPromise = null; reject(new Error('offline')); };
    document.head.appendChild(s);
  });
  return apiPromise;
}

function armFail() {
  clearTimeout(failTimer);
  failTimer = setTimeout(() => cb.fail && cb.fail('timeout'), 20000);
}

function events() {
  return {
    onReady: () => { ready = true; },
    onStateChange: e => {
      if (e.data !== 1) return; // 1 = playing
      clearTimeout(failTimer);
      let title = '';
      try { title = player.getVideoData().title || ''; } catch (err) {}
      if (cb.playing) cb.playing(title);
    },
    onError: e => { clearTimeout(failTimer); if (cb.fail) cb.fail(e.data); }
  };
}

export const video = {
  cb,
  get key() { return current; },
  get active() { return !!player; },

  /* entry: see norm(). container: element that will hold the player. */
  async load(entry, container, opts = {}) {
    const e = norm(entry);
    if (!e) return;
    const key = keyOf(e), wantControls = !!opts.controls;
    host = container;
    try { await api(); } catch (err) { if (cb.fail) cb.fail('offline'); return; }
    armFail();
    if (player && ready && key === current) { try { player.playVideo(); } catch (err) {} return; }
    if (player && ready && e.id && controls === wantControls) {
      current = key;
      try { player.loadVideoById(e.id); } catch (err) {}
      return;
    }
    this.destroy();
    current = key;
    controls = wantControls;
    const vars = { autoplay: 1, playsinline: 1, rel: 0, controls: wantControls ? 1 : 0, iv_load_policy: 3, disablekb: 1, origin: location.origin };
    if (e.list) { vars.listType = 'playlist'; vars.list = e.list; }
    const mount = document.createElement('div');
    container.replaceChildren(mount);
    player = new YT.Player(mount, { width: '100%', height: '100%', videoId: e.id, playerVars: vars, events: events() });
  },

  play() { if (ready) try { player.playVideo(); } catch (e) {} },
  pause() { if (ready) try { player.pauseVideo(); } catch (e) {} },
  setVolume(v) { if (ready) try { player.setVolume(v); } catch (e) {} },

  destroy() {
    clearTimeout(failTimer);
    try { if (player) player.destroy(); } catch (e) {}
    player = null;
    ready = false;
    current = '';
    if (host) host.replaceChildren();
  }
};
