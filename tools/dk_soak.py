#!/usr/bin/env python3
"""Soak test against a real unit (the DK or a core board): steady read-only load from a laptop,
with the unit's own health sampled from /api/info. CIP-26.6 (the 72-hour soak on hardware).

    python tools/dk_soak.py 192.168.1.50 --minutes 60 --csv soak.csv
    python tools/dk_soak.py 192.168.1.50 --minutes 4320          (72 h)

Only Python 3's standard library. It never logs in and never writes: HTTPS GETs and Modbus
reads only, so it is safe on a unit with real settings.

Load: two browsers' worth of HTTPS polling on kept connections (values, alarms, info every
0.5 s), one client opening a new connection every 2 s, and two Modbus TCP clients reading the
platform block (input registers 9000-9019) every 0.2 s.

Sampled every 30 s from /api/info and written to the CSV: uptime, boots, heap free and lowest,
each task's lowest free stack, TLS handshakes and failures, the scan's worst execution time and
overruns, active alarms.

Passes when:
  - the unit never restarted (uptime only grows, boots unchanged);
  - under 0.5 % of requests failed;
  - the heap's lowest-ever free figure stopped falling after the first third;
  - every task kept at least 256 bytes of stack;
  - the scan had no new overruns.
"""
import argparse, csv, http.client, json, socket, ssl, struct, sys, threading, time

CTX = ssl._create_unverified_context()     # each unit has its own self-signed certificate
stop = threading.Event()
lock = threading.Lock()
counts = {"https": 0, "https_fail": 0, "reconnect": 0, "fresh": 0, "fresh_fail": 0, "modbus": 0, "modbus_fail": 0}
errors = []


def note(k, what=None):
    with lock:
        counts[k] += 1
        if what and len(errors) < 40:
            errors.append(f"{time.strftime('%H:%M:%S')} {what}")


def https_kept(host, port):
    conn = None
    while not stop.is_set():
        try:
            if conn is None:
                conn = http.client.HTTPSConnection(host, port, context=CTX, timeout=10)
            for path in ("/api/values", "/api/alarms", "/api/info"):
                conn.request("GET", path)
                r = conn.getresponse()
                b = r.read()
                if r.status != 200 or not b.startswith((b"{", b"[")):
                    note("https_fail", f"kept {path}: {r.status}")
                else:
                    note("https")
                if r.getheader("Connection", "").lower() == "close":
                    conn.close(); conn = None
                    break
        except (http.client.RemoteDisconnected, ConnectionResetError, BrokenPipeError) as e:
            # the unit closed a kept connection it had held long enough (as browsers see it): reconnect
            note("reconnect")
            try:
                conn and conn.close()
            except Exception:
                pass
            conn = None
            continue
        except Exception as e:
            note("https_fail", f"kept: {e}")
            try:
                conn and conn.close()
            except Exception:
                pass
            conn = None
            time.sleep(1)
        time.sleep(0.5)


def https_fresh(host, port):
    while not stop.is_set():
        try:
            c = http.client.HTTPSConnection(host, port, context=CTX, timeout=10)
            c.request("GET", "/api/values", headers={"Connection": "close"})
            r = c.getresponse(); b = r.read(); c.close()
            if r.status == 200 and b.startswith((b"{", b"[")):
                note("fresh")
            else:
                note("fresh_fail", f"fresh: {r.status}")
        except Exception as e:
            note("fresh_fail", f"fresh: {e}")
        time.sleep(2)


def modbus_reader(host, port, n):
    s, tid = None, 0
    while not stop.is_set():
        try:
            if s is None:
                s = socket.create_connection((host, port), timeout=5)
            tid = (tid + 1) & 0xFFFF
            s.sendall(struct.pack(">HHHBBHH", tid, 0, 6, 1, 4, 9000, 20))   # read input registers
            hdr = b""
            while len(hdr) < 7:
                part = s.recv(7 - len(hdr))
                if not part:
                    raise ConnectionError("closed")
                hdr += part
            t, _, ln, _ = struct.unpack(">HHHB", hdr)
            body = b""
            while len(body) < ln - 1:
                part = s.recv(ln - 1 - len(body))
                if not part:
                    raise ConnectionError("closed")
                body += part
            if t != tid or body[0] != 4 or body[1] != 40:
                note("modbus_fail", f"modbus {n}: function {body[0]:#x}")
            else:
                note("modbus")
        except Exception as e:
            note("modbus_fail", f"modbus {n}: {e}")
            try:
                s and s.close()
            except Exception:
                pass
            s = None
            time.sleep(1)
        time.sleep(0.2)


