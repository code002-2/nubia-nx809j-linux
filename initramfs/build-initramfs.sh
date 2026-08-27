#!/usr/bin/env bash
# Build a minimal busybox initramfs (initramfs.cpio.gz) for aarch64.
# Cross-compiles a static busybox, lays out a tiny rootfs, packs cpio+gzip.
set -euo pipefail

ARCH=aarch64
CROSS_COMPILE=aarch64-linux-gnu-
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
OUT=initramfs.cpio.gz

ROOT="$(pwd)/initramfs/rootfs"
WORK="$(pwd)/initramfs/work"
BB_SRC="$(pwd)/initramfs/busybox-src"

echo "==> [1/5] Fetch + cross-compile static busybox"
if [ ! -d "$BB_SRC" ]; then
  git clone --depth 1 -b 1_36_1 https://git.busybox.net/busybox "$BB_SRC"
fi

(
  cd "$BB_SRC"
  make ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" defconfig
  # enable static link
  sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
  echo 'CONFIG_STATIC=y' >> .config
  echo 'CONFIG_FEATURE_HAVE_RPC=n' >> .config
  make -j"$JOBS" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" busybox
)

BB="$BB_SRC/busybox"
file "$BB" | grep -q "$ARCH" || { echo "!! busybox build failed (wrong arch)"; exit 1; }

echo "==> [2/5] Lay out initramfs rootfs"
rm -rf "$ROOT" "$WORK"
mkdir -p "$ROOT"/{bin,sbin,usr/bin,usr/sbin,proc,sys,dev,dev/pts,tmp,etc,root,run}
install -m755 "$BB" "$ROOT/bin/busybox"

# hardlinks for common applets (can't run aarch64 busybox on x86 host, so use a list)
APPLETS="sh ash bash mount umount ls cat echo mkdir rmdir rm cp mv ln touch \
  ps kill sleep killall5 dmesg ip ifconfig route ping vi sed awk grep find tar \
  gzip gunzip zcat head tail wc env chmod chown chgrp id whoami hostname uname \
  reboot poweroff halt sync dd tr cut sort uniq test true false \
  setsid cttyhack clear reset stty setfont loadkmap mknod blkid df free \
  catv less more expr printf"
for applet in $APPLETS; do
  ln -sf /bin/busybox "$ROOT/bin/$applet" 2>/dev/null || true
done

# /init
install -m755 "$(pwd)/initramfs/init" "$ROOT/init"

# /etc/inittab fallback (not strictly needed; init script handles boot)
cat > "$ROOT/etc/inittab" <<'EOF'
::sysinit:/bin/mount -t proc none /proc
::sysinit:/bin/mount -t sysfs none /sys
::sysinit:/bin/mount -t devtmpfs none /dev
::sysinit:/bin/mount -t tmpfs none /tmp
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
console::respawn:/bin/sh
EOF

# minimal profile
cat > "$ROOT/etc/profile" <<'EOF'
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PS1='(initramfs) # '
export HOME=/root
export TERM=linux
alias ll='ls -la'
EOF

echo "==> [3/5] Install device firmware (Adreno 840 GPU)"
FW_REPO="${FW_REPO:-https://github.com/code002-2/nubia-nx809j-firmware.git}"
FW_DIR="$(pwd)/initramfs/firmware-src"
if [ ! -d "$FW_DIR/.git" ]; then
  git clone --depth 1 "$FW_REPO" "$FW_DIR"
fi
# Adreno 840 GPU firmware — names match the in-tree a6xx_catalog a840 entry
# (gen80200_gmu.bin / gen80200_sqe.fw / gen80200_aqe.fw). The kernel searches
# /lib/firmware/ and /lib/firmware/qcom/, so install to both.
mkdir -p "$ROOT/lib/firmware" "$ROOT/lib/firmware/qcom"
cp "$FW_DIR"/qcom/gen80200_gmu.bin "$FW_DIR"/qcom/gen80200_sqe.fw \
   "$FW_DIR"/qcom/gen80200_aqe.fw "$FW_DIR"/qcom/gen80200_zap.mbn \
   "$ROOT/lib/firmware/" 2>/dev/null || true
cp "$FW_DIR"/qcom/gen80200_* "$ROOT/lib/firmware/qcom/" 2>/dev/null || true
# NOTE: remoteproc (adsp/cdsp/modem .mdt+.bXX) and WiFi (peach) firmware are
# intentionally NOT included — they are large and need either a DT firmware-name
# change (.mbn -> .mdt) or ath12k layout conversion before they can load.

echo "==> [4/5] Pack cpio + gzip"
mkdir -p "$WORK"
cp -a "$ROOT"/. "$WORK"/
(
  cd "$WORK"
  find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9
) > "$OUT"

echo "==> [5/5] Done: $OUT ($(du -h "$OUT" | cut -f1))"
