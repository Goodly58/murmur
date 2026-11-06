"""Opens the local web app and checks the loop button: where it sits, that it saves, which scenes have it, the R key, the
mark in the ring, the Keys sheet, and (with a fake clock) that 25·5 really does go round again instead of ending.
    py -3.13 tools/web_check_loop.py   (starts serve.py itself)"""
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


def box(page, sel):
    return page.eval_on_selector(sel, "e => { const r = e.getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height, r: r.right, b: r.bottom }; }")


try:
    with sync_playwright() as p:
        browser = p.chromium.launch(channel="chrome", headless=True)
        page = browser.new_page(viewport={"width": 1280, "height": 800})
        page.route("**/*youtube*/**", lambda r: r.abort())
        page.route("**/*ytimg*/**", lambda r: r.abort())
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto("http://127.0.0.1:8742/", wait_until="load")
        page.wait_for_selector("#plans button")

        # the button beside the plans, and what it says
        check("Focus has the loop button", page.is_visible("#loopSeg") and page.get_attribute("#loop", "aria-pressed") == "false")
        plans, loop = box(page, "#plans"), box(page, "#loopSeg")
        check("it sits right after the plans, on the same line", 0 <= loop["x"] - plans["r"] < 16 and abs((loop["y"] + loop["h"] / 2) - (plans["y"] + plans["h"] / 2)) < 2,
              f"gap {loop['x'] - plans['r']:.1f}px")
        ring = box(page, "#ring")
        group_mid = (plans["x"] + loop["r"]) / 2
        check("the plans and the button are centred together", abs(group_mid - (ring["x"] + ring["w"] / 2)) < 2, f"{group_mid:.1f} vs {ring['x'] + ring['w'] / 2:.1f}")
        check("its tip says what it does", "Loop the rounds" in page.get_attribute("#loop", "title") and "(R)" in page.get_attribute("#loop", "title"), page.get_attribute("#loop", "title"))

        # click, key and saving
        page.click("#loop")
        check("a click turns it on", page.get_attribute("#loop", "aria-pressed") == "true" and "Looping" in page.get_attribute("#loop", "title"))
        page.wait_for_timeout(500)   # the page saves 300 ms after a change
        saved = page.evaluate("() => (JSON.parse(localStorage.getItem('murmur.v2')) || {}).repeat")
        check("and it is saved", saved is True, str(saved))
        page.keyboard.press("r")
        check("the R key turns it off", page.get_attribute("#loop", "aria-pressed") == "false")
        page.keyboard.press("R")
        check("and on again", page.get_attribute("#loop", "aria-pressed") == "true")

        # the mark in the ring
        page.click('#plans button[data-plan="p25"]')
        check("with the loop on, the ring shows a repeat mark after the dots", page.locator("#dots .loop-mark").count() == 1)
        dots = box(page, "#dots")
        mark = box(page, "#dots .loop-mark")
        check("it sits right of the dots without moving them", mark["x"] >= dots["r"] and abs((dots["x"] + dots["w"] / 2) - (ring["x"] + ring["w"] / 2)) < 2,
              f"dots centre {dots['x'] + dots['w'] / 2:.1f}, ring centre {ring['x'] + ring['w'] / 2:.1f}")
        page.click("#loop")
        check("turned off, the mark goes", page.locator("#dots .loop-mark").count() == 0)
        page.click("#loop")

        # scenes with only timers have no button
        page.keyboard.press("8")
        page.wait_for_timeout(300)
        check("Sleep has none, its plans are timers", not page.is_visible("#loopSeg"))
        page.keyboard.press("7")
        page.wait_for_timeout(300)
        check("Calm has none either", not page.is_visible("#loopSeg"))
        for key, name in (("2", "Sprint"), ("3", "Read"), ("4", "Create"), ("5", "Mask"), ("1", "Focus")):
            page.keyboard.press(key)
            page.wait_for_timeout(250)
            check(f"{name} has the button, still on", page.is_visible("#loopSeg") and page.get_attribute("#loop", "aria-pressed") == "true")

        # the open-ended plan has nothing to loop: the button fades and says why
        page.click('#plans button[data-plan="free"]')
        check("on the open-ended plan the button is faded and its tip explains", page.get_attribute("#loopSeg", "data-idle") == "true" and "needs timed rounds" in page.get_attribute("#loop", "title"))
        page.click("#loop")   # it was on: now off
        page.wait_for_timeout(150)
        check("turning it off there says so", page.inner_text("#toastMsg") == "Loop is off", page.inner_text("#toastMsg"))
        page.click("#loop")   # and on again
        page.wait_for_timeout(150)
        check("turning it on there says what it will do", "timed rounds" in page.inner_text("#toastMsg"), page.inner_text("#toastMsg"))
        page.click('#plans button[data-plan="p25"]')

        # the Keys sheet
        page.keyboard.press("?")
        page.wait_for_timeout(200)
        keys = page.inner_text("#sheetKeys")
        check("the Keys sheet lists R", page.evaluate("() => document.querySelector('#sheetKeys').open") and "R" in [k.strip() for k in page.eval_on_selector_all("#sheetKeys kbd", "els => els.map(e => e.textContent)")], keys.replace("\n", " ")[:80])
        page.keyboard.press("Escape")

        # it comes back after a reload
        page.wait_for_timeout(500)
        page.reload(wait_until="load")
        page.wait_for_selector("#plans button")
        check("the setting survives a reload", page.get_attribute("#loop", "aria-pressed") == "true" and page.locator("#dots .loop-mark").count() == 1)

        page.screenshot(path=str(root / "native" / "snaps" / "web_loop.png"))
        check("no errors in the page", not errors, str(errors))
        browser.close()

        # a fake clock: 25·5 goes round again instead of ending
        browser = p.chromium.launch(channel="chrome", headless=True)
        ctx = browser.new_context(viewport={"width": 1280, "height": 800})
        page = ctx.new_page()
        page.route("**/*youtube*/**", lambda r: r.abort())
        page.route("**/*ytimg*/**", lambda r: r.abort())
        page.clock.install()
        page.goto("http://127.0.0.1:8742/", wait_until="load")
        page.wait_for_selector("#plans button")
        page.click("#loop")
        page.click("#play")
        page.clock.run_for(2000)
        check("a session is running on a fake clock", "focus" in page.inner_text("#phase").lower() and page.inner_text("#time") != "25:00", page.inner_text("#time"))
        page.clock.fast_forward((115 * 60 - 2) * 1000)
        page.clock.run_for(2000)
        check("after four rounds comes a break, not Done", page.inner_text("#phase").strip().lower() == "break" and page.locator("#ring[data-kind=break]").count() == 1,
              page.inner_text("#phase") + " " + page.inner_text("#time"))
        check("it shows all four rounds done and the repeat mark", page.locator("#dots i.done").count() == 4 and page.locator("#dots .loop-mark").count() == 1)
        page.clock.fast_forward(5 * 60 * 1000)
        page.clock.run_for(2000)
        check("then the first round starts again", page.inner_text("#phase").strip().lower() == "focus" and page.locator("#dots i.cur").count() == 1 and page.locator("#dots i.done").count() == 0,
              page.inner_text("#phase") + " " + page.inner_text("#time"))
        page.click("#loop")
        page.clock.fast_forward(116 * 60 * 1000)
        page.clock.run_for(5000)
        check("with the loop off the plan ends as before", page.inner_text("#phase").strip().lower() == "done", page.inner_text("#phase"))
        browser.close()
finally:
    server.terminate()
print("ALL PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
