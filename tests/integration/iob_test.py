#!/usr/bin/env python3
"""I/O boards on the RS-485 bus end to end (decision D27): build/netsim with its two
simulated boards, enabled and faulted from the console, checked through the web API
(/api/iob, /api/alarms, the event log) and, with Playwright, the web page.
    python3 tests/integration/iob_test.py"""
import json, os, ssl, subprocess, sys, tempfile, time, urllib.request
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MB, WEB = 15903, 18903
BASE = f"https://127.0.0.1:{WEB}"
CTX = ssl._create_unverified_context()
CHROME = os.environ.get("CHROME", "/opt/pw-browsers/chromium-1194/chrome-linux/chrome")
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1; print("  ok  ", what)
    else:
        failed += 1; print("  FAIL:", what)


def get(path):
    with urllib.request.urlopen(urllib.request.Request(BASE + path, headers={"Connection": "close"}), context=CTX, timeout=10) as f:
        return json.loads(f.read())


def wait(pred, secs=5.0):
    t = time.time() + secs
    while time.time() < t:
        try:
            v = pred()
            if v:
                return v
        except OSError:
            pass
        time.sleep(0.1)
    return None


def board(n):
    return get("/api/iob")["boards"][n - 1]


def alarm(name):
    return any(a["name"] == name and a["active"] for a in get("/api/alarms")["alarms"])


def events(text):
    return [e for e in get("/api/log/events?n=200")["events"] if e["event"] == "iob" and text in e["detail"]]


def screenshot():
    try:
        import asyncio
        from playwright.async_api import async_playwright
    except ImportError:
        print("  (no Playwright: page check skipped)")
        return

    async def run():
        async with async_playwright() as p:
            b = await p.chromium.launch(executable_path=CHROME)
            pg = await b.new_page(viewport={"width": 1200, "height": 900}, ignore_https_errors=True)
            errs = []
            pg.on("pageerror", lambda e: errs.append(str(e)))
            await pg.goto(BASE + "/")
            await pg.wait_for_selector("#iobsec", state="visible", timeout=8000)
            await pg.wait_for_timeout(1500)
            txt = await pg.inner_text("#iobsec")
            check("Board 1" in txt and "Out 3: open circuit" in txt, "web page shows board 1 and its fault")
            check("not fitted" in txt.lower(), "web page shows the boards not fitted")
            await pg.locator("#iobsec").screenshot(path=os.path.join(ROOT, "build", "web_iob.png"))
            check(not errs, "no script errors on the page " + "; ".join(errs))
            await b.close()
    asyncio.run(run())


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, text=True)

    def say(line):
        unit.stdin.write(line + "\n"); unit.stdin.flush()
    try:
        check(wait(lambda: get("/api/info")), "unit up")
        b = board(1)
        check(not b["enabled"] and b["state"] == "not fitted", "boards are off by default")
        check(get("/api/iob")["boards"][2]["first_do"] == 37, "board 3 defaults to outputs 37-54")

        say("set 110 1")                                   # iob1_enable, as a technician would
        check(wait(lambda: board(1)["state"] == "online"), "board 1 online after enabling")
        b = board(1)
        check(b["fw"] == "1.0" and b["errors"] == 0, "identification read, no failed requests")
        check(wait(lambda: board(1)["inputs"] & 1, 13), "input 1 follows the simulated sensor")
        check(events("online"), "event log: board 1 online")

        say("node 1 open 4")                               # output 3 open circuit
        check(wait(lambda: "Out 3: open circuit" in board(1)["faults"]), "open circuit on output 3 reported")
        check(wait(lambda: alarm("IOB_OUT_FAULT"), 4), "output fault alarm raised")
        check(events("out 3 open circuit"), "event log: out 3 open circuit")
        screenshot()
        say("node 1 open 0")
        check(wait(lambda: not board(1)["faults"]), "fault clears")

        say("node 1 fielda 0")
        check(wait(lambda: "Field 24 V A missing" in board(1)["faults"]), "field supply A missing reported")
        check(wait(lambda: alarm("IOB_SUPPLY"), 4), "supply alarm raised")
        say("node 1 fielda 1")

        say("node 1 power 0")
        check(wait(lambda: board(1)["state"] == "not responding"), "board 1 not responding after power off")
        check(wait(lambda: alarm("IOB_LOST"), 3), "IOB_LOST alarm raised")
        check(events("not responding"), "event log: not responding")
        say("node 1 restart")
        say("node 1 power 1")
        check(wait(lambda: board(1)["state"] == "online", 5), "board 1 back online on its own")
        check(wait(lambda: not alarm("IOB_LOST"), 3), "IOB_LOST clears")

        say("node 1 wdg")                                  # node restarted by its watchdog
        check(wait(lambda: alarm("IOB_NODE_RESET"), 3), "node watchdog reset alarm (latched)")

        say("set 122 10")                                  # board 2's outputs overlapping board 1's
        say("set 120 1")
        check(wait(lambda: board(2)["state"] == "settings conflict"), "overlapping channel ranges refused")
        check(wait(lambda: alarm("IOB_CONFIG"), 3), "IOB_CONFIG alarm raised")
        say("set 122 19")
        check(wait(lambda: board(2)["state"] == "online"), "board 2 online once its range is fixed")
        check(wait(lambda: not alarm("IOB_CONFIG"), 3), "IOB_CONFIG clears")
    finally:
        unit.terminate()
        unit.wait(10)
    print(f"I/O BUS: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
