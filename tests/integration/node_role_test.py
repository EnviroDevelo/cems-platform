#!/usr/bin/env python3
"""One source, both roles: build/netsim (the CORE, its I/O-board master on real sockets) against
build/nodesim (the NODE role: platform/net/plat_node.c and the node core node/node.c, the code the
Nucleo-H563ZI runs). Checks identification, discovery, inputs, an output driven by the CORE's
application, and a lost board, through the CORE's web API.
    make build/netsim build/nodesim && python3 tests/integration/node_role_test.py"""
import http.cookiejar, json, os, ssl, subprocess, sys, tempfile, threading, time, urllib.parse, urllib.request
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MB, WEB, IOB, DISC = 15914, 18914, 15503, 15224
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


class Node:
    """build/nodesim with its console; keeps every line it prints"""
    def __init__(self):
        self.p = subprocess.Popen([os.path.join(ROOT, "build", "nodesim"), str(IOB), str(DISC)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                                  env=dict(os.environ, CEMS_NODE_DISC_TO="127.0.0.1"))
        self.lines = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip())

    def say(self, line):
        self.p.stdin.write(line + "\n"); self.p.stdin.flush()

    def saw(self, text):
        return any(text in l for l in self.lines)

    def stop(self):
        if self.p.poll() is None:
            try:
                self.say("quit")
            except OSError:
                pass
            try:
                self.p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.p.kill()


def main():
    tmp = tempfile.mkdtemp()
    node = Node()
    env = dict(os.environ, CEMS_IOB_TCP="1", CEMS_IOB_TCP_PORT=str(IOB), CEMS_IOB_DISC_PORT=str(DISC),
               CEMS_IOB_DISC_TO="127.0.0.1", CEMS_IOB_TCP_HOST="127.0.0.1")
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, text=True, env=env)
    say = lambda line: (unit.stdin.write(line + "\n"), unit.stdin.flush())
    try:
        check(wait(lambda: get("/api/info")), "CORE up")
        check(wait(lambda: node.saw("Modbus TCP on port")), "NODE up")
        say("set 102 1")                                   # iob_link: Ethernet on the main port
        say("set 112 1")                                   # board 1's outputs from channel 1: the demo's valve (DO0) is its output 1
        say("set 110 1")                                   # iob1_enable; iob1_ip stays 192.168.1.61, the node's default
        check(wait(lambda: board(1)["state"] == "online", 8), "board 1 online: the CORE talks to the NODE")
        b = board(1)
        check(b["fw"] == "0.1" and b["errors"] == 0, f"identification read (node firmware 0.1), no failed requests ({b['fw']}, {b['errors']})")

        node.say("in 0x05")
        check(wait(lambda: board(1)["inputs"] & 5 == 5), "NODE inputs 1 and 3 seen by the CORE")
        node.say("in 0")
        check(wait(lambda: board(1)["inputs"] & 5 == 0), "...and off again")

        # round trip through the CORE: the NODE's input 1 is the demo's start input (DI0), and after the
        # start delay its valve (DO0) is the NODE's output 1
        node.lines.clear()
        node.say("in 1")
        check(wait(lambda: node.saw("outputs on 0x0001"), 10), "NODE input 1 -> the CORE's application -> NODE output 1 on")
        node.say("in 0")
        check(wait(lambda: node.saw("outputs all off"), 5), "...and off again")
        check(post("/api/login", {"level": "admin", "password": "2222"}) == 200, "admin login")
        check(post("/api/iob/net/find", {}) in (200, 202), "Find boards")
        found = wait(lambda: get("/api/iob/net")["ok"] and get("/api/iob/net")["boards"], 6)
        check(found and found[0]["ip"] == "192.168.1.61" and found[0]["hw"] == 90 and found[0]["controlled"]
              and found[0]["serial"] == "",
              f"discovery: the NODE answers FIND (bench board, 192.168.1.61, under control) {found}")
        node.say("show")
        check(wait(lambda: node.saw("address 1, ip 192.168.1.61")), "NODE reports address 1 at 192.168.1.61")

        node.stop()
        check(wait(lambda: board(1)["state"] == "not responding"), "board not responding when the NODE stops")
        check(wait(lambda: alarm("IOB_LOST"), 3), "IOB_LOST alarm raised")
        node = Node()
        check(wait(lambda: board(1)["state"] == "online", 10), "board back online after the NODE restarts")
    finally:
        unit.terminate(); unit.wait(timeout=10)
        node.stop()
    print(f"\n{passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
