#!/usr/bin/env bash
# Local one-shot build: kernel + initramfs + boot.img for Nubia NX809J.
# Mirrors what .github/workflows/build.yml does in CI.
#
# Prereqs (Debian/Ubuntu):
#   sudo apt install gcc-aarch64-linux-gnu build-essential bc bison cpio \
#     flex kmod libelf-dev libssl-dev python3 git
set -euo pipefail

KERNEL_REPO="${KERNEL_REPO:-https://github.com/code002-2/sm8850-mainline.git}"
KERNEL_REF="${KERNEL_REF:-main}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

ROOT="$(pwd)"
KDIR="$ROOT/kernel"
OUT="$ROOT/out"
DTB="$KDIR/arch/arm64/boot/dts/qcom/nubia-nx809j.dtb"
IMAGE="$KDIR/arch/arm64/boot/Image.gz"
INITRAMFS="$ROOT/initramfs.cpio.gz"

echo "==> [1/4] Clone/fetch kernel"
if [ ! -d "$KDIR/.git" ]; then
  git clone --depth 1 -b "$KERNEL_REF" "$KERNEL_REPO" "$KDIR"
else
  echo "    (kernel dir exists, skipping clone)"
fi

echo "==> [2/4] Build kernel (Image.gz + dtbs)"
cd "$KDIR"
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" defconfig
# Force-enable every =y from the fragment via scripts/config (raw append to
# .config is unreliable when a duplicate exists, see merge_config.sh notes).
while read -r line; do
  case "$line" in
    CONFIG_[A-Z0-9_]*=y)
      sym="${line%%=*}"; sym="${sym#CONFIG_}"
      ./scripts/config --file .config --enable "$sym";;
  esac
done < "$ROOT/config-fragment"
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS" olddefconfig
grep -E "^CONFIG_(DRM|BACKLIGHT|CLK_KAANAPALI|PINCTRL_|SCSI_UFS|USB_DWC3|PHY_QCOM)" .config | sort || true
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS" Image.gz dtbs
cd "$ROOT"

echo "==> [3/4] Build initramfs"
bash initramfs/build-initramfs.sh

echo "==> [4/4] Package boot.img"
mkdir -p "$OUT"
bash scripts/mkboot.sh "$IMAGE" "$DTB" "$INITRAMFS" "$OUT/boot.img"
cp "$IMAGE" "$OUT/"
cp "$DTB" "$OUT/"

echo
echo "============================================================"
echo " Done. Artifacts in out/:"
ls -lh "$OUT"
echo
echo " Test:   fastboot boot $OUT/boot.img"
echo " Flash:  fastboot flash boot $OUT/boot.img && fastboot reboot"
echo "============================================================"
