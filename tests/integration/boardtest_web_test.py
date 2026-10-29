#!/usr/bin/env python3
"""The factory board test on the Service tab (CIP-23.3) against build/netsim: hidden below factory
login, Run board test starts it, the results table fills while it runs and when it is done, the
test record shows the tests by name, and a refused start (409) is explained. Firmware without the
board test (no "boardtest" in /api/service) gets it simulated in the browser, so the page side is
checked either way. Needs Python Playwright and a Chromium."""
import asyncio, glob, json, os, ssl, subprocess, sys, tempfile, time, urllib.request
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15910, 18910
URL = f"https://127.0.0.1:{WEB}/"
NAMES = ["fram", "ram", "rtc", "image", "log_flash", "net_phy", "net_link", "io_port", "io_link", "display", "supply", "hw_rev"]
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def real_boardtest():
    ctx = ssl._create_unverified_context()
    for _ in range(50):
        try:
            with urllib.request.urlopen(URL + "api/service", context=ctx, timeout=5) as f:
                return "boardtest" in json.loads(f.read())
        except OSError:
            time.sleep(0.2)
    return False


class Sim:                                    # the firmware's board test, as agreed, for builds without it
    def __init__(self): self.t0 = None; self.post_refuse = False
    def state(self):
        if self.t0 is None:                   # like the firmware: every name listed, all skipped, before a run
            return {"running": False, "done": False, "saved": False,
                    "tests": [{"name": n, "result": "skipped", "detail": ""} for n in NAMES]}
        k = min(len(NAMES), int((time.time() - self.t0) / 0.2))
        tests = [{"name": n, "result": "skipped" if n in ("display", "io_link") else "fail" if n == "net_link" else "pass",
                  "detail": "no link" if n == "net_link" else ""} for n in NAMES[:k]]
        return {"running": k < len(NAMES), "done": k == len(NAMES), "saved": k == len(NAMES), "tests": tests}


async def run(sim):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1200, "height": 900}, ignore_https_errors=True, bypass_csp=True)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        if sim:
            async def svc(route):
                r = await route.fetch(); j = await r.json(); st = sim.state(); j["boardtest"] = st
                if st["done"]:
                    j["test"] = {"time": j.get("test", {}) and 0 or 0, "station": "board", "passed": False, "n": 12,
                                 "results": [{"pass": 1, "fail": 2}.get(x["result"], 0) for x in st["tests"]], "names": NAMES}
                await route.fulfill(response=r, json=j)
            async def start(route):
                if route.request.method != "POST": return await route.continue_()
                if sim.post_refuse or (sim.t0 and sim.state()["running"]):
                    return await route.fulfill(status=409, json={"error": "busy"})
                sim.t0 = time.time(); await route.fulfill(json={"ok": True})
            await pg.route("**/api/service", svc)
            await pg.route("**/api/service/boardtest", start)
        for _ in range(100):
            try: await pg.goto(URL + "#service"); break
            except Exception: await asyncio.sleep(0.1)
        await pg.wait_for_selector("#svcsec", state="visible", timeout=8000)
        await pg.wait_for_timeout(1000)
        check(not await pg.is_visible("#btrun"), "no Run button below factory login")
        await pg.select_option("#lvl", "factory"); await pg.fill("#pw", "9999"); await pg.click("#loginbtn")
        await pg.wait_for_selector("#btrun", state="visible", timeout=12000)
        await pg.wait_for_timeout(11000 if not sim else 500)      # the real one refuses until the start-up self-test is done
        await pg.click("#btrun")
        await pg.wait_for_function("document.getElementById('btstate').textContent.includes('running')", timeout=5000)
        check(await pg.is_disabled("#btrun"), "running: the button is disabled")
        await pg.wait_for_function("document.getElementById('btstate').textContent.startsWith('Last board test')", timeout=15000)
        state = await pg.inner_text("#btstate"); rows = await pg.inner_text("#bttab")
        check(len(rows.strip().splitlines()) == 13 and "Settings memory (FRAM)" in rows and "Board revision" in rows,
              f"12 results by name ({state!r})")
        check("saved as the manufacturing test record" in state, "saved as the test record")
        await pg.wait_for_timeout(1500)
        rec = await pg.inner_text("#svctest")
        check("station board" in rec and "Settings memory (FRAM)" in rec, f"the test record names its tests ({rec[:90]!r})")
        if sim:
            sim.post_refuse = True
            await pg.click("#btrun")
            await pg.wait_for_function("document.getElementById('svcerr').textContent.startsWith('Not now')", timeout=5000)
            check(True, "a refused start (409) is explained")
        await pg.locator("#svcsec").screenshot(path=os.path.join(ROOT, "build", "web_boardtest.png"))
        check(errs == [], f"no script errors {errs}")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"), os.path.join(tmp, "n.bin")],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sim = None if real_boardtest() else Sim()
        print("board test:", "simulated in the browser (firmware without it)" if sim else "the firmware's own")
        asyncio.run(run(sim))
    finally:
        unit.terminate()
        try: unit.wait(timeout=10)
        except subprocess.TimeoutExpired: unit.kill()
    print(f"\nBOARD TEST WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
