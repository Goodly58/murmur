<h1 align="center">Murmur</h1>
<p align="center">Focus sounds that run themselves.</p>
<p align="center"><a href="https://goodly58.github.io/murmur/"><b>Open</b></a> &nbsp;&middot;&nbsp; <a href="#install">Install</a> &nbsp;&middot;&nbsp; <a href="#run-locally">Run locally</a></p>

<p align="center"><img src="docs/hero.png" alt="Murmur on desktop and phone" width="100%"></p>

<p align="center"><img src="docs/glance.png" alt="Seven scenes. Custom presets, live streams, spectrum, automatic breaks, offline." width="100%"></p>

<p align="center"><img src="docs/bar.png" alt="The custom bar with saved presets" width="60%"></p>

<p align="center"><img src="docs/light.png" alt="Light theme" width="66%">&nbsp;&nbsp;<img src="docs/phone.png" alt="Phone" width="21%"></p>

### Install

Chrome or Edge: the install icon in the address bar. After the first visit it works offline.

### Run locally

Double-click `Start Murmur.bat`.

<details>
<summary>Under the hood</summary>

<br>

Each sound is synthesised once into a looping stereo buffer in a worker and freed 30 s after it stops, so playback is `BufferSource -> Gain` with no per-sample JavaScript. The audio context sleeps when nothing plays, the YouTube player only exists while a video is wanted, and the spectrum repaints only while the mix changes.

| | |
|---|---|
| `index.html` `style.css` | layout, icons, themes |
| `ui.js` | scenes, ring, mix tiles, custom presets, media panel |
| `engine.js` `dsp.js` `worker.js` | audio graph and sound synthesis |
| `session.js` | plans, phases, timing |
| `video.js` | YouTube wrapper, skips dead streams |
| `spectrum.js` | live spectrum |
| `data.js` | sounds, plans, scenes, stream ids |
| `sw.js` `manifest.webmanifest` | install and offline |

`py -3.13 tools/check_streams.py` lists which stream ids are still live. `node tools/calibrate.js` re-measures the sound loops after a renderer change.

</details>
