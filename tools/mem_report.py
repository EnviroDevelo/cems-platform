#!/usr/bin/env python3
"""Flash and RAM used by each module of the target build, from the linker map (after
unused code has been dropped), written as docs/MEMORY.md. Run after
tools/check_target.sh (it uses the last, fullest configuration):

    tools/mem_report.py build/target/cems_check.map [docs/MEMORY.md]

Keep this file current with every module: it is the record of how close the
firmware is to the chip's limits (decision D18)."""
import re, sys, os, datetime

FLASH_TOTAL = 1024 * 1024          # one bank: an image may use at most half the 2 MB (dual-bank update)
FLASH_IMAGE = FLASH_TOTAL - 8192   # minus the image trailer sector
RAM_TOTAL = 640 * 1024             # STM32H573/H563: SRAM1+2+3

MODULES = [  # (module, object-name patterns)
    ("Scan engine, queue, watchdog, diag", r"scan_engine|plat_cmdq|task_monitor|plat_diag|plat_sys"),
    ("Parameter table", r"^param\.o"),
    ("Parameter store (FRAM)", r"param_store|plat_crc32|nvm_|plat_store_task"),
    ("Event log / data logger", r"plat_log|nor_|log_csv"),
    ("Alarm manager", r"plat_alarm"),
    ("Alarm email (SMTP client)", r"^mail\.o"),
    ("Clock + SNTP", r"plat_clock|^sntp\.o"),
    ("Firmware update, signatures, serial", r"fw_update|fw_flash|fw_sig|fw_pubkey|plat_serial|dev_pubkey"),
    ("Modbus TCP", r"modbus|mbtcp"),
    ("Web server + page + logins", r"http_|web_|plat_sha256|plat_net_tasks"),
    ("HTTPS (tls.c + mbedTLS)", r"^tls\.o|^(aes|asn1parse|asn1write|bignum|bignum_core|cipher|cipher_wrap|constant_time|ctr_drbg|ecdh|ecdsa|ecp|ecp_curves|entropy|gcm|hmac_drbg|md|memory_buffer_alloc|oid|pk|pk_ecc|pk_wrap|pkparse|pkwrite|platform|platform_util|sha256|sha512|rsa|rsa_alt_helpers|base64|ssl_\w+|x509\w*)\.o"),
    ("Touch UI + EVE driver", r"^ui\.o|eve"),
    ("Task layer (FreeRTOS glue)", r"plat_tasks_freertos|plat_ui_task"),
    ("Board support (BSP)", r"bsp_|check_main"),
    ("lwIP + Ethernet driver", r"lwip|ethernetif|lan8742|plat_net_lwip|^(altcp|def|dns|inet_chksum|init|ip|mem|memp|netif|pbuf|raw|stats|sys|tcp|tcp_in|tcp_out|timeouts|udp|acd|autoip|dhcp|etharp|icmp|igmp|ip4|ip4_addr|ip4_frag|api_lib|api_msg|err|if_api|netbuf|netdb|netifapi|sockets|tcpip|ethernet|sys_arch)\.o"),
    ("FreeRTOS kernel + heap", r"^(tasks|list|queue|timers|port|portasm|heap_4)\.o"),
    ("ST HAL + startup", r"stm32h5xx_|system_stm32|startup_"),
    ("Demo application", r"app_demo"),
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
    m = re.search(r"_Min_Stack_Size = (0x[0-9a-f]+)", open(path).read())
    if m:
        stack = int(m.group(1), 16)
    return use, stack


def main():
    mp = sys.argv[1] if len(sys.argv) > 1 else "build/target/cems_check.map"
    out = sys.argv[2] if len(sys.argv) > 2 else "docs/MEMORY.md"
    use, stack = parse(mp)
    order = [n for n, _ in MODULES] + ["Other"]
    tf = sum(f for f, _ in use.values())
    tr = sum(r for _, r in use.values()) + stack
    k = lambda b: f"{b / 1024:.1f}"
    rows = []
    for n in order:
        if n in use and (use[n][0] or use[n][1]):
            rows.append(f"| {n} | {k(use[n][0])} | {k(use[n][1])} |")
    rows.append(f"| Main stack (interrupts, start-up) | 0.0 | {k(stack)} |")
    doc = f"""# Memory budget (decision D18)

Generated by `tools/mem_report.py` from the linker map of the fullest target build
(I2C FRAM, SPI NOR log flash, EVE display, lwIP, every service). Re-run it after every
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

RAM includes every task stack (inside "Task layer"), the FreeRTOS heap (48 KB, in
"FreeRTOS kernel + heap", used by lwIP), and TLS's own fixed 44 KB pool (inside "HTTPS"),
shared by the web server and the alarm-email client one at a time: measured peaks 28 KB
(web), 32 KB (mail server with an RSA-2048 key), 38 KB (RSA-4096). Alarm email added
about 31 KB of flash (TLS client, RSA, SHA-384/512 for the server's certificate, SMTP) and
11 KB of RAM (the larger pool, the client context and buffers).

## Still to come (estimates, update when built)

| Item | Flash | RAM | Note |
|---|---|---|---|
| Application alarm table (the ~50 alarms of 70_alarm_map) | ~2.5 | 0 | 24 bytes of constants each; RAM per alarm is already reserved (96 slots) |
| Translated VM/MUX application | **unknown** | **unknown** | retained data alone is 12.5 KB; size it as soon as the Stage 4 code compiles together |

## Other non-volatile memory

| Store | Size | Used | Note |
|---|---|---|---|
| FRAM (MB85RC256V, 32 KB) | 32 KB | user image: the application's estimated 12.5 KB plus 0.8 KB of platform settings (alarm actions 256 B, relays, mail set-up 0.45 KB) = 13.3 KB of 14.5 KB per copy (92 %), factory 1 KB, log pointer, boot record, HTTPS key record and firmware update history 848 B of 1 KB | **tightest store.** A 128 KB part (MB85RC1MT, same driver) is one setting (D6) |
| Log flash (W25Q128, 16 MB) | 16 MB | ring buffer, always "full" by design | events ~16,000; data 45 days at 10 channels / 60 s |
"""
    open(out, "w").write(doc)
    print(f"flash {k(tf)} KB ({100 * tf / FLASH_IMAGE:.0f} %), RAM {k(tr)} KB ({100 * tr / RAM_TOTAL:.0f} %) -> {out}")


if __name__ == "__main__":
    main()
