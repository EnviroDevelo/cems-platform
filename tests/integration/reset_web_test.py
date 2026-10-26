#!/usr/bin/env python3
"""Lost passwords from the web page (credential recovery, CIP-19.3) against build/netsim with a
developer key: the login's "Lost passwords?" panel shows the line to send the developer, a
malformed token is refused on the page, a token from another key is refused by the unit, the
developer's token (tools/reset_token.py) puts the passwords back to the defaults, and it works
only once. Without a developer key the panel says recovery is not possible.
Needs Python Playwright and a Chromium."""
import asyncio, glob, os, subprocess, sys, tempfile, time
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fwkey
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15903, 18903
URL = f"https://127.0.0.1:{WEB}/"
UID = "0a1b2c3d4e5f60718293a4b5"
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def token(key, n, uid=UID):
    return subprocess.run([sys.executable, os.path.join(ROOT, "tools", "reset_token.py"), "--key", key, "--uid", uid, "--n", str(n)],
                          capture_output=True, text=True, check=True).stdout.strip()


def run_unit(tmp, env):
    return subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, env=env)


def stop(unit):
    unit.terminate()
    try: unit.wait(timeout=10)
    except subprocess.TimeoutExpired: unit.kill(); unit.wait()


async def page(b):
    pg = await b.new_page(viewport={"width": 1200, "height": 900}, ignore_https_errors=True, bypass_csp=True)
    pg.errs = []
    pg.on("pageerror", lambda e: pg.errs.append(str(e)))
    for _ in range(100):
        try: await pg.goto(URL); break
        except Exception: await asyncio.sleep(0.1)
    await pg.wait_for_selector("#loginrow", state="visible", timeout=8000)
    return pg


async def login(pg, lvl, pw):
    await pg.select_option("#lvl", lvl); await pg.fill("#pw", pw); await pg.click("#loginbtn")
    await pg.wait_for_timeout(800)
    return await pg.is_visible("#userrow")


async def err_after(pg, before=""):
    await pg.wait_for_function(f"document.getElementById('rsterr').textContent !== {before!r}", timeout=8000)
    return await pg.inner_text("#rsterr")


async def run(dev, other, tmp):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        print("with the developer key")
        unit = run_unit(tmp, dict(os.environ, CEMS_UID=UID, CEMS_DEV_PUBKEY=fwkey.public_point(dev).hex()))
        try:
            pg = await page(b)
            check(await login(pg, "admin", "2222"), "admin logs in with the default password")
            for lvl in ("admin", "factory"):
                await pg.select_option("#pwlvl", lvl); await pg.fill("#newpw", "lost-it-9"); await pg.click("#pwbtn")
                await pg.wait_for_function("document.getElementById('pwerr').textContent !== ''", timeout=5000)
            await pg.click("#logoutbtn") if await pg.is_visible("#logoutbtn") else None
            await pg.reload(); await pg.wait_for_selector("#loginrow", state="visible")
            check(not await login(pg, "admin", "2222"), "after changing them, the default admin password no longer works")
            await pg.click("#rstbox summary")
            await pg.wait_for_function("document.getElementById('rstmsg').value !== ''", timeout=5000)
            msg = await pg.input_value("#rstmsg")
            check(msg == f"CEMS-RESET v1 uid={UID} n=0" and await pg.is_visible("#rstform"), f"the panel shows the line to send ({msg!r})")
            await pg.fill("#rsttok", "abc"); await pg.click("#rstbtn")
            check("128 hex" in await pg.inner_text("#rsterr"), "a malformed token is refused on the page")
            await pg.fill("#rsttok", token(other, 0)); await pg.click("#rstbtn")
            e = await err_after(pg, await pg.inner_text("#rsterr"))
            check("not valid for this unit" in e, f"a token from another key is refused ({e!r})")
            good = token(dev, 0)
            await pg.fill("#rsttok", good); await pg.click("#rstbtn")
            e = await err_after(pg, e)
            check(e.startswith("Done"), f"the developer's token is accepted ({e!r})")
            await pg.wait_for_selector("#defpw", state="visible", timeout=5000)
            check(True, "the factory-default-passwords warning shows")
            await pg.wait_for_function("document.getElementById('rstmsg').value.endsWith('n=1')", timeout=5000)
            check(True, "the line to send moves on to n=1")
            await pg.locator("#rstbox").screenshot(path=os.path.join(ROOT, "build", "web_reset.png"))
            await pg.fill("#rsttok", good); await pg.click("#rstbtn")
            e2 = await err_after(pg, e)
            check("not valid" in e2, "the same token does not work twice")
            check(await login(pg, "admin", "2222"), "admin logs in with the default password again")
            check(not await pg.is_visible("#rstbox"), "the panel hides once logged in")
            check(pg.errs == [], f"no script errors {pg.errs}")
        finally:
            stop(unit)
        print("without a developer key")
        os.remove(os.path.join(tmp, "f.bin"))
        unit = run_unit(tmp, dict(os.environ, CEMS_UID=UID))
        try:
            pg = await page(b)
            await pg.click("#rstbox summary")
            await pg.wait_for_function("document.getElementById('rstnote').textContent.includes('no developer key')", timeout=5000)
            check(not await pg.is_visible("#rstform"), "says recovery is not possible and shows no token box")
        finally:
            stop(unit)
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    dev, other = os.path.join(tmp, "dev.pem"), os.path.join(tmp, "other.pem")
    so = sys.stdout; sys.stdout = open(os.devnull, "w")
    fwkey.new_key(dev); fwkey.new_key(other)
    sys.stdout = so
    asyncio.run(run(dev, other, tmp))
    print(f"\n{passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
