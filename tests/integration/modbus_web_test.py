#!/usr/bin/env python3
"""The Modbus TCP panel on the Service tab against build/netsim: it shows the server's
state from /api/info "modbus" (clients, requests, refused requests, read-only, allowed
client), a request from a Modbus client shows up in the counts and as "s ago", and
Modbus settings opens the Settings tab filtered to the modbus_* parameters. Firmware without
"modbus" in /api/info gets it simulated in the browser, so the page side is checked either
way. Needs Python Playwright, pymodbus 3.6 and a Chromium."""
import asyncio, glob, json, os, ssl, subprocess, sys, tempfile, time, urllib.request
from playwright.async_api import async_playwright
from pymodbus.client import ModbusTcpClient
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15920, 18920
URL = f"https://127.0.0.1:{WEB}/"
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def real_modbus():
    ctx = ssl._create_unverified_context()
    for _ in range(50):
        try:
            with urllib.request.urlopen(URL + "api/info", context=ctx, timeout=5) as f:
                return "modbus" in json.loads(f.read())
        except OSError:
            time.sleep(0.2)
    return False


def poke():                                          # one Modbus request, whatever register it names
    c = ModbusTcpClient("127.0.0.1", port=MB, timeout=3)
    try:
        c.connect(); c.read_holding_registers(0, 1, slave=1)
    finally:
        c.close()


class Sim:                                           # the firmware's "modbus" block, as agreed, for builds without it
    def __init__(self): self.n = 0; self.t = None
    def poke(self): self.n += 1; self.t = time.time()
    def state(self):
        return {"listening": True, "port": MB, "clients": 0, "max_clients": 4, "requests": self.n, "exceptions": 0,
                "writes_queued": 0, "read_only": True, "allow_ip": None, "accepted": self.n, "refused_ip": 0, "evicted": 0,
                "protocol_errors": 0, "last_request_s": None if self.t is None else int(time.time() - self.t)}


async def run(sim):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page(viewport={"width": 1200, "height": 900}, ignore_https_errors=True, bypass_csp=True)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        if sim:
            async def info(route):
                r = await route.fetch(); j = await r.json(); j["modbus"] = sim.state(); await route.fulfill(response=r, json=j)
            await pg.route("**/api/info", info)
        for _ in range(100):
            try: await pg.goto(URL + "#service"); break
            except Exception: await asyncio.sleep(0.1)
        await pg.wait_for_selector("#mbbox", state="visible", timeout=10000)
        tiles = await pg.inner_text("#mbtiles")
        check("Control systems connected" in tiles and "of 4" in tiles, "clients shown with the limit")
        check("none yet" in tiles, f"no request yet ({' | '.join(tiles.splitlines()[:4])})")
        check("Listening on port" in await pg.inner_text("#mbwhy"), "listening, with the port")
        check("Allowed client" in tiles and "any address" in tiles, "allowed client: any address")
        (sim.poke if sim else poke)()
        await pg.wait_for_function("document.getElementById('mbtiles').textContent.includes(' s ago')", timeout=10000)
        tiles = await pg.inner_text("#mbtiles")
        check(" s ago" in tiles, "a Modbus request shows as seconds ago")
        check("Writes from Modbus" in tiles, "write setting shown")
        await pg.click("#mbset")
        await pg.wait_for_timeout(300)
        check(await pg.input_value("#filter") == "modbus" and await pg.is_visible("#parsec"), "Modbus settings opens Settings, filtered")
        check("modbus_write_en" in await pg.inner_text("#parsec"), "the Modbus write setting is listed")
        check(errs == [], f"no script errors {errs}")
        await b.close()


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"), os.path.join(tmp, "n.bin")],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sim = None if real_modbus() else Sim()
        print("modbus status:", "simulated in the browser (firmware without it)" if sim else "the firmware's own")
        asyncio.run(run(sim))
    finally:
        unit.terminate()
        try: unit.wait(timeout=10)
        except subprocess.TimeoutExpired: unit.kill()
    print(f"\nMODBUS WEB: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
