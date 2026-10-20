#!/usr/bin/env python3
"""Serial number changes end to end (decision D25): build/netsim with a developer key,
tokens made by tools/serial_token.py, the web API and the event log.
    python3 tests/integration/serial_test.py"""
import http.cookiejar, json, os, ssl, subprocess, sys, tempfile, time, urllib.parse, urllib.request
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fwkey, serial_token
MB, WEB = 15902, 18902
BASE = f"https://127.0.0.1:{WEB}"
CTX = ssl._create_unverified_context()
UID = "0a1b2c3d4e5f60718293a4b5"
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1; print("  ok  ", what)
    else:
        failed += 1; print("  FAIL:", what)


class Web:
    def __init__(self):
        self.op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX),
                                              urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def req(self, path, form=None):
        data = urllib.parse.urlencode(form).encode() if form is not None else None
        h = {"X-CEMS": "1", "Content-Type": "application/x-www-form-urlencoded", "Connection": "close"} if data is not None else {"Connection": "close"}
        try:
            with self.op.open(urllib.request.Request(BASE + path, data=data, headers=h), timeout=10) as f:
                return f.status, json.loads(f.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")


def token(key, old, new, uid=UID):
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "serial_token.py"), "--key", key, "--uid", uid,
                          "--old", str(old), "--new", str(new)], capture_output=True, text=True, check=True)
    return out.stdout.strip()


def run_unit(tmp, env):
    return subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, env=env)


def up():
    for _ in range(100):
        try:
            if Web().req("/api/info")[0] == 200:
                return True
        except OSError:
            time.sleep(0.1)
    return False


def stop(unit):
    unit.terminate()
    try:
        unit.wait(timeout=10)
    except subprocess.TimeoutExpired:
        unit.kill(); unit.wait()


def main():
    tmp = tempfile.mkdtemp()
    dev, other = os.path.join(tmp, "dev.pem"), os.path.join(tmp, "other.pem")
    with open(os.devnull, "w") as dn:
        so = sys.stdout; sys.stdout = dn
        fwkey.new_key(dev); fwkey.new_key(other)
        sys.stdout = so
    pub = fwkey.public_point(dev).hex()

    print("firmware without a developer key")
    unit = run_unit(tmp, dict(os.environ, CEMS_UID=UID))
    try:
        up(); w = Web()
        st, j = w.req("/api/serial")
        check(st == 200 and j["uid"] == UID and j["serial"] == 0 and not j["key"], f"chip ID and serial shown, no key ({j})")
        w.req("/api/login", {"level": "factory", "password": "9999"})
        st, j = w.req("/api/serial", {"serial": 5, "sig": token(dev, 0, 5)})
        check(st == 409 and "no developer key" in j["error"], "cannot be changed at all")
        st, _ = w.req("/api/write", {"id": 50, "v": 5})
        check(st == 403, "nor through the parameter write, even at factory level")
    finally:
        stop(unit)

    print("firmware with the developer key")
    os.remove(os.path.join(tmp, "f.bin"))
    unit = run_unit(tmp, dict(os.environ, CEMS_UID=UID, CEMS_DEV_PUBKEY=pub))
    try:
        up(); w = Web()
        st, j = w.req("/api/serial?new=240117")
        check(j["key"] and j["message"] == f"CEMS-SERIAL v1 uid={UID} old=0 new=240117", "the text to sign")
        tok = token(dev, 0, 240117)
        check(serial_token.verify(serial_token.message(UID, 0, 240117), bytes.fromhex(tok), pub), "the tool's --check agrees")
        st, _ = w.req("/api/serial", {"serial": 240117, "sig": tok})
        check(st == 401, "needs a login")
        w.req("/api/login", {"level": "admin", "password": "2222"})
        st, _ = w.req("/api/serial", {"serial": 240117, "sig": tok})
        check(st == 403, "admin is not enough")
        w.req("/api/login", {"level": "factory", "password": "9999"})
        st, _ = w.req("/api/serial", {"serial": 240117, "sig": token(other, 0, 240117)})
        check(st == 403, "a token from another key is refused")
        st, _ = w.req("/api/serial", {"serial": 240117, "sig": token(dev, 0, 240117, uid="ff" * 12)})
        check(st == 403, "a token for another chip is refused")
        st, _ = w.req("/api/serial", {"serial": 240117, "sig": tok})
        check(st == 200, "the developer's token is accepted")
        time.sleep(0.5)
        st, j = w.req("/api/serial")
        check(j["serial"] == 240117, "serial changed")
        st, i = w.req("/api/info")
        check(i["serial"] == 240117, "and shown in the unit's info")
        st, _ = w.req("/api/serial", {"serial": 7, "sig": token(dev, 0, 7)})
        check(st == 403, "an old token (for the previous serial) no longer works")
        time.sleep(1.5)
        st, ev = w.req("/api/log/events?n=40")
        se = [e for e in ev["events"] if e["event"] == "serial"]
        check(any(e["level"] == "factory" and e["detail"] == "developer token" and e["old"] == 0 and e["new"] == 240117 for e in se),
              f"the change is logged with the login ({[e['detail'] for e in se]})")
        check(sum(1 for e in se if e["detail"] == "token refused") >= 3, "refused tokens are logged too")
    finally:
        stop(unit)

    print("after a restart")
    unit = run_unit(tmp, dict(os.environ, CEMS_UID=UID, CEMS_DEV_PUBKEY=pub))
    try:
        up()
        st, j = Web().req("/api/serial")
        check(j["serial"] == 240117, "the serial is kept")
    finally:
        stop(unit)
    print(f"\n{passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
