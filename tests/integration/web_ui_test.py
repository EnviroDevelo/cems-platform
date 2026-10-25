#!/usr/bin/env python3
"""Drive the web page in headless Chromium against build/netsim: view, log in,
change a setting (one refused, one accepted), check for script errors, and save
screenshots to build/. Needs Python Playwright and a Chromium."""
import asyncio, os, signal, subprocess, sys, tempfile, time
from playwright.async_api import async_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME", "/opt/pw-browsers/chromium-1194/chrome-linux/chrome")
URL = "https://127.0.0.1:18095/"

async def run(unit):
    ok = True
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1200, "height": 900}, ignore_https_errors=True)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.on("console", lambda m: errs.append(m.text) if m.type == "error" and "401" not in m.text and "400" not in m.text else None)
        await pg.goto(URL)
        await pg.wait_for_selector("#v1001")
        await pg.wait_for_timeout(1200)
        await pg.screenshot(path=os.path.join(ROOT, "build", "web_view.png"), full_page=True)
        # trends: pick a value, let it record, check the chart, the legend, the CSV and that the pick is remembered
        await pg.select_option("#tradd", label="1002 run_seconds")
        await pg.wait_for_timeout(3500)
        pens = await pg.inner_text("#trpens")
        inked = await pg.evaluate("""() => { const c = document.getElementById('trc'), d = c.getContext('2d')
            .getImageData(0, 0, c.width, c.height).data; let n = 0;
            for (let i = 0; i < d.length; i += 4) if (d[i] === 31 && d[i + 1] === 95 && d[i + 2] === 174) n++; return n; }""")
        async with pg.expect_download() as dl:
            await pg.click("#trcsv")
        csv = open(await (await dl.value).path()).read().splitlines()
        await pg.locator("#trendsec").screenshot(path=os.path.join(ROOT, "build", "web_trend.png"))
        await pg.reload()
        await pg.wait_for_selector("#trv1002")
        print("trend legend:", repr(pens), "| line pixels:", inked, "| csv:", csv[0], len(csv) - 1, "rows")
        ok &= "run_seconds" in pens and inked > 20 and csv[0] == '"time","run_seconds (s)"' and len(csv) >= 5 \
              and all(len(r.split(",")) == 2 and r.split(",")[1].isdigit() for r in csv[1:])
        # phone width: no sideways scrolling of the page (wide tables scroll inside their own section)
        ph = await b.new_page(viewport={"width": 390, "height": 844}, ignore_https_errors=True, has_touch=True)
        await ph.goto(URL)
        await ph.wait_for_selector("#v1001")
        await ph.wait_for_timeout(1500)
        sw = await ph.evaluate("[document.documentElement.scrollWidth, innerWidth]")
        await ph.screenshot(path=os.path.join(ROOT, "build", "web_phone.png"))
        await ph.close()
        print("phone width: page", sw[0], "px in a", sw[1], "px window"); ok &= sw[0] <= sw[1]
        await pg.fill("#pw", "1111")
        await pg.click("#loginbtn")
        await pg.wait_for_selector("#userrow", state="visible")
        await pg.fill("#filter", "start_delay")
        btn = pg.locator('#rows button:has-text("Change")')
        await btn.first.click()
        await pg.fill("#rows td input", "99")
        await pg.click('#rows button:has-text("Set")')
        await pg.wait_for_timeout(400)
        refused = await pg.inner_text("#rows .err")
        await pg.fill("#rows td input", "1500")
        await pg.click('#rows button:has-text("Set")')
        await pg.wait_for_timeout(1000)
        shown = await pg.inner_text("#v1001")
        await pg.fill("#filter", "")
        await pg.wait_for_timeout(5500)                    # the event list refreshes every 5 s
        events = await pg.inner_text("#events")
        print("event list:", events.replace("\n", " | ")[:300])
        ok &= "Setting changed" in events and "start_delay (1001): 2000 → 1500" in events and "Login" in events \
              and "Power-up" in events
        await pg.screenshot(path=os.path.join(ROOT, "build", "web_operator.png"), full_page=True)
        print("refused value message:", repr(refused)); ok &= refused == "Outside the allowed range"
        print("value shown after change:", repr(shown)); ok &= shown == "1500 ms"
        # alarms: badge, list, acknowledge one row
        await pg.fill("#filter", "demo_fault")
        await pg.click('#rows button:has-text("Change")')
        await pg.fill("#rows td input", "1")
        await pg.click('#rows button:has-text("Set")')
        await pg.fill("#filter", "")
        await pg.wait_for_function("document.getElementById('alarmrows').innerText.includes('Demo fault')", timeout=5000)
        badge = await pg.inner_text("#alarmbadge")
        rows = await pg.inner_text("#alarmrows")
        await pg.locator("#alarms").screenshot(path=os.path.join(ROOT, "build", "web_alarms.png"))
        await pg.click('#alarmrows tr:has-text("Demo fault") button:has-text("Acknowledge")')
        await pg.wait_for_timeout(1500)
        rows2 = await pg.inner_text("#alarmrows")
        print("alarm badge:", repr(badge), "| rows:", rows.replace("\n", " | ")[:200], "| after ack:", rows2.replace("\n", " | ")[:200])
        ok &= badge.startswith("2 ALARMS") and "TO ACKNOWLEDGE" in badge and "Demo fault" in rows \
              and "Active, not acknowledged" in rows and "Demo fault" in rows2 and "Active, acknowledged" in rows2
        # the application's own screen (ui_app.h): buttons through the table, a record editor
        await pg.wait_for_selector('#apptabs button:has-text("Demo")')
        await pg.click('#appbody button:has-text("Sequence on")')
        onlit = "[...document.querySelectorAll('#appbody button.lit')].some(b => b.textContent === 'Sequence on')"
        await pg.wait_for_function(onlit, timeout=4000)
        lit = "Sequence on"
        await pg.click('#appbody button:has-text("Sequence off")')
        await pg.wait_for_function("!" + onlit, timeout=4000)
        await pg.click('#appbody button:has-text("Edit steps")')
        await pg.wait_for_selector("#recbox table")
        await pg.fill("#recbox .row input[type=number]", "2"); await pg.dispatch_event("#recbox .row input[type=number]", "change")
        await pg.wait_for_timeout(200)
        cells = pg.locator("#recbox table tr").nth(1).locator("input")
        await cells.nth(0).fill("Purge"); await cells.nth(1).fill("12.5"); await cells.nth(2).fill("2")
        cells2 = pg.locator("#recbox table tr").nth(2).locator("input")
        await cells2.nth(0).fill("Sample"); await cells2.nth(1).fill("0"); await cells2.nth(2).fill("3")
        await pg.click('#recbox button:has-text("Save")')
        await pg.wait_for_function("document.getElementById('recerr').textContent.length > 0", timeout=4000)
        refused_rec = await pg.inner_text("#recerr")
        await pg.locator("#appsec").screenshot(path=os.path.join(ROOT, "build", "web_app_record.png"))
        await cells2.nth(1).fill("30")
        await pg.click('#recbox button:has-text("Save")')
        await pg.wait_for_function("document.getElementById('recerr').textContent === 'Saved'", timeout=4000)
        await pg.fill("#filter", "demo_steps")
        await pg.wait_for_timeout(1500)
        steps = await pg.inner_text("#v1012")
        await pg.fill("#filter", "")
        print("app screen: lit", repr(lit), "| record refused:", repr(refused_rec), "| stored:", steps[:60])
        ok &= lit == "Sequence on" and refused_rec == "Step 2: no time set" and steps.startswith("507572676500")
        # firmware upload as admin
        img = os.path.join(TMPD, "fw.bin"); open(img, "wb").write(os.urandom(150000))
        cfw = os.path.join(TMPD, "fw.cfw")
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkfwimage.py"), img, cfw, "--version", "0.4.0"],
                       check=True, stdout=subprocess.DEVNULL)
        await pg.click("#logoutbtn")
        await pg.wait_for_selector("#loginrow", state="visible")
        await pg.select_option("#lvl", "factory")
        await pg.fill("#pw", "9999")
        await pg.click("#loginbtn")
        await pg.wait_for_selector("#admin", state="visible")
        await pg.set_input_files("#fwfile", cfw)
        await pg.click("#fwup")
        await pg.wait_for_function("document.getElementById('fwerr').textContent.length > 0", timeout=15000)
        fwmsg = await pg.inner_text("#fwerr")
        await pg.wait_for_timeout(2500)
        fwinfo = await pg.inner_text("#fwinfo")
        act = await pg.is_visible("#fwact")
        await pg.locator("#admin").screenshot(path=os.path.join(ROOT, "build", "web_firmware.png"))
        print("firmware:", repr(fwmsg), "|", repr(fwinfo), "| activate button:", act)
        ok &= fwmsg == "Uploaded 0.4.0" and "Ready in the spare bank: 0.4.0" in fwinfo and act
        # alarm actions and email set-up (factory login sees the admin sections)
        await pg.wait_for_function("document.getElementById('actrows').innerText.includes('Demo fault')", timeout=5000)
        row = pg.locator('#actrows tr:has-text("DEMO_FAULT")')
        await row.locator("input[type=checkbox]").nth(2).check()            # relay 1
        await pg.wait_for_timeout(800)
        await pg.select_option("#relayrows tr:nth-child(1) select >> nth=0", "3")
        await pg.wait_for_timeout(800)
        relay_ok = await row.locator("input[type=checkbox]").nth(2).is_checked() and \
                   await pg.locator('#actrows tr:has-text("DEMO_FAULT") button:has-text("Default")').count() == 1
        await pg.select_option("#m70", "1"); await pg.fill("#m71", "10.1.2.3"); await pg.fill("#m77", "cems@site")
        await pg.fill("#m78", "ops@site"); await pg.fill("#m76", "pw-123"); await pg.fill("#m81", "Stack B")
        await pg.click("#mailsave")
        await pg.wait_for_function("document.getElementById('mailerr').textContent === 'Saved'", timeout=8000)
        await pg.wait_for_timeout(600)
        mail_ok = await pg.input_value("#m71") == "10.1.2.3" and await pg.input_value("#m76") == "" \
                  and await pg.input_value("#m81") == "Stack B"
        await pg.locator("#actions").screenshot(path=os.path.join(ROOT, "build", "web_alarm_actions.png"))
        await pg.locator("#mailsec").screenshot(path=os.path.join(ROOT, "build", "web_mail.png"))
        print("alarm actions:", relay_ok, "| mail form:", mail_ok); ok &= relay_ok and mail_ok
        await pg.wait_for_function("document.getElementById('clocknow').textContent.startsWith('Instrument clock: 2')", timeout=6000)
        await pg.click("#clockpc")
        await pg.wait_for_function("document.getElementById('clockerr').textContent === 'Set'", timeout=5000)
        await pg.locator("#clocksec").screenshot(path=os.path.join(ROOT, "build", "web_clock.png"))
        clk = await pg.inner_text("#clocknow"); print("clock:", clk)
        # connection loss: stop the unit, the page must say so and grey out
        unit.send_signal(signal.SIGTERM); unit.wait(timeout=10)
        await pg.wait_for_timeout(2500)
        stale = await pg.evaluate("document.body.classList.contains('stale')")
        banner = await pg.inner_text("#lost")
        await pg.screenshot(path=os.path.join(ROOT, "build", "web_lost.png"))
        print("after the unit stops: stale =", stale, "| banner:", repr(banner)); ok &= stale and banner.startswith("Connection lost")
        errs = [e for e in errs if "ERR_CONNECTION_REFUSED" not in e and "Failed to load resource" not in e]
        print("page script errors:", errs); ok &= not errs
        await b.close()
        return ok

def main():
    global TMPD
    tmp = TMPD = tempfile.mkdtemp()
    fram, nor = os.path.join(tmp, "fram.bin"), os.path.join(tmp, "nor.bin")
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), "15095", "18095", fram, nor],
                            stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(0.8)
    unit.stdin.write("remote\n"); unit.stdin.flush()
    ok = asyncio.run(run(unit))
    print("WEB UI:", "PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
