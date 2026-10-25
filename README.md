<h1 align="center">Murmur</h1>
<p align="center">Focus sounds that run themselves.</p>
<p align="center"><a href="#windows-app"><b>Windows app</b></a> &nbsp;&middot;&nbsp; <a href="https://goodly58.github.io/murmur/">Web</a> &nbsp;&middot;&nbsp; <a href="#run-locally">Run locally</a></p>

<p align="center"><img src="docs/windows.png" alt="Murmur for Windows" width="100%"></p>

<p align="center"><img src="docs/glance.png" alt="Eight scenes. Custom presets, live streams, spectrum, automatic breaks, offline." width="100%"></p>

### Windows app

Native. No browser, no runtime: one 1 MB program, about 22 MB of memory and under 1% of a core while playing.

```
native\build.cmd
native\bin\Murmur.exe --install
```

Then press the Windows key and type **Murmur**. `--uninstall` removes it again.

<p align="center"><img src="docs/mini.png" alt="Mini player" width="24%">&nbsp;&nbsp;&nbsp;<img src="docs/windows-light.png" alt="Light theme" width="62%"></p>

- **N** shrinks it to a mini player that stays on top while you work.
- **Right-click the taskbar icon** for one-click sessions, like Sprint 15 · 3.
- Hover the taskbar icon for previous, play and next.
- Unplug the headphones and it pauses instead of moving to the speakers.

<details>
<summary>Start a session from a shortcut</summary>

<br>

Add these after `Murmur.exe` (it lives in `%LOCALAPPDATA%\Programs\Murmur`), in a desktop shortcut's Target box or in a terminal:

| Add | What it does |
|---|---|
| `--scene sleep` | Picks the scene: `focus` `sprint` `read` `create` `mask` `white` `calm` `sleep` |
| `--plan t60` | Picks the timing: `p25` 25·5, `p50` 50·10, `p15` 15·3, `t10` `t20` `t30` `t60` `t90` minute timers, `free` no timer |
| `--play` | Starts playing straight away |
| `--mini` or `--full` | Opens the mini player or the full window |
| a YouTube link or music files | Plays them instead of the scene's stream |

For example, a 60 minute sleep timer that starts by itself:

```
Murmur.exe --scene sleep --plan t60 --play
```

</details>

Streams and your own music play through [mpv](https://mpv.io), YouTube through yt-dlp. If they're missing, the player shows **Set up streaming**: one click fetches both from their official GitHub releases (about 50 MB, no admin rights) into `%LOCALAPPDATA%\Murmur\tools`, and `--uninstall` removes them again. Murmur keeps that yt-dlp up to date, since YouTube changes often. Already have them on PATH or in `C:\mpv`? Murmur uses those and leaves them alone. The sounds need neither.

### Web app

<p align="center"><a href="https://goodly58.github.io/murmur/"><img src="docs/bar.png" alt="The custom bar with saved presets" width="60%"></a></p>

**[Open the web app](https://goodly58.github.io/murmur/)** in Chrome or Edge and use the install icon in the address bar. After the first visit it works offline.

### Run locally

Double-click `Start Murmur.bat`.

<details>
<summary>Under the hood</summary>

<br>

**Windows app** (`native/`, C++17, Win32 + Direct2D + WASAPI, MinGW-w64): every sound is synthesised once into a looping stereo buffer on a worker thread and freed 30 s after it stops. Audio goes straight to WASAPI in shared mode; the window repaints only the region that changed, the spectrum timer sleeps when the picture settles, and mpv is a separate process that exists only while a stream or a file plays. `Murmur.exe --selftest` runs 300+ checks (audio levels, scheduling, presets, hit-testing and rendered pixels); `--snapshot out.png --do presets --do spectrum` draws the real UI offscreen.

| | |
|---|---|
| `audio.cpp` `dsp.cpp` | mixer, beats, chimes, WASAPI; the sound synthesis |
| `session.cpp` `model.cpp` `app.cpp` | plans and timing, saved state and presets, the controller |
| `ui.cpp` `ui_paint.cpp` `gfx.cpp` | layout and input, drawing, icons from the web sprite |
| `window.cpp` `jumplist.cpp` | window, mini player, timers, drag and drop, taskbar buttons and jump list, single instance |
| `stream.cpp` `resolver.cpp` | mpv over a pipe; finds live streams for each scene daily |
| `setup.cpp` | the Set up streaming button: downloads mpv and yt-dlp, checks their SHA-256, unpacks them; keeps that yt-dlp updated |
| `install.cpp` | per-user install and Start menu entry |
| `version.h` | the version number; `mingw32-make APP_VERSION=1.2.3` sets it |
| `crash.cpp` | when the program dies on an exception it writes a note to `%APPDATA%\Murmur\crash.log`: the exception, the address and the calls |
| `thread.cpp` | threads straight from the Windows API, because the toolchain's `std::thread` crashed now and then on a busy machine |

**Builds and releases**: every push is built with a pinned MinGW and checked on Windows Server 2022 and 2025 (`.github/workflows/windows.yml`): the self-tests of the dev build and of the release exe, and an offscreen draw of the window. Pushing a tag like `v1.2.3` then attaches `Murmur.exe` and its SHA-256 to a GitHub release, with that version stamped into the exe. A tag with a suffix, like `v1.2.3-rc1`, makes a prerelease.

**Web app**: each sound is synthesised once into a looping stereo buffer in a worker, so playback is `BufferSource -> Gain` with no per-sample JavaScript. The audio context sleeps when nothing plays, the YouTube player only exists while a video is wanted, and the spectrum repaints only while the mix changes.

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
