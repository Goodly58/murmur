"""Searches YouTube for live streams and ranks them by viewers: for replacing a stream that is dead or has no audience.
    py -3.13 tools/find_streams.py "nu disco radio 24/7" "jazz hop radio" --min 100 --top 15
Each query is searched with YouTube's Live filter; every hit is then read for its viewer count and whether it can be
embedded. Needs yt-dlp on PATH and a connection that reaches YouTube.
Go easy: about 100 lookups in two minutes got this connection a "confirm you're not a bot" block that lasted a while,
so keep to a handful of queries and few workers."""
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import quote

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ENV = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
SEP = "~|~"  # ASCII on purpose: a non-ASCII separator is mangled on its way into yt-dlp.exe's command line


def ytdlp(args, timeout=120):
    out = subprocess.run(["yt-dlp", "--no-warnings", *args], capture_output=True, text=True, encoding="utf-8", errors="replace", env=ENV, timeout=timeout)
    return out.stdout


def search(query, top):
    url = "https://www.youtube.com/results?search_query=" + quote(query) + "&sp=EgJAAQ%253D%253D"  # filter: Live
    text = ytdlp(["--flat-playlist", "--playlist-end", str(top), "--print", "%(id)s" + SEP + "%(live_status)s", url])
    ids = []
    for line in text.splitlines():
        vid, _, status = line.partition(SEP)
        if status.strip() == "is_live" and len(vid) == 11:
            ids.append(vid)
    return query, ids


def detail(vid):
    fmt = SEP.join(["%(concurrent_view_count)s", "%(playable_in_embed)s", "%(channel)s", "%(title)s"])
    try:
        text = ytdlp(["--skip-download", "--print", fmt, "https://www.youtube.com/watch?v=" + vid], timeout=90).strip().splitlines()
    except subprocess.TimeoutExpired:
        return vid, None
    if not text:
        return vid, None
    parts = text[0].split(SEP, 3)
    if len(parts) < 4:
        return vid, None
    views = int(parts[0]) if parts[0].isdigit() else 0
    return vid, (views, parts[1] == "True", parts[2], parts[3])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("queries", nargs="+")
    ap.add_argument("--top", type=int, default=15, help="live results to read per query")
    ap.add_argument("--min", type=int, default=0, help="hide streams with fewer viewers")
    ap.add_argument("--workers", type=int, default=3)
    a = ap.parse_args()

    with ThreadPoolExecutor(a.workers) as ex:
        found = list(ex.map(lambda q: search(q, a.top), a.queries))
        by_id = {}
        for q, ids in found:
            for v in ids:
                by_id.setdefault(v, []).append(q)
        info = dict(ex.map(detail, by_id))

    rows = []
    for vid, qs in by_id.items():
        d = info.get(vid)
        if d and d[0] >= a.min:
            rows.append((d[0], vid, d[1], d[2], d[3], qs))
    rows.sort(reverse=True)
    for views, vid, embed, channel, title, qs in rows:
        print(f"{views:6d}  {vid}  {'embed' if embed else 'NOEMB'}  {channel[:26]:<26}  {title[:70]}   <- {'; '.join(sorted({q[:22] for q in qs}))}")
    print(f"\n{len(rows)} live streams from {len(a.queries)} queries")


if __name__ == "__main__":
    main()
