# nubia-nx809j-linux

Build scripts for booting **mainline Linux** on the **Nubia Red Magic 11S Pro+ (NX809J)**,
SoC Qualcomm SM8850 (mainline codename: `kaanapali`).

This repo contains only the build glue. The kernel source lives in
[`code002-2/sm8850-mainline`](https://github.com/code002-2/sm8850-mainline)
(vanilla torvalds master + Adreno 840 GPU DT nodes + the NX809J board DT).

## What it builds

- `Image.gz` — arm64 kernel image
- `nubia-nx809j.dtb` — device tree blob
- `initramfs.cpio.gz` — minimal busybox initramfs (drops to a shell on the console)
- `boot.img` — Android v4 boot image ready for `fastboot boot` / `fastboot flash boot`

## CI

`.github/workflows/build.yml` runs on every push to `main` (and via `workflow_dispatch`).
It installs the aarch64 cross toolchain, clones the kernel repo, builds everything,
and uploads `boot.img` + `Image.gz` + the `.dtb` as an artifact.

> The kernel repo (`sm8850-mainline`) is private. The default `GITHUB_TOKEN` can
> only read public repos, so either (a) make `sm8850-mainline` public, or
> (b) add a PAT with `repo` scope as the secret `PAT_TOKEN` and edit the clone step
> to use `https://x-access-token:${{ secrets.PAT_TOKEN }}@github.com/code002-2/sm8850-mainline.git`.

## Build locally

```bash
# Prereqs (Debian/Ubuntu):
sudo apt install gcc-aarch64-linux-gnu build-essential bc bison cpio flex \
  kmod libelf-dev libssl-dev python3 git

# 1. kernel
git clone --depth 1 https://github.com/code002-2/sm8850-mainline.git kernel
cd kernel
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
cat ../config-fragment >> .config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" Image.gz dtbs
cd ..

# 2. initramfs
bash initramfs/build-initramfs.sh

# 3. boot.img
bash scripts/mkboot.sh \
  kernel/arch/arm64/boot/Image.gz \
  kernel/arch/arm64/boot/dts/qcom/nubia-nx809j.dtb \
  initramfs.cpio.gz \
  out/boot.img
```

## Boot on the device

Device must be bootloader-unlocked (already is). With the device in `fastboot` mode:

```bash
# Test without flashing (recommended first):
fastboot boot out/boot.img

# If it works and you want to keep it:
fastboot flash boot out/boot.img
fastboot reboot
```

Console is on **uart7** — hook a USB-TTL to the debug pads to see early boot.
Without a serial adapter, you can try `adb dmesg` once userspace is up.

## Layout

```
.github/workflows/build.yml   CI pipeline
config-fragment               kernel CONFIG_* appended to defconfig
initramfs/
  init                        /init shell script
  build-initramfs.sh          builds busybox + packs cpio.gz
scripts/
  mkboot.sh                   wraps AOSP mkbootimg -> boot.img
```

## Status / caveats

- **GPU (Adreno 840)**: DT nodes + driver in-tree; needs firmware
  `gen80200_gmu.bin` / `gen80200_sqe.fw` / `gen80200_aqe.fw` (copy from the device's
  `/vendor/lib/firmware/qcom/` into the initramfs `/lib/firmware/qcom/` to actually
  use the GPU; without it the GPU won't bind but the rest still boots).
- **Touchscreen** (Synaptics TCM on SPI19): no mainline driver — DT node exists
  but won't probe.
- **TLMM base address**: vendor DT uses `0x0f000000`, mainline `kaanapali.dtsi`
  uses `0x0f100000`. If first boot hangs before the console, this is the first
  thing to check in `arch/arm64/boot/dts/qcom/kaanapali.dtsi`.
- This is a first-boot bring-up image: console + UFS + USB + display panel should
  work; audio / GPU / touch / WiFi are best-effort.
