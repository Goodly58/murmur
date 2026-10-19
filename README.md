# Murmur

Focus sound mixer. Pick a scene (Focus, Read, Create, Mask, White, Calm, Sleep) and it sets the sound mix, starts a live stream, runs the timer (25/5 and 50/10 cycles with automatic breaks, or a fade-out timer for Calm and Sleep) and fades out when the plan ends.

- **Open it:** https://goodly58.github.io/murmur/ . Use the install icon in the address bar to get it as an app window; after the first visit it also works offline (the live streams need a connection, the sounds do not).
- **Run it from this folder:** double-click `Start Murmur.bat` (`py -3.13 serve.py`, localhost only). YouTube refuses to embed into `file://` pages, so a server is needed.

## How it stays light

Each sound is synthesised once into a looping stereo buffer in a worker (`dsp.js`, `worker.js`) and freed 30 s after it stops. Playback is `BufferSource -> Gain`, with no per-sample JavaScript. The audio context is suspended when nothing is playing, the YouTube player only exists while a video is wanted, and the spectrum redraws only while the mix is changing.

## Files

| | |
|---|---|
| `index.html`, `style.css` | layout, icons, themes |
| `ui.js` | scenes, ring, mix tiles, media panel, keyboard |
| `engine.js` | audio graph, beats, chime, local music |
| `session.js` | plan phases and timing |
| `video.js` | YouTube wrapper, skips dead streams |
| `spectrum.js` | live spectrum |
| `data.js` | sounds, plans, scenes, stream ids |
| `sw.js`, `manifest.webmanifest` | install and offline |

## Maintenance

- `py -3.13 tools/check_streams.py` lists which stream ids in `data.js` are still live and embeddable. Live-stream ids rotate when a stream restarts; the player skips dead ones by itself, but replace them when the list thins out.
- `node tools/calibrate.js` re-measures the sound loops and prints the loudness table for `dsp.js` (`LOUD`). Run it after changing a renderer.
