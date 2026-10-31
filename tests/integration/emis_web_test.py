#!/usr/bin/env python3
"""The Emissions tab against build/netsim (emissions engine, docs/EMISSIONS_MATH.md, parameters
400-469): hidden for a viewer while the engine is off, shown at admin login; the setup writes only
what changed and waits until it reads back; a ppm channel without a molar mass is refused on the
page; after enabling the engine with an O2 channel and a CO channel (both from the demo's
measurement), the results table shows the reading, the standard and O2-referenced values with the
units the engine uses, and the status words (an O2 above its limit gives "O2 too high" on the corrected channel); a setting
the engine rejects shows its configuration error; the Plant operating box writes 422; a viewer
sees the results but no setup. Firmware without the engine keeps the tab hidden.
Needs Python Playwright and a Chromium."""
import asyncio, glob, json, os, ssl, subprocess, sys, tempfile, time, urllib.request
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15975, 18975
URL = f"https://127.0.0.1:{WEB}/"
CTX = ssl._create_unverified_context()
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def get(path):
    with urllib.request.urlopen(URL + path, context=CTX, timeout=5) as f:
        return json.loads(f.read())


def has_emis():
    for _ in range(50):
        try: return any(d["id"] == 400 for d in get("api/params"))
        except OSError: time.sleep(0.2)
    return False


