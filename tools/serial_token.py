#!/usr/bin/env python3
"""Developer token to change a unit's serial number (decision D25).

The unit's web page (System > Serial number) shows its chip ID and current serial.
On the developer's PC, with the developer key (never on the unit, never in the repo):

    tools/serial_token.py --key cems-dev-key.pem --uid 0123456789abcdef01234567 --old 0 --new 240117

prints the token (128 hex digits) to paste into the page. The token only works on
that chip and only while its serial is still --old, so it cannot be reused.
--check <token> --pub <public key hex> verifies one instead (tools/fwkey.py hex)."""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfwimage import sign


def message(uid, old, new):
    return f"CEMS-SERIAL v1 uid={uid.lower()} old={old} new={new}".encode()


def verify(msg, sig, pub_hex):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, utils
    pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), bytes.fromhex(pub_hex))
    der = utils.encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    try:
        pub.verify(der, msg, ec.ECDSA(hashes.SHA256())); return True
    except Exception:
        return False


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument("--uid", required=True, help="chip ID, 24 hex digits (from the unit)")
    a.add_argument("--old", required=True, type=int, help="the serial the unit has now")
    a.add_argument("--new", required=True, type=int, help="the serial to give it")
    a.add_argument("--key", help="developer private key (PEM)")
    a.add_argument("--check", help="a token to verify instead")
    a.add_argument("--pub", help="developer public key hex (with --check)")
    o = a.parse_args()
    if len(o.uid) != 24 or any(c not in "0123456789abcdefABCDEF" for c in o.uid):
        a.error("--uid must be 24 hex digits")
    for n in (o.old, o.new):
        if not 0 <= n <= 0xFFFFFFFF:
            a.error("serials are 0..4294967295")
    msg = message(o.uid, o.old, o.new)
    if o.check:
        ok = verify(msg, bytes.fromhex(o.check), o.pub)
        print("valid" if ok else "NOT valid"); return 0 if ok else 1
    if not o.key:
        a.error("--key is needed")
    print(sign(msg, o.key).hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
