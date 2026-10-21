#!/usr/bin/env python3
"""End-to-end test of the VM/MUX application on real sockets (build/vm_netsim).

Drives the application the way a site will: an engineer configures it on the touchscreen,
an operator runs the Multiplexor from the web page, a PLC reads it over Modbus TCP, and the
settings survive a restart. Plain sockets and urllib only (no third-party packages).

    make -f app_vmmux/app.mk build/vm_netsim && python3 app_vmmux/tests/integration/vm_net_test.py
"""
import http.cookiejar, json, os, signal, socket, ssl, struct, subprocess, sys, tempfile, threading, time
import urllib.parse, urllib.request, urllib.error

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
MB, WEB = 15120, 18180
TMP = tempfile.mkdtemp()
FRAM, NOR = os.path.join(TMP, "fram.bin"), os.path.join(TMP, "nor.bin")
BASE = f"https://127.0.0.1:{WEB}"
CTX = ssl._create_unverified_context()          # the unit's certificate is self-signed
passed = failed = 0

# parameter IDs (include/vm_params.h, vm_hmi.h, vm_params_modbus.h)
MAXV, NUMMUX, DOSING, MUXEN = 1091, 1094, 1056, 2906
MUX_MANUAL, MUX_YES, MUX_START, MUX_INLET = 5012, 5013, 5015, 5016
MUX_POPUP, MUX_INLET_ON, MUX_MANUAL_STATE = 5126, 5132, 5122


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  FAIL:", what)


class Unit:
    def __init__(self):
        self.p = subprocess.Popen([os.path.join(ROOT, "build", "vm_netsim"), str(MB), str(WEB), FRAM, NOR],
                                  stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.log = []
        threading.Thread(target=self._read, daemon=True).start()
        for _ in range(200):
            try:
                urllib.request.urlopen(BASE + "/api/info", timeout=1, context=CTX).read(); return
            except Exception:
                time.sleep(0.05)
        raise RuntimeError("vm_netsim did not start: " + "".join(self.log))

    def _read(self):
        for line in self.p.stderr:
            self.log.append(line)

    def touch(self, cmd):                        # the touchscreen stand-in on stdin
        self.p.stdin.write(cmd + "\n"); self.p.stdin.flush(); time.sleep(0.1)

    def stop(self):
        self.p.send_signal(signal.SIGTERM); self.p.wait(timeout=10); time.sleep(0.2)


class Web:
    def __init__(self):
        self.op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX),
                                              urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def req(self, path, form=None):
        data, headers = None, {}
        if form is not None:
            data = urllib.parse.urlencode(form).encode()
            headers = {"Content-Type": "application/x-www-form-urlencoded", "X-CEMS": "1"}
        r = urllib.request.Request(BASE + path, data=data, headers=headers)
        try:
            with self.op.open(r, timeout=5) as f:
                return f.status, json.loads(f.read() or b"null")
        except urllib.error.HTTPError as e:
            body = e.read()
            return e.code, (json.loads(body) if body else None)

    def values(self):
        return self.req("/api/values")[1]["v"]

    def write(self, pid, v):
        return self.req("/api/write", [("id", pid), ("v", v)])


