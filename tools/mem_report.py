#!/usr/bin/env python3
"""Flash and RAM used by each module of the target build, from the linker map (after
unused code has been dropped), written as docs/MEMORY.md. Run after
tools/check_target.sh (it uses the last, fullest configuration):

    tools/mem_report.py build/target/cems_check.map [docs/MEMORY.md [app.map "app name"]]

The optional second map is the same build with the real application linked in
(make target APP=app_vmmux): its totals go in their own section.

Keep this file current with every module: it is the record of how close the
firmware is to the chip's limits (decision D18)."""
import re, sys, os, datetime

FLASH_TOTAL = 1024 * 1024          # one bank: an image may use at most half the 2 MB (dual-bank update)
FLASH_IMAGE = FLASH_TOTAL - 8192   # minus the image trailer sector
RAM_TOTAL = 640 * 1024             # STM32H573/H563: SRAM1+2+3

MODULES = [  # (module, object-name patterns)
    ("Scan engine, queue, watchdog, diag", r"scan_engine|plat_cmdq|task_monitor|plat_diag|plat_sys|fault_stm32h5"),
    ("Parameter table", r"^param\.o"),
    ("Parameter store (FRAM)", r"param_store|plat_crc32|nvm_|plat_store_task"),
    ("Event log / data logger", r"plat_log|nor_|log_csv"),
    ("Alarm manager, state, supply, calibration, service, self-test", r"plat_alarm|plat_state|plat_power|plat_cal|plat_service|plat_selftest"),
    ("Emissions calculations (emis)", r"emis"),
    ("Alarm email (SMTP client)", r"^mail\.o"),
    ("Clock + SNTP", r"plat_clock|^sntp\.o"),
    ("Firmware update, signatures, serial", r"fw_update|fw_flash|fw_sig|fw_pubkey|plat_serial|dev_pubkey"),
    ("I/O boards (Ethernet/RS-485 master)", r"iobus|mb_rtu|rs485|iob_net|iob_tcp|w5500"),
    ("Modbus TCP", r"modbus|mbtcp"),
    ("Web server + page + logins", r"http_|web_|plat_sha256|plat_net_tasks"),
    ("HTTPS (tls.c + mbedTLS)", r"^tls\.o|^(aes|asn1parse|asn1write|bignum|bignum_core|cipher|cipher_wrap|constant_time|ctr_drbg|ecdh|ecdsa|ecp|ecp_curves|entropy|gcm|hmac_drbg|md|memory_buffer_alloc|oid|pk|pk_ecc|pk_wrap|pkparse|pkwrite|platform|platform_util|sha256|sha512|rsa|rsa_alt_helpers|base64|ssl_\w+|x509\w*)\.o"),
    ("Touch UI + EVE driver", r"^ui\.o|eve"),
    ("Task layer (FreeRTOS glue)", r"plat_tasks_freertos|plat_ui_task"),
    ("Board support (BSP)", r"bsp_|check_main|supply_stm32h5"),
    ("lwIP + Ethernet driver", r"lwip|ethernetif|lan8742|plat_net_lwip|^(altcp|def|dns|inet_chksum|init|ip|mem|memp|netif|pbuf|raw|stats|sys|tcp|tcp_in|tcp_out|timeouts|udp|acd|autoip|dhcp|etharp|icmp|igmp|ip4|ip4_addr|ip4_frag|api_lib|api_msg|err|if_api|netbuf|netdb|netifapi|sockets|tcpip|ethernet|sys_arch)\.o"),
    ("FreeRTOS kernel + heap", r"^(tasks|list|queue|timers|port|portasm|heap_4)\.o"),
    ("ST HAL + startup", r"stm32h5xx_|system_stm32|startup_"),
    ("Application", r"app_demo|^vm_|_faithful|app_vmmux"),
    ("C library", r"libc|libm|libgcc|lib_a|crt"),
]


def classify(obj):
    base = os.path.basename(obj.split("(")[-1].rstrip(")")) if "(" in obj else os.path.basename(obj)
    for name, pat in MODULES:
        if re.search(pat, base) or re.search(pat, obj):
            return name
    return "Other"


