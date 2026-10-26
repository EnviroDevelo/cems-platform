#!/usr/bin/env python3
"""Several people viewing at once: 4 headless Chromium browsers (separate profiles, so
separate connections) keep the page open on build/netsim for 30 s. Each must still get
its value polls at the page's rate (2 a second) promptly, and none may show "connection
lost". Before waiting connections were held in a table (http_server.h), spare sockets
that browsers open and never use held the one-at-a-time server for 2 s each, and four
browsers got one value poll every 5-8 s. Needs Python Playwright and a Chromium."""
import asyncio, glob, json, os, ssl, subprocess, sys, tempfile, time, urllib.request
from playwright.async_api import async_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROME = os.environ.get("CHROME") or (sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux*/chrome")) or [None])[-1]
URL = "https://127.0.0.1:18097/"
BROWSERS, SECONDS = 4, 30
CTX = ssl.create_default_context(); CTX.check_hostname = False; CTX.verify_mode = ssl.CERT_NONE

def tls_stats():
    return json.load(urllib.request.urlopen(URL + "api/info", context=CTX, timeout=10))["tls"]

async def run():
    ok = True
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pages, stats = [], []
        for _ in range(BROWSERS):
            pg = await (await b.new_context(ignore_https_errors=True)).new_page()
            st, t0 = {"ok": 0, "fail": 0, "lat": [], "stale": 0}, {}
            pg.on("request", lambda r, t0=t0: t0.__setitem__(r, time.time()) if "/api/values" in r.url else None)
            pg.on("requestfinished", lambda r, st=st, t0=t0: (st.__setitem__("ok", st["ok"] + 1),
                  st["lat"].append(time.time() - t0.pop(r, time.time()))) if "/api/values" in r.url else None)
            pg.on("requestfailed", lambda r, st=st: st.__setitem__("fail", st["fail"] + 1) if "/api/values" in r.url else None)
            await pg.goto(URL)
            pages.append(pg); stats.append(st)
        await asyncio.sleep(3)                               # page loads settle
        for st in stats: st.update(ok=0, fail=0, lat=[])
        s0, t = tls_stats(), time.time()
        while time.time() - t < SECONDS:
            for k, pg in enumerate(pages):
                if await pg.evaluate("document.body.classList.contains('stale')"): stats[k]["stale"] += 1
            await asyncio.sleep(1)
        s1, el = tls_stats(), time.time() - t
        for k, st in enumerate(stats):
            lat = sorted(st["lat"]) or [99]
            rate, p95 = st["ok"] / el, lat[int(len(lat) * 0.95)]
            print(f"browser {k + 1}: {rate:.2f} value polls/s, {st['fail']} failed, p95 {p95 * 1000:.0f} ms, "
                  f"connection lost shown {st['stale']} s")
            ok &= rate >= 1.5 and p95 < 1.0 and st["stale"] == 0
        hs = s1["handshakes"] - s0["handshakes"]
        print(f"server: {hs / el:.1f} TLS connections/s, {s1['resumed'] - s0['resumed']} of {hs} resumed")
        await b.close()
    return ok

def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), "15097", "18097", os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, text=True)
    time.sleep(1)
    try:
        ok = asyncio.run(run())
    finally:
        unit.terminate()
    print("MULTI VIEWER:", "PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
