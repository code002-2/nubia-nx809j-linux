#!/usr/bin/env bash
# Package U-Boot as an Android boot image that ABL can chainload.
#
#   Usage: uboot/pack.sh <u-boot.bin> <out.img> [workdir]
#
# ABL loads the "kernel" of a boot image into RAM and jumps to it. U-Boot is
# built with CONFIG_POSITION_INDEPENDENT=y, so the load address does not
# matter, and it finds its own appended devicetree (CONFIG_OF_SEPARATE) rather
# than the one ABL passes in x0 - the ABL devicetree is only used for the RAM
# map (and the internal DT carries its own /memory as a fallback).
#
# Two things are deliberately NOT in this image:
#   * a ramdisk - U-Boot needs no initramfs;
#   * a dtb - boot header v4 has no dtb field at all. AOSP mkbootimg accepts
#     --dtb for v3/v4 and silently *ignores* it (write_data() only writes the
#     dtb for header_version == 2), and Android 12+ devices take the
#     devicetree from the vendor_boot image instead. ABL therefore passes its
#     own platform devicetree, which is what we want.
set -euo pipefail

UBOOT_BIN="${1:?path to u-boot.bin}"
OUT="${2:?output boot.img path}"
WORK="${3:-$(mktemp -d)}"

[ -f "$UBOOT_BIN" ] || { echo "!! no such file: $UBOOT_BIN" >&2; exit 1; }
mkdir -p "$WORK" "$(dirname "$OUT")"

# --- mkbootimg (AOSP) -------------------------------------------------------
if [ ! -d "$WORK/mkbootimg" ]; then
  echo "==> fetching AOSP mkbootimg"
  git clone --depth 1 https://github.com/LineageOS/android_system_tools_mkbootimg \
    "$WORK/mkbootimg" 2>/dev/null \
    || git clone --depth 1 https://android.googlesource.com/platform/system/tools/mkbootimg \
         "$WORK/mkbootimg"
fi
MKBI="$(find "$WORK/mkbootimg" -name 'mkbootimg.py' -o -name 'mkbootimg' -type f | head -1)"
[ -n "$MKBI" ] || { echo "!! mkbootimg not found" >&2; exit 1; }
echo "==> mkbootimg: $MKBI"

# --- payload ----------------------------------------------------------------
# ABL decompresses a gzip-compressed kernel exactly as it does for the real
# kernel image, so ship the compressed binary: ~500 KiB instead of ~1 MiB.
gzip -9 -c "$UBOOT_BIN" > "$WORK/u-boot.bin.gz"
: > "$WORK/empty-ramdisk"
echo "==> payload: u-boot.bin $(stat -c %s "$UBOOT_BIN") B -> gz $(stat -c %s "$WORK/u-boot.bin.gz") B"

# --- package ----------------------------------------------------------------
# Same shape as the kernel images this device already boots: header v4, 4 KiB
# pages (ro.boot.hardware.cpu.pagesize=4096), no cmdline (ABL supplies
# bootargs in the devicetree it passes).
python3 "$MKBI" \
  --header_version 4 \
  --kernel "$WORK/u-boot.bin.gz" \
  --ramdisk "$WORK/empty-ramdisk" \
  --pagesize 4096 \
  --os_version 16 \
  --os_patch_level 2026-05 \
  --cmdline "" \
  -o "$OUT"

echo "==> boot.img: $OUT ($(du -h "$OUT" | cut -f1))"

# --- verify -----------------------------------------------------------------
# AOSP mkbootimg packs the header fields with pack('I', ...) - *native* byte
# order, so the header is little-endian in practice. That matches the stock
# boot.img on this device (header_size reads 1584, not 805699584). Confirm it
# rather than assume it: a header ABL cannot parse is a silent hang at the
# splash screen with nothing on the panel.
python3 - "$OUT" <<'PY'
import struct, sys
path = sys.argv[1]
blob = open(path, "rb").read(4096)
magic = blob[:8]
# v3/v4 header: magic[8] kernel_size os_version header_size reserved[4]
#               header_version cmdline[1536] (header_version at offset 40)
ksize, rsize, osv, hsize = struct.unpack_from("<4I", blob, 8)
hver, = struct.unpack_from("<I", blob, 40)
cmdline = blob[44:44 + 1536].split(b"\x00")[0]
print("==> header: magic=%s version=%d kernel_size=%d ramdisk_size=%d "
      "os_version=%#x header_size=%d cmdline=%r"
      % (magic.decode(), hver, ksize, rsize, osv, hsize, cmdline))
assert magic == b"ANDROID!", magic
assert hver == 4, hver
assert hsize == 1584, hsize
kernel = open(path, "rb").read()[4096:4096 + 2]
assert kernel == b"\x1f\x8b", kernel          # gzip payload ABL can inflate
print("==> payload at offset 4096 is gzip: OK")
PY
