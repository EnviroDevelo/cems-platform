#!/usr/bin/env python3
"""The web page sends one request at a time (DK bench, 28 Oct: parallel polls each cost a TLS
handshake on the H5 and some timed out). Against build/netsim, logged in as admin so every
poller runs, for 15 s: at most one request in flight, the browser stays on very few
connections, the values keep updating, and a write made meanwhile goes through. On the
Overview tab, the closed tabs' data is asked for at most every 30 s.
Needs Python Playwright and a Chromium."""
import asyncio, glob, os, subprocess, sys, tempfile
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
MB, WEB = 15904, 18904
URL = f"https://127.0.0.1:{WEB}/"
passed = failed = 0

COUNT_FETCH = """(() => {
  const f = window.fetch; window.__inflight = 0; window.__maxInflight = 0; window.__requests = 0; window.__paths = [];
  window.fetch = async (...a) => {
    window.__inflight++; window.__requests++; window.__paths.push(String(a[0]).split("?")[0]); window.__maxInflight = Math.max(window.__maxInflight, window.__inflight);
    try { return await f(...a); } finally { window.__inflight--; }
  };
})();"""


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


async def run():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        ctx = await b.new_context(viewport={"width": 1200, "height": 900}, ignore_https_errors=True, bypass_csp=True)
        await ctx.add_init_script(COUNT_FETCH)
        pg = await ctx.new_page()
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        cdp = await ctx.new_cdp_session(pg)
        await cdp.send("Network.enable")
        conns, api_calls = set(), [0]
        def on_resp(ev):
            if "/api/" in ev["response"]["url"]:
                api_calls[0] += 1
                conns.add(ev["response"].get("connectionId"))
        cdp.on("Network.responseReceived", on_resp)
        for _ in range(100):
            try: await pg.goto(URL); break
            except Exception: await asyncio.sleep(0.1)
        await pg.wait_for_selector("#loginrow", state="visible", timeout=8000)
        await pg.select_option("#lvl", "admin"); await pg.fill("#pw", "2222"); await pg.click("#loginbtn")
        await pg.wait_for_selector("#userrow", state="visible", timeout=5000)
        conns.clear(); api_calls[0] = 0
        await pg.evaluate("window.__maxInflight = 0; window.__requests = 0")
        up0 = await pg.evaluate("lastOk")
        await pg.wait_for_timeout(7000)
        # a write while everything polls: the page's own write path (the clock's UTC offset field is admin-level)
        res = await pg.evaluate("""api('/api/info').then(i => api('/api/values')).then(() => 'ok', e => String(e))""")
        check(res == "ok", f"requests made while the pollers run complete ({res})")
        await pg.wait_for_timeout(8000)
        mx, n = await pg.evaluate("window.__maxInflight"), await pg.evaluate("window.__requests")
        check(mx == 1, f"at most one request in flight ({mx} max over {n} requests in 15 s)")
        check(len(conns) <= 2, f"the browser stays on {len(conns)} connection(s) for {api_calls[0]} API responses")
        up1 = await pg.evaluate("lastOk")
        check(up1 - up0 > 13000 and await pg.inner_text("#conn") == "live", "values still update, page live")
        lost = await pg.evaluate("document.body.classList.contains('stale')")
        check(not lost, "no connection-lost banner")
        # tabs: what a closed tab shows is asked for at most every 30 s
        await pg.click('#tabbar button[data-tab="overview"]')
        await pg.wait_for_timeout(1000)
        await pg.evaluate("window.__paths = []")
        await pg.wait_for_timeout(10000)
        paths = await pg.evaluate("window.__paths")
        bg = {k: paths.count(k) for k in ("/api/log/events", "/api/service", "/api/iob", "/api/cal")}
        check(all(n <= 1 for n in bg.values()) and paths.count("/api/values") >= 10,
              f"on Overview for 10 s: {paths.count('/api/values')} value polls, closed tabs {bg}")
        await pg.click('#tabbar button[data-tab="events"]')
        await pg.wait_for_timeout(1500)
        check(await pg.evaluate("window.__paths.slice(-6).includes('/api/log/events')"), "opening the Event log tab asks for the events at once")
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
    print(f"\nQUEUE: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
