"""Samples live YouTube streams for a minute and measures what makes background music easy to ignore:
tempo, how clear the pulse is, and how much the loudness swings. Steady and predictable beats surprising.

    py -3.13 tools/measure_streams.py ID [ID ...]            (needs yt-dlp and mpv on PATH or in C:/mpv)
    py -3.13 tools/measure_streams.py --seconds 45 ID ...

Prints one line per stream: tempo (BPM), pulse clarity (0-1, higher = more metronomic), loudness spread
(dB, 10th to 90th percentile of one-second RMS; lower = fewer surprises), and the title.
"""
import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np


def find(name):
    for cand in (shutil.which(name), f"C:/mpv/{name}.exe", f"C:/ffmpeg/bin/{name}.exe"):
        if cand and os.path.exists(cand):
            return cand
    raise SystemExit(f"{name} not found")


YTDLP, MPV = find("yt-dlp"), find("mpv")


def read_wav(path):
    """mpv writes its pcm file while it plays, so the header can be unfinished: read the chunks by hand."""
    raw = open(path, "rb").read()
    ch, sr, bits = 1, 22050, 16
    i = raw.find(b"fmt ")
    if i >= 0:
        ch = int.from_bytes(raw[i + 10:i + 12], "little")
        sr = int.from_bytes(raw[i + 12:i + 16], "little")
        bits = int.from_bytes(raw[i + 22:i + 24], "little")
    j = raw.find(b"data")
    payload = raw[j + 8:] if j >= 0 else b""
    if bits != 16 or len(payload) < 4:
        return None
    y = np.frombuffer(payload[: len(payload) // 2 * 2], dtype=np.int16).astype(np.float32) / 32768.0
    if ch > 1:
        y = y[: len(y) // ch * ch].reshape(-1, ch).mean(axis=1)
    return y, sr


def sample(video_id, seconds, tmp):
    url = f"https://www.youtube.com/watch?v={video_id}"
    info = subprocess.run([YTDLP, "--no-warnings", "--skip-download", "--print", "%(title)s", url],
                          capture_output=True, text=True, timeout=90)
    lines = [l for l in info.stdout.splitlines() if l.strip()]
    title = lines[0] if lines else "(unavailable)"
    if info.returncode or not lines:
        return None, title
    out = os.path.join(tmp, f"{video_id}.wav")
    subprocess.run([MPV, "--no-config", "--no-video", "--really-quiet", "--no-terminal", "--ytdl=yes",
                    f"--script-opts=ytdl_hook-ytdl_path={YTDLP}", "--ytdl-format=bestaudio/best", "--ao=pcm",
                    f"--ao-pcm-file={out}", "--audio-samplerate=22050", "--audio-channels=mono", "--audio-format=s16",
                    f"--length={seconds}", url], capture_output=True, timeout=seconds + 120)
    if not os.path.exists(out):
        return None, title
    audio = read_wav(out)
    if audio is None or len(audio[0]) < audio[1] * 10:
        return None, title + " (too little audio)"
    return audio, title


def analyse(y, sr):
    y = y[int(sr * 5):]  # the first seconds hold start-up silence
    # loudness: one-second RMS in dB
    n = sr
    frames = [y[i:i + n] for i in range(0, len(y) - n, n // 2)]
    rms = np.array([np.sqrt(np.mean(f ** 2)) + 1e-9 for f in frames])
    db = 20 * np.log10(rms)
    db = db[db > -60] if (db > -60).sum() > 8 else db  # capture gaps are not music
    spread = float(np.percentile(db, 90) - np.percentile(db, 10))

    # tempo: spectral flux -> autocorrelation, with a mild preference for 90-140 BPM to settle octave doubts
    n_fft, hop = 1024, 256
    win = np.hanning(n_fft)
    count = 1 + (len(y) - n_fft) // hop
    idx = np.arange(n_fft)[None, :] + hop * np.arange(count)[:, None]
    mag = np.abs(np.fft.rfft(y[idx] * win, axis=1))
    mag = np.log1p(30 * mag)
    flux = np.maximum(0, np.diff(mag, axis=0)).sum(axis=1)
    flux = flux - np.convolve(flux, np.ones(43) / 43, mode="same")
    flux = np.maximum(flux, 0)
    fps = sr / hop
    ac = np.correlate(flux, flux, mode="full")[len(flux) - 1:]
    ac = ac / (ac[0] + 1e-9)
    best, best_score, best_r = 0.0, -1.0, 0.0
    for bpm in np.arange(60, 200, 0.5):
        lag = fps * 60.0 / bpm
        i = int(round(lag))
        if i + 1 >= len(ac):
            continue
        r = max(ac[i - 1], ac[i], ac[i + 1])
        prior = np.exp(-0.5 * (np.log2(bpm / 115.0) / 0.9) ** 2)
        if r * prior > best_score:
            best, best_score, best_r = bpm, r * prior, r
    return best, float(best_r), spread


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ids", nargs="+")
    ap.add_argument("--seconds", type=int, default=50)
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp, cf.ThreadPoolExecutor(args.workers) as ex:
        futs = {ex.submit(sample, i, args.seconds, tmp): i for i in args.ids}
        results = {}
        for f in cf.as_completed(futs):
            i = futs[f]
            try:
                audio, title = f.result()
                results[i] = (analyse(*audio), title) if audio else (None, title)
            except Exception as e:  # keep going: one dead stream must not stop the rest
                results[i] = (None, f"error: {e}")
    for i in args.ids:
        res, title = results[i]
        title = title.encode("ascii", "replace").decode()[:70]
        if res is None:
            print(f"{i}  --          {title}")
        else:
            bpm, clarity, spread = res
            print(f"{i}  {bpm:5.1f} BPM  pulse {clarity:4.2f}  loudness spread {spread:4.1f} dB  {title}")


if __name__ == "__main__":
    sys.exit(main())
