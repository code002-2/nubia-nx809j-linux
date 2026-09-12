#!/usr/bin/env python3
"""Validate the boot_b staging FAT image before it is published as an artifact.

Two facts the device depends on, both of which fail silently on a panel that may
not even be readable:

  * the FAT must carry 4096-byte sectors, because the UFS LUNs have a 4096-byte
    logical block size and U-Boot's FAT driver reads at the wrong offsets when
    the two disagree (it logs "FAT sector size mismatch (fs=512, dev=4096)" and
    then carries on as if they matched);
  * the image must fit the boot_b partition, which is 100663296 bytes.

Usage: check-fat.py <image> [max-bytes] [expected-sector-size]
"""
import os
import struct
import sys

path = sys.argv[1]
max_bytes = int(sys.argv[2]) if len(sys.argv) > 2 else 100663296
want_bps = int(sys.argv[3]) if len(sys.argv) > 3 else 4096

size = os.path.getsize(path)
with open(path, "rb") as f:
    bs = f.read(512)

bps, = struct.unpack_from("<H", bs, 11)
spc = bs[13]
nfats = bs[16]
rootent, = struct.unpack_from("<H", bs, 17)
fatsz16, = struct.unpack_from("<H", bs, 22)
tot32, = struct.unpack_from("<I", bs, 32)
label = bs[43:54].decode("latin-1").strip()
fstype = bs[54:62].decode("latin-1").strip()

print("==> %s: %d bytes" % (path, size))
print("    bytes/sector=%d  sectors/cluster=%d  cluster=%d B  label=%r  fstype=%r"
      % (bps, spc, bps * spc, label, fstype))
print("    FATs=%d  root entries=%d  FAT size=%d sectors  total sectors=%d"
      % (nfats, rootent, fatsz16, tot32))

fails = []
if bps != want_bps:
    fails.append("bytes/sector is %d, expected %d (UFS logical block size)"
                 % (bps, want_bps))
if size > max_bytes:
    fails.append("image is %d bytes, does not fit the %d-byte boot_b partition"
                 % (size, max_bytes))
if bs[510:512] != b"\x55\xaa":
    fails.append("missing the 0x55AA boot signature")
if not bps or bps & (bps - 1) or not spc or spc & (spc - 1):
    fails.append("sector size / sectors per cluster are not powers of two")

for f in fails:
    print("!! %s" % f)
sys.exit(1 if fails else 0)
