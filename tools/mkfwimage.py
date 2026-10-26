#!/usr/bin/env python3
"""Make a firmware update file (.cfw) from the raw binary of a build.

    tools/mkfwimage.py build/cems.bin cems-0.4.0.cfw --version 0.4.0 --key cems-fw-key.pem \
        [--hw 0-1] [--layout 3] [--build-id <git hash>]
    tools/mkfwimage.py --check cems-0.4.0.cfw [--pub <public key hex>]

Writes header version 2: --hw is the range of board revisions (the straps, CORE_PCB.md 12) the
image runs on (default 0-255: any); a board outside it refuses the image (CIP-03.2). --layout is
the settings layout version the build uses, --build-id the source revision (default: git's short
hash of the working tree, CIP-25.1).

--key signs the image (ECDSA P-256 over header bytes 0..83, which hold the payload's
SHA-256, followed by bytes 148..251 for version 2). A production build refuses unsigned images;
see tools/fwkey.py (D14).

The binary must be linked for the start of flash (0x08000000): the running image is
always mapped there, whichever bank holds it. In STM32CubeIDE: Project > Properties >
C/C++ Build > Settings > MCU Post build outputs > "Convert to binary file".

Header layout: platform/include/fw_update.h (fw_header_t, 256 bytes, little-endian)."""
import argparse, hashlib, struct, subprocess, sys, time, zlib

MAGIC = 0x31574643          # "CFW1"
PRODUCT = 0x434D5331        # PLAT_FW_PRODUCT
HDR = 256
MAX_IMAGE = 1024 * 1024 - 8192   # bank minus the trailer sector


def version_word(v):
    parts = [int("".join(c for c in p if c.isdigit()) or 0) for p in v.split("-")[0].split(".")[:3]]
    parts += [0] * (3 - len(parts))
    return (parts[0] & 0xFF) << 16 | (parts[1] & 0xFF) << 8 | (parts[2] & 0xFF)


SIGNED_LEN = 84                 # header bytes covered by the signature
SIGNED2_OFF = 148               # version 2 also signs bytes 148..251


def signed_msg(h):
    hv = struct.unpack_from("<H", h, 6)[0]
    return h[:SIGNED_LEN] + (h[SIGNED2_OFF:252] if hv >= 2 else b"")


def sign(msg, key_path):
    """64-byte r||s ECDSA P-256 / SHA-256 signature of msg."""
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
        k = serialization.load_pem_private_key(open(key_path, "rb").read(), password=None)
        r, s = utils.decode_dss_signature(k.sign(msg, ec.ECDSA(hashes.SHA256())))
    except ImportError:
        import subprocess
        der = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key_path], input=msg, capture_output=True, check=True).stdout
        r, s = der_rs(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def der_rs(der):
    """r, s from a DER ECDSA signature: SEQUENCE { INTEGER r, INTEGER s }."""
    i = 2 if der[1] < 0x80 else 3
    out = []
    for _ in range(2):
        n = der[i + 1]; out.append(int.from_bytes(der[i + 2:i + 2 + n], "big")); i += 2 + n
    return out


def header(payload, version, product, build_time, key=None, hw=(0, 255), layout=0, build_id=""):
    vs = version.encode()[:23].ljust(24, b"\0")
    bid = build_id.encode()[:12].ljust(12, b"\0")
    def pack(flags, sig):
        return struct.pack("<IHHII24sIII32s64sBBH12s84sI", MAGIC, HDR, 2, product, version_word(version), vs,
                           len(payload), flags, build_time, hashlib.sha256(payload).digest(), sig,
                           hw[0], hw[1], layout, bid, b"\0" * 84, 0)
    if key:   # sign the header exactly as sent (flags already 1), as the firmware checks it
        h = pack(1, sign(signed_msg(pack(1, b"\0" * 64)), key))
    else:
        h = pack(0, b"\0" * 64)
    return h + struct.pack("<I", zlib.crc32(h) & 0xFFFFFFFF)


def verify(hdr, pub_hex):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, utils
    from cryptography.exceptions import InvalidSignature
    k = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), bytes.fromhex(pub_hex))
    sig = hdr[84:148]
    try:
        k.verify(utils.encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")),
                 signed_msg(hdr), ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def check(path, pub=None):
    d = open(path, "rb").read()
    if len(d) < HDR:
        return "too short"
    f = struct.unpack("<IHHII24sIII32s64s100sII", d[:HDR])
    magic, size, hv, product, ver, vs, isize, flags, bt, sha = f[:10]
    if magic != MAGIC or size != HDR or hv not in (1, 2):
        return "not a CEMS firmware image"
    if zlib.crc32(d[:252]) & 0xFFFFFFFF != f[-1]:
        return "header CRC wrong"
    if isize != len(d) - HDR:
        return f"length {len(d) - HDR} but header says {isize}"
    if hashlib.sha256(d[HDR:]).digest() != sha:
        return "SHA-256 mismatch"
    if pub and not (flags & 1 and verify(d[:HDR], pub)):
        return "signature not valid for this key"
    name = vs.rstrip(b"\0").decode()
    extra = ""
    if hv >= 2:
        hmin, hmax, lay, bid = struct.unpack_from("<BBH12s", d, SIGNED2_OFF)
        extra = f", boards {hmin}-{hmax}, layout {lay}, build {bid.rstrip(b'\0').decode() or '?'}"
    print(f"{path}: version {name} (0x{ver:06x}), product 0x{product:08x}, "
          f"{isize} bytes, built {time.strftime('%Y-%m-%d %H:%M', time.gmtime(bt))} UTC, "
          f"{'signed' if flags & 1 else 'unsigned'}, sha256 {sha.hex()[:16]}...{extra}")
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="raw binary (.bin), or the .cfw with --check")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--version", help="e.g. 0.4.0")
    ap.add_argument("--product", type=lambda s: int(s, 0), default=PRODUCT)
    ap.add_argument("--check", action="store_true", help="verify an existing .cfw")
    ap.add_argument("--key", help="private signing key (PEM): sign the image")
    ap.add_argument("--hw", default="0-255", help="board revisions it runs on, e.g. 0-1 (default any)")
    ap.add_argument("--layout", type=int, default=0, help="settings layout version of the build (0 = not stated)")
    ap.add_argument("--build-id", help="source revision (default: git rev-parse --short=10 HEAD)")
    ap.add_argument("--pub", help="with --check: public key (hex, from tools/fwkey.py hex) to verify the signature")
    a = ap.parse_args()
    if a.check:
        err = check(a.input, a.pub)
        if err:
            print(f"{a.input}: {err}"); return 1
        return 0
    if not a.output or not a.version:
        ap.error("input, output and --version are needed")
    payload = open(a.input, "rb").read()
    if not payload or len(payload) > MAX_IMAGE:
        print(f"image size {len(payload)} outside 1..{MAX_IMAGE}"); return 1
    lo, _, hi = a.hw.partition("-")
    hw = (int(lo), int(hi or lo))
    if not (0 <= hw[0] <= hw[1] <= 255):
        ap.error("--hw: a range within 0-255")
    bid = a.build_id
    if bid is None:
        try:
            bid = subprocess.run(["git", "rev-parse", "--short=10", "HEAD"], capture_output=True, text=True).stdout.strip()
        except OSError:
            bid = ""
    open(a.output, "wb").write(header(payload, a.version, a.product, int(time.time()), a.key, hw, a.layout, bid) + payload)
    return 1 if check(a.output) else 0


if __name__ == "__main__":
    sys.exit(main())
