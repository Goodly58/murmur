"""Opens the local web app, picks Sprint and checks the scene list, plan, tile levels and the 2 shortcut.
    py -3.13 tools/web_check_sprint.py   (starts serve.py itself, then checks the scene list and shortcuts)"""
import subprocess
import sys
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

root = Path(__file__).resolve().parent.parent
server = subprocess.Popen([sys.executable, str(root / "serve.py")], cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.5)
ok = True


def check(name, cond, detail=""):
    global ok
    ok &= bool(cond)
    print(("PASS  " if cond else "FAIL  ") + name + (f"   [{detail}]" if detail else ""))


try:
    with sync_playwright() as p:
        browser = p.chromium.launch(channel="chrome", headless=True)
        page = browser.new_page(viewport={"width": 1280, "height": 800})
        page.route("**/*youtube*/**", lambda r: r.abort())
        page.route("**/*ytimg*/**", lambda r: r.abort())
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto("http://127.0.0.1:8742/", wait_until="load")
        page.wait_for_selector("#scenes .scene")
        names = page.eval_on_selector_all("#scenes .scene", "els => els.map(e => e.dataset.scene)")
        check("eight scenes in the rail, Sprint second", len(names) == 8 and names[1] == "sprint", str(names))
        page.click('#scenes .scene[data-scene="sprint"]')
        page.wait_for_timeout(600)
        plans = page.eval_on_selector_all("#plans button", "els => els.map(e => e.textContent + (e.getAttribute('aria-checked') === 'true' ? '*' : ''))")
        check("plans are 15·3 (selected), 25·5, ∞", plans == ["15·3*", "25·5", "∞"], str(plans))
        levels = page.evaluate("() => Object.fromEntries([...document.querySelectorAll('#tiles .tile')].map(t => [t.dataset.ch, t.getAttribute('aria-valuenow')]))")
        check("pink 14, brown 8, beats 24", levels.get("pink") == "14" and levels.get("brown") == "8" and levels.get("beats") == "24", str(levels))
        check("the ring is labelled Sprint", page.inner_text("#phase").strip().lower() == "sprint")
        page.click('#scenes .scene[data-scene="focus"]')
        page.wait_for_timeout(300)
        page.keyboard.press("2")
        page.wait_for_timeout(500)
        cur = page.eval_on_selector('#scenes .scene[aria-current="true"]', "e => e.dataset.scene")
        check("the 2 key goes to Sprint", cur == "sprint", cur)
        page.keyboard.press("8")
        page.wait_for_timeout(300)
        cur = page.eval_on_selector('#scenes .scene[aria-current="true"]', "e => e.dataset.scene")
        check("the 8 key goes to Sleep", cur == "sleep", cur)
        page.keyboard.press("9")
        page.wait_for_timeout(200)
        check("an unused digit does nothing and does not crash", not errors, str(errors))
        page.click('#scenes .scene[data-scene="sprint"]')
        page.wait_for_timeout(400)
        page.screenshot(path=str(root / "native" / "snaps" / "web_sprint.png"))
        browser.close()
finally:
    server.terminate()
print("ALL PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
