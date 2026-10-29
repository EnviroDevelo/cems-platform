#!/usr/bin/env python3
"""Forcing Modbus values from the Service tab against build/netsim: the Force box shows only at
admin login, a value forced for 30 s appears in the table with what Modbus serves, the real
value and a countdown, the header shows MODBUS FORCED on every tab, a test bit toggles forced
on, forced off and released, a refused force (409, 16 already forced) is explained, and Release
and Clear all end forces. Firmware without
/api/modbus/force gets it simulated in the browser (the agreed API), so the page side is checked
either way. Needs Python Playwright, pymodbus 3.6 and a Chromium."""
import asyncio, glob, json, os, ssl, subprocess, sys, tempfile, time, urllib.parse, urllib.request
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15925, 18925
URL = f"https://127.0.0.1:{WEB}/"
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def get(path, cookie=None):
    req = urllib.request.Request(URL + path, headers={"Cookie": cookie} if cookie else {})
    with urllib.request.urlopen(req, context=ssl._create_unverified_context(), timeout=5) as f:
        return f.status, json.loads(f.read() or b"{}")


def real_force():
    for _ in range(50):
        try:
            get("api/info"); break
        except OSError: time.sleep(0.2)
    try: get("api/modbus/force"); return True
    except urllib.error.HTTPError as e: return e.code != 404
    except OSError: return False


class Sim:                                            # the firmware's force API, as agreed, for builds without it
    def __init__(self):
        self.forces = {}; self.full = False
        self.forceable = [{"id": 307, "name": "link_age_s", "type": "uint", "count": None, "unit": "s"},
                          {"id": 305, "name": "demo_array", "type": "uint", "count": 4, "unit": ""},
                          {"id": 310, "name": "test_relay", "type": "bool", "count": None, "unit": ""}]
    def state(self):
        now = time.time()
        self.forces = {k: f for k, f in self.forces.items() if f["until"] > now}
        return {"max_s": 30, "max": 16, "forceable": self.forceable,
                "forces": [{"id": k[0], "index": k[1], "name": f["name"], "v": f["v"], "left_s": int(f["until"] - now), "by": "web"}
                           for k, f in self.forces.items()]}
    def post(self, q):
        if self.full: return 409
        fd = next((f for f in self.forceable if str(f["id"]) == q.get("id")), None)
        if not fd: return 404
        idx = int(q["index"]) if q.get("index") else None
        v = q.get("v", "")
        if fd["type"] == "bool":
            if v not in ("true", "false"): return 400
            v = v == "true"
        else:
            try: v = float(v)
            except ValueError: return 400
        self.forces[(fd["id"], idx)] = {"name": fd["name"], "v": v, "until": time.time() + 30}
        return 200
    def clear(self, q):
        if q.get("all"): self.forces = {}
        else: self.forces.pop((int(q["id"]), int(q["index"]) if q.get("index") else None), None)
        return 200


