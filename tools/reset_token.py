#!/usr/bin/env python3
"""Developer token to put a unit's passwords back to the defaults (credential recovery, CIP-19.3).

For a unit whose passwords are lost, the factory one included. Its web page (login > "Lost
passwords") or GET /api/auth/reset shows its chip ID and how many resets it has had. On the
developer's PC, with the developer key (the one that signs serial numbers, tools/fwkey.py):

    tools/reset_token.py --key cems-dev-key.pem --uid 0123456789abcdef01234567 --n 0

prints the token (128 hex digits) to paste in (POST /api/auth/reset sig=<token>). It only works
on that chip and only once (the count moves on). --check <token> --pub <public key hex> verifies
one instead."""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfwimage import sign
from serial_token import verify


def message(uid, n):
    return f"CEMS-RESET v1 uid={uid.lower()} n={n}".encode()


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument("--uid", required=True, help="chip ID, 24 hex digits (from the unit)")
    a.add_argument("--n", required=True, type=int, help="resets the unit has had (from the unit)")
    a.add_argument("--key", help="developer private key (PEM)")
    a.add_argument("--check", help="a token to verify instead")
    a.add_argument("--pub", help="developer public key hex (with --check)")
    o = a.parse_args()
    if len(o.uid) != 24 or any(c not in "0123456789abcdefABCDEF" for c in o.uid):
        a.error("--uid must be 24 hex digits")
    if not 0 <= o.n <= 0xFFFFFFFF:
        a.error("--n is 0..4294967295")
    msg = message(o.uid, o.n)
    if o.check:
        ok = verify(msg, bytes.fromhex(o.check), o.pub)
        print("valid" if ok else "NOT valid"); return 0 if ok else 1
    if not o.key:
        a.error("--key is needed")
    print(sign(msg, o.key).hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
