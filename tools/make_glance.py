"""Draws docs/glance.png: the scenes and the five things the app does, from the web app's own icon sprite.
    py -3.13 tools/make_glance.py"""
import re
import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

root = Path(__file__).resolve().parent.parent
index = (root / "index.html").read_text(encoding="utf-8")
sprite = re.search(r'<svg class="sprite".*?</svg>', index, re.S).group(0)

SCENES = [("i-target", "Focus"), ("i-bolt", "Sprint"), ("i-book", "Read"), ("i-bulb", "Create"), ("i-shield", "Mask"),
          ("i-bars", "White"), ("i-leaf", "Calm"), ("i-moon", "Sleep")]
FEATURES = [("i-bookmark", "Custom"), ("i-video", "Live streams"), ("i-ocean", "Spectrum"), ("i-pause", "Auto breaks"), ("i-download", "Offline")]


def icon(name, size):
    solid = ""
    return f'<svg class="ic{solid}" width="{size}" height="{size}"><use href="#{name}"/></svg>'


tiles = "".join(
    f'<div class="scene{" on" if i == 0 else ""}"><div class="tile">{icon(n, 22)}</div><span>{label}</span></div>' for i, (n, label) in enumerate(SCENES))
feats = "".join(f'<div class="feat">{icon(n, 17)}<span>{label}</span></div>' for n, label in FEATURES)
doc = f"""<!doctype html><meta charset="utf-8"><body>{sprite}
<style>
  :root {{ --bg:#0c0e11; --raised:#181c22; --line:#20252c; --text:#eceef1; --text2:#a4acb7; --accent:#f2b563; }}
  body {{ margin:0; width:900px; height:235px; background:var(--bg); color:var(--text); font:500 10px Inter,'Segoe UI',sans-serif; position:relative; }}
  .ic {{ fill:none; stroke:currentColor; stroke-width:1.6; stroke-linecap:round; stroke-linejoin:round; }}
  .ic.solid {{ fill:currentColor; stroke:none; }}
  .scenes {{ position:absolute; left:45px; right:45px; top:57px; display:flex; justify-content:space-between; }}
  .scene {{ width:57px; display:grid; justify-items:center; gap:9px; color:var(--text2); }}
  .tile {{ width:57px; height:57px; border-radius:14px; background:var(--raised); display:grid; place-items:center; }}
  .on .tile {{ color:var(--accent); }}
  .rule {{ position:absolute; left:45px; right:45px; top:161px; height:1px; background:var(--line); }}
  .feats {{ position:absolute; left:45px; right:45px; top:188px; display:flex; justify-content:space-between; }}
  .feat {{ display:flex; align-items:center; gap:7px; font-weight:600; font-size:10.5px; }}
  .feat .ic {{ color:var(--accent); }}
</style>
<div class="scenes">{tiles}</div><div class="rule"></div><div class="feats">{feats}</div></body>"""

with sync_playwright() as p:
    browser = p.chromium.launch(channel="chrome", headless=True)
    page = browser.new_page(viewport={"width": 900, "height": 235}, device_scale_factor=2)
    page.set_content(doc)
    page.wait_for_timeout(300)
    page.screenshot(path=str(root / "docs" / "glance.png"))
    browser.close()
print("wrote docs/glance.png")
sys.exit(0)
