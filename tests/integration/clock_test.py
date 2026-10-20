#!/usr/bin/env python3
"""Setting the clock end to end: build/netsim, the web API, and SNTP against a small
time server written here (UDP; it can answer early or late, unsynchronised, or not at all).

    python3 tests/integration/clock_test.py"""
import http.cookiejar, json, os, socket, ssl, struct, subprocess, sys, tempfile, threading, time
import urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MB, WEB, NTP = 15702, 18702, 12123
BASE = f"https://127.0.0.1:{WEB}"
CTX = ssl._create_unverified_context()
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1; print("  ok  ", what)
    else:
        failed += 1; print("  FAIL:", what)


class Ntp:
    """offset_s: how far this server's UTC is from the PC's; stratum 0 = kiss-o'-death; mute = no answer"""
    def __init__(self):
        self.offset_s, self.stratum, self.mute, self.queries = 0, 2, False, 0
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); self.s.bind(("127.0.0.1", NTP))
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            data, addr = self.s.recvfrom(512)
            self.queries += 1
            if self.mute or len(data) < 48 or data[0] & 7 != 3:
                continue
            t = time.time() + self.offset_s + 2208988800
            sec, frac = int(t), int((t % 1) * 2**32)
            p = bytearray(48)
            p[0] = (0 << 6) | (4 << 3) | 4; p[1] = self.stratum; p[2] = 6; p[3] = 0xEC
            p[24:32] = data[40:48]                                  # origin = the client's transmit
            p[32:40] = struct.pack(">II", sec, frac); p[40:48] = struct.pack(">II", sec, frac)
            self.s.sendto(bytes(p), addr)


class Web:
    def __init__(self):
        self.op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX),
                                              urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def req(self, path, form=None):
        data = urllib.parse.urlencode(form).encode() if form is not None else None
        h = {"X-CEMS": "1", "Content-Type": "application/x-www-form-urlencoded"} if data is not None else {}
        try:
            with self.op.open(urllib.request.Request(BASE + path, data=data, headers=h), timeout=10) as f:
                return f.status, json.loads(f.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")


def local_unix(off=0):
    """the PC's local wall clock as seconds since 1970 (what the instrument's clock holds)"""
    t = time.time() + off
    return int(t + time.localtime(t).tm_gmtoff)


def wait(cond, t=10):
    end = time.time() + t
    while time.time() < end:
        try:
            r = cond()
            if r:
                return r
        except OSError:
            pass
        time.sleep(0.2)
    return None


def main():
    tmp = tempfile.mkdtemp()
    env = dict(os.environ, CEMS_NTP_PORT=str(NTP))
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
    try:
        wait(lambda: Web().req("/api/info")[0] == 200)
        ntp = Ntp()
        w = Web()
        st, t = w.req("/api/time")
        check(st == 200 and t["set"] and abs(t["unix"] - local_unix()) <= 2 and not t["sntp"]["on"], f"clock reported ({t})")
        st, _ = w.req("/api/time", {"t": local_unix(3600)})
        check(st == 401, "setting the clock needs a login")
        w.req("/api/login", {"level": "operator", "password": "1111"})
        st, _ = w.req("/api/time", {"t": local_unix(3600)})
        check(st == 403, "... at admin level")
        w.req("/api/login", {"level": "admin", "password": "2222"})
        st, _ = w.req("/api/time", {"t": 1000})
        check(st == 400, "a time before 2020 refused")
        st, _ = w.req("/api/time", {"t": local_unix(3600)})
        t = wait(lambda: (lambda x: x if abs(x["unix"] - local_unix(3600)) <= 2 else None)(w.req("/api/time")[1]))
        check(st == 200 and t, "set one hour ahead from the web")

        print("SNTP")
        st, _ = w.req("/api/time", {"sync": 1})
        check(st == 409, "sync refused with no time server")
        off = time.localtime().tm_gmtoff // 60
        for pid, v in ((82, off), (84, 1), (83, "127.0.0.1")):
            w.req("/api/write", {"id": pid, "v": v})
        t = wait(lambda: (lambda x: x if x["sntp"]["last_ok_s_ago"] is not None and abs(x["unix"] - local_unix()) <= 2 else None)(w.req("/api/time")[1]), 15)
        check(t and abs(t["sntp"]["diff_s"] + 3600) <= 2 and t["sntp"]["steps"] == 1 and t["sntp"]["stratum"] == 2,
              f"time server found the clock an hour fast and set it ({t and t['sntp']})")
        steps = t["sntp"]["steps"] if t else 0
        w.req("/api/time", {"sync": 1}); time.sleep(1.5)
        st, t = w.req("/api/time")
        check(t["sntp"]["steps"] == steps and abs(t["sntp"]["diff_s"]) <= 1, "in step: no correction (no log noise)")
        ntp.stratum = 0
        w.req("/api/time", {"sync": 1}); time.sleep(1.5)
        st, t = w.req("/api/time")
        check("not synchronised" in t["sntp"]["error"], f"an unsynchronised server is ignored ({t['sntp']['error']})")
        ntp.stratum, ntp.mute = 2, True
        w.req("/api/time", {"sync": 1}); time.sleep(2.5)
        st, t = w.req("/api/time")
        check(t["sntp"]["error"] == "no answer from the time server", "no answer reported")
        ntp.mute, ntp.offset_s = False, -30
        w.req("/api/time", {"sync": 1})
        t = wait(lambda: (lambda x: x if x["sntp"]["error"] == "" and abs(x["unix"] - local_unix(-30)) <= 2 else None)(w.req("/api/time")[1]))
        check(t is not None, "follows the server 30 s back")
        st, ev = w.req("/api/log/events?n=40")
        ts = [e for e in ev["events"] if e["event"] == "time_set"]
        check(any(e["source"] == "web" and e["level"] == "admin" and e["detail"] == "web" for e in ts), "web setting logged with the login")
        check(sum(1 for e in ts if e["detail"] == "sntp") == 2, f"two SNTP steps logged ({[e['detail'] for e in ts]})")
        check(len(ts) == 3, "no step logged twice")
    finally:
        unit.terminate()
        try:
            unit.wait(timeout=10)
        except subprocess.TimeoutExpired:
            unit.kill()
    print(f"\n{passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
