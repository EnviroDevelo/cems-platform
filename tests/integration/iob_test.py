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


def commission():
    """A new board (no address, answering at 246) found and given address 2 from the web page."""
    try:
        import asyncio
        from playwright.async_api import async_playwright
    except ImportError:
        print("  (no Playwright: commissioning page check skipped)")
        return

    async def run():
        async with async_playwright() as p:
            b = await p.chromium.launch(executable_path=CHROME)
            pg = await b.new_page(viewport={"width": 1200, "height": 900}, ignore_https_errors=True)
            errs = []
            pg.on("pageerror", lambda e: errs.append(str(e)))
            pg.on("dialog", lambda d: asyncio.ensure_future(d.accept()))
            await pg.goto(BASE + "/")
            await pg.select_option("#lvl", "admin")
            await pg.fill("#pw", "2222")
            await pg.click("#loginbtn")
            await pg.wait_for_selector("#cmfind", state="visible", timeout=8000)
            await pg.click("#cmfind")
            await pg.wait_for_selector("#cmset", state="visible", timeout=8000)
            txt = await pg.inner_text("#cminfo")
            check("246 (new board" in txt and "serial" in txt, "web page finds the new board at 246: " + txt)
            await pg.select_option("#cmto", "2")
            await pg.locator("#cminfo").scroll_into_view_if_needed()
            await pg.locator("#admin").screenshot(path=os.path.join(ROOT, "build", "web_iob_commission.png"))
            await pg.click("#cmbtn")
            await pg.wait_for_function("document.getElementById('cminfo').textContent.includes('changed to 2')", timeout=8000)
            check(True, "web page sets address 2")
            await pg.click("#cmfind")
            await pg.wait_for_function("document.getElementById('cminfo').textContent.includes('no reply')", timeout=8000)
            check(True, "no new board left at 246")
            check(not errs, "no script errors on the page " + "; ".join(errs))
            await b.close()
    asyncio.run(run())


def net_commission():
    """A new board on Ethernet (no IP, no address) found on the network and given board slot 2."""
    try:
        import asyncio
        from playwright.async_api import async_playwright
    except ImportError:
        print("  (no Playwright: Ethernet commissioning page check skipped)")
        return

    async def run():
        async with async_playwright() as p:
            b = await p.chromium.launch(executable_path=CHROME)
            pg = await b.new_page(viewport={"width": 1200, "height": 900}, ignore_https_errors=True)
            errs = []
            pg.on("pageerror", lambda e: errs.append(str(e)))
            pg.on("dialog", lambda d: asyncio.ensure_future(d.accept()))
            await pg.goto(BASE + "/")
            await pg.select_option("#lvl", "admin")
            await pg.fill("#pw", "2222")
            await pg.click("#loginbtn")
            await pg.wait_for_selector("#dcfind", state="visible", timeout=8000)
            check(not await pg.is_visible("#cmfind"), "RS-485 address panel hidden on Ethernet")
            await pg.click("#dcfind")
            await pg.wait_for_function("document.getElementById('dcinfo').textContent.includes('2 boards found')", timeout=8000)
            rows = await pg.inner_text("#dcrows")
            check("none (new)" in rows and "Board 1, running" in rows, "web page lists board 1 and the new board: " + rows)
            new_row = pg.locator("#dcrows tr", has_text="none (new)")
            await new_row.locator("select").select_option("2")
            await pg.locator("#dcsec").scroll_into_view_if_needed()
            await pg.locator("#dcsec").screenshot(path=os.path.join(ROOT, "build", "web_iob_net.png"))
            await new_row.locator("button").click()
            await pg.wait_for_function("document.getElementById('dcinfo').textContent.includes('Done')", timeout=8000)
            rows = await pg.inner_text("#dcrows")
            check("192.168.1.62" in rows and "none (new)" not in rows, "web page sets the board's IP: " + rows)
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
        time.sleep(1.5)
        check(alarm("IOB_LOST"), "IOB_LOST stays latched until acknowledged (board and master)")
        say("set 40 12")                                   # acknowledge IOB_LOST, as from the touchscreen
        check(wait(lambda: not alarm("IOB_LOST"), 3), "IOB_LOST clears once acknowledged")

        say("node 1 wdg")                                  # node restarted by its watchdog
        check(wait(lambda: alarm("IOB_NODE_RESET"), 3), "node watchdog reset alarm (latched)")

        say("set 122 10")                                  # board 2's outputs overlapping board 1's
        say("set 120 1")
        check(wait(lambda: board(2)["state"] == "settings conflict"), "overlapping channel ranges refused")
        check(wait(lambda: alarm("IOB_CONFIG"), 3), "IOB_CONFIG alarm raised")
        say("set 122 19")
        check(wait(lambda: board(2)["state"] == "online"), "board 2 online once its range is fixed")
        check(wait(lambda: not alarm("IOB_CONFIG"), 3), "IOB_CONFIG clears")

        check(get("/api/iob/net")["link"] == "ethernet", "the boards are on Ethernet by default")
        say("node 2 new")                                  # board 2 replaced by a new board (no IP, no address)
        check(wait(lambda: board(2)["state"] == "not responding"), "board 2 not responding without an IP")
        net_commission()
        check(wait(lambda: board(2)["state"] == "online", 5), "board 2 online again with its IP")
        check(events("IP .1.62 set"), "event log: IP set")

        say("set 102 0")                                   # iob_link: RS-485
        check(wait(lambda: board(1)["state"] == "online" and board(2)["state"] == "online", 5), "both boards online over RS-485")
        say("node 2 addr 0")                               # board 2 replaced by a new board (no address)
        check(wait(lambda: board(2)["state"] == "not responding"), "board 2 not responding without an address")
        commission()
        check(wait(lambda: board(2)["state"] == "online", 5), "board 2 online again at its new address")
        check(events("addr 246->2 set"), "event log: address set")
    finally:
        unit.terminate()
        unit.wait(10)
    print(f"I/O BUS: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
