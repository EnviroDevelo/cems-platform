#!/usr/bin/env python3
"""Login feedback on a poor connection (DK bench, 31 Oct: a wrong or blank password showed nothing
while the header said "connecting") against build/netsim: a blank password is refused on the
page; while /api/login is pending the button says "Logging in…" and is disabled; a login with no
answer ends after 10 s with "No answer from the unit", and a network error says the same, never
"Wrong password"; a wrong password says "Wrong password"; while the values poll fails a note by
the login box says the password can't be checked. Needs Python Playwright and a Chromium."""
import asyncio, glob, os, subprocess, sys, tempfile, time
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15960, 18960
URL = f"https://127.0.0.1:{WEB}/"
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


async def run():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1200, "height": 900}, ignore_https_errors=True, bypass_csp=True)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        for _ in range(100):
            try: await pg.goto(URL); break
            except Exception: await asyncio.sleep(0.1)
        await pg.wait_for_function("document.getElementById('conn').textContent === 'live'", timeout=10000)
        check(not await pg.is_visible("#loginconn"), "connected: no 'not connected' note")
        err = lambda: pg.inner_text("#loginerr")
        await pg.select_option("#lvl", "admin"); await pg.fill("#pw", ""); await pg.click("#loginbtn")
        check(await err() == "Enter the password", f"blank password refused on the page ({await err()!r})")
        # no answer: the request hangs
        hold = asyncio.Event()
        async def hang(route):
            await hold.wait()
            try: await route.abort()
            except Exception: pass
        await pg.route("**/api/login", hang)
        await pg.fill("#pw", "2222"); t0 = time.time(); await pg.click("#loginbtn")
        await pg.wait_for_timeout(300)
        check(await pg.inner_text("#loginbtn") == "Logging in…" and await pg.is_disabled("#loginbtn"), "pending: 'Logging in…', button disabled")
        await pg.wait_for_function("document.getElementById('loginerr').textContent.length > 0", timeout=15000)
        dt = time.time() - t0
        check(await err() == "No answer from the unit, check the connection" and 9 <= dt <= 12, f"no answer after {dt:.1f} s: {await err()!r}")
        check(await pg.inner_text("#loginbtn") == "Log in" and await pg.is_enabled("#loginbtn"), "button back to 'Log in'")
        hold.set(); await pg.unroute("**/api/login")
        # a network error
        await pg.route("**/api/login", lambda route: route.abort())
        await pg.click("#loginbtn")
        await pg.wait_for_function("document.getElementById('loginerr').textContent.length > 0", timeout=5000)
        check(await err() == "No answer from the unit, check the connection", f"network error: {await err()!r}")
        await pg.unroute("**/api/login")
        # a wrong password, answered
        await pg.fill("#pw", "9876"); await pg.click("#loginbtn")
        await pg.wait_for_function("document.getElementById('loginerr').textContent.length > 0", timeout=10000)
        check(await err() == "Wrong password", f"wrong password: {await err()!r}")
        # the link goes: the note shows by the login box, and goes when it comes back
        await pg.route("**/api/values*", lambda route: route.abort())
        await pg.wait_for_selector("#loginconn", state="visible", timeout=10000)
        check("can't be checked" in await pg.inner_text("#loginconn"), "offline: " + await pg.inner_text("#loginconn"))
        await pg.unroute("**/api/values*")
        await pg.wait_for_selector("#loginconn", state="hidden", timeout=10000)
        check(True, "back online: note gone")
        await pg.fill("#pw", "2222"); await pg.click("#loginbtn")
        await pg.wait_for_selector("#logoutbtn", state="visible", timeout=10000)
        check(True, "the right password still logs in")
        check(errs == [], f"no script errors {errs}")
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
    print(f"\nLOGIN FEEDBACK WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