class Modbus:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", MB), timeout=3); self.tid = 0

    def pdu(self, body):
        self.tid += 1
        self.s.sendall(struct.pack(">HHHB", self.tid, 0, len(body) + 1, 1) + body)
        hdr = self.s.recv(7); n = struct.unpack(">HHHB", hdr)[2] - 1
        r = b""
        while len(r) < n: r += self.s.recv(n - len(r))
        return r

    def holding(self, addr, n):
        r = self.pdu(struct.pack(">BHH", 3, addr, n))
        return list(struct.unpack(">" + "H" * n, r[2:])) if r[0] == 3 else None

    def coils(self, addr, n):
        r = self.pdu(struct.pack(">BHH", 1, addr, n))
        return [(r[2 + i // 8] >> (i % 8)) & 1 for i in range(n)] if r[0] == 1 else None


def wait_for(fn, timeout=3.0):
    t = time.time()
    while time.time() - t < timeout:
        if fn(): return True
        time.sleep(0.05)
    return False


def main():
    print("vm_netsim with a blank FRAM")
    unit = Unit(); web = Web(); mb = Modbus()

    st, info = web.req("/api/info")
    check(st == 200 and "Valve Manager" in json.dumps(info), "/api/info names the application")
    st, params = web.req("/api/params")
    ids = {p["id"]: p for p in params}
    check(st == 200 and MUX_START in ids and ids[MUX_START]["name"] == "mux_manual_start", "screen commands listed")
    check("confirm" in ids[MUX_START]["flags"], "valve-moving commands carry 'confirm'")

    # engineer: configure a 4-valve MUX on the touchscreen (LOCAL)
    unit.touch(f"set {MAXV} 10"); unit.touch(f"set {NUMMUX} 4")
    check(wait_for(lambda: web.values()[str(NUMMUX)] == 4), "touchscreen: 4 MUX valves")
    # the DO map is an array: written from the web page later (REMOTE), as a site would

    # operator on the web page: refused in LOCAL, accepted in REMOTE
    st, _ = web.req("/api/login", [("level", "operator"), ("password", "1111")])
    check(st == 200, "web login (operator)")
    st, j = web.write(MUX_MANUAL, 1)
    check(st == 409, "web command refused in LOCAL mode")
    unit.touch("remote")
    st, _ = web.req("/api/login", [("level", "admin"), ("password", "2222")])
    check(st == 200, "web login (admin)")
    st, j = web.req("/api/write", [("id", DOSING), ("o", 0), ("v", "5"), ("id", DOSING), ("o", 2), ("v", "6")])
    check(st == 200, f"web: DO map written ({st} {j})")
    st, _ = web.write(MUX_MANUAL, 1)
    check(st == 200 and wait_for(lambda: web.values()[str(MUX_POPUP)] in (True, 1)), "web: Manual -> confirmation shown")
    web.write(MUX_YES, 1); web.write(MUX_START, 1)
    check(wait_for(lambda: web.values()[str(MUX_MANUAL_STATE)] == 1), "web: manual mode running")
    st, _ = web.write(MUX_INLET, 2)
    check(st == 200 and wait_for(lambda: web.values()[str(MUX_INLET_ON)][1] in (True, 1)), "web: inlet 2 open")

    # the PLC sees it: holding 760 = CurrentModeMUX (3 = manual)
    check(wait_for(lambda: (mb.holding(760, 1) or [0])[0] == 3), "Modbus: MUX mode 3 (manual) at holding 760")
    # inlet 4 does not exist with 4 valves: the interlock refuses it at apply time
    web.write(MUX_INLET, 4); time.sleep(0.3)
    check(web.values()[str(MUX_INLET_ON)][3] in (False, 0), "web: inlet 4 refused (not fitted)")

    # master enable off from the web page (admin): everything MUX stops
    st, _ = web.write(MUXEN, 0)
    check(st == 200 and wait_for(lambda: (mb.holding(760, 1) or [9])[0] == 0), "mux_enable off: MUX at rest")
    check(not any(web.values()[str(MUX_INLET_ON)]), "mux_enable off: no inlet open")

    # restart: settings kept, mode LOCAL again
    unit.stop(); unit = Unit(); web = Web(); mb = Modbus()
    v = web.values()
    check(v[str(NUMMUX)] == 4 and v[str(MAXV)] == 10, "counts kept through a restart")
    check(v[str(MUXEN)] in (False, 0), "mux_enable kept off through a restart")
    check(v["1"] == 0, "LOCAL mode after a restart")
    st, ev = web.req("/api/log/events")
    check(st == 200, "event log readable")
    unit.stop()
    print(f"\nvm_net_test: {passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
