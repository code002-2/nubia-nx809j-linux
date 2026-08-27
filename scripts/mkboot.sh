#!/usr/bin/env bash
# Package an Android boot.img (header v4) for `fastboot boot` / flashing.
#   Usage: mkboot.sh <Image.gz> <nubia-nx809j.dtb> <initramfs.cpio.gz> <out.img>
set -euo pipefail

KERNEL="${1:?kernel Image.gz path}"
DTB="${2:?dtb path}"
RAMDISK="${3:?initramfs cpio.gz path}"
OUT="${4:?output boot.img path}"

# Fetch mkbootimg (AOSP) if not present
if [ ! -d scripts/mkbootimg ]; then
  echo "==> fetching AOSP mkbootimg"
  git clone --depth 1 https://github.com/LineageOS/android_system_tools_mkbootimg scripts/mkbootimg 2>/dev/null \
    || git clone --depth 1 https://android.googlesource.com/platform/system/tools/mkbootimg scripts/mkbootimg
fi
# locate the actual entry script (mkbootimg.py in older, mkbootimg in newer)
MKBI="$(find scripts/mkbootimg -name 'mkbootimg.py' -o -name 'mkbootimg' -type f 2>/dev/null | head -1)"
[ -n "$MKBI" ] || { echo "!! mkbootimg not found"; exit 1; }
echo "==> mkbootimg: $MKBI"

# Boot command line. console= order matters: the LAST one owns /dev/console,
# so ttyGS0 (USB serial gadget) is last; printk still fans out to all of them.
CMDLINE="${CMDLINE:-console=ttyMSM0,115200n8 console=tty1 console=ttyGS0,115200n8}"
echo "==> cmdline: $CMDLINE"

# header v4, 4KiB pages (device ro.boot.hardware.cpu.pagesize=4096)
python3 "$MKBI" \
  --header_version 4 \
  --kernel "$KERNEL" \
  --ramdisk "$RAMDISK" \
  --dtb "$DTB" \
  --cmdline "$CMDLINE" \
  --pagesize 4096 \
  --os_version 16 \
  --os_patch_level 2026-05 \
  -o "$OUT"

echo "==> boot.img: $OUT ($(du -h "$OUT" | cut -f1))"
