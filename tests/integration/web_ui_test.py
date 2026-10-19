#!/usr/bin/env python3
"""Drive the web page in headless Chromium against build/netsim: view, log in,
change a setting (one refused, one accepted), check for script errors, and save
screenshots to build/. Needs Python Playwright and a Chromium."""
import asyncio, os, signal, subprocess, sys, tempfile, time
from playwright.async_api import async_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME", "/opt/pw-browsers/chromium-1194/chrome-linux/chrome")
URL = "http://127.0.0.1:18095/"

async def run(unit):
    ok = True
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1200, "height": 900})
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.on("console", lambda m: errs.append(m.text) if m.type == "error" and "401" not in m.text and "400" not in m.text else None)
        await pg.goto(URL)
        await pg.wait_for_selector("#v1001")
        await pg.wait_for_timeout(1200)
        await pg.screenshot(path=os.path.join(ROOT, "build", "web_view.png"), full_page=True)
        await pg.fill("#pw", "1111")
        await pg.click("#loginbtn")
        await pg.wait_for_selector("#userrow", state="visible")
        await pg.fill("#filter", "start_delay")
        btn = pg.locator('#rows button:has-text("Change")')
        await btn.first.click()
        await pg.fill("#rows td input", "99")
        await pg.click('#rows button:has-text("Set")')
        await pg.wait_for_timeout(400)
        refused = await pg.inner_text("#rows .err")
        await pg.fill("#rows td input", "1500")
        await pg.click('#rows button:has-text("Set")')
        await pg.wait_for_timeout(1000)
        shown = await pg.inner_text("#v1001")
        await pg.fill("#filter", "")
        await pg.wait_for_timeout(5500)                    # the event list refreshes every 5 s
        events = await pg.inner_text("#events")
        print("event list:", events.replace("\n", " | ")[:300])
        ok &= "Setting changed" in events and "start_delay (1001): 2000 → 1500" in events and "Login" in events \
              and "Power-up" in events
        await pg.screenshot(path=os.path.join(ROOT, "build", "web_operator.png"), full_page=True)
        print("refused value message:", repr(refused)); ok &= refused == "Outside the allowed range"
        print("value shown after change:", repr(shown)); ok &= shown == "1500 ms"
        # connection loss: stop the unit, the page must say so and grey out
        unit.send_signal(signal.SIGTERM); unit.wait(timeout=10)
        await pg.wait_for_timeout(2500)
        stale = await pg.evaluate("document.body.classList.contains('stale')")
        banner = await pg.inner_text("#lost")
        await pg.screenshot(path=os.path.join(ROOT, "build", "web_lost.png"))
        print("after the unit stops: stale =", stale, "| banner:", repr(banner)); ok &= stale and banner.startswith("Connection lost")
        errs = [e for e in errs if "ERR_CONNECTION_REFUSED" not in e and "Failed to load resource" not in e]
        print("page script errors:", errs); ok &= not errs
        await b.close()
        return ok

def main():
    tmp = tempfile.mkdtemp()
    fram, nor = os.path.join(tmp, "fram.bin"), os.path.join(tmp, "nor.bin")
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), "15095", "18095", fram, nor],
                            stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(0.8)
    unit.stdin.write("remote\n"); unit.stdin.flush()
    ok = asyncio.run(run(unit))
    print("WEB UI:", "PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
