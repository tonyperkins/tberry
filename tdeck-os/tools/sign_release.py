#!/usr/bin/env python3
"""Signing for tdeck-os over-the-air updates (Ed25519, via PyNaCl).

  sign_release.py keygen                      create the signing key if it does not exist
  sign_release.py header [include/update_key.h]   write the public key as a C header
  sign_release.py sign FIRMWARE.bin --version 0.2.0 --build 2 --url URL [--out manifest.json]

The private key is a 32-byte seed in hex at ~/.config/tberry/signing.key (mode 600), outside the
repository. Keep a copy in a password manager: without it, devices in the field cannot accept
updates and need USB.

What is signed is a short message, not the whole image:  "TBERRYFW1" + build (u32 LE) +
size (u32 LE) + SHA-256 of the image. The device hashes the image while writing it and checks the
signature before switching to it.
"""
import argparse, hashlib, json, os, struct, sys
from nacl.signing import SigningKey

KEY_PATH = os.path.expanduser("~/.config/tberry/signing.key")
MAGIC = b"TBERRYFW1"


def load_key():
    if not os.path.exists(KEY_PATH):
        sys.exit(f"no signing key at {KEY_PATH}; run: sign_release.py keygen")
    return SigningKey(bytes.fromhex(open(KEY_PATH).read().strip()))


def keygen():
    if os.path.exists(KEY_PATH):
        sk = load_key()
        print(f"key exists: {KEY_PATH}")
    else:
        os.makedirs(os.path.dirname(KEY_PATH), mode=0o700, exist_ok=True)
        sk = SigningKey.generate()
        fd = os.open(KEY_PATH, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(sk.encode().hex() + "\n")
        print(f"created {KEY_PATH} (mode 600). Back it up somewhere safe.")
    print("public key:", sk.verify_key.encode().hex())


def header(path):
    pub = load_key().verify_key.encode()
    body = ", ".join(f"0x{b:02x}" for b in pub)
    open(path, "w").write(
        "#pragma once\n// Public key that update images must be signed with (tools/sign_release.py header).\n"
        f"static const unsigned char UPDATE_PUBLIC_KEY[32] = {{ {body} }};\n")
    print("wrote", path)


def sign(fw, version, build, url, out):
    data = open(fw, "rb").read()
    digest = hashlib.sha256(data).digest()
    msg = MAGIC + struct.pack("<II", build, len(data)) + digest
    sig = load_key().sign(msg).signature
    manifest = {"version": version, "build": build, "size": len(data), "sha256": digest.hex(),
                "sig": sig.hex(), "url": url}
    open(out, "w").write(json.dumps(manifest, indent=1) + "\n")
    print(f"signed {fw}: {len(data)} bytes, build {build} -> {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen")
    h = sub.add_parser("header"); h.add_argument("path", nargs="?", default="include/update_key.h")
    s = sub.add_parser("sign")
    s.add_argument("firmware"); s.add_argument("--version", required=True)
    s.add_argument("--build", type=int, required=True); s.add_argument("--url", required=True)
    s.add_argument("--out", default="manifest.json")
    a = ap.parse_args()
    if a.cmd == "keygen": keygen()
    elif a.cmd == "header": header(a.path)
    else: sign(a.firmware, a.version, a.build, a.url, a.out)


if __name__ == "__main__":
    main()
