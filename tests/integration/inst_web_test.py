#!/usr/bin/env python3
"""The Instruments tab (third-party analysers over Modbus TCP, docs/MODBUS_ROLES.md role 2) against
build/netsim and its simulated gas analyser at 192.168.1.70, the way a technician sets one up:
the register explorer reads the analyser, every decoding is shown, the value the analyser displays
is found (O2 as a CDAB float), "use this" fills a new point, Save writes the settings (parameters
500-621), and the point's live value appears with its quality. The analyser's status word turns
into "maintenance", Export profile saves the device as JSON and Import profile loads it into
another instrument, and a viewer sees values but no editor. Firmware without /api/inst (before
PR #16) keeps the tab hidden, and the test only checks that. Needs Python Playwright and a Chromium."""
import asyncio, glob, json, os, ssl, subprocess, sys, tempfile, time, urllib.request
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15945, 18945
URL = f"https://127.0.0.1:{WEB}/"
passed = failed = 0
REQS = []


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def has_inst():
    ctx = ssl._create_unverified_context()
    for _ in range(50):
        try:
            with urllib.request.urlopen(URL + "api/inst", context=ctx, timeout=5) as f:
                return "instruments" in json.loads(f.read())
        except urllib.error.HTTPError:
            return False
        except OSError:
            time.sleep(0.2)
    return False


def passed_last(prof):
    return (prof["kind"] == "cems-instrument-profile" and len(prof["points"]) == 2 and prof["points"][0]["type"] == "f32"
            and prof["points"][0]["order"] == "CDAB" and prof["status"]["point"] == 1 and prof["status"]["maint_bits"] == 2)


async def save(pg):
    await pg.evaluate("document.getElementById('ieerr').textContent = ''")
    await pg.click("#iesave")
    try: await pg.wait_for_function("document.getElementById('ieerr').textContent !== ''", timeout=20000)
    except Exception: print("   requests:", [x for x in REQS if x[0] == "POST"][-4:])
    msg = await pg.inner_text("#ieerr")
    check(msg == "Saved", f"saved ({msg})")


