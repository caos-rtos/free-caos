#!/usr/bin/env python3
"""caos_image.py - build or inspect a caOS image for the bootloader slots.

A slot holds a 1 KB header followed by the image, linked to start right after
it (workspace image.cmake -> <mcu>_<app>_slotA / _slotB). The bootloader
(workspace/app_2, guide in workspace/app_2/doc) checks the header and the
SHA-256 before it starts the image with ca_os_end().

  caos_image.py elfs/H563/h563_app_1_slotA.elf             -> .cimg next to it
  caos_image.py elfs/H563/h563_app_1_slotA.elf --version 3 --tag "v1.3 field test"
  caos_image.py --info elfs/H563/h563_app_1_slotA.cimg     check and print a header
  caos_image.py elfs/H563/h563_app_1_slotA.elf --sign      signed, demo key
  caos_image.py elfs/H563/h563_app_1_slotA.elf --sign my.pem
  caos_image.py --keygen --key my.pem                      your own key pair

Header (little endian, 1024 bytes, rest 0xFF):
  0   magic        'CAIM'
  4   u16          header version (1)
  6   u16          flags: b0 = signed (Ed25519)
  8   u32          image size, bytes after the header
  12  u32          link address of the vector table = slot base + 0x400
  16  u32          version (compared by the bootloader)
  20  char[32]     tag, free text
  52  u32          build time, epoch
  56  u8[32]       SHA-256 of header bytes 0..55 + the image
  88  u8[64]       Ed25519 signature of bytes 0..87 (zero when unsigned): the
                   SHA-256 in it covers the image, so the signature does too

Keys: the repository ships a DEMO key pair, workspace/app_2/boot_ed25519.pem
(private, public on purpose: anyone can sign images for this example) and
workspace/app_2/boot_pubkey.h (the public half, compiled into the bootloader).
A real product MUST make its own: --keygen --key <pem> writes the private key
there (mode 600, keep it out of any repository) and regenerates boot_pubkey.h.
Signing and checking use OpenSSL >= 1.1.1 (pkeyutl -rawin).

The .cimg is written at the slot base (slot A after the bootloader, slot B at
the start of bank 2): the map is read from boot_image.h, its one place.
Python 3.6+, standard library only; needs arm-none-eabi-objcopy / -nm.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HDR_SIZE   = 0x400
ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF_KEY    = os.path.join(ROOT, "workspace", "app_2", "boot_ed25519.pem")  # demo key
DEF_PUBH   = os.path.join(ROOT, "workspace", "app_2", "boot_pubkey.h")
MAGIC      = b"CAIM"
HDR_VER    = 1


def boot_map():
    """CA_BOOT_LOADER_LEN, CA_BOOT_SLOT_LEN, CA_BOOT_SLOT_B_BASE from
    boot_image.h: caos/inc/ in the repository, include/ in the FREE package."""
    for h in (os.path.join(ROOT, "caos", "inc", "boot_image.h"),
              os.path.join(ROOT, "include", "boot_image.h")):
        if os.path.exists(h):
            v = {}
            for line in open(h):
                m = re.match(r"#define\s+CA_BOOT_(LOADER_LEN|SLOT_LEN|SLOT_B_BASE)\s+(0x[0-9A-Fa-f]+)", line)
                if m:
                    v[m.group(1)] = int(m.group(2), 16)
            if len(v) == 3:
                return v
    sys.exit("ERROR: boot map (CA_BOOT_*) not found in boot_image.h")


_MAP       = boot_map()
SLOTS      = {0x08000000 + _MAP["LOADER_LEN"]: "A", _MAP["SLOT_B_BASE"]: "B"}
SLOT_LEN   = _MAP["SLOT_LEN"]
FIXED      = struct.Struct("<4sHHIII32sI")      # bytes 0..55, covered by the hash
SHA_OFF    = FIXED.size                          # 56
SIG_OFF    = SHA_OFF + 32                        # 88


def tool(name):
    t = shutil.which("arm-none-eabi-" + name)
    if not t:
        sys.exit("ERROR: arm-none-eabi-%s not found in PATH (Arm GNU Toolchain)" % name)
    return t


def openssl(*args, data=None):
    if not shutil.which("openssl"):
        sys.exit("ERROR: openssl not found in PATH")
    r = subprocess.run(["openssl"] + list(args), input=data, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    return r


def raw_public(pem):
    """The 32 raw bytes of the Ed25519 public key of a (private) PEM."""
    r = openssl("pkey", "-in", pem, "-pubout", "-outform", "DER")
    if r.returncode != 0 or len(r.stdout) != 44:
        sys.exit("ERROR: %s is not an Ed25519 key: %s" % (pem, r.stderr.decode().strip()))
    return r.stdout[-32:]


def sign(pem, msg):
    with tempfile.TemporaryDirectory() as d:
        m = os.path.join(d, "m")
        open(m, "wb").write(msg)
        r = openssl("pkeyutl", "-sign", "-inkey", pem, "-rawin", "-in", m)
    if r.returncode != 0 or len(r.stdout) != 64:
        sys.exit("ERROR: signing with %s failed: %s" % (pem, r.stderr.decode().strip()))
    return r.stdout


def verify(pem, msg, sig):
    with tempfile.TemporaryDirectory() as d:
        m, s = os.path.join(d, "m"), os.path.join(d, "s")
        open(m, "wb").write(msg)
        open(s, "wb").write(sig)
        r = openssl("pkeyutl", "-verify", "-inkey", pem, "-rawin", "-in", m, "-sigfile", s)
    return r.returncode == 0


def key_id(pub):
    return hashlib.sha256(pub).hexdigest()[:8]


def keygen(a):
    pem, pubh = a.key or DEF_KEY, a.pubkey_h
    if not a.key:
        sys.exit("ERROR: --keygen needs --key <pem>: the demo key %s is the repository's "
                 "and stays as it is." % DEF_KEY)
    if os.path.exists(pem):
        sys.exit("ERROR: %s exists - a new key would make every signed image invalid. "
                 "Remove it by hand if that is what you want." % pem)
    os.makedirs(os.path.dirname(os.path.abspath(pem)), exist_ok=True)
    old = os.umask(0o077)
    try:
        r = openssl("genpkey", "-algorithm", "ed25519", "-out", pem)
    finally:
        os.umask(old)
    if r.returncode != 0:
        sys.exit("ERROR: openssl genpkey: %s" % r.stderr.decode().strip())
    os.chmod(pem, 0o600)
    pub = raw_public(pem)
    rows = ",\n".join("    " + ", ".join("0x%02X" % b for b in pub[i:i + 8]) for i in range(0, 32, 8))
    open(pubh, "w").write("""/*
 * boot_pubkey.h - the Ed25519 PUBLIC key the bootloader trusts.
 * Generated by tools/caos_image.py --keygen; the private half is %s,
 * on the PC that signs.
 */
