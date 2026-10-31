#!/usr/bin/env python3
"""tools/iob_sim.py (the pretend I/O board for the DK bench) against the real master: build/netsim
with its I/O boards on real sockets (CEMS_IOB_TCP), so the simulator speaks to the same iobus.c /
iob_tcp.c code the DK runs. Checks identification, outputs, inputs, faults, a lost board and the
temperatures, through the web API.
    python3 tests/integration/iob_sim_test.py"""
import http.cookiejar, json, os, ssl, subprocess, sys, tempfile, time, urllib.parse, urllib.request
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MB, WEB, IOB, DISC = 15913, 18913, 15502, 15223
BASE = f"https://127.0.0.1:{WEB}"
CTX = ssl._create_unverified_context()
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1; print("  ok  ", what)
    else:
        failed += 1; print("  FAIL:", what)


def get(path):
    with urllib.request.urlopen(urllib.request.Request(BASE + path, headers={"Connection": "close"}), context=CTX, timeout=10) as f:
        return json.loads(f.read())


def wait(pred, secs=6.0):
    t = time.time() + secs
    while time.time() < t:
        try:
            v = pred()
            if v:
                return v
        except (OSError, KeyError, IndexError):
            pass
        time.sleep(0.1)
    return None


OPENER = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX),
                                     urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))


def post(path, form):
    r = urllib.request.Request(BASE + path, data=urllib.parse.urlencode(form).encode(),
                               headers={"Content-Type": "application/x-www-form-urlencoded", "X-CEMS": "1"})
    try:
        with OPENER.open(r, timeout=10) as f:
            return f.status
    except urllib.error.HTTPError as e:
        return e.code


def board(n):
    return get("/api/iob")["boards"][n - 1]


def alarm(name):
    return any(a["name"] == name and a["active"] for a in get("/api/alarms")["alarms"])


def main():
    tmp = tempfile.mkdtemp()
    sim = subprocess.Popen([sys.executable, "-I", os.path.join(ROOT, "tools", "iob_sim.py"), "--boards", "1",
                            "--port", str(IOB), "--disc-port", str(DISC)],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    env = dict(os.environ, CEMS_IOB_TCP="1", CEMS_IOB_TCP_PORT=str(IOB), CEMS_IOB_DISC_PORT=str(DISC),
               CEMS_IOB_DISC_TO="127.0.0.1", CEMS_IOB_TCP_HOST="127.0.0.1")
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, text=True, env=env)
    say = lambda line: (unit.stdin.write(line + "\n"), unit.stdin.flush())
    node = lambda line: (sim.stdin.write(line + "\n"), sim.stdin.flush())
    try:
        check(wait(lambda: get("/api/info")), "unit up")
        say("set 102 1")                                   # iob_link: Ethernet on the main port
        # iob1_ip stays at its default 192.168.1.61: CEMS_IOB_TCP_HOST sends it to 127.0.0.1 (on the DK: this PC's IP)
        say("set 110 1")                                   # iob1_enable
        check(wait(lambda: board(1)["state"] == "online", 8), "board 1 online against the simulator")
        b = board(1)
        check(b["fw"] == "1.0" and b["errors"] == 0, "identification read (map 2, firmware 1.0), no failed requests")

        node("in 0x05")
        check(wait(lambda: board(1)["inputs"] & 5 == 5), "inputs 1 and 3 seen by the unit")
        node("in 0")
        check(wait(lambda: board(1)["inputs"] & 5 == 0), "...and off again")

        node("open 4")
        check(wait(lambda: "Out 3: open circuit" in board(1)["faults"]), "open circuit on output 3 reported")
        check(wait(lambda: alarm("IOB_OUT_FAULT"), 4), "output fault alarm raised")
        node("open 0")
        check(wait(lambda: not board(1)["faults"]), "fault clears")

        node("fielda 0")
        check(wait(lambda: "Field 24 V A missing" in board(1)["faults"]), "field supply A missing reported")
        node("fielda 1")

        node("power 0")
        check(wait(lambda: board(1)["state"] == "not responding"), "board not responding when the simulator stops answering")
        check(wait(lambda: alarm("IOB_LOST"), 3), "IOB_LOST alarm raised")
        node("power 1")
        check(wait(lambda: board(1)["state"] == "online", 8), "board back online")

        say("remote")                                      # web settings need REMOTE, as on the unit
        time.sleep(0.2)
        check(post("/api/login", {"level": "admin", "password": "2222"}) == 200, "admin login")
        check(post("/api/write", {"id": 162, "v": "1,0,0"}) == 200, "connector 1 set to type K thermocouples")
        node("temp 1 123.4")
        check(wait(lambda: any(t["ch"] == 1 and t["ok"] and abs(t["t"] - 123.4) < 0.15 for t in board(1)["temps"]), 8),
              "temperature channel 1 reads 123.4 degC")
        node("temp 2 open")
        check(wait(lambda: any(t["ch"] == 2 and t["state"] == "open circuit" for t in board(1)["temps"]), 8),
              "channel 2 open circuit reported")
    finally:
        unit.terminate(); unit.wait(timeout=10)
        sim.terminate(); sim.wait(timeout=10)
    print(f"\n{passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
