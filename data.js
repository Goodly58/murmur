/* Catalogue: sounds, brainwave bands, session plans and scenes. Nothing here touches the DOM or audio. */

export const CHANNELS = [
  { id: 'brown',  name: 'Brown', icon: 'i-brown',  tip: 'Brown noise: deep rumble' },
  { id: 'pink',   name: 'Pink',  icon: 'i-pink',   tip: 'Pink noise: soft and balanced' },
  { id: 'white',  name: 'White', icon: 'i-white',  tip: 'White noise: bright hiss' },
  { id: 'speech', name: 'Mask',  icon: 'i-bubble', tip: 'Speech-band noise: blurs nearby voices' },
  { id: 'rain',   name: 'Rain',  icon: 'i-rain',   tip: 'Rain' },
  { id: 'ocean',  name: 'Ocean', icon: 'i-ocean',  tip: 'Ocean swells' },
  { id: 'wind',   name: 'Wind',  icon: 'i-wind',   tip: 'Wind' },
  { id: 'fire',   name: 'Fire',  icon: 'i-fire',   tip: 'Fireplace crackle' },
  { id: 'cabin',  name: 'Cabin', icon: 'i-cabin',  tip: 'Cabin hum: engine drone' }
];

export const BANDS = [
  { id: 'delta', sym: 'δ', hz: 2 },
  { id: 'theta', sym: 'θ', hz: 6 },
  { id: 'alpha', sym: 'α', hz: 10 },
  { id: 'beta',  sym: 'β', hz: 16 },
  { id: 'gamma', sym: 'γ', hz: 40 }
];

const MIN = 60000;

/* A cycle plan alternates focus and break and ends after the last focus block. */
function cycle(focus, rest, rounds) {
  const out = [];
  for (let i = 0; i < rounds; i++) {
    out.push({ kind: 'focus', ms: focus * MIN });
    if (i < rounds - 1) out.push({ kind: 'break', ms: rest * MIN });
  }
  return out;
}

/* phases === null means open-ended. fadeMs is the fade-out at the end of a timer. */
export const PLANS = {
  p25:  { label: '25·5',  phases: cycle(25, 5, 4) },
  p50:  { label: '50·10', phases: cycle(50, 10, 3) },
  free: { label: '∞',     phases: null },
  t10:  { label: '10m', phases: [{ kind: 'timer', ms: 10 * MIN }], fadeMs: 30000 },
  t20:  { label: '20m', phases: [{ kind: 'timer', ms: 20 * MIN }], fadeMs: 30000 },
  t30:  { label: '30m', phases: [{ kind: 'timer', ms: 30 * MIN }], fadeMs: 60000 },
  t60:  { label: '60m', phases: [{ kind: 'timer', ms: 60 * MIN }], fadeMs: 60000 },
  t90:  { label: '90m', phases: [{ kind: 'timer', ms: 90 * MIN }], fadeMs: 60000 }
};

/* What plays during a break: a softer mix with no beats. */
export const BREAK_MIX = { ocean: 34, wind: 12, rain: 10 };

/* Video entries are YouTube ids of 24/7 live streams (all checked embeddable and live on 2026-10-19).
   Live-stream ids rotate when a stream restarts (the old Lofi Girl main id already died), so every
   scene lists several and the player skips to the next one when one fails to start. */
export const SCENES = [
  { id: 'focus',  name: 'Focus',  icon: 'i-target', mix: { brown: 45, pink: 18 },
    beat: { band: 'beta', level: 22 }, plans: ['p25', 'p50', 'free'],
    videos: ['rFZHOHl-L8A', '5yx6BWlEVcY', '3PFJ9SETS4M', '0muHFBSiybw', '1Tl2FtV06qo'] },
  { id: 'read',   name: 'Read',   icon: 'i-book', mix: { pink: 36, rain: 16 },
    beat: null, plans: ['p50', 'p25', 'free'],
    videos: ['jXAEIWcGXwE', 'N0snMcR6aaA', 'E_XmwjgRLz8'] },
  { id: 'create', name: 'Create', icon: 'i-bulb', mix: { pink: 38, wind: 18, rain: 22 },
    beat: { band: 'alpha', level: 20 }, plans: ['p25', 'p50', 'free'],
    videos: ['4xDzrJKXOOY', 'A8jDx9TLMQc', 'E2vONfzoyRI', 'CwPCy1GLS38'] },
  { id: 'mask',   name: 'Mask',   icon: 'i-shield', mix: { speech: 50, brown: 20 },
    beat: null, plans: ['free', 'p25', 'p50'], videos: [] },
  { id: 'white',  name: 'White',  icon: 'i-bars', mix: { white: 34, pink: 20 },
    beat: null, plans: ['free', 'p25', 'p50'], videos: [] },
  { id: 'calm',   name: 'Calm',   icon: 'i-leaf', mix: { ocean: 40, wind: 14, rain: 10 },
    beat: null, plans: ['t10', 't20', 'free'],
    videos: ['q_4KI-ChIIs', 'JK0pBKoK5FQ', 'GSfT7H87zq4'] },
  { id: 'sleep',  name: 'Sleep',  icon: 'i-moon', mix: { ocean: 46, brown: 22 },
    beat: null, plans: ['t60', 't30', 't90'],
    videos: ['nI725iVsyoQ', 'xORCbIptqcc', 'VAlMDl00mYY', 'JD-kMIpDfnY'] }
];
