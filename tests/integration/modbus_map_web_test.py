#!/usr/bin/env python3
"""The Modbus handover file from the Service tab against build/netsim: "Modbus map for the
integrator (Excel)" downloads an .xlsx written in the browser from GET /api/modbus/map. The
file must be a valid zip, open in openpyxl, list one row per map entry with its 1-based
reference (40001 for holding 0, 39001 for input 9000), function codes, format, word order and
scale, and carry an Instrument sheet (product, serial, map version, port). Firmware without
/api/modbus/map gets it simulated in the browser (the agreed shape). Needs Python Playwright,
openpyxl and a Chromium."""
import asyncio, glob, io, json, os, ssl, subprocess, sys, tempfile, time, urllib.request, zipfile
import openpyxl
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15935, 18935
URL = f"https://127.0.0.1:{WEB}/"
passed = failed = 0
SIM_MAP = {"map_version": 3, "block_layout": 3, "product": "CEMS Demo", "entries": [
    {"table": "holding", "fc_read": 3, "fc_write": 16, "addr": 0, "count": 1, "format": "u16", "word_order": "high_first", "scale_dec": 0,
     "id": 1001, "name": "start_delay", "unit": "ms", "access": "read/write", "block": False, "forceable": False},
    {"table": "input", "fc_read": 4, "fc_write": None, "addr": 100, "count": 2, "format": "f32", "word_order": "high_first", "scale_dec": 0,
     "id": 300, "name": "o2 & <dry>", "unit": "%", "access": "read", "block": False, "forceable": True},
    {"table": "input", "fc_read": 4, "fc_write": None, "addr": 9000, "count": 1, "format": "u16", "word_order": "high_first", "scale_dec": 1,
     "id": 306, "name": "heartbeat", "unit": "", "access": "read", "block": True, "forceable": False},
    {"table": "coil", "fc_read": 1, "fc_write": 5, "addr": 0, "count": 1, "format": "bit", "word_order": "high_first", "scale_dec": 0,
     "id": 1007, "name": "demo_fault", "unit": "", "access": "read/write", "block": False, "forceable": False}]}


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def real_map():
    ctx = ssl._create_unverified_context()
    for _ in range(50):
        try:
            with urllib.request.urlopen(URL + "api/modbus/map", context=ctx, timeout=5) as f:
                return json.loads(f.read())
        except urllib.error.HTTPError:
            return None
        except OSError:
            time.sleep(0.2)
    return None