def info(host, port):
    c = http.client.HTTPSConnection(host, port, context=CTX, timeout=10)
    c.request("GET", "/api/info", headers={"Connection": "close"})
    r = c.getresponse(); b = r.read(); c.close()
    return json.loads(b)


def row(i):
    h = i.get("health", {})
    st = h.get("stacks", {})
    return {
        "time": time.strftime("%Y-%m-%d %H:%M:%S"), "uptime_s": i.get("uptime_s"),
        "boots": i.get("last_fault", {}).get("boots"),
        "heap_free": h.get("heap_free"), "heap_min": h.get("heap_min"),
        "stack_min_words": min(st.values()) if st else "", "stack_min_task": min(st, key=st.get) if st else "",
        "tls_handshakes": i.get("tls", {}).get("handshakes"), "tls_failed": i.get("tls", {}).get("failed"),
        "tls_dropped": i.get("tls", {}).get("dropped"), "tls_last_error": i.get("tls", {}).get("last_error"),
        "tls_pool_peak": i.get("tls", {}).get("pool_peak"),
        "scan_max_us": i.get("scan", {}).get("max_exec_us"), "scan_overruns": i.get("scan", {}).get("overruns"),
        "alarms_active": i.get("alarms", {}).get("active"),
        **{k: counts[k] for k in counts},
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--https-port", type=int, default=443)
    ap.add_argument("--modbus-port", type=int, default=502)
    ap.add_argument("--csv", default="soak.csv")
    ap.add_argument("--every", type=float, default=30, help="seconds between health samples")
    a = ap.parse_args()

    first = info(a.host, a.https_port)
    if not first.get("health", {}).get("known"):
        print("note: this firmware does not report heap and stacks (health.known false)")
    th = [threading.Thread(target=https_kept, args=(a.host, a.https_port), daemon=True) for _ in range(2)]
    th.append(threading.Thread(target=https_fresh, args=(a.host, a.https_port), daemon=True))
    th += [threading.Thread(target=modbus_reader, args=(a.host, a.modbus_port, n), daemon=True) for n in range(2)]
    for t in th:
        t.start()
    rows = [row(first)]
    end = time.time() + a.minutes * 60
    print(f"soak {a.host} for {a.minutes:g} min, samples every {a.every:g} s -> {a.csv}")
    with open(a.csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerow(rows[0]); f.flush()
        while time.time() < end:
            time.sleep(min(a.every, max(0.0, end - time.time())))
            try:
                r = row(info(a.host, a.https_port))
            except Exception as e:
                note("https_fail", f"sample: {e}")
                continue
            rows.append(r); w.writerow(r); f.flush()
            print(f"  {r['time']}  up {r['uptime_s']} s  heap min {r['heap_min']}  stack min {r['stack_min_words']} words "
                  f"({r['stack_min_task']})  tls {r['tls_handshakes']}/{r['tls_failed']} failed/{r['tls_dropped']} dropped (last {r['tls_last_error']})  "
                  f"ok https {counts['https'] + counts['fresh']} modbus {counts['modbus']}", flush=True)
    stop.set()
    for t in th:
        t.join(timeout=15)

    ok = True
    def check(cond, what):
        nonlocal ok
        print(("  ok    " if cond else "  FAIL  ") + what); ok &= bool(cond)
    ups = [r["uptime_s"] for r in rows if r["uptime_s"] is not None]
    check(all(b > a_ for a_, b in zip(ups, ups[1:])) and rows[-1]["boots"] == rows[0]["boots"],
          f"no restart (uptime {ups[0]} -> {ups[-1]} s, boots {rows[0]['boots']} -> {rows[-1]['boots']})")
    total = sum(counts[k] for k in ("https", "fresh", "modbus"))
    failed = sum(counts[k] for k in ("https_fail", "fresh_fail", "modbus_fail"))
    check(total > 0 and failed <= 0.005 * (total + failed), f"requests: {total} ok, {failed} failed {errors[:5]}")
    mins = [r["heap_min"] for r in rows if isinstance(r["heap_min"], int)]
    if len(mins) >= 3 and mins[0]:
        third = len(mins) // 3
        check(mins[-1] >= mins[third] - 256, f"heap lowest free steady after warm-up ({mins[third]} -> {mins[-1]} bytes)")
    stk = [r["stack_min_words"] for r in rows if isinstance(r["stack_min_words"], int)]
    if stk:
        check(min(stk) >= 64, f"every task kept 256 bytes of stack (lowest {min(stk) * 4} bytes, {rows[-1]['stack_min_task']})")
    ov = [r["scan_overruns"] for r in rows if isinstance(r["scan_overruns"], int)]
    if ov:
        check(ov[-1] == ov[0], f"no new scan overruns ({ov[0]} -> {ov[-1]})")
    print("SOAK:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
