#!/usr/bin/env python3
"""Two web page behaviours against build/netsim:
- closing the page logs out: a reload and a second tab keep the login; a page opened after every
  page of the instrument was closed ends the left-over login;
- the Service tab's System health panel (memory and stack headroom, scans, TLS, last fault), with
  the target's health figures injected into /api/info (the PC build has none).
Needs Python Playwright and a Chromium."""
import asyncio, glob, os, subprocess, sys, tempfile
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15909, 18909
URL = f"https://127.0.0.1:{WEB}/"
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


async def open_page(ctx, url=URL):
    pg = await ctx.new_page()
    pg.errs = []
    pg.on("pageerror", lambda e: pg.errs.append(str(e)))
    for _ in range(100):
        try: await pg.goto(url); break
        except Exception: await asyncio.sleep(0.1)
    await pg.wait_for_timeout(1500)
    return pg


async def logged_in(pg):
    await pg.wait_for_timeout(600)
    return await pg.is_visible("#userrow")


async def run():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        ctx = await b.new_context(viewport={"width": 1200, "height": 900}, ignore_https_errors=True, bypass_csp=True)
        print("closing the page logs out")
        a = await open_page(ctx)
        await a.fill("#pw", "1111"); await a.click("#loginbtn")
        check(await logged_in(a), "operator logged in")
        await a.reload(); await a.wait_for_timeout(1500)
        check(await logged_in(a), "a reload keeps the login")
        b2 = await open_page(ctx)
        check(await logged_in(b2) and await logged_in(a), "a second tab next to an open one keeps it, in both")
        await a.close(); await b2.close()
        c = await open_page(ctx)
        check(not await logged_in(c) and await c.is_visible("#loginrow"), "a page opened after all were closed ends the left-over login")
        errs = c.errs
        await c.close()
        print("System health panel")
        async def add_health(route):
            r = await route.fetch(); j = await r.json()
            j["health"] = {"known": True, "heap_size": 65536, "heap_free": 40960, "heap_min": 30720, "low": True,
                           "why": "stack of task net below 512 bytes", "stacks": {"scan": 1024, "net": 100, "store": 600}}
            j["last_fault"] = {"code": 3, "name": "bus", "arg": 0x0801A2C4, "lr": 0x08004001, "cfsr": 0x8200, "hfsr": 0x40000000, "addr": 0x20050000, "boots": 4}
            await route.fulfill(response=r, json=j)
        await ctx.route("**/api/info", add_health)
        h = await open_page(ctx, URL + "#service")
        await h.wait_for_selector("#hlstacks", state="visible", timeout=8000)
        tiles = await h.inner_text("#hltiles")
        check("40 KB (30 KB) of 64 KB" in tiles and "Secure connections" in tiles, f"memory tile ({tiles.splitlines()[:2]})")
        rows = await h.inner_text("#hlstacks")
        red = await h.evaluate("[...document.querySelectorAll('#hlstacks td')].filter(td => td.style.color).map(td => td.textContent)")
        check("net\t400 bytes" in rows and "scan\t4096 bytes" in rows and red == ["400 bytes"], f"stack table in bytes, short one in red ({red})")
        why = await h.inner_text("#hlwhy")
        check(why.startswith("LOW: stack of task net"), f"low headroom explained ({why!r})")
        flt = await h.inner_text("#hlfault")
        check("bus at 0x0801A2C4" in flt and "LR 0x08004001" in flt and "address 0x20050000" in flt and "Starts since" in flt, f"last fault ({flt!r})")
        await h.locator("#svcsec").screenshot(path=os.path.join(ROOT, "build", "web_health.png"))
        check(errs + h.errs == [], f"no script errors {errs + h.errs}")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"), os.path.join(tmp, "n.bin")],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try: asyncio.run(run())
    finally:
        unit.terminate()
        try: unit.wait(timeout=10)
        except subprocess.TimeoutExpired: unit.kill()
    print(f"\nSESSION/HEALTH: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