async def run(mp, sim):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        ctx = await b.new_context(viewport={"width": 1200, "height": 900}, ignore_https_errors=True, bypass_csp=True, accept_downloads=True)
        pg = await ctx.new_page()
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        if sim:
            await pg.route("**/api/modbus/map", lambda route: route.fulfill(json=SIM_MAP))
            async def info(route):
                r = await route.fetch(); j = await r.json()
                j.setdefault("modbus", {"listening": True, "port": 502, "clients": 0, "max_clients": 4, "requests": 0, "exceptions": 0,
                                        "read_only": True, "allow_ip": None, "last_request_s": None})
                await route.fulfill(response=r, json=j)
            await pg.route("**/api/info", info)
        for _ in range(100):
            try: await pg.goto(URL + "#service"); break
            except Exception: await asyncio.sleep(0.1)
        await pg.wait_for_selector("#mbbox", state="visible", timeout=10000)
        await pg.wait_for_timeout(500)
        check(not await pg.is_visible("#mbxlsx"), "no handover download before a login")
        await pg.select_option("#lvl", "operator"); await pg.fill("#pw", "1111"); await pg.click("#loginbtn")
        await pg.wait_for_selector("#mbxlsx", state="visible", timeout=10000)
        async with pg.expect_download(timeout=15000) as dl:
            await pg.click("#mbxlsx")
        d = await dl.value
        data = open(await d.path(), "rb").read()
        check(d.suggested_filename.startswith("modbus-map-") and d.suggested_filename.endswith(".xlsx"), f"file name {d.suggested_filename}")
        z = zipfile.ZipFile(io.BytesIO(data))
        check(z.testzip() is None, f"a valid zip ({len(z.namelist())} parts, {len(data)} bytes)")
        wb = openpyxl.load_workbook(io.BytesIO(data))
        check(wb.sheetnames == ["Cover", "Standard values", "Project map", "Platform block", "Codes", "Commissioning"], f"sheets {wb.sheetnames}")
        ws = wb["Project map"]
        rows = [[c.value for c in r] for r in ws.iter_rows()]
        head = rows[0]
        check(head[:3] == ["Table", "Address (0-based)", "Modicon"] and head[-1] == "Checked by integrator", "header row")
        check(ws["A1"].font.b, "header is bold")
        check(len(rows) - 1 == len(mp["entries"]), f"one row per map entry ({len(rows) - 1} of {len(mp['entries'])})")
        col = {h: k for k, h in enumerate(head)}
        def ref(table, addr): return next((r[col["Modicon"]] for r in rows[1:] if r[0].lower().startswith(table) and r[1] == addr), None)
        hold = next((e for e in mp["entries"] if e["table"] == "holding"), None)
        if hold: check(ref("holding", hold["addr"]) == (f"{40001 + hold['addr']:05d}" if hold["addr"] < 9999 else str(400001 + hold["addr"])),
                       f"holding {hold['addr']} -> {ref('holding', hold['addr'])}")
        coil = next((e for e in mp["entries"] if e["table"] == "coil"), None)
        if coil: check(ref("coil", coil["addr"]) == f"{1 + coil['addr']:05d}", f"coil {coil['addr']} -> {ref('coil', coil['addr'])}")
        blk = next((e for e in mp["entries"] if e["table"] == "input" and e["addr"] >= 9000), None)
        if blk: check(ref("input", blk["addr"]) == str(30001 + blk["addr"]), f"input {blk['addr']} -> {ref('input', blk['addr'])}")
        wide = [r for r in rows[1:] if r[col["Format"]] and "32-bit" in r[col["Format"]]]
        check(all(r[col["Word order"]] in ("high word first", "low word first") for r in wide), f"32-bit values carry the word order ({len(wide)})")
        if sim:
            check(any(r[col["Name"]] == "o2 & <dry>" for r in rows[1:]), "names with & and < survive")
            check(any(r[col["Scaling"]] and "÷ 10" in r[col["Scaling"]] for r in rows[1:]), "scaled value says ÷ 10")
        nblk = sum(1 for e in mp["entries"] if e.get("block"))
        check(wb["Platform block"].max_row - 1 == nblk, f"platform block sheet: {nblk} entries")
        cover = {r[0].value: r[1].value for r in wb["Cover"].iter_rows()}
        check(cover.get("Register map version") == mp["map_version"] and cover.get("Modbus TCP port") and cover.get("Serial number") is not None
              and "operator login" in (cover.get("Generated") or ""), f"Cover (map v{cover.get('Register map version')}, port {cover.get('Modbus TCP port')}, {cover.get('Generated')})")
        codes = [r[0].value for r in wb["Codes"].iter_rows(min_row=2)]
        check("Alarm ID" in codes and codes.count("Modbus exception") == len((mp.get("codes") or {}).get("exceptions") or [1, 2, 3, 4, 6]),
              f"Codes sheet: {sorted(set(codes))}, exceptions once")
        with urllib.request.urlopen(URL + "api/alarms/config", context=ssl._create_unverified_context(), timeout=5) as f:
            al = json.loads(f.read())["alarms"]
        arow = {r[1].value: r[2].value for r in wb["Codes"].iter_rows(min_row=2) if r[0].value == "Alarm ID"}
        check(al and all(arow.get(a["id"]) == (a["text"] or a["name"]) for a in al), f"alarms listed by their current label ({arow.get(al[0]['id']) if al else ''!r})")
        if mp.get("codes"):
            check("Quality" in codes and "State" in codes and "Common alarm bit" in codes, "the map's codes (quality, state, common alarm bits)")
        nf = sum(1 for e in mp["entries"] if e.get("forceable"))
        check(sum(1 for r in wb["Commissioning"].iter_rows() if r[0].value in ("Coil", "Discrete input", "Input register", "Holding register")) == nf,
              f"Commissioning lists the {nf} forceable entries")
        async with pg.expect_download(timeout=15000) as dl:
            await pg.click("#mbcsv")
        d = await dl.value
        csv = open(await d.path(), encoding="utf-8-sig").read().splitlines()
        check(d.suggested_filename.endswith(".csv") and csv[0].startswith("Table,Address (0-based),Modicon") and len(csv) - 1 == len(mp["entries"]),
              f"CSV beside it ({len(csv) - 1} rows)")
        check(errs == [], f"no script errors {errs}")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"), os.path.join(tmp, "n.bin")],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        mp = real_map()
        sim = mp is None
        print("modbus map:", "simulated in the browser (firmware without it)" if sim else f"the firmware's own ({len(mp['entries'])} entries)")
        asyncio.run(run(SIM_MAP if sim else mp, sim))
    finally:
        unit.terminate()
        try: unit.wait(timeout=10)
        except subprocess.TimeoutExpired: unit.kill()
    print(f"\nMODBUS MAP WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
