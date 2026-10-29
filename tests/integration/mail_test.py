#!/usr/bin/env python3
"""Alarm email end to end: build/netsim against a small SMTP server written here
(plain, STARTTLS and implicit TLS; RSA and ECDSA certificates made with openssl).

Checks: configuration over HTTPS, the test email, an alarm routed to email (and its
clear), batching, AUTH PLAIN over TLS, login refused without TLS, the certificate
fingerprint and pinning (a changed certificate is refused), retries then NOTIFY_FAIL,
the password never readable (web, backup file), the per-alarm action switch.

    python3 tests/integration/mail_test.py"""
import base64, hashlib, http.cookiejar, json, os, socket, ssl, subprocess, sys, tempfile, threading, time
import urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MB, WEB = 15602, 18602
BASE = f"https://127.0.0.1:{WEB}"
CTX = ssl._create_unverified_context()
TMP = tempfile.mkdtemp()
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1; print("  ok  ", what)
    else:
        failed += 1; print("  FAIL:", what)


def make_cert(name, kind):
    key, crt = os.path.join(TMP, name + ".key"), os.path.join(TMP, name + ".crt")
    alg = ["-newkey", "rsa:" + os.environ.get("RSA_BITS", "2048")] if kind == "rsa" else ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"]
    subprocess.run(["openssl", "req", "-x509", "-nodes", *alg, "-keyout", key, "-out", crt, "-days", "30",
                    "-subj", "/CN=mail.site.local", "-sha384"], check=True, capture_output=True)
    der = ssl.PEM_cert_to_DER_cert(open(crt).read())
    fp = ":".join(f"{b:02X}" for b in hashlib.sha256(der).digest())
    return key, crt, fp


class Smtp:
    """mode: plain | starttls | tls. Records every message; can refuse logins."""
    def __init__(self, port, mode, cert=None, user="cems", password="Secr3t pw"):
        self.mode, self.cert, self.user, self.password = mode, cert, user, password
        self.msgs, self.auths, self.sessions = [], [], 0
        self.ls = socket.socket(); self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind(("127.0.0.1", port)); self.ls.listen(5)
        threading.Thread(target=self.run, daemon=True).start()

    def ctx(self):
        c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain(self.cert[1], self.cert[0]); return c

    def run(self):
        while True:
            try:
                c, _ = self.ls.accept()
            except OSError:
                return
            threading.Thread(target=self.session, args=(c,), daemon=True).start()

    def session(self, c):
        self.sessions += 1
        try:
            if self.mode == "tls":
                c = self.ctx().wrap_socket(c, server_side=True)
            f = c.makefile("rb")
            def send(s): c.sendall(s.encode() + b"\r\n")
            send("220 mail.site.local ESMTP test")
            secure = self.mode == "tls"
            mail = {"rcpt": []}
            while True:
                ln = f.readline()
                if not ln:
                    return
                ln = ln.decode().rstrip("\r\n")
                cmd = ln.split(" ")[0].upper()
                if cmd == "EHLO":
                    caps = ["250-mail.site.local"]
                    if self.mode == "starttls" and not secure:
                        caps.append("250-STARTTLS")
                    if secure:
                        caps.append("250-AUTH LOGIN PLAIN")
                    caps.append("250 8BITMIME")
                    c.sendall(("\r\n".join(caps) + "\r\n").encode())
                elif cmd == "STARTTLS":
                    send("220 go ahead")
                    c = self.ctx().wrap_socket(c, server_side=True); f = c.makefile("rb"); secure = True
                elif cmd == "AUTH":
                    parts = ln.split(" ")
                    if parts[1].upper() == "PLAIN":
                        _, u, p = base64.b64decode(parts[2]).decode().split("\0")
                        self.auths.append((u, p))
                        send("235 ok" if (u, p) == (self.user, self.password) else "535 5.7.8 Authentication credentials invalid")
                    else:
                        send("504 not here")
                elif cmd == "MAIL":
                    mail = {"from": ln[10:].strip("<>"), "rcpt": []}; send("250 ok")
                elif cmd == "RCPT":
                    mail["rcpt"].append(ln[8:].strip("<>")); send("250 ok")
                elif cmd == "DATA":
                    send("354 end with .")
                    body = []
                    while True:
                        l2 = f.readline().decode("utf-8")
                        if l2 in (".\r\n", ""):
                            break
                        body.append(l2[1:] if l2.startswith("..") else l2)
                    mail["data"] = "".join(body); mail["secure"] = secure
                    self.msgs.append(mail); send("250 queued")
                elif cmd == "QUIT":
                    send("221 bye"); return
                else:
                    send("502 ?")
        except Exception as e:
            print("   smtp server:", e)
        finally:
            try: c.close()
            except Exception: pass

    def close(self):
        self.ls.close()


