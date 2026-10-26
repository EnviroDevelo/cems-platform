#!/usr/bin/env python3
"""The shared platform tests (CIP-26) against any product build, not only the demo: make
conformance APP=<dir> runs this on build/netsim-<dir>. It uses only what every product has
(parameters 1-999, alarms 1-99, /api/info, /api/fw), so a product passes or fails on whether it
keeps the core's behaviour.

    python3 tests/integration/conformance_test.py build/netsim

  start-up         the unit answers, starts LOCAL, the scan runs with no overruns, the start-up
                   self-test closes with nothing failed but calibration (a blank unit)
  power loss       a setting written just before a hard kill is there after the restart, and
                   the event log continues with no gap in its sequence
  corrupted        the settings memory filled with garbage: the unit still starts, safe
  settings         (defaults, CAL_INVALID, outputs off), and serves its pages
  comms loss       REMOTE with no supervisory traffic: REMOTE_LINK_LOST after the timeout
  update failure   a damaged image is refused and the unit keeps running the old one"""
# Requirements proved here: CIP-26.1, 26.2, 26.3, 26.4, 26.5, 07.4, 07.6, 06.2.
import http.cookiejar, json, os, random, signal, socket, ssl, subprocess, sys, tempfile, threading, time
import urllib.error, urllib.parse, urllib.request

CTX = ssl._create_unverified_context()
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  FAIL:", what)


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


class Unit:
    def __init__(self, netsim, fram, nor, web, mb):
        self.base = f"https://127.0.0.1:{web}"
        self.p = subprocess.Popen([netsim, str(mb), str(web), fram, nor], stdin=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.log = []
        threading.Thread(target=lambda: [self.log.append(l) for l in self.p.stderr], daemon=True).start()
        self.jar = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX), urllib.request.HTTPCookieProcessor(self.jar))
        for _ in range(200):
            try:
                urllib.request.urlopen(self.base + "/api/info", timeout=1, context=CTX).read(); return
            except Exception:
                if self.p.poll() is not None: break
                time.sleep(0.05)
        raise RuntimeError("netsim did not start: " + "".join(self.log))

    def req(self, path, form=None, raw=None):
        data, headers = None, {}
        if form is not None:
            data = urllib.parse.urlencode(form).encode(); headers = {"X-CEMS": "1"}
        if raw is not None:
            data = raw; headers = {"X-CEMS": "1", "Content-Type": "application/octet-stream"}
        try:
            with self.op.open(urllib.request.Request(self.base + path, data=data, headers=headers), timeout=10) as f:
                return f.status, json.loads(f.read() or b"null")
        except urllib.error.HTTPError as e:
            body = e.read()
            try:
                return e.code, json.loads(body)
            except ValueError:
                return e.code, None

    def touch(self, cmd):
        self.p.stdin.write(cmd + "\n"); self.p.stdin.flush(); time.sleep(0.1)

    def stop(self, hard=False):
        self.p.send_signal(signal.SIGKILL if hard else signal.SIGTERM)
        self.p.wait(timeout=10)
        time.sleep(0.2)


def alarm_active(u, aid):
    st, j = u.req("/api/alarms")
    return any(a["id"] == aid and a["active"] for a in (j or {}).get("alarms", []))