async def run(sim):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1200, "height": 1000}, ignore_https_errors=True, bypass_csp=True)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        if sim:
            async def info(route):
                r = await route.fetch(); j = await r.json()
                j.setdefault("modbus", {"listening": True, "port": MB, "clients": 0, "max_clients": 4, "requests": 0, "exceptions": 0,
                                        "read_only": True, "allow_ip": None, "last_request_s": None})
                j["modbus"]["forced"] = len(sim.state()["forces"])
                await route.fulfill(response=r, json=j)
            async def force(route):
                rq = route.request
                q = dict(urllib.parse.parse_qsl(rq.post_data or ""))
                if rq.method == "GET": return await route.fulfill(json=sim.state())
                st = sim.clear(q) if rq.url.endswith("/clear") else sim.post(q)
                await route.fulfill(status=st, json={"ok": True} if st == 200 else {"error": str(st)})
            await pg.route("**/api/info", info)
            await pg.route("**/api/modbus/force", force)
            await pg.route("**/api/modbus/force/clear", force)
        for _ in range(100):
            try: await pg.goto(URL + "#service"); break
            except Exception: await asyncio.sleep(0.1)
        await pg.wait_for_selector("#mbbox", state="visible", timeout=10000)
        await pg.wait_for_timeout(1500)
        check(not await pg.is_visible("#fcbox"), "no Force box before an admin login")
        await pg.select_option("#lvl", "admin"); await pg.fill("#pw", "2222"); await pg.click("#loginbtn")
        await pg.wait_for_selector("#fcbox", state="visible", timeout=10000)
        opts = await pg.eval_on_selector_all("#fcid option", "o => o.map(x => x.textContent)")
        check(len(opts) >= 1, f"forceable values listed ({opts[:3]})")
        bits = await pg.inner_text("#fcbits")
        check(not sim or "Test bit test_relay: real" in bits, f"test bits as buttons ({bits!r})")
        # force the first listed value
        first = (await pg.eval_on_selector_all("#fcid option", "o => o.map(x => x.value)"))[0]
        await pg.select_option("#fcid", first)
        if await pg.is_visible("#fcidx"): await pg.fill("#fcidx", "0")
        await pg.fill("#fcv", "7")
        await pg.click("#fcgo")
        await pg.wait_for_selector("#fctab", state="visible", timeout=5000)
        row = await pg.inner_text("#fctab tbody tr")
        check(" s" in row and "web" in row, f"forced value in the table with its countdown ({row!r})")
        if not sim:                                   # what a Modbus client reads is the forced value
            _, mp = get("api/modbus/map")
            e = next((x for x in mp["entries"] if str(x["id"]) == first), None)
            if e and e["table"] in ("input", "holding") and e["format"] in ("u16", "i16", "u32", "i32", "array"):
                from pymodbus.client import ModbusTcpClient
                c = ModbusTcpClient("127.0.0.1", port=MB, timeout=3); c.connect()
                r = (c.read_input_registers if e["table"] == "input" else c.read_holding_registers)(e["addr"], e["count"], slave=1)
                c.close()
                regs = getattr(r, "registers", None)
                val = None if regs is None else regs[0] if e["format"] in ("u16", "i16", "array") else (regs[0] << 16 | regs[1]) if e["word_order"] != "low_first" else (regs[1] << 16 | regs[0])
                check(val == 7, f"a Modbus client reads the forced value ({e['name']} at {e['table']} {e['addr']}: {val})")
        await pg.wait_for_selector("#forcebadge", state="visible", timeout=5000)
        check((await pg.inner_text("#forcebadge")).startswith("MODBUS FORCED 1"), "header shows MODBUS FORCED")
        l1 = await pg.inner_text("#fctab tbody tr td:nth-child(4)"); await pg.wait_for_timeout(2200)
        l2 = await pg.inner_text("#fctab tbody tr td:nth-child(4)")
        check(int(l2.split()[0]) < int(l1.split()[0]), f"countdown runs ({l1} -> {l2})")
        if sim:                                       # the Settings row of a forced value says what Modbus serves
            await pg.click('#tabbar [data-tab="settings"]'); await pg.fill("#filter", "link_age_s"); await pg.dispatch_event("#filter", "input")
            await pg.wait_for_function("(document.getElementById('v307') || {}).textContent?.includes('Modbus: forced')", timeout=5000)
            check(True, "Settings row: " + await pg.inner_text("#v307"))
            await pg.fill("#filter", ""); await pg.dispatch_event("#filter", "input")
        await pg.click('#tabbar [data-tab="overview"]')
        check(await pg.is_visible("#forcebadge"), "badge stays on other tabs")
        await pg.click('#tabbar [data-tab="service"]')
        if await pg.is_visible("#fcbits button"):
            await pg.click("#fcbits button")
            await pg.wait_for_function("document.querySelector('#fcbits button').textContent.endsWith('forced on')", timeout=5000)
            await pg.click("#fcbits button")
            await pg.wait_for_function("document.querySelector('#fcbits button').textContent.endsWith('forced off')", timeout=5000)
            await pg.click("#fcbits button")
            await pg.wait_for_function("document.querySelector('#fcbits button').textContent.endsWith(': real')", timeout=5000)
            check(True, "test bit: forced on, forced off, released")
        if sim:
            sim.full = True
            await pg.fill("#fcv", "2"); await pg.click("#fcgo")
            await pg.wait_for_function("document.getElementById('fcerr').textContent.includes('already forced')", timeout=5000)
            check(True, "409 explained (16 already forced)")
            sim.full = False
        await pg.fill("#fcv", "abc"); await pg.click("#fcgo")
        await pg.wait_for_function("document.getElementById('fcerr').textContent.length > 0", timeout=5000)
        check("valid" in await pg.inner_text("#fcerr"), "a bad value is refused with a reason")
        await pg.click("#fctab tbody tr button")
        await pg.wait_for_selector("#fctab", state="hidden", timeout=5000)
        check(True, "Release ends the force")
        await pg.fill("#fcv", "1"); await pg.click("#fcgo")
        await pg.wait_for_selector("#fcall", state="visible", timeout=5000)
        await pg.click("#fcall")
        await pg.wait_for_selector("#fctab", state="hidden", timeout=5000)
        await pg.wait_for_selector("#forcebadge", state="hidden", timeout=5000)
        check(True, "Clear all ends every force and the header badge goes")
        check(errs == [], f"no script errors {errs}")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"), os.path.join(tmp, "n.bin")],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sim = None if real_force() else Sim()
        print("modbus force:", "simulated in the browser (firmware without it)" if sim else "the firmware's own")
        asyncio.run(run(sim))
    finally:
        unit.terminate()
        try: unit.wait(timeout=10)
        except subprocess.TimeoutExpired: unit.kill()
    print(f"\nMODBUS FORCE WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