def parse(path):
    lines = open(path).read().split("\n")
    start = next(i for i, l in enumerate(lines) if l.startswith("Linker script and memory map"))
    use, rod = {}, {}
    sec = None
    for l in lines[start:]:
        m = re.match(r"^ (\.[\w.$]+)\s*$", l)
        if m:
            sec = m.group(1); continue
        m = re.match(r"^ (\.[\w.$]+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", l)
        if not m:
            continue
        name = m.group(1) or sec
        sec = None
        size, obj = int(m.group(3), 16), m.group(4).strip()
        if not name or int(m.group(2), 16) == 0 or name.startswith((".ARM.attributes", ".comment", ".debug")):
            continue                                  # not loaded into the chip
        if not name or size == 0 or obj.startswith("0x") or " " in obj and not obj.endswith(")"):
            continue
        if name.startswith(".rodata"):
            kind = "rodata"
        elif name.startswith((".text", ".ARM", ".init", ".fini", ".isr_vector")):
            kind = "flash"
        elif name.startswith(".data"):
            kind = "data"
        elif name.startswith((".bss", ".noinit", "COMMON")):
            kind = "ram"
        else:
            continue
        mod = classify(obj)
        if kind == "rodata":
            rod[mod] = rod.get(mod, 0) + size
            continue
        f, r = use.get(mod, (0, 0))
        if kind == "flash":
            f += size
        elif kind == "data":
            f += size; r += size
        else:
            r += size
        use[mod] = (f, r)
    # constants: the map lists strings before the linker merges duplicates, so scale
    # each module's share to the real .rodata size in the ELF
    elf = os.path.splitext(path)[0] + ".elf"
    real = None
    try:
        import subprocess, shutil
        tool = shutil.which("arm-none-eabi-size") or os.path.join(os.environ.get("EXT", os.path.abspath("../ext")),
                                                                  "xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-size")
        out = subprocess.run([tool, "-A", elf], capture_output=True, text=True).stdout
        real = sum(int(l.split()[1]) for l in out.splitlines() if l.startswith(".rodata"))
    except Exception:
        pass
    total_rod = sum(rod.values()) or 1
    scale = (real / total_rod) if real else 1.0
    for mod, b in rod.items():
        f, r = use.get(mod, (0, 0))
        use[mod] = (f + int(b * scale), r)
    stack = 0
    text = open(path).read()
    for sym in ("_Min_Stack_Size", "_Min_Heap_Size"):      # the main stack and the C library's heap
        m = re.search(sym + r" = (0x[0-9a-f]+)", text)
        if m:
            stack += int(m.group(1), 16)
    return use, stack


