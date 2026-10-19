#!/usr/bin/env python3
"""End-to-end test of the platform's Modbus TCP and web servers on real sockets.

Starts build/netsim, then talks to it with pymodbus (an independent Modbus
implementation) and plain HTTP, and checks what the platform promises:
mode arbitration, range checks, atomic multi-register writes, text through
registers, client limits, the remote-link watchdog, sessions and passwords,
backup/restore, concurrent clients, and that everything survives a restart.

    PYTHONPATH=<pymodbus source> python3 tests/integration/net_test.py
"""
import http.cookiejar, ssl, json, os, signal, struct, subprocess, sys, tempfile, threading, time, urllib.request, urllib.error

from pymodbus.client import ModbusTcpClient
from pymodbus.pdu import ExceptionResponse

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MB, WEB = 15020, 18080
TMP = tempfile.mkdtemp()
FRAM = os.path.join(TMP, "fram.bin")
NOR = os.path.join(TMP, "nor.bin")
BASE = f"https://127.0.0.1:{WEB}"
CTX = ssl._create_unverified_context()   # the unit's certificate is self-signed (checked separately)
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  FAIL:", what)


class Unit:
    def __init__(self):
        self.p = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), FRAM, NOR],
                                  stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.log = []
        threading.Thread(target=self._read, daemon=True).start()
        for _ in range(100):
            try:
                urllib.request.urlopen(BASE + "/api/info", timeout=1, context=CTX).read(); return
            except Exception:
                time.sleep(0.05)
        raise RuntimeError("netsim did not start: " + "".join(self.log))

    def _read(self):
        for line in self.p.stderr:
            self.log.append(line)

    def touch(self, cmd):                       # the touchscreen
        self.p.stdin.write(cmd + "\n"); self.p.stdin.flush(); time.sleep(0.05)

    def stop(self):
        self.p.send_signal(signal.SIGTERM)
        self.p.wait(timeout=10)
        time.sleep(0.2)
        return "".join(self.log)


