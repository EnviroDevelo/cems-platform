#!/usr/bin/env python3
"""Calibration from the web page (CIP-18, /api/cal) against build/netsim with the demo
application's "Level" channel: the section appears, Begin is refused in LOCAL mode with
the instrument's reason, a factory calibration runs (CALIBRATION in the header, Accept
puts it in force), the history records it, and a second one aborted is recorded too.
Then the start-up self-test (/api/info) and the Service section (/api/service): hours and settings checksum shown, a service log
entry and a manufacturing test record added at factory login.
Needs Python Playwright and a Chromium."""
import asyncio, glob, os, subprocess, sys, tempfile, time
from playwright.async_api import async_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
URL = "https://127.0.0.1:18101/"
passed = failed = 0

def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1

async def run(say):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1200, "height": 900}, ignore_https_errors=True, bypass_csp=True)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.on("dialog", lambda d: asyncio.ensure_future(d.accept()))
        await pg.goto(URL)
        await pg.wait_for_selector("#calsec", state="visible", timeout=8000)
        rows = await pg.inner_text("#calrows")
        check("Level" in rows and "none" in rows, "calibration section lists the Level channel, no record yet")
        await pg.select_option("#lvl", "factory"); await pg.fill("#pw", "9999"); await pg.click("#loginbtn")
        await pg.wait_for_selector('#calrows button:has-text("Begin")', timeout=5000)
        await pg.click('#calrows button:has-text("Begin")')
        await pg.wait_for_function("document.getElementById('calerr').textContent !== ''", timeout=5000)
        err = await pg.inner_text("#calerr")
        check(err != "", f"Begin refused in LOCAL mode with a reason ({err!r})")
        say("remote")
        await pg.wait_for_timeout(2500)
        await pg.click('#calrows button:has-text("Begin")')
        await pg.wait_for_selector("#calrun", state="visible", timeout=5000)
        await pg.wait_for_function("document.getElementById('state').textContent === 'CALIBRATION'", timeout=5000)
        check(True, "calibrating: the dialog shows and the header says CALIBRATION")
        await pg.locator("#calsec").screenshot(path=os.path.join(ROOT, "build", "web_cal_running.png"))
        await pg.wait_for_timeout(1500)                      # the demo takes its point and proposes coefficients
        await pg.click("#calaccept")
        await pg.wait_for_selector("#calrun", state="hidden", timeout=5000)
        await pg.wait_for_timeout(1500)
        rows = await pg.inner_text("#calrows")
        await pg.click("#calsec summary")
        hist = await pg.inner_text("#calhist")
        check("factory" in rows and "none" not in rows.split("\n")[0], f"in force after Accept: {rows.splitlines()[0] if rows else ''!r}")
        check("pass" in hist and "50" in hist, f"history records the pass with its point ({hist.splitlines()[0] if hist else ''!r})")
        await pg.click('#calrows button:has-text("Begin")')
        await pg.wait_for_selector("#calrun", state="visible", timeout=5000)
        await pg.click("#calabort")
        await pg.wait_for_selector("#calrun", state="hidden", timeout=5000)
        await pg.wait_for_timeout(1500)
        hist = await pg.inner_text("#calhist")
        check(hist.splitlines()[0].count("aborted") == 1 if hist else False, "an aborted calibration is recorded, newest first")
        await pg.locator("#calsec").screenshot(path=os.path.join(ROOT, "build", "web_cal.png"))
        # start-up self-test (CIP-11.1): a blank unit has no factory calibration image at start
        await pg.wait_for_function("!['-', 'Running...'].includes(document.getElementById('stsum').textContent)", timeout=15000)
        stsum, stl = await pg.inner_text("#stsum"), await pg.inner_text("#stlist")
        check(stsum.startswith("FAILED: Calibration") and "RAM: pass" in stl and "Ethernet: not checked" in stl and "demo_counter: pass" in stl,
              f"self-test shown ({stsum!r})")
        # service history (CIP-22, 23.4)
        crc = await pg.inner_text("#svccrc")
        check(len(crc) == 8 and (await pg.inner_text("#svchours")).endswith(" h"), f"service: hours and settings checksum shown ({crc})")
        await pg.fill("#svctech", "G. Brown"); await pg.fill("#svcwork", "Replaced sample pump")
        await pg.click("#svcadd")
        await pg.wait_for_function("document.getElementById('svclog').textContent.includes('Replaced sample pump')", timeout=8000)
        log = await pg.inner_text("#svclog")
        check("G. Brown" in log and "factory" in log, f"service log entry recorded ({log.splitlines()[0]!r})")
        await pg.fill("#svcres", "1,1,9")
        await pg.click("#svcrec")
        check("digits" in await pg.inner_text("#svcerr"), "a malformed test result is refused on the page")
        await pg.fill("#svcstation", "EOL-1"); await pg.fill("#svcres", "1,1,2,0")
        await pg.click("#svcrec")
        await pg.wait_for_function("document.getElementById('svctest').textContent.includes('EOL-1')", timeout=8000)
        tst = await pg.inner_text("#svctest")
        check(tst.startswith("FAILED") and "3 fail" in tst and "4 not run" in tst, f"test record shown ({tst!r})")
        await pg.locator("#svcsec").screenshot(path=os.path.join(ROOT, "build", "web_service.png"))
        check(not errs, "no script errors " + "; ".join(errs))
        await b.close()

def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), "15101", "18101", os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, text=True)
    def say(line):
        unit.stdin.write(line + "\n"); unit.stdin.flush()
    time.sleep(1)
    try:
        asyncio.run(run(say))
    finally:
        unit.terminate()
    print(f"CAL/SERVICE WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