def main():
    mp = sys.argv[1] if len(sys.argv) > 1 else "build/target/cems_check.map"
    out = sys.argv[2] if len(sys.argv) > 2 else "docs/MEMORY.md"
    app_sec = ""
    if len(sys.argv) > 4:
        au, ast = parse(sys.argv[3])
        af = sum(f for f, _ in au.values()); ar = sum(r for _, r in au.values()) + ast
        kk = lambda b: f"{b / 1024:.1f}"
        a_f, a_r = au.get("Application", (0, 0))
        app_sec = f"""
## With the {sys.argv[4]}

The same build with the real application linked in (`make target APP=...`).

| Resource | Used | Limit | Used % | Free |
|---|---|---|---|---|
| Flash | {kk(af)} KB | {kk(FLASH_IMAGE)} KB | {100 * af / FLASH_IMAGE:.0f} % | {kk(FLASH_IMAGE - af)} KB |
| RAM | {kk(ar)} KB | {kk(RAM_TOTAL)} KB | {100 * ar / RAM_TOTAL:.0f} % | {kk(RAM_TOTAL - ar)} KB |

The application itself: {kk(a_f)} KB of flash, {kk(a_r)} KB of RAM (its tags, timers and tables).
"""
    use, stack = parse(mp)
    order = [n for n, _ in MODULES] + ["Other"]
    tf = sum(f for f, _ in use.values())
    tr = sum(r for _, r in use.values()) + stack
    k = lambda b: f"{b / 1024:.1f}"
    rows = []
    for n in order:
        if n in use and (use[n][0] or use[n][1]):
            rows.append(f"| {n} | {k(use[n][0])} | {k(use[n][1])} |")
    rows.append(f"| Main stack (interrupts, start-up) + C heap | 0.0 | {k(stack)} |")
    doc = f"""# Memory budget (decision D18)

Generated by `tools/mem_report.py` from the linker map of the fullest target build
(I2C FRAM, SPI NOR log flash, EVE display, the W5500 I/O port, the supply-sense ADC, lwIP, every service). Re-run it after every
module, and before choosing hardware. Last run: {datetime.date.today().isoformat()}.

## Totals

| Resource | Used | Limit | Used % | Free |
|---|---|---|---|---|
| Flash (one bank: the image limit with dual-bank updates) | {k(tf)} KB | {k(FLASH_IMAGE)} KB | {100 * tf / FLASH_IMAGE:.0f} % | {k(FLASH_IMAGE - tf)} KB |
| RAM (STM32H573/H563, 640 KB) | {k(tr)} KB | {k(RAM_TOTAL)} KB | {100 * tr / RAM_TOTAL:.0f} % | {k(RAM_TOTAL - tr)} KB |

## By module (KB)

| Module | Flash | RAM |
|---|---|---|
""" + "\n".join(rows) + f"""
{app_sec}

RAM includes every task stack (inside "Task layer"), the FreeRTOS heap (64 KB, in
"FreeRTOS kernel + heap", used by lwIP), and TLS's own fixed 80 KB pool (inside "HTTPS"),
shared by the web server (three connections: two kept browsers and a spare, ~70 KB) and the alarm-email client one at a time: measured peaks 28 KB
(one web connection), 32 KB (mail server with an RSA-2048 key), 38 KB (RSA-4096). Alarm email added
about 31 KB of flash (TLS client, RSA, SHA-384/512 for the server's certificate, SMTP) and
11 KB of RAM (the larger pool, the client context and buffers). The 128 KB FRAM map (D6)
added about 40 KB of RAM: the store's working copy of a 40 KB settings slot. Since 29 Oct
that working copy is also the backup/restore buffer, lent to the web server between saves
(param_store_lease), so the web server's own buffer is 8 KB instead of 49 KB, and lwIP's
unused PBUF_POOL (12 KB) is gone: 52 KB less RAM in all. The CORE PCB adds an 8 MB PSRAM
(Gary, 29 Oct): with PLAT_PSRAM=1 the TLS pool and that staging block (127 KB) move there and
the MCU's RAM in use drops to about 339 KB (`tools/check_target.sh` prints both builds).

## Still to come (estimates, update when built)

| Item | Flash | RAM | Note |
|---|---|---|---|
| Application alarm table (the ~50 alarms of 70_alarm_map) | ~2.5 | 0 | 24 bytes of constants each; RAM per alarm is already reserved (96 slots) |

## Other non-volatile memory

| Store | Size | Used | Note |
|---|---|---|---|
| FRAM (MB85RC1MT, 128 KB; D6, Gary 20 Oct) | 128 KB | user image: the VM/MUX application's 13.4 KB including the platform settings, of 40 KB per copy (34 %); factory 4 KB per copy, of which the calibration records take 2.4 KB (8 channels, 12 history records; 2 and 3 on the 32 KB part) and the service history 0.6 KB; log pointer, boot record, HTTPS key record and firmware update history 848 B | the 32 KB breakout (MB85RC256V) still fits the application (90 % per copy) with PLAT_FRAM_SIZE=32768u; the driver refuses a part smaller than the build's map |
| Log flash (W25Q128, 16 MB) | 16 MB | ring buffer, always "full" by design | events ~16,000; data 45 days at 10 channels / 60 s |
"""
    open(out, "w").write(doc)
    print(f"flash {k(tf)} KB ({100 * tf / FLASH_IMAGE:.0f} %), RAM {k(tr)} KB ({100 * tr / RAM_TOTAL:.0f} %) -> {out}")


if __name__ == "__main__":
    main()