class Web:
    def __init__(self):
        self.jar = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX), urllib.request.HTTPCookieProcessor(self.jar))

    def req(self, path, form=None, raw=None, csrf=True):
        data, headers = None, {}
        if form is not None:
            if isinstance(form, dict):
                form = list(form.items())
            data = urllib.parse.urlencode(form).encode(); headers["Content-Type"] = "application/x-www-form-urlencoded"
        if raw is not None:
            data = raw; headers["Content-Type"] = "application/octet-stream"
        if data is not None and csrf:
            headers["X-CEMS"] = "1"
        r = urllib.request.Request(BASE + path, data=data, headers=headers)
        try:
            with self.op.open(r, timeout=5) as f:
                return f.status, f.read(), dict(f.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    def json(self, path, form=None):
        st, body, _ = self.req(path, form)
        return st, json.loads(body)


def u32(regs):                                   # high word first
    return regs[0] << 16 | regs[1]


def text_regs(s, n):
    b = s.encode().ljust(2 * n, b"\0")
    return [b[2 * k] << 8 | b[2 * k + 1] for k in range(n)]


def main():
    print("netsim with a blank FRAM")
    unit = Unit()
    web = Web()
    mb = ModbusTcpClient("127.0.0.1", port=MB); mb.connect()

    # ---------------- web: read side, JSON validity ----------------
    st, info = web.json("/api/info")
    check(st == 200 and info["mode"] == "local", "info / local mode at power-up")
    check(info["cal_valid"] is False, "uncalibrated unit reports calibration invalid")
    check(info["default_passwords"] is True, "default password warning")
    st, params = web.json("/api/params")
    names = {p["name"]: p for p in params}
    check(st == 200 and "start_delay" in names and names["start_delay"]["max"] == 60000, "params JSON")
    check(names["valve_name"]["type"] == "text" and names["ack"]["type"] == "array", "block types in params")
    st, vals = web.json("/api/values")
    check(st == 200 and vals["v"]["1001"] == 2000 and vals["v"]["1005"] == "Valve 1", "values JSON")
    st, body, hdr = web.req("/")
    check(st == 200 and hdr.get("Content-Encoding") == "gzip" and len(body) > 1000, "web page served")

    # ---------------- Modbus: read side ----------------
    r = mb.read_holding_registers(0, 4, slave=1)
    check(not r.isError() and u32(r.registers[0:2]) == 2000, "FC3 start_delay U32 = 2000")
    r = mb.read_holding_registers(10, 6, slave=1)
    name = b"".join(struct.pack(">H", x) for x in r.registers).rstrip(b"\0").decode()
    check(name == "Valve 1", f"FC3 text through registers ({name!r})")
    r = mb.read_holding_registers(4, 1, slave=1)
    check(isinstance(r, ExceptionResponse) and r.exception_code == 2, "unmapped register -> exception 02")
    r = mb.read_discrete_inputs(0, 2, slave=1)
    check(not r.isError(), "FC2 discrete inputs")
    r = mb.read_device_information(slave=1)
    check(not r.isError() and r.information.get(1, b"").startswith(b"CEMS Supervisor"), "FC43/14 device id")

    # ---------------- arbitration ----------------
    r = mb.write_registers(0, [0, 1500], slave=1)
    check(isinstance(r, ExceptionResponse) and r.exception_code == 4, "Modbus write refused in LOCAL (04)")
    unit.touch("remote")
    r = mb.write_registers(0, [0, 1500], slave=1)
    check(not r.isError(), "Modbus write accepted in REMOTE")
    time.sleep(0.05)
    check(u32(mb.read_holding_registers(0, 2, slave=1).registers) == 1500, "value applied within a scan")
    r = mb.write_registers(0, [0, 50], slave=1)
    check(isinstance(r, ExceptionResponse) and r.exception_code == 3, "below minimum -> 03")
    r = mb.write_register(1, 7, slave=1)
    check(isinstance(r, ExceptionResponse) and r.exception_code == 2, "half of a 32-bit value -> 02")
    r = mb.write_registers(2, [0, 5], slave=1)
    check(isinstance(r, ExceptionResponse) and r.exception_code == 2, "read-only register -> 02")
    r = mb.write_registers(10, text_regs("Stack B", 6), slave=1)
    check(not r.isError(), "text written through registers")
    r = mb.write_coils(0, [True, False, True, True], slave=1)
    check(not r.isError(), "FC15 coils")
    time.sleep(0.05)
    check(mb.read_coils(0, 4, slave=1).bits[:4] == [True, False, True, True], "coils read back")
    st, vals = web.json("/api/values?ids=1005,1006")
    check(vals["v"]["1005"] == "Stack B" and vals["v"]["1006"] == [True, False, True, True], "web sees Modbus writes")

    # ---------------- web: sessions, writes, passwords ----------------
    st, _ = web.json("/api/write", {"id": 1001, "v": 2500})
    check(st == 401, "web write without login -> 401")
    st, _, _ = web.req("/api/login", {"level": "admin", "password": "2222"}, csrf=False)
    check(st == 403, "POST without X-CEMS -> 403")
    st, j = web.json("/api/login", {"level": "admin", "password": "wrong"})
    check(st == 401, "wrong password -> 401")
    st, j = web.json("/api/login", {"level": "admin", "password": "2222"})
    check(st == 200 and j["level"] == "admin", "admin login")
    st, j = web.json("/api/write", [("id", 1001), ("v", 2500), ("id", 1005), ("v", "Inlet 9")])
    check(st == 200 and j["queued"] == 2, "web batch write")
    time.sleep(0.05)
    check(u32(mb.read_holding_registers(0, 2, slave=1).registers) == 2500, "Modbus sees web write")
    st, j = web.json("/api/write", [("id", 1001), ("v", 3000), ("id", 1005), ("v", "x" * 11)])
    check(st == 400 and j["error"] == "range", "web batch with one bad item refused")
    time.sleep(0.05)
    check(u32(mb.read_holding_registers(0, 2, slave=1).registers) == 2500, "...and nothing applied")
    st, j = web.json("/api/write", {"id": 1, "v": 0})
    check(st == 409 and j["error"] == "mode", "only the touchscreen changes the mode")
    st, j = web.json("/api/password", {"level": "admin", "password": "Cems-2026"})
    check(st == 200, "admin password changed")

    # ---------------- client limit and IP filter ----------------
    extra = [ModbusTcpClient("127.0.0.1", port=MB) for _ in range(4)]
    for c in extra:
        c.connect(); c.read_holding_registers(0, 1, slave=1); time.sleep(0.05)
    r = mb.read_holding_registers(0, 1, slave=1)
    check(r.isError() or not mb.connected, "oldest silent client evicted when a 5th connects")
    newest = extra[-1].read_holding_registers(0, 1, slave=1)
    check(not newest.isError(), "newest client served")
    for c in extra:
        c.close()
    st, _ = web.json("/api/write", {"id": 5, "v": "10.0.0.1"})
    check(st == 200, "Modbus client filter set from the web")
    time.sleep(0.05)
    c = ModbusTcpClient("127.0.0.1", port=MB, retries=0, timeout=1); c.connect()
    r = c.read_holding_registers(0, 1, slave=1)
    check(r.isError(), "connection from a non-allowed IP refused")
    c.close()
    web.json("/api/write", {"id": 5, "v": "0"})
    time.sleep(0.05)

    # ---------------- concurrency ----------------
    errors, counts = [], [0, 0, 0, 0]

    def mb_poller(i):
        c = ModbusTcpClient("127.0.0.1", port=MB); c.connect()
        end = time.time() + 4
        while time.time() < end:
            r = c.read_holding_registers(0, 4, slave=1)
            if r.isError(): errors.append(("mb", i, r))
            counts[i] += 1
        c.close()

    def web_poller():
        w = Web(); end = time.time() + 4
        while time.time() < end:
            st, j = w.json("/api/values")
            if st != 200: errors.append(("web", st))
            counts[3] += 1

    ts = [threading.Thread(target=mb_poller, args=(i,)) for i in range(3)] + [threading.Thread(target=web_poller)]
    for t in ts: t.start()
    for t in ts: t.join()
    check(not errors, f"3 Modbus clients + 1 browser for 4 s: {counts} requests, errors {errors[:3]}")

    # ---------------- remote-link watchdog ----------------
    time.sleep(10.5)
    st, info = web.json("/api/info")
    check(info["link_lost"] is True, "no remote traffic for 10 s in REMOTE -> link lost")
    mb2 = ModbusTcpClient("127.0.0.1", port=MB); mb2.connect(); mb2.read_holding_registers(0, 1, slave=1)
    time.sleep(0.05)
    st, info = web.json("/api/info")
    check(info["link_lost"] is False, "a Modbus request clears it")
    mb2.close()

    # ---------------- backup / restore ----------------
    st, backup, hdr = web.req("/api/backup")
    check(st == 200 and backup[:4] == b"CEMX", f"backup download ({len(backup)} bytes)")
    web.json("/api/write", {"id": 1001, "v": 9999})
    time.sleep(0.05)
    st, body, _ = web.req("/api/restore", raw=backup)
    check(st == 200, "restore accepted")
    time.sleep(0.05)
    st, vals = web.json("/api/values?ids=1001")
    check(vals["v"]["1001"] == 2500, "restore put the setting back")
    bad = bytearray(backup); bad[-1] ^= 1
    st, body, _ = web.req("/api/restore", raw=bytes(bad))
    check(st == 400, "damaged backup refused")

    # ---------------- HTTPS ----------------
    import hashlib, socket
    def tls_conn(session=None):
        raw = socket.create_connection(("127.0.0.1", WEB), timeout=5)
        t = CTX.wrap_socket(raw, server_hostname="127.0.0.1", session=session)
        t.sendall(b"GET /api/info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
        data = b""
        while True:
            chunk = t.recv(4096)
            if not chunk: break
            data += chunk
        return t, data
    t1, d1 = tls_conn()
    der = t1.getpeercert(binary_form=True)
    fp = ":".join(f"{b:02X}" for b in hashlib.sha256(der).digest())
    info = json.loads(d1.split(b"\r\n\r\n", 1)[1])
    check(t1.version() == "TLSv1.2" and t1.cipher()[0] == "ECDHE-ECDSA-AES128-GCM-SHA256", f"TLS 1.2 ECDHE-ECDSA-AES128-GCM ({t1.version()} {t1.cipher()[0]})")
    check(info["tls_fingerprint"] == fp, "fingerprint on the page matches the certificate the browser got")
    t2, d2 = tls_conn(session=t1.session)
    check(t2.session_reused and d2.startswith(b"HTTP/1.1 200"), "next connection resumes the session (no public-key work)")
    st, body, hdr = web.req("/api/tls/cert", csrf=False)
    check(st == 200 and body == der, "certificate download is the served certificate")
    try:
        CTX2 = ssl.create_default_context(); CTX2.load_verify_locations(cadata=der)
        with socket.create_connection(("127.0.0.1", WEB), timeout=5) as raw, CTX2.wrap_socket(raw, server_hostname="127.0.0.1") as t3:
            ok_verify = True
    except ssl.SSLError as e:
        ok_verify = False; print("   verify:", e)
    check(ok_verify, "once the downloaded certificate is trusted, the connection verifies (IP address in the certificate)")
    import http.client
    hc = http.client.HTTPConnection("127.0.0.1", WEB + 1, timeout=5); hc.request("GET", "/api/values")
    rr = hc.getresponse()
    check(rr.status == 301 and rr.getheader("Location") == "https://127.0.0.1/api/values", "plain HTTP redirects to https://")
    raw = socket.create_connection(("127.0.0.1", WEB), timeout=5); raw.sendall(b"GET / HTTP/1.1\r\n\r\n")
    try:
        junk = raw.recv(100)
    except OSError:
        junk = b""
    check(not junk.startswith(b"HTTP/"), "plain HTTP to the HTTPS port gets no page")
    raw.close()
    st, info = web.json("/api/info")
    print(f"   TLS: {info['tls']}")
    check(info["tls"]["resumed"] >= 1 and info["tls"]["pool_peak"] < 36864, "TLS statistics")

    # ---------------- event log ----------------
    st, j = web.json("/api/log/events?n=200")
    ev = j["events"] if st == 200 else []
    kinds = [e["event"] for e in ev]
    check(st == 200 and kinds[-1] == "power_up" and ev[-1]["seq"] == 1, "event log starts with the power-up")
    check(any(e["event"] == "mode" and e["source"] == "touchscreen" and e["new"] == 1 for e in ev), "mode change logged")
    check(any(e["event"] == "param" and e["source"] == "modbus" and e["arg"] == 1001 and e["old"] == 2000 and e["new"] == 1500
              for e in ev), "Modbus write logged with old and new value")
    check(any(e["event"] == "param" and e["source"] == "web" and e["level"] == "admin" and e["new"] == 2500 for e in ev),
          "web write logged with the login level")
    check(kinds.count("login_fail") >= 1 and "login" in kinds and "password" in kinds and "restore_backup" in kinds,
          "logins, password change and restore logged")
    check(sum(1 for e in ev if e["event"] == "param" and e["arg"] == 1006) == 1, "a multi-coil write is one block event")
    st, body, hdr = web.req("/api/log/events.csv", csrf=False)
    lines = body.decode().splitlines()
    check(st == 200 and hdr.get("Content-Type", "").startswith("text/csv") and lines[0].startswith("seq,time,event")
          and len(lines) == len(ev) + 1, f"event CSV ({len(lines) - 1} rows)")
    st, j = web.json("/api/log/info")
    check(st == 200 and j["status"] == 0 and j["events"]["used"] == len(ev), "log info")
    n_events = len(ev)

    time.sleep(1.5)                                  # let the store save
    log = unit.stop()
    check("stopped" in log, "clean stop")

    # ---------------- restart: everything persisted ----------------
    print("restart with the same FRAM file")
    unit = Unit()
    web = Web()
    st, vals = web.json("/api/values?ids=1,1001,1005,5")
    check(vals["v"]["1"] == 0, "mode is LOCAL again after power-up")
    check(vals["v"]["1001"] == 2500 and vals["v"]["1005"] == "Inlet 9", "settings survived the restart")
    check(vals["v"]["5"] == 0, "filter reset survived")
    st, j = web.json("/api/login", {"level": "admin", "password": "2222"})
    check(st == 401, "old admin password no longer works")
    st, j = web.json("/api/login", {"level": "admin", "password": "Cems-2026"})
    check(st == 200, "changed admin password survived the restart")
    st, j = web.json("/api/log/events?n=200")
    ev = j["events"] if st == 200 else []
    check(len(ev) == n_events + 3 and [e["event"] for e in ev[:3]] == ["login", "login_fail", "power_up"],
          "event log continues across the restart")
    check([e["seq"] for e in ev] == list(range(len(ev), 0, -1)), "sequence numbers contiguous")
    st, j = web.json("/api/log/info")
    check(st == 200 and j["status"] == 0, "write position from FRAM (not rebuilt)")
    log = unit.stop()
    print(log.strip().splitlines()[-1])

    print(f"\n{passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
