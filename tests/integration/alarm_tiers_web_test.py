#!/usr/bin/env python3
"""Alarm priority tiers on the Alarm setup tab against build/netsim (firmware PR #19): the tiers
table lists 5 tiers with name, actions and live counts; a renamed tier and a tier's action read
back from /api/alarms/config; each alarm's Tier picker sets parameter 52 (default, a tier, or off
where allowed); a Critical alarm shows its tier in the alarm list, is marked, and the header badge
says CRITICAL; the Excel handover file lists common-alarm bits 5 and 6 and each alarm's tier.
Firmware without tiers keeps the table and column hidden, and the test only checks that.
Needs Python Playwright, openpyxl and a Chromium."""
import asyncio, glob, io, json, os, ssl, subprocess, sys, tempfile, time, urllib.request
import openpyxl
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15965, 18965
URL = f"https://127.0.0.1:{WEB}/"
CTX = ssl._create_unverified_context()
DEMO_ALARM = 100
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def get(path):
    with urllib.request.urlopen(URL + path, context=CTX, timeout=5) as f:
        return json.loads(f.read())


def has_tiers():
    for _ in range(50):
        try: return "tiers" in get("api/alarms/config")
        except OSError: time.sleep(0.2)
    return False


async def until(fn, what, t=10):
    for _ in range(int(t * 5)):
        try:
            if fn(): return True
        except Exception: pass
        await asyncio.sleep(0.2)
    return False


async def run(say, real):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        ctx = await b.new_context(viewport={"width": 1300, "height": 1000}, ignore_https_errors=True, bypass_csp=True, accept_downloads=True)
        pg = await ctx.new_page()
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        for _ in range(100):
            try: await pg.goto(URL); break
            except Exception: await asyncio.sleep(0.1)
        await pg.wait_for_timeout(1000)
        await pg.select_option("#lvl", "admin"); await pg.fill("#pw", "2222"); await pg.click("#loginbtn")
        await pg.wait_for_selector('#tabbar [data-tab="alarmcfg"]', state="visible", timeout=10000)
        await pg.click('#tabbar [data-tab="alarmcfg"]')
        await pg.wait_for_selector("#actrows tr", timeout=10000)
        await pg.wait_for_timeout(500)
        if not real:
            check(not await pg.is_visible("#tierbox") and not await pg.is_visible("#actions th.tiercol"), "firmware without tiers: no tiers table or Tier column")
            check(errs == [], f"no script errors {errs}")
            await b.close(); return
        check(len(await pg.query_selector_all("#tiertab tbody tr")) == 5, "5 tiers listed")
        # rename tier 1
        await pg.fill("#tiertab tbody tr:nth-child(1) input:not([type])", "Urgent"); await pg.press("#tiertab tbody tr:nth-child(1) input:not([type])", "Tab")
        ok = await until(lambda: get("api/alarms/config")["tiers"][0]["name"] == "Urgent", "rename")
        check(ok, "tier 1 renamed 'Urgent' (reads back)")
        # tier 2 emails
        await pg.wait_for_timeout(800)
        await pg.click("#tiertab tbody tr:nth-child(2) input[type=checkbox] >> nth=0")
        ok = await until(lambda: (t := get("api/alarms/config")["tiers"][1])["configured"] and t["route"] & 1, "route")
        check(ok, "tier 2: Email ticked (alarm_tier_route reads back)")
        # the demo fault alarm to tier 1
        await pg.wait_for_timeout(800)
        row = pg.locator("#actrows tr", has_text=f"({DEMO_ALARM})")
        opts = await row.locator("select").first.evaluate("s => [...s.options].map(o => o.text)")
        check(opts[0].startswith("default (") and "Urgent" in opts, f"Tier picker: {opts}")
        await row.locator("select").first.select_option("1")
        ok = await until(lambda: next(a for a in get("api/alarms/config")["alarms"] if a["id"] == DEMO_ALARM)["priority"] == 1, "prio")
        check(ok, f"alarm {DEMO_ALARM} set to tier 1")
        await pg.wait_for_timeout(800)
        sel = await pg.locator("#actrows tr", has_text=f"({DEMO_ALARM})").locator("select").first.input_value()
        check(sel == "1", f"picker shows the setting after the reload ({sel})")
        offs = [a for a in get("api/alarms/config")["alarms"] if not a["can_switch_off"]]
        if offs:
            o2 = await pg.locator("#actrows tr", has_text=f"({offs[0]['id']})").locator("select").first.evaluate("s => [...s.options].map(o => o.value)")
            check("6" not in o2, f"an alarm that can't be switched off has no Off ({offs[0]['name']})")
        # raise it: the list shows the tier, marked, the badge says CRITICAL
        await pg.evaluate("fetch('/api/write', {method: 'POST', headers: {'X-CEMS': '1', 'Content-Type': 'application/x-www-form-urlencoded'}, body: 'id=1007&v=1'})")
        await pg.click('#tabbar [data-tab="overview"]')
        await pg.wait_for_function("[...document.querySelectorAll('#alarmrows tr')].some(r => r.textContent.includes('Urgent,'))", timeout=10000)
        crit = await pg.evaluate("[...document.querySelectorAll('#alarmrows tr')].some(r => r.textContent.includes('Urgent,') && r.classList.contains('crit'))")
        check(crit, "alarm list: tier shown, row marked")
        await pg.wait_for_function("document.getElementById('alarmbadge').textContent.startsWith('CRITICAL')", timeout=8000)
        check(True, "header badge: " + await pg.inner_text("#alarmbadge"))
        t1 = await until(lambda: get("api/alarms/config")["tiers"][0]["active"] >= 1, "count")
        check(t1, "tier 1 counts the active alarm")
        # handover file
        await pg.click('#tabbar [data-tab="service"]')
        await pg.wait_for_selector("#mbxlsx", state="visible", timeout=10000)
        async with pg.expect_download(timeout=15000) as dl:
            await pg.click("#mbxlsx")
        wb = openpyxl.load_workbook(io.BytesIO(open(await (await dl.value).path(), "rb").read()))
        rows = [[c.value for c in r] for r in wb["Codes"].iter_rows(min_row=2)]
        bits = {r[1] for r in rows if r[0] == "Common alarm bit"}
        check({5, 6} <= bits, f"Excel Codes: common alarm bits 5 and 6 ({sorted(bits)})")
        dm = next((r for r in rows if r[0] == "Alarm ID" and r[1] == DEMO_ALARM), None)
        check(dm is not None and "Urgent" in dm[3], f"Excel Codes: alarm {DEMO_ALARM} names its tier ({dm and dm[3]!r})")
        check(errs == [], f"no script errors {errs}")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"), os.path.join(tmp, "n.bin")],
                            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    def say(line): unit.stdin.write(line + "\n"); unit.stdin.flush()
    try:
        real = has_tiers()
        print("alarm tiers:", "the firmware's own" if real else "firmware without tiers: hidden check only")
        asyncio.run(run(say, real))
    finally:
        unit.terminate()
        try: unit.wait(timeout=10)
        except subprocess.TimeoutExpired: unit.kill()
    print(f"\nALARM TIERS WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
