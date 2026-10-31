#!/usr/bin/env python3
"""Third-party instruments end to end (inst.h, docs/MODBUS_ROLES.md role 2): build/netsim with its
simulated gas analyser on the I/O network (192.168.1.70), set up through the web API as a
technician would (/api/write of parameters 500-621), read back on /api/inst and /api/values, the
analyser's status word turning into quality, the analyser going silent, and the register explorer.
    python3 tests/integration/inst_test.py"""
import http.cookiejar, json, os, ssl, struct, subprocess, sys, tempfile, time, urllib.parse, urllib.request
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MB, WEB = 15913, 18913
BASE = f"https://127.0.0.1:{WEB}"
CTX = ssl._create_unverified_context()
JAR = http.cookiejar.CookieJar()
OPEN = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX), urllib.request.HTTPCookieProcessor(JAR))
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1; print("  ok  ", what)
    else:
        failed += 1; print("  FAIL:", what)


def get(path):
    with OPEN.open(urllib.request.Request(BASE + path, headers={"Connection": "close"}), timeout=10) as f:
        return json.loads(f.read())


def post(path, pairs):
    body = urllib.parse.urlencode(pairs).encode()
    req = urllib.request.Request(BASE + path, data=body, headers={"X-CEMS": "1", "Connection": "close",
                                 "Content-Type": "application/x-www-form-urlencoded"})
    try:
        with OPEN.open(req, timeout=10) as f:
            return f.status, json.loads(f.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def wait(pred, secs=6.0):
    t = time.time() + secs
    while time.time() < t:
        try:
            v = pred()
            if v:
                return v
        except OSError:
            pass
        time.sleep(0.1)
    return None


def inst(n):
    return get("/api/inst")["instruments"][n - 1]


def pt(n):
    return next(p for p in get("/api/inst")["points"] if p["n"] == n)


def main():
    tmp = tempfile.mkdtemp()
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, text=True)

    def say(line):
        unit.stdin.write(line + "\n"); unit.stdin.flush()
    try:
        check(wait(lambda: get("/api/info")), "unit up")
        check(inst(1)["state"] == "off" and get("/api/inst")["points"] == [], "no instruments by default")
        st, _ = post("/api/login", [("level", "admin"), ("password", "2222")])
        check(st == 200, "admin login")
        say("set 1 1")                                     # REMOTE: the web may change settings
        time.sleep(0.3)
        # instrument 1: the analyser; status word at holding 10 (bit 0 fault, 1 maintenance, 2 calibration)
        w = [("id", 501), ("o", 0), ("v", "192.168.1.70"),
             ("id", 508), ("o", 0), ("v", "3"),
             ("id", 509), ("o", 0), ("v", "1"), ("id", 510), ("o", 0), ("v", "2"), ("id", 511), ("o", 0), ("v", "4"),
             ("id", 521), ("v", "Stack analyser"),
             # points 1-4: O2 f32 CDAB at 0, CO f32 CDAB at 2, status u16 at 10, cell temperature i16 x0.1 input 0
             ("id", 530), ("o", 0), ("v", "1,1,1,1"),
             ("id", 531), ("o", 0), ("v", "3,3,3,4"),
             ("id", 532), ("o", 0), ("v", "0,2,10,0"),
             ("id", 533), ("o", 0), ("v", "4,4,0,1"),
             ("id", 534), ("o", 0), ("v", "1,1,0,0"),
             ("id", 536), ("o", 3), ("v", "0.1"),
             ("id", 538), ("o", 0), ("v", "0"), ("id", 539), ("o", 0), ("v", "25"),
             ("id", 550), ("v", "O2"), ("id", 590), ("v", "%"),
             ("id", 551), ("v", "CO"), ("id", 591), ("v", "mg/m³"), ("id", 593), ("v", "°C")]
        groups, cur = [], []                               # a few items per request (the write queue is short)
        for kv in w:
            if kv[0] == "id" and len([x for x in cur if x[0] == "id"]) >= 6:
                groups.append(cur); cur = []
            cur.append(kv)
        groups.append(cur)
        res = [post("/api/write", g) for g in groups]   # "o" (first element) goes before its "v"
        check(all(r[0] == 200 for r in res), f"settings written {res}")
        time.sleep(0.2)
        st, j = post("/api/write", [("id", 500), ("o", 0), ("v", "1")])
        check(st == 200, "instrument 1 enabled")
        check(wait(lambda: inst(1)["state"] == "ok"), "analyser polled: " + inst(1)["state"])
        o2, co, stw, temp = pt(1), pt(2), pt(3), pt(4)
        check(o2["value"] is not None and 7.4 < o2["value"] < 8.1 and o2["q"] == "valid", f"O2 {o2['value']} {o2['q']}")
        check(co["value"] is not None and 11.9 < co["value"] < 16.1, f"CO {co['value']}")
        check(o2["name"] == "O2" and o2["unit"] == "%" and o2["type"] == "f32" and o2["order"] == "CDAB", "point described")
        check(co["unit"] == "mg/m³" and temp["unit"] == "°C", f"units with symbols: {co['unit']} {temp['unit']}")
        check(temp["value"] is not None and 44.9 < temp["value"] < 46.1, f"cell temperature {temp['value']}")
        check(inst(1)["name"] == "Stack analyser" and inst(1)["errors"] == 0, "instrument named, no errors")
        v = get("/api/values?ids=540")["v"]["540"]
        check(isinstance(v, list) and abs(v[0] - o2["value"]) < 1.0, "values in /api/values too")
        say("analyser status 2")
        check(wait(lambda: pt(1)["q"] == "maint"), "status bit 1: maintenance quality")
        say("analyser status 4")
        check(wait(lambda: pt(1)["q"] == "cal"), "status bit 2: calibration quality")
        say("analyser status 1")
        check(wait(lambda: pt(2)["q"] == "fault"), "status bit 0: fault quality")
        say("analyser status 0")
        check(wait(lambda: pt(1)["q"] == "valid"), "status clear: valid")
        say("analyser off")
        check(wait(lambda: pt(1)["q"] == "stale"), "silent analyser: stale")
        check(wait(lambda: inst(1)["state"] == "lost" and pt(1)["q"] == "unavailable"), "then lost, unavailable")
        say("analyser on")
        check(wait(lambda: inst(1)["state"] == "ok" and pt(1)["q"] == "valid", 10), "back after it answers again")
        # the register explorer
        st, _ = post("/api/inst/explore", [("ip", "192.168.1.70"), ("unit", "1"), ("fc", "3"), ("addr", "0"), ("count", "4")])
        check(st == 202, "explorer read started")
        ex = wait(lambda: (lambda e: e if not e["busy"] else None)(get("/api/inst/explore")))
        check(ex and ex["ok"] and len(ex["words"]) == 4, f"explorer read 4 words: {ex}")
        if ex and ex["ok"]:
            wds = ex["words"]
            f = struct.unpack(">f", struct.pack(">HH", wds[1], wds[0]))[0]     # CDAB
            check(7.4 < f < 8.1, f"the words decode to the O2 as CDAB: {f}")
        st, _ = post("/api/inst/explore", [("ip", "192.168.1.70"), ("fc", "3"), ("addr", "600"), ("count", "1")])
        ex = wait(lambda: (lambda e: e if not e["busy"] else None)(get("/api/inst/explore")))
        check(ex and ex["result"] == "exception" and ex["exception"] == 2, "explorer: exception 02 past the map")
        st, _ = post("/api/inst/explore", [("ip", "192.168.1.99"), ("fc", "3"), ("addr", "0"), ("count", "1")])
        ex = wait(lambda: (lambda e: e if not e["busy"] else None)(get("/api/inst/explore")))
        check(ex and ex["result"] == "no connection", "explorer: nothing at that address")
    finally:
        unit.terminate()
        unit.wait(5)
    print(f"{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
