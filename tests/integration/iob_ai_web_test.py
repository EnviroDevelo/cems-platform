#!/usr/bin/env python3
"""Analog inputs of a map 3 I/O board (the CORE PCB as a node) on the I/O boards tab against
build/netsim: the card lists the 8 inputs (4-20 mA 1-6, level, mass flow) with their values and the
10 V / 5 V supplies; it says whether they are mapped to AI channels; at admin login (REMOTE) "Map to
AI 1-8" writes iobN_first_ai and each input then names its AI channel, and the button undoes it.
A board without analog inputs shows no section. Needs Python Playwright and a Chromium."""
import asyncio, glob, json, os, ssl, subprocess, sys, tempfile, time, urllib.request
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15980, 18980
URL = f"https://127.0.0.1:{WEB}/"
CTX = ssl._create_unverified_context()
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def board(n):
    with urllib.request.urlopen(URL + "api/iob", context=CTX, timeout=5) as f:
        return json.loads(f.read())["boards"][n - 1]


def until(fn, t=15):
    for _ in range(int(t * 5)):
        try:
            if fn(): return True
        except Exception: pass
        time.sleep(0.2)
    return False


async def run(say):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1300, "height": 1000}, ignore_https_errors=True, bypass_csp=True)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        await pg.goto(URL + "#iob")
        await pg.wait_for_selector("#iobsec", state="visible", timeout=10000)
        await pg.wait_for_function("document.getElementById('iobcards').innerText.includes('Analog inputs')", timeout=10000)
        card = await pg.inner_text("#iobcards .iob:nth-child(1)")
        check("4-20 mA 1" in card and "4.00 mA" in card and "mass flow" in card and "6.000 V" in card, "the 8 inputs with their values")
        check("supplies 10.02 V, 4.99 V" in card and "not mapped to AI channels" in card, "supplies, and not mapped yet")
        check("Analog inputs" not in await pg.inner_text("#iobcards .iob:nth-child(2)"), "a board without analog inputs shows none")
        check(not await pg.is_visible(".aimap"), "viewer: no mapping button")
        say("set 1 1")
        await pg.select_option("#lvl", "admin"); await pg.fill("#pw", "2222"); await pg.click("#loginbtn")
        await pg.wait_for_selector(".aimap", state="visible", timeout=10000)
        await pg.click(".aimap")
        await pg.wait_for_function("document.getElementById('iobcards').innerText.includes('as AI 1-8')", timeout=10000)
        check(until(lambda: board(1)["ai"][7]["channel"] == 8), "Map to AI 1-8: iob1_first_ai written, input 8 is AI 8")
        await pg.wait_for_function("document.getElementById('iobcards').innerText.includes('AI 8')", timeout=10000)
        await pg.wait_for_function("document.querySelector('.aimap') && document.querySelector('.aimap').textContent.startsWith('Stop')", timeout=10000)
        await pg.click(".aimap")
        await pg.wait_for_function("document.getElementById('iobcards').innerText.includes('not mapped to AI channels')", timeout=10000)
        check(board(1)["ai"][0]["channel"] == 0, "Stop mapping: back to not mapped")
        check(errs == [], f"no script errors {errs}")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"), os.path.join(tmp, "n.bin")],
                            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    def say(line): unit.stdin.write(line + "\n"); unit.stdin.flush()
    try:
        time.sleep(1.5)
        say("set 110 1"); say("set 120 1")              # boards 1 and 2 enabled
        say("node 1 ai 1"); say("node 1 restart")        # board 1 is a map 3 board
        ok = until(lambda: board(1)["ai"] is not None, 30)
        check(ok, "board 1 reports its analog inputs in /api/iob")
        if ok: asyncio.run(run(say))
    finally:
        unit.terminate()
        try: unit.wait(timeout=10)
        except subprocess.TimeoutExpired: unit.kill()
    print(f"\nIOB ANALOG INPUTS WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
