# Development

## Build

With MinGW-w64 installed:

```
native\build.cmd
native\bin\Murmur.exe --install
```

The web app runs from `Start Murmur.bat`.

## Checks

- `Murmur.exe --selftest` runs more than 1,300 checks: audio levels, scheduling, presets, hit-testing, rendered pixels, and the Spotify player against a stand-in for Spotifast.
- `--snapshot out.png --do presets --do spectrum` draws the real UI offscreen.
- `py -3.13 tools/check_streams.py` lists which stream ids are still live.
- `node tools/calibrate.js` re-measures the sound loops after a renderer change.

## Builds and releases

Every push is built with a pinned MinGW and checked on Windows Server 2022 and 2025 (`.github/workflows/windows.yml`). Pushing a tag such as `v1.2.3` attaches `Murmur.exe` and its SHA-256 to a GitHub release once the checks pass. A suffix, like `v1.2.3-rc1`, makes a prerelease.

## Windows app

C++17 on Win32, Direct2D and WASAPI. Each sound is synthesised once into a looping buffer and freed 30 s after it stops. The window repaints only what changed, and mpv exists only while a stream or a file plays.

| File | |
|---|---|
| `audio.cpp` `dsp.cpp` | mixer, beats, chimes, WASAPI, sound synthesis |
| `session.cpp` `model.cpp` `app.cpp` | plans and timing, saved state, the controller |
| `ui.cpp` `ui_paint.cpp` `gfx.cpp` | layout and input, drawing, icons |
| `window.cpp` `jumplist.cpp` | window, mini player, timers, taskbar, single instance |
| `stream.cpp` `resolver.cpp` | mpv over a pipe; finds live streams daily |
| `spotify.cpp` | a scene's playlist through Spotifast's command line |
| `plugins.cpp` | programs that follow the session |
| `setup.cpp` | downloads mpv and yt-dlp, checks them, keeps yt-dlp updated |
| `install.cpp` | per-user install and Start menu entry |
| `crash.cpp` | writes `%APPDATA%\Murmur\crash.log` when the program dies |
| `thread.cpp` | threads straight from the Windows API |

## Web app

Each sound is synthesised once in a worker, so playback is `BufferSource -> Gain`. The audio context sleeps when nothing plays.

| File | |
|---|---|
| `index.html` `style.css` | layout, icons, themes |
| `ui.js` | scenes, ring, mix tiles, presets, media panel |
| `engine.js` `dsp.js` `worker.js` | audio graph and sound synthesis |
| `session.js` | plans, phases, timing |
| `video.js` | YouTube wrapper, skips dead streams |
| `spectrum.js` | live spectrum |
| `data.js` | sounds, plans, scenes, stream ids |
| `sw.js` `manifest.webmanifest` | install and offline |

## Other programs

mpv, yt-dlp and Spotifast are separate programs with their own licenses. Murmur downloads mpv and yt-dlp only when you click **Set up streaming**; Spotifast you install yourself.
