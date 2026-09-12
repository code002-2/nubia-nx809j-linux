#!/usr/bin/env bash
# Build the boot_b staging FAT image that U-Boot's sysboot reads.
#
#   bootb/mkfat.sh <Image> <board.dtb> <initramfs.cpio.gz> <out.img> [size_mb]
#
# The sector size is the whole point of this script. U-Boot's FAT driver
# compares the FAT's sector size against the block device's, and the UFS LUNs
# on this device report a 4096-byte logical block size. A 512-byte-sector FAT
# - what mformat and mkfs.fat produce by default - makes U-Boot log
#
#     FAT sector size mismatch (fs=512, dev=4096)
#
# and then read as if the sizes matched: the filesystem mounts, every read
# lands at the wrong offset, and sysboot ends with "Error reading config file"
# (and, worse, a corrupted kernel image that faults the moment it runs). So
# the staging image is created with -S 4096.
#
# mkfs.fat (dosfstools) rather than mformat: the mtools in the CI image
# (4.0.32) rejects the option combination outright - "argssize must be less
# than 6", with a usage line that does not even list mformat's -T. mcopy is
# still used to populate the filesystem, which it handles fine.
set -euo pipefail

IMAGE="${1:?path to the kernel Image (uncompressed)}"
DTB="${2:?path to the board dtb}"
INITRD="${3:?path to initramfs.cpio.gz}"
OUT="${4:?output image path}"
SZ_MB="${5:-80}"

HERE="$(cd "$(dirname "$0")" && pwd)"
CONF="${CONF:-$HERE/extlinux.conf}"
CHECK="$HERE/check-fat.py"

for f in "$IMAGE" "$DTB" "$INITRD" "$CONF" "$CHECK"; do
  [ -f "$f" ] || { echo "!! missing $f" >&2; exit 1; }
done

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"

echo "==> creating a ${SZ_MB} MiB FAT with 4096-byte sectors"
mkfs.fat -C -S 4096 -F 16 -n NX809J "$OUT" "$((SZ_MB * 1024))"

echo "==> populating"
mmd -i "$OUT" ::/extlinux
mcopy -i "$OUT" "$IMAGE"  ::/Image
mcopy -i "$OUT" "$DTB"    ::/nubia-nx809j.dtb
mcopy -i "$OUT" "$INITRD" ::/initramfs.cpio.gz
mcopy -i "$OUT" "$CONF"   ::/extlinux/extlinux.conf

echo "==> verifying"
python3 "$CHECK" "$OUT" 100663296 4096
mdir -i "$OUT" -/ ::
ls -lh "$OUT"
