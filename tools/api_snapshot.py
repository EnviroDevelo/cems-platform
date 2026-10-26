#!/usr/bin/env python3
"""api_snapshot.py - the web API of a build as a plain-text snapshot (spec 9.3, CIP-14.4/14.5):
every route, every JSON field of every GET answer with its type, and the parameter table.

Starts the given netsim on a blank FRAM, logs in at factory level, reads every GET route that
platform/net/http_api.c serves, and writes one line per route and field:

    GET /api/info .core string
    GET /api/alarms .alarms[].help string
    POST /api/write
    param 30 log_interval uint s level=2 flags=persist

    tools/api_snapshot.py build/netsim docs/interface/app_demo/API.txt            write it
    tools/api_snapshot.py build/netsim docs/interface/app_demo/API.txt --check    compare

--check exits 1 and prints the difference when the API changed: a field or route removed or
retyped needs a MAJOR core version (or a new product map version for a product parameter);
one added needs MINOR. Then write the snapshot again to accept it."""
import difflib, http.cookiejar, json, os, re, signal, socket, ssl, subprocess, sys, tempfile, time
import urllib.error, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CTX = ssl._create_unverified_context()
SKIP = {"/api/backup", "/api/tls/cert", "/api/record", "/api/log/events.csv", "/api/log/data.csv"}   # binary, or need an argument


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def routes():
    src = open(os.path.join(ROOT, "platform", "net", "http_api.c"), encoding="utf-8").read()
    gets = re.findall(r'get && !strcmp\(r->path, "(/api/[^"]+)"\)', src)
    posts = re.findall(r'else if \(!strcmp\(r->path, "(/api/[^"]+)"\)\)', src)
    posts += re.findall(r'!strcmp\(r->method, "POST"\) && !strcmp\(r->path, "(/api/[^"]+)"\)', src)
    return sorted(set(gets)), sorted(set(posts))


def kind(v):
    if v is None: return "null"
    if isinstance(v, bool): return "bool"
    if isinstance(v, (int, float)): return "number"
    if isinstance(v, str): return "string"
    if isinstance(v, list): return "array"
    return "object"


def flatten(v, path, out):
    if isinstance(v, dict):
        keys = list(v.keys())
        if keys and all(k.isdigit() for k in keys):          # keyed by parameter ID: the shape, not the IDs
            for k in keys:
                flatten(v[k], path + ".<id>", out)
            return
        for k in keys:
            flatten(v[k], path + "." + k, out)
        if not keys:
            out.add(f"{path or '.'} object")
    elif isinstance(v, list):
        if not v:
            out.add(f"{path}[] empty")
        for e in v:
            if isinstance(e, (dict, list)):
                flatten(e, path + "[]", out)
            else:
                out.add(f"{path}[] {kind(e)}")
    else:
        out.add(f"{path or '.'} {kind(v)}")


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    netsim, snap, check = sys.argv[1], sys.argv[2], "--check" in sys.argv
    tmp = tempfile.mkdtemp()
    mb, web = free_port(), free_port()
    base = f"https://127.0.0.1:{web}"
    p = subprocess.Popen([netsim, str(mb), str(web), os.path.join(tmp, "fram.bin"), os.path.join(tmp, "nor.bin")],
                         stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        for _ in range(200):
            try:
                urllib.request.urlopen(base + "/api/info", timeout=1, context=CTX).read(); break
            except Exception:
                time.sleep(0.05)
        jar = http.cookiejar.CookieJar()
        op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX), urllib.request.HTTPCookieProcessor(jar))

        def req(path, form=None):
            data = urllib.parse.urlencode(form).encode() if form else None
            r = urllib.request.Request(base + path, data=data, headers={"X-CEMS": "1"} if data else {})
            try:
                with op.open(r, timeout=5) as f:
                    return f.status, f.read()
            except urllib.error.HTTPError as e:
                return e.code, e.read()

        st, _ = req("/api/login", {"level": "factory", "password": "9999"})
        if st != 200:
            print("api_snapshot: factory login failed", st); return 1
        gets, posts = routes()
        lines = set()
        params = []
        for path in gets:
            if path in SKIP:
                lines.add(f"GET {path}")
                continue
            q = "?ids=1" if path == "/api/values" else ""
            st, body = req(path + q)
            try:
                j = json.loads(body)
            except ValueError:
                lines.add(f"GET {path} ({st}, not JSON)")
                continue
            out = set()
            flatten(j, "", out)
            lines.update(f"GET {path} {f}" for f in out)
            if path == "/api/params":
                for d in j:
                    params.append(f"param {d['id']} {d['name']} {d['type']} {d.get('unit', '-')} level={d['level']} "
                                  f"flags={','.join(d.get('flags', [])) or '-'}")
        lines.update(f"POST {path}" for path in posts)
        text = "\n".join(sorted(lines) + sorted(params, key=lambda s: int(s.split()[1]))) + "\n"
    finally:
        p.send_signal(signal.SIGTERM)
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
    if check:
        old = open(snap, encoding="utf-8").read() if os.path.exists(snap) else ""
        if old != text:
            sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), text.splitlines(True), snap, "now"))
            print("the web API changed (above): bump the core version (removed or retyped: MAJOR; added: MINOR), "
                  "then make snapshots")
            return 1
        print(f"{snap} matches the firmware")
        return 0
    os.makedirs(os.path.dirname(snap) or ".", exist_ok=True)
    open(snap, "w", encoding="utf-8").write(text)
    print(f"wrote {snap}: {text.count(chr(10))} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