async def run(say, real):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        ctx = await b.new_context(viewport={"width": 1300, "height": 1000}, ignore_https_errors=True, bypass_csp=True, accept_downloads=True)
        pg = await ctx.new_page()
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.on("request", lambda r: REQS.append((r.method, r.url[-40:], (r.post_data or "")[:80])))
        pg.on("dialog", lambda d: asyncio.ensure_future(d.accept()))
        for _ in range(100):
            try: await pg.goto(URL); break
            except Exception: await asyncio.sleep(0.1)
        await pg.wait_for_timeout(1500)
        check(not await pg.is_visible('#tabbar [data-tab="inst"]'), "no Instruments tab for a viewer while none is set up")
        if not real:
            await pg.select_option("#lvl", "admin"); await pg.fill("#pw", "2222"); await pg.click("#loginbtn")
            await pg.wait_for_timeout(2500)
            check(not await pg.is_visible('#tabbar [data-tab="inst"]'), "firmware without /api/inst: no Instruments tab at admin either")
            check(errs == [], f"no script errors {errs}")
            await b.close(); return
        say("set 1 1")                                     # REMOTE: instrument settings are not any-mode
        await pg.select_option("#lvl", "admin"); await pg.fill("#pw", "2222"); await pg.click("#loginbtn")
        await pg.wait_for_selector('#tabbar [data-tab="inst"]', state="visible", timeout=10000)
        await pg.click('#tabbar [data-tab="inst"]')
        await pg.wait_for_selector("#exbox", state="visible", timeout=5000)
        check(not await pg.is_visible("#instmode"), "REMOTE and admin: no mode note")
        # explorer: read the analyser's first 12 holding registers
        await pg.fill("#ex_ip", "192.168.1.70"); await pg.fill("#ex_count", "12"); await pg.click("#exgo")
        await pg.wait_for_selector("#extab", state="visible", timeout=10000)
        heads = await pg.eval_on_selector_all("#extab thead th", "t => t.map(x => x.textContent)")
        check(len(heads) == 18 and "f32 CDAB" in heads and "i32 DCBA" in heads and "u16 swapped" in heads, f"every decoding has a column ({len(heads)})")
        check(len(await pg.query_selector_all("#extab tbody tr")) == 12, "12 rows")
        await pg.fill("#ex_find", "7.7"); await pg.wait_for_timeout(200)
        await pg.fill("#ex_find", "")
        # the O2 the analyser shows: find it in the f32 CDAB column at address 0
        col = heads.index("f32 CDAB")
        o2 = float(await pg.inner_text(f"#extab tbody tr:nth-child(1) td:nth-child({col + 1})"))
        check(7.4 < o2 < 8.1, f"address 0 as f32 CDAB reads the O2 ({o2})")
        await pg.fill("#ex_find", f"{o2:.2f}")
        hits = await pg.eval_on_selector_all("#extab td.hit", "t => t.map(x => x.title)")
        check(any("f32 CDAB at 0" in h for h in hits), f"Find value marks it ({len(hits)} marked)")
        await pg.click(f"#extab tbody tr:nth-child(1) td:nth-child({col + 1})")
        check("f32 CDAB" in await pg.inner_text("#expicktxt"), "picked: " + await pg.inner_text("#expicktxt"))
        # what the explorer says when a read goes wrong (then read the good range again)
        await pg.fill("#ex_addr", "600"); await pg.fill("#ex_count", "1"); await pg.click("#exgo")
        await pg.wait_for_function("document.getElementById('exstate').textContent.includes('exception')", timeout=10000)
        check("02" in await pg.inner_text("#exstate") and not await pg.is_visible("#extab"), "an exception is explained: " + await pg.inner_text("#exstate"))
        await pg.fill("#ex_ip", "192.168.1.99"); await pg.fill("#ex_addr", "0"); await pg.click("#exgo")
        await pg.wait_for_function("document.getElementById('exstate').textContent.startsWith('No answer')", timeout=15000)
        check(True, "no device at that IP: " + await pg.inner_text("#exstate"))
        await pg.fill("#ex_ip", "192.168.1.70"); await pg.fill("#ex_count", "12"); await pg.fill("#ex_find", ""); await pg.click("#exgo")
        await pg.wait_for_selector("#extab", state="visible", timeout=10000)
        await pg.click(f"#extab tbody tr:nth-child(1) td:nth-child({col + 1})")
        # no instrument set up yet: set up instrument 1 from the list, then use the pick
        await pg.click("#insttab tbody tr button")
        await pg.wait_for_selector("#instedit", state="visible")
        await pg.fill("#ie_ip", "192.168.1.70"); await pg.fill("#ie_name", "Stack analyser")
        await pg.check("#ie_en")
        await save(pg)
        await pg.wait_for_timeout(1200)
        await pg.select_option("#expinst", "1")
        await pg.click("#exuse")
        await pg.wait_for_function("document.querySelectorAll('#ptab tbody tr').length === 1", timeout=5000)
        row = "#ptab tbody tr:nth-child(1)"
        check(await pg.input_value(f"{row} [data-f=addr]") == "0" and await pg.eval_on_selector(f"{row} [data-f=type]", "e => e.selectedOptions[0].text") == "f32"
              and await pg.eval_on_selector(f"{row} [data-f=order]", "e => e.selectedOptions[0].text") == "CDAB", "the pick filled a new point (03 holding 0, f32 CDAB)")
        await pg.fill(f"{row} [data-f=name]", "O2"); await pg.fill(f"{row} [data-f=unit]", "%")
        await pg.fill(f"{row} [data-f=lo]", "0"); await pg.fill(f"{row} [data-f=hi]", "25")
        # a second point by hand: the status word at holding 10, and make it the status point
        await pg.click("#ieadd")
        r2 = "#ptab tbody tr:nth-child(2)"
        await pg.fill(f"{r2} [data-f=name]", "Status"); await pg.fill(f"{r2} [data-f=addr]", "10")
        await save(pg)
        try: await pg.select_option("#ie_stpt", "2", timeout=5000)
        except Exception as e:
            print("   DBG", await pg.evaluate("(() => { const e = document.getElementById('ie_stpt'); const r = e.getBoundingClientRect(); return [e.disabled, r.width, r.height, [...e.options].map(o => o.value + ':' + o.text), document.getElementById('instedit').style.display, mode, level]; })()"))
            raise
        await pg.fill("#ie_mm", "0x0002")
        await save(pg)
        await pg.wait_for_function("[...document.querySelectorAll('#insttab .tile')].some(t => t.textContent.startsWith('O2') && t.textContent.includes(' %'))", timeout=10000)
        tile = await pg.eval_on_selector_all("#insttab .tile", "t => t.map(x => x.innerText.replace(/\\n/g, ' | '))")
        check(any(t.startswith("O2") for t in tile), f"live value on the tab: {tile[0] if tile else ''}")
        try: await pg.wait_for_function("document.querySelector('#insttab tbody tr td:nth-child(4)').textContent === 'ok'", timeout=8000)
        except Exception: pass
        st = await pg.inner_text("#insttab tbody tr:nth-child(1) td:nth-child(4)")
        check(st == "ok", f"instrument state {st}")
        say("analyser status 2")
        await pg.wait_for_function("[...document.querySelectorAll('#insttab .tile .v')].some(v => v.dataset.q === 'maintenance')", timeout=10000)
        check(True, "status word bit 1: the values read maintenance")
        say("analyser status 0")
        # profile out, then into instrument 2
        async with pg.expect_download(timeout=10000) as dl:
            await pg.click("#ieexp")
        prof = json.loads(open(await (await dl.value).path()).read())
        check(prof["kind"] == "cems-instrument-profile" and len(prof["points"]) == 2 and prof["points"][0]["type"] == "f32" and prof["points"][0]["order"] == "CDAB"
              and prof["status"]["point"] == 1 and prof["status"]["maint_bits"] == 2 and "ip" not in json.dumps(prof["device"]), "profile exported (no IP)" + ("" if passed_last(prof) else " " + json.dumps(prof)[:400]))
        tmp = os.path.join(tempfile.mkdtemp(), "p.json"); open(tmp, "w").write(json.dumps(prof))
        await pg.click("#iecancel")
        await pg.wait_for_function("[...document.querySelectorAll('#insttab button')].some(b => b.textContent === 'Set up')", timeout=5000)
        await pg.click("#insttab button:text-is('Set up')")
        await pg.wait_for_function("document.getElementById('iehead').textContent === 'Instrument 2'", timeout=5000)
        await pg.fill("#ie_ip", "192.168.1.70"); await pg.check("#ie_en")
        await pg.set_input_files("#ieimp", tmp)
        await pg.wait_for_function("document.getElementById('ieerr').textContent.startsWith('Profile loaded')", timeout=15000)
        n2 = len(await pg.query_selector_all("#ptab tbody tr"))
        if n2 != 2 or await pg.input_value("#ie_mm") != "0x0002" or not await pg.input_value("#ie_name"):
            await asyncio.sleep(2)
            ctx2 = ssl._create_unverified_context()
            with urllib.request.urlopen(URL + "api/values?ids=522,530,510,508", context=ctx2, timeout=5) as f: print("   DBG after 2 s:", f.read()[:300])
        check(n2 == 2 and await pg.input_value("#ie_name") == "Stack analyser" and await pg.input_value("#ie_mm") == "0x0002", f"profile imported into instrument 2 ({n2} points, {await pg.input_value('#ie_name')!r}, {await pg.input_value('#ie_mm')!r}, {await pg.inner_text('#ieerr')!r})")
        stp = await pg.eval_on_selector("#ie_stpt", "e => e.selectedOptions[0].text")
        check(stp.startswith("point 4"), f"status point mapped to the new point number ({stp})")
        await pg.wait_for_function("document.querySelectorAll('#insttab .tile').length === 4", timeout=10000)
        check(True, "both instruments show their points")
        # a viewer: values, no editor, no explorer
        await pg.click("#logoutbtn")
        await pg.wait_for_timeout(1500)
        check(await pg.is_visible('#tabbar [data-tab="inst"]') and not await pg.is_visible("#exbox") and not await pg.is_visible("#insttab button"),
              "viewer: tab and values, no Edit or explorer")
        check(errs == [], f"no script errors {errs}")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"), os.path.join(tmp, "n.bin")],
                            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    def say(line): unit.stdin.write(line + "\n"); unit.stdin.flush()
    try:
        real = has_inst()
        print("instruments:", "the firmware's own, simulated analyser at 192.168.1.70" if real else "firmware without /api/inst: hidden-tab check only")
        asyncio.run(run(say, real))
    finally:
        unit.terminate()
        try: unit.wait(timeout=10)
        except subprocess.TimeoutExpired: unit.kill()
    print(f"\nINSTRUMENTS WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
