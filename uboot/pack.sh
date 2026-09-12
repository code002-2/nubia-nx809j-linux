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
python3 - "$OUT" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read(4096)
magic, ksize, rsize, osv, hsize, r0, r1, r2, r3, hver = struct.unpack_from('>10I', b, 0)
print("==> header: magic=%s kernel=%d ramdisk=%d os_version=%#x header_size=%d version=%d"
      % (magic.decode(), ksize, rsize, osv, hsize, hver))
PY