class Web:
    def __init__(self):
        self.op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX),
                                              urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def req(self, path, form=None):
        data = urllib.parse.urlencode(form).encode() if form is not None else None
        h = {"X-CEMS": "1", "Content-Type": "application/x-www-form-urlencoded"} if data is not None else {}
        try:
            with self.op.open(urllib.request.Request(BASE + path, data=data, headers=h), timeout=30) as f:
                body = f.read()
                return f.status, json.loads(body) if body[:1] in (b"{", b"[") else body
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def set(self, pid, v, o=None):
        f = [("id", pid)] + ([("o", o)] if o is not None else []) + [("v", v)]
        st, j = self.req("/api/write", f)
        if st != 200:
            print("   write", pid, v, st, j)
        return st


def wait(cond, t=15):
    end = time.time() + t
    while time.time() < end:
        try:
            r = cond()
        except OSError:
            r = None
        if r:
            return r
        time.sleep(0.2)
    return None


def mail_status(w):
    return w.req("/api/mail")[1]


def main():
    rsa = make_cert("rsa", "rsa")
    ecc = make_cert("ecc", "ecc")
    env = dict(os.environ, CEMS_MAIL_RETRY_MS="1000")
    unit = subprocess.Popen([os.path.join(ROOT, "build", "netsim"), str(MB), str(WEB), os.path.join(TMP, "fram.bin"),
                             os.path.join(TMP, "nor.bin")], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
    wait(lambda: Web().req("/api/info")[0] == 200, 10)
    w = Web()
    check(w.req("/api/login", {"level": "admin", "password": "2222"})[0] == 200, "admin login")
    st, j = w.req("/api/mail/test", {})
    check(st == 409, "test refused while mail is off")

    print("plain SMTP, test email")
    plain = Smtp(12525, "plain")
    for pid, v in ((81, "Stack A"), (71, "2130706433"), (72, 12525), (73, 0), (77, "cems@site.local"),
                   (78, "ops@site.local, shift@site.local"), (80, 1), (70, 1)):
        w.set(pid, v)
    time.sleep(0.1)
    st, j = w.req("/api/mail/test", {})
    check(st == 200, "test requested")
    m = wait(lambda: plain.msgs)
    check(m and "Subject: [Stack A] Test email" in m[0]["data"] and m[0]["rcpt"] == ["ops@site.local", "shift@site.local"]
          and m[0]["from"] == "cems@site.local", "test email received by both recipients")
    s = wait(lambda: (lambda x: x if x["sent"] >= 1 else None)(mail_status(w)))
    check(s and s["last_ok"] and s["server_fp"] is None, "status: sent, no certificate on plain SMTP")

    print("alarm routed to email")
    w.set(46, 0x81, o=100)                         # DEMO_FAULT: email only
    w.set(1007, 1)
    m = wait(lambda: len(plain.msgs) >= 2 and plain.msgs)
    d = m[1]["data"] if m else ""
    check("Subject: [Stack A] ALARM: Demo fault" in d and "RAISED" in d and "DEMO_FAULT 100" in d,
          "alarm email: subject and line")
    check("waiting for an acknowledge" in d, "email lists what is active now")
    open(os.path.join(ROOT, "build", "sample_alarm_email.txt"), "w").write(d)
    w.set(1007, 0)
    w.req("/api/alarms/ack", {"id": 100})          # latched: clears on acknowledge
    m = wait(lambda: len(plain.msgs) >= 3 and plain.msgs)
    check(m and "Cleared: Demo fault" in m[2]["data"], "clear emailed (mail_on_clear)")
    w.set(80, 5); w.set(46, 0x81, o=101); w.set(1008, 95); time.sleep(1.5); w.set(1007, 1)   # two alarms close together
    m = wait(lambda: len(plain.msgs) >= 4 and plain.msgs, 20)
    d = m[3]["data"] if m else ""
    check("DEMO_LEVEL_RANGE 101" in d and "DEMO_FAULT 100" in d and "(and more)" in d, "a burst is one email")
    w.set(1007, 0); w.set(1008, 50); w.req("/api/alarms/ack", {"id": "all"}); w.set(80, 1)
    time.sleep(3)

    print("login: refused without TLS")
    w.set(75, "cems"); w.set(76, "Secr3t pw")
    w.req("/api/mail/test", {})
    s = wait(lambda: (lambda x: x if x["failed"] >= 1 else None)(mail_status(w)))
    check(s and s["last"] == "login needs TLS", f"password never sent in clear ({s and s['last']})")
    plain.close()

    print("STARTTLS with an RSA certificate, AUTH PLAIN")
    stls = Smtp(12587, "starttls", rsa)
    w.set(72, 12587); w.set(73, 1)
    n0 = mail_status(w)["sent"]
    w.req("/api/mail/test", {})
    s = wait(lambda: (lambda x: x if x["sent"] > n0 else None)(mail_status(w)))
    print("   status:", s or mail_status(w))
    check(stls.msgs and stls.msgs[0]["secure"] and stls.auths == [("cems", "Secr3t pw")], "sent over STARTTLS, logged in")
    check(s and s["server_fp"] == rsa[2] and s["pinned"] is None, "server fingerprint reported, nothing pinned yet")
    st, j = w.req("/api/mail/trust", {"trust": 1}); time.sleep(0.2)
    check(st == 200 and mail_status(w)["pinned"] == rsa[2], "trust: the server's certificate is pinned")
    stls.close()

    print("the server's certificate changes: refused, retried, NOTIFY_FAIL")
    evil = Smtp(12588, "starttls", ecc)
    w.set(72, 12588); w.set(46, 0x81, o=100); w.set(1007, 1)
    s = wait(lambda: (lambda x: x if x["dropped"] >= 1 else None)(mail_status(w)), 30)
    check(s and s["last"] == "server certificate does not match" and not evil.msgs and not evil.auths,
          "pinned: a different certificate is refused before logging in")
    st, al = w.req("/api/alarms")
    check(any(a["id"] == 10 and a["active"] for a in al["alarms"]), "after three tries: NOTIFY_FAIL alarm")
    evil.close()

    print("implicit TLS (465) with an ECDSA certificate")
    tls = Smtp(12465, "tls", ecc)
    w.req("/api/mail/trust", {"trust": 0}); w.set(72, 12465); w.set(73, 2)
    n0 = mail_status(w)["sent"]
    w.req("/api/mail/test", {})
    s = wait(lambda: (lambda x: x if x["sent"] > n0 else None)(mail_status(w)))
    check(tls.msgs and tls.msgs[0]["secure"] and s and s["server_fp"] == ecc[2], "sent over TLS (ECDSA)")
    st, al = w.req("/api/alarms")
    check(not any(a["id"] == 10 and a["active"] for a in al["alarms"]), "NOTIFY_FAIL clears on success")
    w.set(1007, 0); w.req("/api/alarms/ack", {"id": "all"})

    print("the password stays secret")
    st, vals = w.req("/api/values?ids=75,76")
    check(vals["v"]["75"] == "cems" and vals["v"]["76"] is None, "password not readable on the web")
    st, params = w.req("/api/params")
    check(any(p["id"] == 76 and "secret" in p["flags"] for p in params), "marked secret")
    time.sleep(1.5)
    st, backup = w.req("/api/backup")
    check(isinstance(backup, bytes) and backup[:4] == b"CEMX" and b"Secr3t" not in backup and b"cems@site.local" in backup,
          "backup file has the settings but not the password")
    st, _ = w.req("/api/restore", None) if False else (200, None)
    print("route off: no email")
    w.set(46, 0x80, o=100)
    n = len(tls.msgs)
    w.set(1007, 1); time.sleep(3)
    check(len(tls.msgs) == n, "actions set to 'nothing': no email")
    w.set(1007, 0); w.req("/api/alarms/ack", {"id": "all"})
    st, ev = w.req("/api/log/events?n=100")
    check(any(e["event"] == "mail" and e["new"] == 1 for e in ev["events"]) and
          any(e["event"] == "mail" and e["new"] == 0 for e in ev["events"]), "sends and failures in the event log")
    st, info = w.req("/api/info")
    print("   TLS pool peak:", info["tls"]["pool_peak"])
    check(info["tls"]["pool_peak"] < 80 * 1024, "client and server share the TLS pool without overflow")
    tls.close()
    unit.terminate(); unit.wait(timeout=10)
    print(f"\n{passed} checks passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
