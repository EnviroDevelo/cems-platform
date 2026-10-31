#!/usr/bin/env python3
"""A pretend CEMS I/O board (register map 2) on Ethernet, for testing a real core unit (the DK or a
core board) before an I/O board exists. Standard-library Python 3 only; runs on Windows or Linux.

    python tools/iob_sim.py                       (board address 1, Modbus TCP port 502)
    python tools/iob_sim.py --boards 1,2,3        (three boards: give this PC one extra IP per board)
    python tools/iob_sim.py --port 5020 --no-discovery

The protocol is the firmware's own: platform/include/iobus.h (registers), iob_net.h (Modbus TCP
and discovery). The behaviour follows the C simulated node, platform/bsp/iob_node_sim.c:
  - outputs switch only after the first valid write of register 0 (the safe-state gate), and turn
    off when no write has come for the hold time (register 32, default 5 min);
  - COMMS_LOST latches after 1 s without a request once the master has talked to it;
  - an input change is always shown once to the master, even if it changed back in between;
  - writes are checked completely first (exception 02 / 03), as on the board.

On the unit (touchscreen or web, admin): iob_link (102) = 1 (Ethernet on the main port),
iob1_ip (146) = this PC's IP, iob1_enable (110) = 1. Board k at address k uses parameters
110 + 10(k-1) and 146 + (k-1). The unit refuses two boards at one IP (settings conflict), so for
more than one board give this PC's network adapter an extra IP per board; the simulator answers
on all of them and tells the boards apart by unit id.

Console commands (board number first, default board 1):
  [b] in <bits>        inputs 1-8 as a bit mask, e.g. "in 0x05" (inputs 1 and 3 on)
  [b] fielda 0|1       field 24 V for bank A and the inputs; "fieldb" for bank B
  [b] open|short|hot <bits>   bank A open circuit / overload / over-temperature, outputs 1-8
  [b] wire <bits>      broken input wires (reported only where the master enabled the check)
  [b] bfault 0|1       bank B fault line
  [b] power 0|1        0: the board stops answering (cable pulled, power off)
  [b] restart | wdg    restart the board (wdg: as its watchdog would)
  [b] temp <ch> <degC> | temp <ch> off|open|short   temperature channel 1-6
  [b] rpm <n>          pump speed reported
  [b] show             all of the board's state
  quit
Output changes are printed as they happen.
"""
import argparse, socket, struct, sys, threading, time

IOB_MAGIC, MAP_VERSION, TEMPS, CONNS = 0x494F, 2, 6, 3
TEMP_INVALID = -32768
ADDR_SET_KEY, COMMISSION_ADDR, ADDR_MAX = 0xA55A, 246, 245
DISC_PORT, DISC_LEN, DISC_MAGIC = 50223, 80, b"CEMSIOB1"
REG_SPACE = 107
# registers (iobus.h)
OUT_CMD, RELAY_CMD, CMD_SEQ, PWM_CMD, PWM_DIR = 0, 1, 2, 3, 4
STATUS, INPUTS, OUT_APPLIED, RELAY_APPLIED, A_OPEN, A_OVERLOAD, A_OVERTEMP, B_FAULT = 16, 17, 18, 19, 20, 21, 22, 23
WIREBREAK, CMD_ECHO, RX_ERRORS, RESETS, PWM_APPLIED, TACH_RPM, TEMP_FLAGS, TEMP_COUNT = 24, 25, 26, 27, 28, 29, 30, 31
HOLD_DS, FILTER_MS, WB_ENABLE, OL_ENABLE, FAULT_CLEAR, NODE_ADDR, ADDR_SET, SERIAL = 32, 33, 34, 35, 36, 38, 40, 52
MAGIC, MAPV, HW_REV, FW_VERSION, FW_BUILD, UID = 64, 65, 66, 67, 68, 69
N_OUT_A, N_OUT_B, N_RELAYS, N_INPUTS, N_TEMP_PWM = 75, 76, 77, 78, 79
TEMP, TEMP_STATE, CJ_TEMP, TEMP_TYPE, NTC_R25, NTC_BETA, PWM_FREQ, TACH_PPR = 80, 86, 92, 96, 99, 102, 105, 106
# status bits
ST_OUT_ENABLED, ST_TIMEOUT, ST_FIELD_A, ST_FIELD_B, ST_WDG, ST_CONFIG, ST_COMMS_LOST = 1, 2, 4, 8, 0x10, 0x20, 0x100
TS_OK, TS_OFF, TS_WAIT, TS_OPEN, TS_SHORT = 0, 1, 2, 3, 4
TT_NAMES = {0: "none", 1: "K", 2: "J", 3: "T", 4: "E", 5: "S", 10: "PT100 2w", 11: "PT100 3w", 12: "PT100 4w",
            13: "PT1000 2w", 14: "PT1000 3w", 15: "PT1000 4w", 20: "NTC"}

