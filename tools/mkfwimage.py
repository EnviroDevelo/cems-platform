#!/usr/bin/env python3
"""Make a firmware update file (.cfw) from the raw binary of a build.

    tools/mkfwimage.py build/cems.bin cems-0.4.0.cfw --version 0.4.0
    tools/mkfwimage.py --check cems-0.4.0.cfw

The binary must be linked for the start of flash (0x08000000): the running image is
always mapped there, whichever bank holds it. In STM32CubeIDE: Project > Properties >
C/C++ Build > Settings > MCU Post build outputs > "Convert to binary file".

Header layout: platform/include/fw_update.h (fw_header_t, 256 bytes, little-endian)."""
import argparse, hashlib, struct, sys, time, zlib

MAGIC = 0x31574643          # "CFW1"
PRODUCT = 0x434D5331        # PLAT_FW_PRODUCT
HDR = 256
MAX_IMAGE = 1024 * 1024 - 8192   # bank minus the trailer sector


def version_word(v):
    parts = [int("".join(c for c in p if c.isdigit()) or 0) for p in v.split("-")[0].split(".")[:3]]
    parts += [0] * (3 - len(parts))
    return (parts[0] & 0xFF) << 16 | (parts[1] & 0xFF) << 8 | (parts[2] & 0xFF)


def header(payload, version, product, build_time):
    vs = version.encode()[:23].ljust(24, b"\0")
    h = struct.pack("<IHHII24sIII32s64s100sI", MAGIC, HDR, 1, product, version_word(version), vs,
                    len(payload), 0, build_time, hashlib.sha256(payload).digest(), b"\0" * 64, b"\0" * 100, 0)
    return h + struct.pack("<I", zlib.crc32(h) & 0xFFFFFFFF)


def check(path):
    d = open(path, "rb").read()
    if len(d) < HDR:
        return "too short"
    f = struct.unpack("<IHHII24sIII32s64s100sII", d[:HDR])
    magic, size, hv, product, ver, vs, isize, flags, bt, sha = f[:10]
    if magic != MAGIC or size != HDR or hv != 1:
        return "not a CEMS firmware image"
    if zlib.crc32(d[:252]) & 0xFFFFFFFF != f[-1]:
        return "header CRC wrong"
    if isize != len(d) - HDR:
        return f"length {len(d) - HDR} but header says {isize}"
    if hashlib.sha256(d[HDR:]).digest() != sha:
        return "SHA-256 mismatch"
    name = vs.rstrip(b"\0").decode()
    print(f"{path}: version {name} (0x{ver:06x}), product 0x{product:08x}, "
          f"{isize} bytes, built {time.strftime('%Y-%m-%d %H:%M', time.gmtime(bt))} UTC, "
          f"{'signed' if flags & 1 else 'unsigned'}, sha256 {sha.hex()[:16]}...")
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="raw binary (.bin), or the .cfw with --check")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--version", help="e.g. 0.4.0")
    ap.add_argument("--product", type=lambda s: int(s, 0), default=PRODUCT)
    ap.add_argument("--check", action="store_true", help="verify an existing .cfw")
    a = ap.parse_args()
    if a.check:
        err = check(a.input)
        if err:
            print(f"{a.input}: {err}"); return 1
        return 0
    if not a.output or not a.version:
        ap.error("input, output and --version are needed")
    payload = open(a.input, "rb").read()
    if not payload or len(payload) > MAX_IMAGE:
        print(f"image size {len(payload)} outside 1..{MAX_IMAGE}"); return 1
    open(a.output, "wb").write(header(payload, a.version, a.product, int(time.time())) + payload)
    return 1 if check(a.output) else 0


if __name__ == "__main__":
    sys.exit(main())
