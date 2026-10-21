#!/usr/bin/env python3
"""The VM/MUX screens on the unit's web page, in headless Chromium against
build/netsim-app_vmmux (make netsim APP=app_vmmux): every application tab is drawn, a manual
VM route is set by clicking the screen (BAT 5.1), and the sequence editor refuses a period
with no inlet and then saves a good one. Screenshots go to build/vmweb_*.png.

    make build/netsim-app_vmmux APP=app_vmmux && python3 app_vmmux/tests/integration/vm_web_ui_test.py

Needs Python Playwright and a Chromium (CHROME=...)."""
import asyncio, json, os, signal, subprocess, sys, tempfile, time, ssl, urllib.request
from playwright.async_api import async_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
CHROME = os.environ.get("CHROME", "/opt/pw-browsers/chromium-1194/chrome-linux/chrome")
MB, WEB = 15121, 18181
URL = f"https://127.0.0.1:{WEB}/"
CTX = ssl._create_unverified_context()
TABS = ["Valve Mgr", "Multiplexor", "Sequences", "Layout", "Process", "Inputs", "Names"]
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond: passed += 1
    else:
        failed += 1; print("  FAIL:", what)


def shot(name):
    return os.path.join(ROOT, "build", f"vmweb_{name}.png")


async def run(unit):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1280, "height": 1000}, ignore_https_errors=True)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.on("dialog", lambda d: asyncio.ensure_future(d.accept()))      # "are you sure?" (PF_CONFIRM)
        await pg.goto(URL)
        await pg.wait_for_selector('#apptabs button:has-text("Valve Mgr")')
        tabs = [t.strip() for t in await pg.locator("#apptabs button").all_inner_texts()]
        check(all(t in tabs for t in TABS), f"web: the seven application tabs ({tabs})")

        # every tab draws; screenshot each
        for t in TABS:
            await pg.click(f'#apptabs button:has-text("{t}")')
            await pg.wait_for_timeout(700)
            body = await pg.inner_text("#appbody")
            check(len(body.strip()) > 0, f"web: tab {t} has content")
            await pg.locator("#appsec").screenshot(path=shot(t.replace(" ", "_").lower()))
        body = await pg.inner_text("#appbody")
        check("Inlet 1" in body, "web: Names tab lists the inlet names")

        # log in as operator; the web page writes in REMOTE only
        unit.stdin.write("remote\n"); unit.stdin.flush(); time.sleep(0.3)
        await pg.fill("#pw", "1111")
        await pg.click("#loginbtn")
        await pg.wait_for_selector("#userrow", state="visible")

        # BAT 5.1 from the web page: Manual, Yes, Start, inlet 2, outlet 1
        await pg.click('#apptabs button:has-text("Valve Mgr")')
        await pg.wait_for_timeout(500)
        await pg.click('#appbody button:has-text("Manual mode")')
        await pg.wait_for_selector('#appsec button:has-text("Yes")', timeout=4000)
        await pg.locator("#appsec").screenshot(path=shot("manual_dialog"))
        await pg.click('#appsec button:has-text("Yes")')
        await pg.wait_for_selector('#appbody button:has-text("Start")', timeout=4000)
        await pg.click('#appbody button:has-text("Start")')
        await pg.wait_for_timeout(800)
        items = await pg.locator("#appbody button.gitem").all_inner_texts()
        check(items == ["1", "2", "3", "1", "2"], f"web: inlets 1-3 and outlets 1-2 ({items})")
        await pg.locator('#appbody button.gitem:text-is("2")').nth(0).click()     # inlet 2
        await pg.wait_for_timeout(500)
        await pg.locator('#appbody button.gitem:text-is("1")').nth(1).click()     # outlet 1
        await pg.wait_for_timeout(1500)
        lit = await pg.eval_on_selector_all("#appbody button.gitem.lit", "els => els.map(e => e.textContent)")
        check(lit == ["2", "1"], f"web: inlet 2 and outlet 1 lit, held ({lit})")
        await pg.locator("#appsec").screenshot(path=shot("manual_route"))
        await pg.click('#appbody button:has-text("Manual mode")')               # manual off
        await pg.wait_for_timeout(800)

        # sequence editor: a period with no inlet is refused with the reason, then a good one saved
        await pg.click('#apptabs button:has-text("Sequences")')
        await pg.wait_for_timeout(500)
        await pg.click('#appbody button:has-text("Sequence 1")')
        await pg.wait_for_selector("#recbox table")
        await pg.fill("#recbox .row input[type=number]", "1")
        await pg.dispatch_event("#recbox .row input[type=number]", "change")
        await pg.wait_for_timeout(200)
        cells = pg.locator("#recbox table tr").nth(1).locator("input")
        await cells.nth(0).fill("Zero"); await cells.nth(1).fill("30")
        await pg.click('#recbox button:has-text("Save")')
        await pg.wait_for_function("document.getElementById('recerr').textContent.length > 0", timeout=4000)
        why = await pg.inner_text("#recerr")
        check(why == "Inlet: 1 .. 3", f"web: editor refuses inlet 0, range from FirstInlets ({why!r})")
        await pg.locator("#appsec").screenshot(path=shot("seq_refused"))
        await cells.nth(2).fill("2"); await cells.nth(3).fill("1")
        await pg.click('#recbox button:has-text("Save")')
        await pg.wait_for_function("document.getElementById('recerr').textContent === 'Saved'", timeout=4000)
        check(True, "web: sequence saved")
        await pg.locator("#appsec").screenshot(path=shot("seq_saved"))
        check(not errs, f"web: no script errors ({errs[:3]})")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    fram, nor = os.path.join(tmp, "fram.bin"), os.path.join(tmp, "nor.bin")
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim-app_vmmux"), str(MB), str(WEB), fram, nor],
                            stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    try:
        for _ in range(200):
            try:
                urllib.request.urlopen(URL + "api/info", timeout=1, context=CTX).read(); break
            except Exception:
                time.sleep(0.05)
        # an engineer's configuration on the touchscreen (LOCAL): 10 valves, 1 tube, 3 inlets, 2 outlets, 4 MUX valves
        for c in ("set 1091 10", "set 1093 1", "set 1064 3", "set 1065 2", "set 1094 4"):
            unit.stdin.write(c + "\n"); unit.stdin.flush(); time.sleep(0.1)
        time.sleep(0.5)
        asyncio.run(run(unit))
    finally:
        unit.send_signal(signal.SIGTERM); unit.wait(timeout=10)
    print(f"\nvm_web_ui_test: {passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
