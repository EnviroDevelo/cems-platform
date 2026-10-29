#!/usr/bin/env python3
"""End-to-end test of the platform's Modbus TCP and web servers on real sockets.

Starts build/netsim, then talks to it with pymodbus (an independent Modbus
implementation) and plain HTTP, and checks what the platform promises:
mode arbitration, range checks, atomic multi-register writes, text through
registers, client limits, the remote-link watchdog, sessions and passwords,
backup/restore, concurrent clients, and that everything survives a restart.

    PYTHONPATH=<pymodbus source> python3 tests/integration/net_test.py
"""
# Requirements proved here: CIP-07.6, 26.1, 26.4, 05.5, 12.3, 12.4, 15.4, 17.2, 17.5, 20.1, 13.1.
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
    import hashlib, socket, http.client
    def tls_conn(session=None):
        raw = socket.create_connection(("127.0.0.1", WEB), timeout=5)
        t = CTX.wrap_socket(raw, server_hostname="127.0.0.1", session=session)
        t.sendall(b"GET /api/info HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
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
    # keep-alive: one TLS connection for many polls; a waiting client takes over; POST closes
    kc = http.client.HTTPSConnection("127.0.0.1", WEB, context=CTX, timeout=5)
    def kinfo():
        kc.request("GET", "/api/info"); rr = kc.getresponse(); return json.loads(rr.read())["tls"]["handshakes"]
    hs0 = kinfo()
    ok_all = True
    for k in range(20):
        kc.request("GET", "/api/values?ids=1"); rr = kc.getresponse(); body = rr.read()
        ok_all &= rr.status == 200 and rr.getheader("Connection") == "keep-alive" and body.startswith(b'{"t":')
    hs1 = kinfo()
    check(ok_all and hs1 == hs0, f"22 requests on one kept connection: no further TLS handshakes ({hs1 - hs0})")
    kc.request("GET", "/api/log/events.csv"); rr = kc.getresponse(); body = rr.read()
    check(rr.getheader("Transfer-Encoding") == "chunked" and body.startswith(b"seq,time,event"), "a streamed CSV is chunked on a kept connection")
    # a second browser keeps its own connection: both are served in turn, no new handshakes
    kc2 = http.client.HTTPSConnection("127.0.0.1", WEB, context=CTX, timeout=5)
    kc2.request("GET", "/api/values?ids=1"); kc2.getresponse().read()
    hs2 = kinfo()
    ok_all = True
    for k in range(10):
        for c in (kc, kc2):
            c.request("GET", "/api/values?ids=1"); rr = c.getresponse(); body = rr.read()
            ok_all &= rr.status == 200 and rr.getheader("Connection") == "keep-alive"
    check(ok_all and kinfo() == hs2, "two kept connections served in turn, no further TLS handshakes")
    time.sleep(0.2)
    kc2.request("GET", "/api/values?ids=1"); kc2.getresponse().read()   # kc is now the longer idle one
    other = Web(); other.json("/api/info")                # a third client: the longest idle gives way
    def alive(c):
        try:
            c.request("GET", "/api/values?ids=1"); rr = c.getresponse(); rr.read(); return True
        except (http.client.RemoteDisconnected, ConnectionError, ssl.SSLError):
            return False
    check(not alive(kc) and alive(kc2), "a third client takes the place of the longest idle kept connection")
    kc.close(); kc2.close()
    kc = http.client.HTTPSConnection("127.0.0.1", WEB, context=CTX, timeout=5)
    kc.request("POST", "/api/logout", body=b"", headers={"X-CEMS": "1", "Content-Type": "application/x-www-form-urlencoded"})
    rr = kc.getresponse(); rr.read()
    check(rr.getheader("Connection") == "close", "after a POST the connection closes")
    kc.close()
    st, info = web.json("/api/info")
    print(f"   TLS: {info['tls']}")
    check(info["tls"]["resumed"] >= 1 and info["tls"]["pool_peak"] < 60 * 1024, "TLS statistics")

    # ---------------- alarms ----------------
    def alarms(w=web):
        st, j = w.json("/api/alarms")
        return j, {a["id"]: a for a in j["alarms"]}
    j, a = alarms()
    check(1 in a and a[1]["severity"] == "fault" and a[1]["active"] and a[1]["name"] == "CAL_INVALID",
          "platform alarm: calibration invalid")
    check(a[1]["help"].startswith("Neither copy of the factory data"), "alarm list carries what to check (help)")
    web.json("/api/write", {"id": 1007, "v": 1}); time.sleep(0.3)
    j, a = alarms()
    check(100 in a and a[100]["active"] and a[100]["unacked"] and a[100]["latch"], "application alarm raised (latching)")
    check(j["alarms"][0]["id"] == 100, "most urgent first (unacknowledged fault, newest)")
    web.json("/api/write", {"id": 1007, "v": 0}); time.sleep(0.3)
    j, a = alarms()
    check(100 in a and a[100]["active"], "latched after its condition went")
    mbc = ModbusTcpClient("127.0.0.1", port=MB); mbc.connect()
    rr = mbc.read_input_registers(0, 2, slave=1)
    rb = mbc.read_input_registers(10, 16, slave=1); ru = mbc.read_input_registers(30, 16, slave=1)
    check(not rr.isError() and rr.registers[0] == j["active"] and rr.registers[1] == j["unacked"],
          f"Modbus: alarm counts ({rr.registers if not rr.isError() else rr})")
    check(not rb.isError() and rb.registers[100 // 16] & (1 << (100 % 16)) and ru.registers[100 // 16] & (1 << (100 % 16)),
          "Modbus: active and unacknowledged bits per alarm (K12)")
    rc = mbc.read_input_registers(2, 1, slave=1)
    check(not rc.isError() and (rc.registers[0] & 0x5) == 0x5, f"Modbus: common alarm word (Modbus-routed fault active) {rc.registers if not rc.isError() else rc}")
    web.json("/api/write", [("id", 46), ("o", 1), ("v", 0x80)]); time.sleep(0.3)
    rc = mbc.read_input_registers(2, 1, slave=1)
    check(not rc.isError() and (rc.registers[0] & 0x1) == 0 and (rc.registers[0] & 0x4), "Modbus: an alarm switched off for Modbus leaves the common bit")
    web.json("/api/write", [("id", 46), ("o", 1), ("v", 0)])
    wr = mbc.write_register(20, 100, slave=1); time.sleep(0.3)
    j, a = alarms()
    check(not wr.isError() and 100 not in a, "acknowledged from Modbus: the latched alarm clears")
    ru = mbc.read_input_registers(30, 16, slave=1)
    check(not (ru.registers[100 // 16] & (1 << (100 % 16))), "Modbus: bit cleared")
    st, j2 = web.json("/api/write", [("id", 1010), ("v", "Stack level")]); time.sleep(0.05)
    web.json("/api/write", {"id": 1008, "v": 95}); time.sleep(1.0)
    j, a = alarms()
    check(101 not in a, "range alarm waits for its 2 s on-delay")
    time.sleep(1.5)
    j, a = alarms()
    check(101 in a and a[101]["text"] == "Stack level", f"range alarm raised, with its edited description ({a.get(101, {}).get('text')})")
    st, _ = Web().json("/api/alarms/ack", {"id": "all"})
    check(st == 401, "acknowledging from the web needs a login")
    st, _ = web.json("/api/alarms/ack", {"id": 77})
    check(st == 404, "unknown alarm")
    st, _ = web.json("/api/alarms/ack", {"id": "all"}); time.sleep(0.3)
    j, a = alarms()
    check(st == 200 and j["unacked"] == 0 and a[101]["active"], "acknowledge all from the web (still active)")
    web.json("/api/write", {"id": 1009, "v": 0}); time.sleep(0.3)
    j, a = alarms()
    check(101 not in a, "range alarm switched off: suppressed at once")
    web.json("/api/write", {"id": 1009, "v": 1}); web.json("/api/write", {"id": 1008, "v": 50})
    st, info = web.json("/api/info")
    check(info["alarms"]["active"] == j["active"], "alarm counts in /api/info")
    mbc.close()

    # ---------------- calibration records (CIP-18) ----------------
    st, cal = web.json("/api/cal")
    check(st == 200 and cal["channels"][0]["name"] == "Level" and cal["channels"][0]["in_force"] is None
          and cal["active"] is None, "calibrated channels listed, nothing in force")
    st, j = web.json("/api/cal", {"cmd": "begin", "ch": 0})
    check(st == 409 and "FAULT" in j["error"], f"admin may not calibrate a unit with no calibration data ({j})")
    fac = Web()
    st, j = fac.json("/api/login", {"level": "factory", "password": "9999"})
    check(st == 200, "factory login")
    st, j = fac.json("/api/cal", {"cmd": "begin", "ch": 0})
    check(st == 200, f"factory calibration started ({j})")
    time.sleep(0.1)
    st, cal = web.json("/api/cal")
    check(cal["active"] == 0, "calibration running on channel 0")
    st, j = fac.json("/api/cal", {"cmd": "accept"})
    check(st == 200, "accepted")
    time.sleep(0.1)
    st, cal = web.json("/api/cal")
    rec = cal["channels"][0]["in_force"] or {}
    check(rec.get("kind") == "factory" and rec.get("outcome") == "pass" and rec.get("level") == "factory"
          and rec.get("src") == "web" and rec.get("points") == [[50, 50, 50]] and rec.get("coef") == [0, 1],
          f"factory record in force ({rec})")
    check(cal["channels"][0]["factory"] == rec and len(cal["history"]) == 1, "factory record and history")
    check(cal["channels"][0]["due_time"] == rec.get("time", -1) + 90 * 86400, "due in 90 days")
    check(cal["channels"][0]["overdue"] is False, "not overdue")
    # ---------------- service history (CIP-22.2, 22.4, 23.4, 05.4) ----------------
    st, j = web.json("/api/service/log", {"tech": "Sam", "work": "Pump replaced"})
    check(st == 403, "service log needs factory login")
    st, j = fac.json("/api/service/log", {"tech": "Sam", "work": "Pump replaced"})
    check(st == 200, "service log entry")
    st, j = fac.json("/api/service/test", {"station": "EOL-1", "r": "1,1,1,2"})
    check(st == 200, "test record")
    time.sleep(0.1)
    st, sv = web.json("/api/service")
    check(st == 200 and sv["log"] and sv["log"][0]["tech"] == "Sam" and sv["log"][0]["level"] == "factory"
          and sv["test"]["passed"] is False and sv["test"]["results"] == [1, 1, 1, 2], f"service history ({sv})")
    st, info = web.json("/api/info")
    check(len(info["config_crc"]) == 8 and info["config_crc"] == sv["config_crc"] and info["board_rev"] == 0
          and info["build"], "config checksum, board revision, build on /api/info")
    for _ in range(120):                                # the start-up self-test closes 10 s after start
        st, info = web.json("/api/info")
        if info["selftest"]["done"]: break
        time.sleep(0.1)
    stt = {t["name"]: t["result"] for t in info["selftest"]["tests"]}
    check(info["selftest"]["done"] and list(stt)[:3] == ["image", "ram", "rtc"] and stt["ram"] == "pass"
          and stt["image"] == "skipped" and stt["settings"] == "pass" and stt["log_flash"] == "pass",
          f"start-up self-test list on /api/info ({info['selftest']})")
    fac.req("/api/logout", {})
    st, j = web.json("/api/password", {"level": "admin", "password": "Cems-2026"})
    check(st == 200, "admin password changed")

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
    check(any(e["event"] == "alarm" and e["detail"] == "DEMO_FAULT" and e["arg"] == 100 for e in ev), "alarm raise logged")
    check(any(e["event"] == "alarm_ack" and e["detail"] == "DEMO_FAULT" and e["source"] == "modbus" for e in ev),
          "Modbus acknowledge logged with its source")
    check(any(e["event"] == "alarm_ack" and e["detail"] == "DEMO_LEVEL_RANGE" and e["source"] == "web" and e["level"] == "admin"
              for e in ev), "web acknowledge logged with the login")
    check(sum(1 for e in ev if e["event"] == "param" and e["arg"] == 1006) == 1, "a multi-coil write is one block event")
    st, body, hdr = web.req("/api/log/events.csv", csrf=False)
    lines = body.decode().splitlines()
    check(st == 200 and hdr.get("Content-Type", "").startswith("text/csv") and lines[0].startswith("seq,time,event")
          and len(lines) == len(ev) + 1, f"event CSV ({len(lines) - 1} rows)")
    st, j = web.json("/api/log/info")
    check(st == 200 and j["status"] == 0 and j["events"]["used"] == len(ev), "log info")
    n_events = len([e for e in ev if not e["event"].startswith(("alarm", "state"))])   # the platform's own alarms and the state they force come and go

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
    st, cal = web.json("/api/cal")
    check((cal["channels"][0]["factory"] or {}).get("outcome") == "pass", "calibration record survived the restart")
    st, sv = web.json("/api/service")
    check(sv["log"] and sv["log"][0]["work"] == "Pump replaced" and sv["test"]["station"] == "EOL-1",
          "service history survived the restart")
    st, j = web.json("/api/log/events?n=200")
    allev = j["events"] if st == 200 else []
    ev = [e for e in allev if not e["event"].startswith(("alarm", "state"))]
    check(len(ev) == n_events + 3 and [e["event"] for e in ev[:3]] == ["login", "login_fail", "power_up"],
          "event log continues across the restart")
    check([e["seq"] for e in allev] == list(range(len(allev), 0, -1)), "sequence numbers contiguous")
    st, j = web.json("/api/log/info")
    check(st == 200 and j["status"] == 0, "write position from FRAM (not rebuilt)")
    log = unit.stop()
    print(log.strip().splitlines()[-1])

    print(f"\n{passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
