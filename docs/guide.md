# Guide

## Install

`Murmur.exe --install` copies it to `%LOCALAPPDATA%\Programs\Murmur` and adds a Start menu entry. It asks first; `--quiet` skips the question. `--uninstall` removes it. The [releases page](https://github.com/Goodly58/murmur/releases) lists `Murmur.exe.sha256` to check the download against.

## Keys

Press `?` in the app, or the keyboard button in its top bar.

## It does these by itself

- Starts breaks, and fades timers out at the end.
- Opens where you were if you closed it mid-round and come back within half an hour.
- Offers a YouTube or Spotify link you just copied.
- Lights up the taskbar button when a break or a round starts behind another window.
- Pauses when the headphones are unplugged.
- Clears a finished task when the plan starts again.
- Counts days in a row beside today's time. The day starts at 4 am.

## Taskbar

Right-click the icon for one-click sessions. Hover it for previous, play and next.

## Start a session from a shortcut

Add these after `Murmur.exe`:

| Add | |
|---|---|
| `--scene sleep` | `focus` `sprint` `read` `create` `mask` `white` `calm` `sleep` |
| `--plan t60` | `p25` `p50` `p15` rounds, `t10` `t20` `t30` `t60` `t90` timers, `free` |
| `--play` | Start at once |
| `--loop` `--loop=4` `--no-loop` | Loop until stopped, for four sets, or not at all |
| `--mini` `--full` | Mini player or full window |
| a link or music files | A YouTube link, a Spotify playlist link, or audio files |

```
Murmur.exe --scene sleep --plan t60 --play
```

## Streams and your own music

They play through [mpv](https://mpv.io) and yt-dlp. If they're missing, the player shows **Set up streaming**: one click fetches both (about 50 MB, no admin rights) into `%LOCALAPPDATA%\Murmur\tools`. Copies already on PATH or in `C:\mpv` are used and left alone. The sounds need neither.