#ifndef BOOT_PUBKEY_H
#define BOOT_PUBKEY_H

#define BOOT_PUBKEY_ID  "%s"   /* first 4 bytes of SHA-256(key), as printed by --info */

static const u8 s_boot_pubkey[32] = {
%s
};

#endif
""" % (os.path.basename(pem), key_id(pub), rows))
    print("private key: %s (mode 600 - keep it off the repository)" % pem)
    print("public key : %s, id %s" % (pubh, key_id(pub)))


def vector_table(elf):
    out = subprocess.run([tool("nm"), elf], stdout=subprocess.PIPE,
                         universal_newlines=True, check=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == "g_pfnVectors":
            return int(p[0], 16)
    sys.exit("ERROR: no g_pfnVectors in %s - not a caOS firmware?" % elf)


def build(a):
    link = vector_table(a.elf)
    base = link - HDR_SIZE
    if base not in SLOTS:
        sys.exit("ERROR: vector table at 0x%08X: not linked for a slot (expected 0x%08X or "
                 "0x%08X) - use the <app>_slotA/_slotB elf" % (link, *(b + HDR_SIZE for b in sorted(SLOTS))))
    with tempfile.TemporaryDirectory() as d:
        binf = os.path.join(d, "image.bin")
        subprocess.run([tool("objcopy"), "-O", "binary", a.elf, binf], check=True)
        image = open(binf, "rb").read()
    if len(image) + HDR_SIZE > SLOT_LEN:
        sys.exit("ERROR: image %u bytes does not fit a %u KB slot" % (len(image), SLOT_LEN // 1024))
    tag = a.tag.encode("ascii", "replace")[:31]
    flags = 1 if a.sign else 0          # in the hashed bytes: cannot be dropped
    fixed = FIXED.pack(MAGIC, HDR_VER, flags, len(image), link, a.version,
                       tag.ljust(32, b"\0"), int(time.time()))
    sha = hashlib.sha256(fixed + image).digest()
    sig = sign(a.sign, fixed + sha) if a.sign else bytes(64)
    hdr = (fixed + sha + sig).ljust(HDR_SIZE, b"\xff")
    out = a.out or os.path.splitext(a.elf)[0] + ".cimg"
    with open(out, "wb") as f:
        f.write(hdr + image)
    print("written %s: slot %s, image %u bytes, version %u, sha256 %s..., %s"
          % (out, SLOTS[base], len(image), a.version, sha.hex()[:16],
             "signed, key %s" % key_id(raw_public(a.sign)) if a.sign else "unsigned"))
    print("  write at 0x%08X" % base)


def info(path, pem=None):
    data = open(path, "rb").read()
    if len(data) < HDR_SIZE:
        sys.exit("ERROR: %s: shorter than a header" % path)
    magic, hv, flags, size, link, ver, tag, btime = FIXED.unpack_from(data, 0)
    sha = data[SHA_OFF:SHA_OFF + 32]
    image = data[HDR_SIZE:HDR_SIZE + size]
    ok = (magic == MAGIC and hv == HDR_VER and len(image) == size and
          hashlib.sha256(data[:SHA_OFF] + image).digest() == sha)
    base = link - HDR_SIZE
    sig_line = None
    if flags & 1:
        if pem:
            good = verify(pem, data[:SIG_OFF], data[SIG_OFF:SIG_OFF + 64])
            ok = ok and good
            sig_line = "  signature %s with key %s" % ("VALID" if good else "NOT VALID",
                                                       key_id(raw_public(pem)))
        else:
            sig_line = "  signature present (--key PEM to check it)"
    print("%s: %s" % (path, "VALID" if ok else "INVALID"))
    print("  magic %s  header v%u  flags 0x%04X%s" % (magic, hv, flags,
          " (signed)" if flags & 1 else ""))
    print("  slot %s (base 0x%08X), vector table 0x%08X" % (SLOTS.get(base, "?"), base, link))
    print("  image %u bytes, version %u, tag \"%s\"" % (size, ver, tag.rstrip(b"\0").decode("ascii", "replace")))
    print("  built %s" % time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(btime)))
    print("  sha256 %s" % sha.hex())
    if sig_line:
        print(sig_line)
    if len(image) >= 8:
        sp, pc = struct.unpack_from("<II", image, 0)
        print("  MSP 0x%08X  Reset_Handler 0x%08X" % (sp, pc))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog=__doc__.split("\n\n", 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="?", help="a slot elf: <mcu>_<app>_slotA.elf / _slotB.elf")
    ap.add_argument("--version", type=int, default=0, help="image version (u32)")
    ap.add_argument("--tag", default="", help="free text, up to 31 characters")
    ap.add_argument("-o", "--out", help="output .cimg (default: next to the elf)")
    ap.add_argument("--info", metavar="CIMG", help="check and print the header of a .cimg")
    ap.add_argument("--sign", metavar="PEM", nargs="?", const=DEF_KEY,
                    help="sign with this Ed25519 private key (default: the demo key)")
    ap.add_argument("--keygen", action="store_true", help="create the key pair (see Keys)")
    ap.add_argument("--key", metavar="PEM", help="--keygen: where the private key goes; "
                    "--info: key to check the signature with (default %s)" % DEF_KEY)
    ap.add_argument("--pubkey-h", default=DEF_PUBH, help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.keygen:
        keygen(a)
        return
    if a.info:
        pem = a.key or (DEF_KEY if os.path.exists(DEF_KEY) else None)
        sys.exit(info(a.info, pem))
    if not a.elf:
        ap.error("an elf, or --info CIMG")
    build(a)


if __name__ == "__main__":
    main()