lock = threading.Lock()


def now_ms():
    return int(time.monotonic() * 1000)


class Node:
    def __init__(self, address):
        self.address = address
        self.powered = True
        self.uid = [(0x1000 * address + i) & 0xFFFF for i in range(6)]
        self.mac = bytes([0x02] + [(self.uid[i // 2] >> (8 * (1 - i % 2))) & 255 for i in range(5)])
        self.serial = ("SIM-IOB-%03d" % address).encode()
        self.ip = self.mask = self.gw = self.master_ip = 0
        self.inputs = 0
        self.field_a = self.field_b = True
        self.a_open = self.a_overload = self.a_overtemp = self.wirebreak = self.b_fault = 0
        self.temp_set = [250 + 10 * k for k in range(TEMPS)]       # 25.0, 26.0 ... degC until changed
        self.temp_forced_state = [None] * TEMPS
        self.rpm = 0
        self.rx_errors = 0
        self.resets = 0
        self.restart(False)
        self.resets = 0

    def restart(self, wdg):
        self.out_cmd = self.relay_cmd = self.seq = self.echo = 0
        self.hold_ds, self.filter_ms, self.wb_en, self.ol_en = 3000, 8, 0, 0xFF
        self.pwm_cmd = self.pwm_dir = 0
        self.pwm_hz, self.tach_ppr = 1000, 2
        self.temp_type = [0] * CONNS
        self.ntc_r25, self.ntc_beta = [1000] * CONNS, [3435] * CONNS
        self.config_needed, self.timeout_latched, self.wdg_latched = True, False, wdg
        self.gate = False
        self.last_write = now_ms()
        self.comms_seen = self.comms_lost = False
        self.last_comms = 0
        self.in_rep = self.in_pend = 0
        self.temp_count = 0
        self.resets += 1

    # ---- time ----
    def tick(self, t):
        if self.gate and t - self.last_write >= self.hold_ds * 100:
            self.gate = False
            self.timeout_latched = True
            print(f"[board {self.address}] hold time ran out: outputs off", flush=True)
        if self.comms_seen and t - self.last_comms >= 1000 and not self.comms_lost:
            self.comms_lost = True
            print(f"[board {self.address}] no request for 1 s: COMMS_LOST latched", flush=True)
        if self.field_a:
            self.in_pend |= self.inputs ^ self.in_rep
        else:
            self.in_pend = self.in_rep

    # ---- temperatures ----
    def temp_channel(self, ch):
        conn, pair_b = ch // 2, ch % 2
        tt = self.temp_type[conn]
        if tt == 0 or (pair_b and tt >= 10):              # nothing fitted, or the B side of an RTD/NTC
            return TEMP_INVALID, TS_OFF
        f = self.temp_forced_state[ch]
        if f is not None:
            return TEMP_INVALID, f
        return self.temp_set[ch], TS_OK

    def reg(self, r):
        if TEMP <= r < TEMP + TEMPS:
            return self.temp_channel(r - TEMP)[0] & 0xFFFF
        if TEMP_STATE <= r < TEMP_STATE + TEMPS:
            return self.temp_channel(r - TEMP_STATE)[1]
        if TEMP_TYPE <= r < TEMP_TYPE + CONNS:
            return self.temp_type[r - TEMP_TYPE]
        if NTC_R25 <= r < NTC_R25 + CONNS:
            return self.ntc_r25[r - NTC_R25]
        if NTC_BETA <= r < NTC_BETA + CONNS:
            return self.ntc_beta[r - NTC_BETA]
        if UID <= r < UID + 6:
            return self.uid[r - UID]
        if SERIAL <= r < SERIAL + 8:
            s = self.serial.ljust(16, b"\0")
            k = 2 * (r - SERIAL)
            return s[k] << 8 | s[k + 1]
        if r == STATUS:
            st = 0
            st |= ST_OUT_ENABLED if self.gate else 0
            st |= ST_TIMEOUT if self.timeout_latched else 0
            st |= ST_FIELD_A if self.field_a else 0
            st |= ST_FIELD_B if self.field_b else 0
            st |= ST_WDG if self.wdg_latched else 0
            st |= ST_CONFIG if self.config_needed else 0
            st |= ST_COMMS_LOST if self.comms_lost else 0
            return st
        if r == OUT_APPLIED:
            if not self.gate:
                return 0
            return self.out_cmd & (0x00FF if self.b_fault & 2 else 0xFFFF)
        return {
            OUT_CMD: self.out_cmd, RELAY_CMD: self.relay_cmd, CMD_SEQ: self.seq, PWM_CMD: self.pwm_cmd,
            PWM_DIR: self.pwm_dir, INPUTS: (self.in_rep ^ self.in_pend) & 0xFF,
            RELAY_APPLIED: (self.relay_cmd & 3) if self.gate else 0,
            A_OPEN: self.a_open & self.ol_en, A_OVERLOAD: self.a_overload, A_OVERTEMP: self.a_overtemp,
            B_FAULT: self.b_fault, WIREBREAK: self.wirebreak & self.wb_en, CMD_ECHO: self.echo,
            RX_ERRORS: self.rx_errors & 0xFFFF, RESETS: self.resets & 0xFFFF,
            PWM_APPLIED: self.pwm_cmd if self.gate else 0, TACH_RPM: self.rpm,
            TEMP_FLAGS: 0, TEMP_COUNT: self.temp_count & 0xFFFF,
            HOLD_DS: self.hold_ds, FILTER_MS: self.filter_ms, WB_ENABLE: self.wb_en, OL_ENABLE: self.ol_en,
            NODE_ADDR: self.address, MAGIC: IOB_MAGIC, MAPV: MAP_VERSION, HW_REV: 1, FW_VERSION: 0x0100,
            FW_BUILD: 1, N_OUT_A: 8, N_OUT_B: 8, N_RELAYS: 2, N_INPUTS: 8, N_TEMP_PWM: TEMPS | 1 << 8,
            CJ_TEMP: 230, PWM_FREQ: self.pwm_hz, TACH_PPR: self.tach_ppr,
        }.get(r, 0)

    # ---- writes ----
    @staticmethod
    def writable(r):
        return (r <= PWM_DIR or HOLD_DS <= r <= FAULT_CLEAR or r in (PWM_FREQ, TACH_PPR)
                or TEMP_TYPE <= r < NTC_BETA + CONNS)

    @staticmethod
    def value_ok(r, v):
        if TEMP_TYPE <= r < TEMP_TYPE + CONNS:
            return v in TT_NAMES
        if NTC_R25 <= r < NTC_R25 + CONNS:
            return v >= 10
        if NTC_BETA <= r < NTC_BETA + CONNS:
            return 1000 <= v <= 10000
        lim = {PWM_CMD: (0, 1000), PWM_DIR: (0, 1), PWM_FREQ: (10, 25000), TACH_PPR: (1, 12), RELAY_CMD: (0, 3),
               HOLD_DS: (1, 6000), FILTER_MS: (0, 100), WB_ENABLE: (0, 255), OL_ENABLE: (0, 255), FAULT_CLEAR: (0, 3)}
        lo, hi = lim.get(r, (0, 0xFFFF))
        return lo <= v <= hi

    def write_check(self, start, vals):
        if self.address == 0:
            return 4
        for i, v in enumerate(vals):
            if not self.writable(start + i):
                return 2
            if not self.value_ok(start + i, v):
                return 3
        return 0

    def write_apply(self, t, start, vals):
        before = (self.reg(OUT_APPLIED), self.reg(RELAY_APPLIED), self.reg(PWM_APPLIED))
        w0 = False
        for i, v in enumerate(vals):
            r = start + i
            if r == OUT_CMD: self.out_cmd = v; w0 = True
            elif r == RELAY_CMD: self.relay_cmd = v
            elif r == CMD_SEQ: self.seq = v
            elif r in (HOLD_DS, FILTER_MS, WB_ENABLE, OL_ENABLE):
                setattr(self, {HOLD_DS: "hold_ds", FILTER_MS: "filter_ms", WB_ENABLE: "wb_en", OL_ENABLE: "ol_en"}[r], v)
                self.config_needed = False
            elif r == PWM_CMD: self.pwm_cmd = v
            elif r == PWM_DIR: self.pwm_dir = v
            elif r == PWM_FREQ: self.pwm_hz = v
            elif r == TACH_PPR: self.tach_ppr = v
            elif r == FAULT_CLEAR:
                if v & 1: self.timeout_latched = self.wdg_latched = False
                if v & 2: self.comms_lost = False
            elif TEMP_TYPE <= r < TEMP_TYPE + CONNS: self.temp_type[r - TEMP_TYPE] = v
            elif NTC_R25 <= r < NTC_R25 + CONNS: self.ntc_r25[r - NTC_R25] = v
            elif NTC_BETA <= r < NTC_BETA + CONNS: self.ntc_beta[r - NTC_BETA] = v
        if w0:
            self.last_write = t
            self.gate = True
        self.echo = self.seq
        after = (self.reg(OUT_APPLIED), self.reg(RELAY_APPLIED), self.reg(PWM_APPLIED))
        if after != before:
            print(f"[board {self.address}] outputs {self.outputs_text()}", flush=True)

    def outputs_text(self):
        o, rl = self.reg(OUT_APPLIED), self.reg(RELAY_APPLIED)
        on = [str(k + 1) for k in range(16) if o >> k & 1] + [f"relay{k + 1}" for k in range(2) if rl >> k & 1]
        pwm = f", pump {self.reg(PWM_APPLIED) / 10:.1f} %{' DIR' if self.pwm_dir and self.gate else ''}" if self.reg(PWM_APPLIED) else ""
        return ("on: " + " ".join(on) if on else "all off") + pwm + ("" if self.gate else " (gate closed)")

    # ---- one request: unit id + PDU in, PDU out (None: no answer) ----
    def pdu(self, t, unit, req):
        if not self.powered:
            return None
        self.tick(t)
        if unit != (COMMISSION_ADDR if self.address == 0 else self.address):
            return None
        if self.address:
            self.comms_seen, self.last_comms = True, t
        if len(req) < 1:
            return None
        fc = req[0]
        exc = lambda code: bytes([fc | 0x80, code])
        a = c = 0
        if len(req) >= 5:
            a, c = struct.unpack(">HH", req[1:5])
        if fc == 16 and len(req) >= 6 and ((a == ADDR_SET and c == 3) or (a == SERIAL and c == 8)):
            if req[5] != 2 * c or len(req) != 6 + 2 * c:
                return exc(3)
            if a == SERIAL:
                return exc(2)                             # USB only
            key, new, inv = struct.unpack(">HHH", req[6:12])
            if key != ADDR_SET_KEY or inv != new ^ 0xFFFF or not 1 <= new <= ADDR_MAX:
                return exc(3)
            print(f"[board {self.address}] address changed to {new}", flush=True)
            self.address = new
            return req[:5]
        out = None
        if fc in (3, 4):
            out = self.read(fc, a, c) if len(req) == 5 else exc(3)
        elif fc == 6:
            if len(req) != 5:
                return exc(3)
            e = self.write_check(a, [c])
            if e:
                return exc(e)
            self.write_apply(t, a, [c])
            out = req[:5]
        elif fc == 16:
            if len(req) < 6 or c == 0 or c > 123 or req[5] != 2 * c or len(req) != 6 + 2 * c:
                return exc(3)
            vals = list(struct.unpack(">%dH" % c, req[6:6 + 2 * c]))
            e = 2 if a + c > REG_SPACE else self.write_check(a, vals)
            if e:
                return exc(e)
            self.write_apply(t, a, vals)
            out = req[:5]
        elif fc == 23:
            if len(req) < 10:
                return exc(3)
            wa, wc, nb = struct.unpack(">HHB", req[5:10])
            if wc == 0 or wc > 121 or nb != 2 * wc or len(req) != 10 + 2 * wc or c == 0 or c > 125:
                return exc(3)
            vals = list(struct.unpack(">%dH" % wc, req[10:10 + 2 * wc]))
            e = 2 if wa + wc > REG_SPACE or a + c > REG_SPACE else self.write_check(wa, vals)
            if e:
                return exc(e)
            self.write_apply(t, wa, vals)
            out = self.read(fc, a, c)
        else:
            return exc(1)
        if out and not out[0] & 0x80 and fc in (3, 4, 23) and a <= INPUTS < a + c:
            self.in_rep ^= self.in_pend                   # the master has seen these changes
            self.in_pend = 0
        if out and not out[0] & 0x80 and fc in (3, 23) and a <= TEMP < a + c:
            self.temp_count += 1
        return out

    def read(self, fc, a, c):
        if c == 0 or c > 125:
            return bytes([fc | 0x80, 3])
        if a + c > REG_SPACE:
            return bytes([fc | 0x80, 2])
        return bytes([fc, 2 * c]) + b"".join(struct.pack(">H", self.reg(a + k)) for k in range(c))

    def show(self):
        t = [f"{self.temp_channel(k)[0] / 10:.1f}" if self.temp_channel(k)[1] == TS_OK else
             ("-" if self.temp_channel(k)[1] == TS_OFF else f"state {self.temp_channel(k)[1]}") for k in range(TEMPS)]
        print(f"board {self.address}: {'answering' if self.powered else 'POWERED OFF'}; outputs {self.outputs_text()}\n"
              f"  inputs 0x{self.inputs:02X} (master saw 0x{self.reg(INPUTS):02X}), field A {'ok' if self.field_a else 'MISSING'},"
              f" field B {'ok' if self.field_b else 'MISSING'}\n"
              f"  status 0x{self.reg(STATUS):04X}, hold {self.hold_ds / 10:g} s, filter {self.filter_ms} ms,"
              f" wire-break check 0x{self.wb_en:02X}, open-circuit check 0x{self.ol_en:02X}\n"
              f"  sensors {[TT_NAMES.get(x, x) for x in self.temp_type]}, temperatures {t}\n"
              f"  IP {ip_text(self.ip)}, master {ip_text(self.master_ip)}, serial {self.serial.decode()}", flush=True)


def ip_text(ip):
    return ".".join(str(ip >> s & 255) for s in (24, 16, 8, 0)) if ip else "not set"


# ---------------- Modbus TCP ----------------
def serve_tcp(nodes, port, allow):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    print(f"Modbus TCP on port {port}, boards at unit id {', '.join(str(n.address) for n in nodes)}", flush=True)
    while True:
        c, peer = srv.accept()
        if allow and peer[0] != allow:
            print(f"refused a connection from {peer[0]} (--master-ip {allow})", flush=True)
            c.close()
            continue
        print(f"master connected from {peer[0]}", flush=True)
        threading.Thread(target=client, args=(c, peer, nodes), daemon=True).start()


def recv_n(c, n):
    b = b""
    while len(b) < n:
        part = c.recv(n - len(b))
        if not part:
            raise ConnectionError("closed")
        b += part
    return b


def client(c, peer, nodes):
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        while True:
            tid, proto, ln = struct.unpack(">HHH", recv_n(c, 6))
            if proto != 0 or ln < 2 or ln > 254:
                break
            body = recv_n(c, ln)
            unit, pdu = body[0], body[1:]
            rep = None
            with lock:
                t = now_ms()
                for n in nodes:
                    r = n.pdu(t, unit, pdu)
                    if r is not None:
                        rep = r
            if rep is not None:                           # no reply: the master's timeout, as on the wire
                c.sendall(struct.pack(">HHHB", tid, 0, len(rep) + 1, unit) + rep)
    except (ConnectionError, OSError):
        pass
    print(f"master {peer[0]} disconnected", flush=True)
    c.close()


# ---------------- discovery (iob_net.h) ----------------
def disc_encode(op, seq, n):
    b = bytearray(DISC_LEN)
    b[0:8] = DISC_MAGIC
    b[8], b[9] = 1, op
    struct.pack_into(">H6H", b, 10, seq, *n.uid)
    b[24:30] = n.mac
    b[30:30 + len(n.serial[:16])] = n.serial[:16]
    flags = (0 if n.ip else 1) | (2 if n.comms_seen and not n.comms_lost else 0)
    struct.pack_into(">HHHHIIIIB", b, 46, 0x0100, 1, 1, flags, n.ip, n.mask, n.gw, n.master_ip, n.address)
    return bytes(b)


def settings_ok(ip, mask, gw, master, addr):
    def host_ok(x):
        h = x & ~mask & 0xFFFFFFFF
        return 0 < x >> 24 < 224 and x >> 24 != 127 and h != 0 and h != ~mask & 0xFFFFFFFF
    inv = ~mask & 0xFFFFFFFF
    if mask < 0xFF000000 or mask > 0xFFFFFFFC or inv & (inv + 1):
        return False
    if not host_ok(ip) or not host_ok(master) or ip == master or ip & mask != master & mask:
        return False
    if gw and (not host_ok(gw) or gw & mask != ip & mask or gw == ip):
        return False
    return 1 <= addr <= ADDR_MAX


def serve_disc(nodes, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("0.0.0.0", port))
    print(f"discovery on UDP {port}", flush=True)
    while True:
        b, peer = s.recvfrom(256)
        if len(b) != DISC_LEN or b[:8] != DISC_MAGIC or b[8] != 1 or b[9] not in (1, 3):
            continue
        op, seq = b[9], struct.unpack_from(">H", b, 10)[0]
        uid = list(struct.unpack_from(">6H", b, 12))
        ip, mask, gw, master = struct.unpack_from(">IIII", b, 54)
        addr = b[70]
        frm = struct.unpack(">I", socket.inet_aton(peer[0]))[0]
        with lock:
            for n in nodes:
                if not n.powered:
                    continue
                if op == 3:
                    if uid != n.uid or not settings_ok(ip, mask, gw, master, addr) or (n.master_ip and n.master_ip != frm):
                        continue
                    n.ip, n.mask, n.gw, n.master_ip, n.address = ip, mask, gw, master, addr
                    print(f"[board {addr}] SET: IP {ip_text(ip)}, address {addr}, master {ip_text(master)}"
                          " (this PC keeps its own IP: set the unit's iob IP to this PC)", flush=True)
                msg = disc_encode(2, seq, n)
                for to in ("255.255.255.255", peer[0]):
                    try:
                        s.sendto(msg, (to, peer[1]))
                    except OSError:
                        pass


# ---------------- console ----------------
def console(nodes):
    by = {n.address: n for n in nodes}
    for line in sys.stdin:
        w = line.split()
        if not w:
            continue
        if w[0] in ("quit", "exit"):
            return True
        n = nodes[0]
        if w[0].isdigit():
            n = by.get(int(w[0]))
            w = w[1:]
            if n is None or not w:
                print("no such board"); continue
        cmd, arg = w[0], w[1:]
        num = lambda k=0: int(arg[k], 0)
        try:
            with lock:
                if cmd == "in": n.inputs = num() & 0xFF
                elif cmd == "fielda": n.field_a = bool(num())
                elif cmd == "fieldb": n.field_b = bool(num())
                elif cmd == "open": n.a_open = num() & 0xFF
                elif cmd == "short": n.a_overload = num() & 0xFF
                elif cmd == "hot": n.a_overtemp = num() & 0xFF
                elif cmd == "wire": n.wirebreak = num() & 0xFF
                elif cmd == "bfault": n.b_fault = 1 if num() else 0
                elif cmd == "power": n.powered = bool(num())
                elif cmd == "restart": n.restart(False)
                elif cmd == "wdg": n.restart(True)
                elif cmd == "rpm": n.rpm = num()
                elif cmd == "temp":
                    ch = num() - 1
                    if not 0 <= ch < TEMPS: raise ValueError("channel 1-6")
                    v = arg[1]
                    states = {"off": TS_OFF, "open": TS_OPEN, "short": TS_SHORT, "ok": None}
                    if v in states: n.temp_forced_state[ch] = states[v]
                    else: n.temp_set[ch] = int(round(float(v) * 10)); n.temp_forced_state[ch] = None
                elif cmd == "show": n.show(); continue
                else: print("unknown command (see --help)"); continue
            print(f"board {n.address}: {cmd} {' '.join(arg)}", flush=True)
        except (ValueError, IndexError) as e:
            print("bad value:", e)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--boards", default="1", help="board addresses, e.g. 1,2,3 (0 = a new board, answering at 246)")
    ap.add_argument("--port", type=int, default=502, help="Modbus TCP port (502)")
    ap.add_argument("--disc-port", type=int, default=DISC_PORT)
    ap.add_argument("--no-discovery", action="store_true")
    ap.add_argument("--master-ip", default="", help="only accept the unit at this IP")
    a = ap.parse_args()
    nodes = [Node(int(x)) for x in a.boards.split(",")]
    threading.Thread(target=serve_tcp, args=(nodes, a.port, a.master_ip), daemon=True).start()
    if not a.no_discovery:
        threading.Thread(target=serve_disc, args=(nodes, a.disc_port), daemon=True).start()
    try:
        if not console(nodes) and not sys.stdin.isatty():   # end of input (run in the background): keep serving
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
