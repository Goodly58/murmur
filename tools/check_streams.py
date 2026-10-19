"""Health check for the live streams listed in data.js: embeddable (oEmbed) and still live (yt-dlp).
Run: py -3.13 tools/check_streams.py      Needs yt-dlp on PATH and a connection that reaches YouTube.
Dead ids are skipped by the player automatically; use this to find which ones to replace."""
import json
import re
import subprocess
import sys
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
text = (Path(__file__).resolve().parent.parent / 'data.js').read_text(encoding='utf-8').split('export const SCENES')[1]
scenes = {}
for m in re.finditer(r"id: '(\w+)',\s+name:.*?videos: \[(.*?)\]", text, re.S):
    scenes[m.group(1)] = re.findall(r"'([\w-]{11})'", m.group(2))


def check(vid):
    embed = 'no'
    title = ''
    try:
        url = f'https://www.youtube.com/oembed?url=https://www.youtube.com/watch?v={vid}&format=json'
        with urllib.request.urlopen(url, timeout=15) as r:
            embed = 'yes'
            title = json.load(r).get('title', '')
    except urllib.error.HTTPError as e:
        embed = f'no ({e.code})'
    except Exception:
        embed = 'error'
    out = subprocess.run(['yt-dlp', '--skip-download', '--no-warnings', '--print', '%(live_status)s',
                          f'https://www.youtube.com/watch?v={vid}'], capture_output=True, text=True, timeout=90)
    status = out.stdout.strip() or ('dead' if 'not available' in out.stderr else 'unknown')
    return vid, embed, status, title


ids = sorted({v for vs in scenes.values() for v in vs})
with ThreadPoolExecutor(6) as ex:
    res = {r[0]: r for r in ex.map(check, ids)}

bad = 0
for scene, vids in scenes.items():
    print(f'\n{scene}')
    for v in vids:
        _, embed, status, title = res[v]
        ok = embed == 'yes' and status == 'is_live'
        bad += not ok
        print(f"  {'ok  ' if ok else 'DEAD'} {v}  embed={embed:<8} {status:<10} {title[:60]}")
print(f'\n{bad} problem(s)')
sys.exit(1 if bad else 0)