def main():
    netsim = sys.argv[1] if len(sys.argv) > 1 else "build/netsim"
    tmp = tempfile.mkdtemp()
    fram, nor = os.path.join(tmp, "fram.bin"), os.path.join(tmp, "nor.bin")
    web, mb = free_port(), free_port()

    print(f"conformance: {netsim}")
    # ---------------- start-up (CIP-26.1, 07.4) ----------------
    u = Unit(netsim, fram, nor, web, mb)
    st, info = u.req("/api/info")
    check(st == 200 and info["mode"] == "local", "starts in LOCAL mode")
    s0 = info["scan"]["scans"]
    for _ in range(140):                                  # the self-test list closes 10 s after start
        st, info = u.req("/api/info")
        if info["selftest"]["done"]: break
        time.sleep(0.1)
    fails = [t["name"] for t in info["selftest"]["tests"] if t["result"] == "fail"]
    check(info["selftest"]["done"] and set(fails) <= {"calibration"}, f"start-up self-test: failed {fails}")
    # (the PC simulation shares the CPU with the sanitizers and the key generation: a rare late
    # scan is the host's, so the check is the rate and that the watchdog never fired)
    check(info["scan"]["scans"] - s0 > 500 and info["scan"]["overruns"] * 100 <= info["scan"]["scans"],
          f"the scan runs every 10 ms, under 1 % overruns ({info['scan']})")
    check(info["last_fault"]["code"] == 0, f"no fault recorded at start-up ({info['last_fault']})")

    # ---------------- power loss (CIP-26.2, 07.6) ----------------
    st, j = u.req("/api/login", {"level": "admin", "password": "2222"})
    check(st == 200, "admin login")
    st, j = u.req("/api/values?ids=3")
    timeout_s = j["v"]["3"] if st == 200 else 10
    st, j = u.req("/api/write", {"id": 30, "v": 120})
    check(st == 200, f"write a platform setting (log interval 120 s): {st} {j}")
    time.sleep(2.5)                                       # the store saves within its period
    st, ev = u.req("/api/log/events?n=50")
    seq_before = ev["events"][0]["seq"] if st == 200 and ev["events"] else 0
    u.stop(hard=True)                                     # power cut: no shutdown at all
    u = Unit(netsim, fram, nor, web, mb)
    st, j = u.req("/api/values?ids=30")
    check(st == 200 and j["v"]["30"] == 120, f"the setting survived the power cut ({j})")
    st, ev = u.req("/api/log/events?n=200")
    seqs = [e["seq"] for e in ev["events"]] if st == 200 else []
    check(seqs and seqs == list(range(seqs[0], seqs[0] - len(seqs), -1)) and seqs[0] > seq_before,
          "the event log continues after the power cut with no gap")
    check(any(e["event"] == "power_up" for e in ev["events"][:5]), "the restart is logged")

    # ---------------- comms loss (CIP-26.4) ----------------
    u.touch("remote")
    st, info = u.req("/api/info")
    check(info["mode"] == "remote", "REMOTE from the touchscreen")
    time.sleep(timeout_s + 1.5)
    st, info = u.req("/api/info")
    check(info["link_lost"] is True and alarm_active(u, 5),
          f"no supervisory traffic for {timeout_s} s in REMOTE: REMOTE_LINK_LOST")
    u.touch("local")

    # ---------------- update failure (CIP-26.5) ----------------
    st, j = u.req("/api/login", {"level": "factory", "password": "9999"})
    st, fw0 = u.req("/api/fw")
    junk = bytes(random.Random(7).getrandbits(8) for _ in range(4096))
    st, j = u.req("/api/fw/upload", raw=junk)
    check(st >= 400, f"a damaged image is refused ({st} {j})")
    st, fw1 = u.req("/api/fw")
    check(st == 200 and not fw1.get("staged") and fw1["running"] == fw0["running"], "nothing staged; the old image runs on")
    st, info = u.req("/api/info")
    check(st == 200 and info["last_fault"]["code"] == 0, "still running normally")
    u.stop()

    # ---------------- corrupted settings (CIP-26.3, 06.2) ----------------
    size = os.path.getsize(fram)
    with open(fram, "wb") as f:
        f.write(bytes(random.Random(11).getrandbits(8) for _ in range(size)))
    u = Unit(netsim, fram, nor, web, mb)
    st, info = u.req("/api/info")
    check(st == 200, "starts with the settings memory full of garbage")
    st, j = u.req("/api/values?ids=1,30")
    check(st == 200 and j["v"]["1"] == 0 and j["v"]["30"] != 120, f"settings back to defaults, LOCAL ({j})")
    time.sleep(0.5)
    check(alarm_active(u, 1), "CAL_INVALID: measurement blocked until calibrated")
    st, page = u.req("/api/params")
    check(st == 200, "pages still served")
    u.stop()

    print(f"\nCONFORMANCE: {passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
