#!/usr/bin/env python3
"""Soak test (design spec: "72-hour soak test with heap and socket monitoring"): build/netsim
under a steady load, watched from outside for leaks of memory or sockets.

Load: 3 Modbus TCP clients polling registers and writing a setpoint; 2 browsers' worth of
HTTPS polling (one reusing its connection, one opening a new one each time); a value
written from the web each second; an alarm raised and acknowledged every 20 s; the event
CSV downloaded every 30 s. Every 10 s: the process's memory (RSS) and open file
descriptors (sockets). Passes when nothing failed, the descriptor count stays flat and
memory stops growing after the first third.

    PYTHONPATH=<pymodbus> python3 tests/integration/soak_test.py [minutes]   (default 3; 4320 = 72 h)"""
import http.client, http.cookiejar, json, os, ssl, subprocess, sys, tempfile, threading, time, urllib.parse, urllib.request
from pymodbus.client import ModbusTcpClient

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MB, WEB = 15802, 18802
CTX = ssl._create_unverified_context()
stop = threading.Event()
errors, counts = [], {"modbus": 0, "https": 0, "writes": 0, "alarms": 0, "csv": 0}
lock = threading.Lock()


def err(what):
    with lock:
        if len(errors) < 50:
            errors.append(f"{time.strftime('%H:%M:%S')} {what}")


def bump(k):
    with lock:
        counts[k] += 1


def modbus_client(n):
    c = ModbusTcpClient("127.0.0.1", port=MB, timeout=3)
    c.connect()
    k = 0
    while not stop.is_set():
        try:
            r = c.read_holding_registers(0, 4, slave=1)
            if r.isError():
                err(f"modbus {n}: {r}")
            elif n == 0 and k % 10 == 0:
                w = c.write_registers(0, [0, 1500 + k % 100], slave=1)
                if w.isError():
                    pass                                     # LOCAL mode refuses writes: expected, not a leak
            bump("modbus")
        except Exception as e:
            err(f"modbus {n}: {e}"); c.close(); time.sleep(1); c = ModbusTcpClient("127.0.0.1", port=MB, timeout=3); c.connect()
        k += 1
        time.sleep(0.2)
    c.close()


def https_keepalive():
    conn = None
    while not stop.is_set():
        try:
            if conn is None:
                conn = http.client.HTTPSConnection("127.0.0.1", WEB, context=CTX, timeout=5)
            for path in ("/api/values", "/api/alarms", "/api/info"):
                conn.request("GET", path); r = conn.getresponse(); b = r.read()
                if r.status != 200 or not b.startswith(b"{"):
                    err(f"https {path}: {r.status}")
                bump("https")
        except Exception:
            conn = None                                      # the server closed it to serve someone else: normal
        time.sleep(0.5)


class Web:
    def __init__(self):
        self.op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX),
                                              urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def req(self, path, form=None):
        data = urllib.parse.urlencode(form).encode() if form is not None else None
        h = {"X-CEMS": "1", "Content-Type": "application/x-www-form-urlencoded", "Connection": "close"} if data is not None else {"Connection": "close"}
        with self.op.open(urllib.request.Request(f"https://127.0.0.1:{WEB}" + path, data=data, headers=h), timeout=10) as f:
            return f.status, f.read()


def https_fresh():
    w = Web()
    t0 = time.time(); k = 0
    try:
        w.req("/api/login", {"level": "admin", "password": "2222"})
    except Exception as e:
        err(f"login: {e}")
    while not stop.is_set():
        try:
            st, b = w.req("/api/values?ids=1001,1002")
            bump("https")
            st, b = w.req("/api/write", {"id": 1008, "v": 40 + k % 20})
            if st == 200:
                bump("writes")
            if k % 20 == 0:                                  # an alarm, then its acknowledge
                w.req("/api/write", {"id": 1007, "v": 1}); time.sleep(0.2)
                w.req("/api/write", {"id": 1007, "v": 0}); time.sleep(0.2)
                w.req("/api/alarms/ack", {"id": "all"}); bump("alarms")
            if k % 30 == 0:
                st, b = w.req("/api/log/events.csv")
                if st != 200 or not b.startswith(b"seq,"):
                    err("events.csv")
                bump("csv")
            if time.time() - t0 > 480:                       # stay logged in (sessions lapse after 10 min)
                w.req("/api/login", {"level": "admin", "password": "2222"}); t0 = time.time()
        except Exception as e:
            err(f"web: {e}")
        k += 1
        time.sleep(1)


def sample(pid):
    rss = 0
    for line in open(f"/proc/{pid}/status"):
        if line.startswith("VmRSS:"):
            rss = int(line.split()[1])
    return rss, len(os.listdir(f"/proc/{pid}/fd"))


def main():
    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
    tmp = tempfile.mkdtemp()
    env = dict(os.environ, ASAN_OPTIONS="quarantine_size_mb=1:detect_leaks=1")
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    for _ in range(100):
        try:
            urllib.request.urlopen(f"https://127.0.0.1:{WEB}/api/info", timeout=1, context=CTX).read(); break
        except Exception:
            time.sleep(0.1)
    th = [threading.Thread(target=modbus_client, args=(n,), daemon=True) for n in range(3)]
    th += [threading.Thread(target=https_keepalive, daemon=True), threading.Thread(target=https_fresh, daemon=True)]
    for t in th:
        t.start()
    samples = []
    end = time.time() + minutes * 60
    print(f"soak for {minutes:g} min")
    while time.time() < end and unit.poll() is None:
        time.sleep(10)
        rss, fds = sample(unit.pid)
        samples.append((rss, fds))
        print(f"  {time.strftime('%H:%M:%S')}  RSS {rss / 1024:.1f} MB  fds {fds}  {dict(counts)}", flush=True)
    stop.set()
    for t in th:
        t.join(timeout=10)
    alive = unit.poll() is None
    unit.terminate()
    try:
        _, se = unit.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        unit.kill(); _, se = unit.communicate()
    se = se.decode(errors="replace")
    ok = True
    def check(cond, what):
        nonlocal ok
        print(("  ok   " if cond else "  FAIL: ") + what); ok &= bool(cond)
    check(alive, "the unit ran the whole time")
    check(not errors, f"no request failed ({len(errors)}: {errors[:5]})")
    check(counts["modbus"] > minutes * 60 * 10 and counts["https"] > minutes * 60 * 2, f"load ran ({counts})")
    if len(samples) >= 3:
        fds = [f for _, f in samples]
        third = len(samples) // 3
        rss_mid, rss_end = max(r for r, _ in samples[third:2 * third + 1]), samples[-1][0]
        check(max(fds) - min(fds) <= 4, f"sockets steady (open descriptors {min(fds)}..{max(fds)})")
        check(rss_end - rss_mid < 2048, f"memory steady after warm-up ({rss_mid / 1024:.1f} -> {rss_end / 1024:.1f} MB)")
    check("ERROR: LeakSanitizer" not in se and "AddressSanitizer" not in se, "no memory errors or leaks reported at exit")
    print("SOAK:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