async def run(say, real):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1400, "height": 1100}, ignore_https_errors=True, bypass_csp=True)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        for _ in range(100):
            try: await pg.goto(URL); break
            except Exception: await asyncio.sleep(0.1)
        await pg.wait_for_timeout(2500)
        check(not await pg.is_visible('#tabbar [data-tab="emis"]'), "viewer, engine off: no Emissions tab")
        if not real:
            check(errs == [], f"no script errors {errs}")
            await b.close(); return
        say("set 1 1")                                   # REMOTE: the setup is not any-mode
        await pg.select_option("#lvl", "admin"); await pg.fill("#pw", "2222"); await pg.click("#loginbtn")
        await pg.wait_for_selector('#tabbar [data-tab="emis"]', state="visible", timeout=10000)
        await pg.click('#tabbar [data-tab="emis"]')
        await pg.wait_for_selector("#emedit", state="visible", timeout=10000)
        await pg.wait_for_function("document.querySelectorAll('#emplantf label').length > 10", timeout=10000)
        check(await pg.is_visible("#emnone"), "no channels yet")
        check(await pg.is_enabled("#emsave"), "admin, REMOTE: setup editable")
        # O2 dry from measurement 0 (the demo level, %), CO from the same measurement as ppm
        await pg.select_option("#emaddslot", "0"); await pg.click("#emadd")
        r1 = "#emchtab tbody tr:nth-child(1)"
        await pg.select_option(f"{r1} [data-f=src]", "0")
        await pg.select_option("#emaddslot", "3"); await pg.click("#emadd")
        r2 = "#emchtab tbody tr:nth-child(2)"
        check(await pg.input_value(f"{r2} [data-f=mm]") == "28.01" and await pg.is_checked(f"{r2} [data-f=ref]"), "a new CO row comes with its molar mass and O2 reference")
        await pg.select_option(f"{r2} [data-f=src]", "0")
        await pg.fill(f"{r2} [data-f=mm]", "")
        await pg.click("#emsave")
        await pg.wait_for_function("document.getElementById('emerr').textContent.length > 0", timeout=5000)
        check("molar mass of CO" in await pg.inner_text("#emerr"), "a ppm reading without a molar mass is refused: " + await pg.inner_text("#emerr"))
        await pg.fill(f"{r2} [data-f=mm]", "28.010")
        await pg.check('#emplantf [data-id="400"]')
        await pg.evaluate("document.getElementById('emerr').textContent = ''")
        await pg.click("#emsave")
        await pg.wait_for_function("document.getElementById('emerr').textContent.length > 0", timeout=20000)
        msg = await pg.inner_text("#emerr")
        check(msg.startswith("Saved ("), f"saved: {msg}")
        v = get("api/values?ids=400,430,431,435,433")["v"]
        check(v["400"] is True and v["430"][0] == 0 and v["431"][3] == 1 and abs(v["435"][3] - 28.01) < 1e-3 and v["433"][3], "settings read back")
        # results
        await pg.wait_for_function("document.querySelectorAll('#emtab tbody tr').length === 2 && document.querySelector('#emtab tbody tr:nth-child(2)').innerText.includes('no QAL2')", timeout=15000)
        rows = await pg.eval_on_selector_all("#emtab tbody tr", "t => t.map(r => [...r.cells].map(c => c.innerText))")
        print("   rows:", rows)
        if os.environ.get("SHOT"): await pg.locator("#emsec").screenshot(path=os.environ["SHOT"])
        o2, co = rows
        check(o2[0].startswith("O2 dry") and o2[1].endswith(" %") and o2[3] == "", f"O2 row: reading {o2[1]!r}, no O2-referenced value for O2 itself")
        check(co[0].startswith("CO") and co[1].endswith(" ppm") and co[2].endswith(" mg/Nm3"), f"CO row: {co[1]!r} raw, {co[2]!r} standard dry")
        std = float(co[2].split()[0]); raw = float(co[1].split()[0])
        check(abs(std - raw * 28.01 / 22.414) / std < 0.01, f"ppm to mg/Nm3 at 273.15 K, 101.325 kPa ({raw} ppm -> {std})")
        check("no QAL2" in co[0] and "O2 too high" in co[0], f"CO status: {co[0]!r} (b = 0; the demo's 50 % O2 is above the 20 % limit)")
        # the engine rejects: water measured but the water channel is not set up
        await pg.select_option('#emplantf [data-id="408"]', "1")
        await pg.check(f"{r2} [data-f=wet]")
        await pg.evaluate("document.getElementById('emerr').textContent = ''")
        await pg.click("#emsave")
        await pg.wait_for_function("document.getElementById('emerr').textContent.length > 0", timeout=20000)
        await pg.wait_for_selector("#emcfg", state="visible", timeout=8000)
        check(True, "configuration error shown: " + await pg.inner_text("#emcfg"))
        await pg.click("#emreload"); await pg.wait_for_timeout(500)
        await pg.select_option('#emplantf [data-id="408"]', "0"); await pg.uncheck(f"{r2} [data-f=wet]")
        await pg.evaluate("document.getElementById('emerr').textContent = ''")
        await pg.click("#emsave")
        await pg.wait_for_function("document.getElementById('emerr').textContent.startsWith('Saved')", timeout=20000)
        await pg.wait_for_selector("#emcfg", state="hidden", timeout=8000)
        check(True, "fixed again: no configuration error")
        # plant operating
        await pg.uncheck("#emop")
        await pg.wait_for_function("document.getElementById('emplant').textContent.includes('NOT operating')", timeout=8000)
        check(get("api/values?ids=422")["v"]["422"] is False, "Plant operating box writes 422")
        await pg.check("#emop")
        # viewer
        await pg.click("#logoutbtn"); await pg.wait_for_timeout(2500)
        await pg.click('#tabbar [data-tab="emis"]'); await pg.wait_for_timeout(1500)
        check(await pg.is_visible("#emtab") and not await pg.is_visible("#emedit"), "viewer: results, no setup")
        check(errs == [], f"no script errors {errs}")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"), os.path.join(tmp, "n.bin")],
                            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    def say(line): unit.stdin.write(line + "\n"); unit.stdin.flush()
    try:
        real = has_emis()
        print("emissions:", "the firmware's engine" if real else "firmware without the engine: hidden check only")
        asyncio.run(run(say, real))
    finally:
        unit.terminate()
        try: unit.wait(timeout=10)
        except subprocess.TimeoutExpired: unit.kill()
    print(f"\nEMISSIONS WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
