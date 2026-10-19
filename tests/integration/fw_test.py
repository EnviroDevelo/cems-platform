#!/usr/bin/env python3
"""Firmware update end to end against build/netsim: an image made by
tools/mkfwimage.py is uploaded over HTTP, activated (netsim swaps its file-backed
banks and exits as if reset), started again as the new version, confirmed after a
healthy trial; then a second update whose trial "crashes" is rolled back."""
import http.cookiejar, json, os, random, subprocess, sys, tempfile, time, urllib.request, urllib.error

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WEB, MB = 18097, 15097
BASE = f"http://127.0.0.1:{WEB}"
TMP = tempfile.mkdtemp()
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  FAIL:", what)


class Unit:
    def __init__(self, version, cause="power", trial_ms=3000):
        env = dict(os.environ, CEMS_FW_VERSION=version, CEMS_RESET_CAUSE=cause, CEMS_FW_TRIAL_MS=str(trial_ms))
        self.p = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB),
                                   os.path.join(TMP, "fram.bin"), os.path.join(TMP, "nor.bin")],
                                  stdin=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        for _ in range(100):
            if self.p.poll() is not None:
                return                                   # exited during start-up (rollback)
            try:
                urllib.request.urlopen(BASE + "/api/info", timeout=1).read(); return
            except Exception:
                time.sleep(0.05)

    def wait_exit(self, timeout=10):
        try:
            return self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def kill(self):
        self.p.kill(); self.p.wait()

    def stop(self):
        self.p.terminate(); self.p.wait(timeout=10)


class Web:
    def __init__(self):
        self.op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def req(self, path, data=None, ctype="application/x-www-form-urlencoded"):
        h = {"X-CEMS": "1", "Content-Type": ctype} if data is not None else {}
        try:
            with self.op.open(urllib.request.Request(BASE + path, data=data, headers=h), timeout=20) as f:
                return f.status, json.loads(f.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def login(self):
        return self.req("/api/login", b"level=admin&password=2222")[0]


def image(version, size, product=None):
    b = os.path.join(TMP, f"{version}.bin")
    with open(b, "wb") as f:
        f.write(random.randbytes(size))
    out = os.path.join(TMP, f"{version}.cfw")
    cmd = [sys.executable, os.path.join(ROOT, "tools", "mkfwimage.py"), b, out, "--version", version]
    if product:
        cmd += ["--product", hex(product)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return open(out, "rb").read()


def events(web):
    st, j = web.req("/api/log/events?n=50")
    return j.get("events", [])


def main():
    random.seed(3)
    print("update 0.3.0 -> 0.4.0")
    u = Unit("0.3.0")
    w = Web()
    st, j = w.req("/api/fw")
    check(st == 200 and j["running"] == "0.3.0" and j["state"] == "idle" and j["staged"] is None, f"status before ({j})")
    img = image("0.4.0", 300_000)
    st, j = w.req("/api/fw/upload", img, "application/octet-stream")
    check(st == 401, "upload needs a login")
    check(w.login() == 200, "admin login")
    t = time.time()
    st, j = w.req("/api/fw/upload", img, "application/octet-stream")
    check(st == 200 and j.get("staged") == "0.4.0", f"upload accepted ({st} {j}, {time.time() - t:.1f} s)")
    st, j = w.req("/api/fw")
    check(j["staged"] == "0.4.0" and j["received"] == len(img), "staged image reported")
    bad = bytearray(img); bad[5000] ^= 1
    st, j = w.req("/api/fw/upload", bytes(bad), "application/octet-stream")
    check(st == 400 and j.get("error") == "checksum mismatch", f"damaged file refused ({j})")
    st, j = w.req("/api/fw/upload", image("0.4.9", 1000, product=0x1234), "application/octet-stream")
    check(st == 400 and j.get("error") == "wrong product", "image for other hardware refused")
    st, j = w.req("/api/fw/activate", b"")
    check(st == 404, "nothing to activate after a refused upload (the free bank was overwritten)")
    check(w.req("/api/fw/upload", img, "application/octet-stream")[0] == 200, "upload again")
    st, j = w.req("/api/fw/activate", b"")
    check(st == 200 and j.get("restarting"), "activate")
    check(u.wait_exit() == 3, "unit swapped banks and restarted")

    u = Unit("0.4.0", "software")
    w = Web()
    st, j = w.req("/api/fw")
    check(j["running"] == "0.4.0" and j["state"] == "trial", f"new image in trial ({j})")
    time.sleep(5.5)
    st, j = w.req("/api/fw")
    check(j["state"] == "idle", f"confirmed after a healthy trial ({j})")
    ev = events(w)
    check(any(e["event"] == "fw_update" and e["detail"] == "0.3.0>0.4.0" for e in ev), "fw_update event 0.3.0>0.4.0")
    check(any(e["event"] == "fw_update" and e["detail"] == "activate" and e["level"] == "admin" for e in ev),
          "activation logged with the login level")

    print("update 0.4.0 -> 0.5.0, crashes in the trial, rolls back")
    check(w.login() == 200, "login")
    check(w.req("/api/fw/upload", image("0.5.0", 200_000), "application/octet-stream")[0] == 200, "upload 0.5.0")
    w.req("/api/fw/activate", b"")
    check(u.wait_exit() == 3, "restart for 0.5.0")
    u = Unit("0.5.0", "software", trial_ms=60000)
    st, j = Web().req("/api/fw")
    check(j["state"] == "trial", "0.5.0 in trial")
    u.kill()                                             # it "hangs": the watchdog resets it
    u = Unit("0.5.0", "iwdg")
    check(u.wait_exit() == 3, "watchdog reset in the trial -> swapped back")
    u = Unit("0.4.0", "software")
    w = Web()
    st, j = w.req("/api/fw")
    check(j["running"] == "0.4.0" and j["state"] == "idle" and j["rolled_back"] is True, f"back on 0.4.0 ({j})")
    check(j["staged"] == "0.5.0", "the failed image stays in the free bank")
    check(any(e["event"] == "fw_rollback" for e in events(w)), "fw_rollback event")
    u.stop()
    print(f"\n{passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
