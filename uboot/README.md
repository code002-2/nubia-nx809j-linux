# U-Boot for the Nubia Red Magic 11S Pro+ (NX809J)

U-Boot port for the NX809J, built on top of
[`infiniti-mainline/u-boot`](https://github.com/infiniti-mainline/u-boot)'s
Kaanapali (SM8850) SoC support — the OnePlus 15 port. Same silicon, so the
clock driver, UFS PHY/host and hyp-SMMU work apply unchanged; only the board
identity, the panel geometry and the RAM map are NX809J-specific.

The port is built as an Android boot image and **chainloaded by the stock
ABL**, exactly like the mainline Linux kernel images this device already
boots. The stock bootloader chain is never modified.

## Why U-Boot

This board has no reachable console at all:

* no UART pads brought out, and `qcom_geni_serial.con_enabled=0` in the stock
  bootargs;
* the USB gadget (`ttyGS0`) never enumerates in a Linux bring-up kernel;
* a Linux DRM console needs MDSS/DPU plus its power domains and NoC paths.

U-Boot has a way around all of it. ABL leaves the DPU scanning out of the
splash buffer at `0xfc800000` (carve-out `0x2b00000`, confirmed in the device's
own devicetree as `reserved-memory/splash_region`), so U-Boot's
`CONFIG_VIDEO_SIMPLE` console just draws pixels into that buffer — **no display
driver needed**. `stdout`/`stderr` are pointed at `vidconsole` in
`board/qualcomm/nx809j.env`, and the banner and the bring-up dump appear on the
panel.

This is also why the mainline Linux DT must *not* use `simple-framebuffer`
(it stalls on `msm_mdss_enable()`/NoC); U-Boot never touches MDSS.

## Files

| Path | What it is |
| --- | --- |
| `arch/arm/dts/kaanapali-nubia-nx809j.dts` | board devicetree: `simple-framebuffer` for the console, GCC/TCSR/RPMh clock controllers, the apps SMMU and the UFS PHY + host |
| `configs/qcom_nx809j_defconfig` | `qcom_nx809j_defconfig` — the infiniti defconfig with the board DT and env file swapped |
| `board/qualcomm/nx809j.env` | default environment: video console routing and a bring-up `bootcmd` that dumps the ABL RAM map, clocks and UFS/GPT layout onto the panel |
| `pack.sh` | wraps `u-boot.bin` into a boot image (header v4, 4 KiB pages) |

These mirror the U-Boot tree layout so CI can drop them straight in with
`cp -av uboot/arch uboot/board uboot/configs uboot-src/`.

### The one deviation from the reference port: `/memory`

The reference port deliberately omits `/memory` so that U-Boot takes the RAM
ranges from the devicetree ABL passes in `x0`. That is the right default, but
it has a failure mode that is **invisible on this board**: if ABL ever starts
U-Boot without a usable devicetree, `qcom_parse_memory()` fails on both the
internal and the external tree and `board_fdt_blob_setup()` calls
`panic("No valid memory ranges found!")` — from `fdtdec_setup()`, long before
the video console exists. With `CONFIG_PANIC_HANG=y` that hangs the panel with
no output, which is indistinguishable from a dead port.

So the NX809J devicetree *does* carry a `/memory` node, with ranges copied
verbatim from this device's own ABL-supplied devicetree (read back from a stock
Android boot via `/sys/firmware/fdt`). They are ABL's real usable ranges, not a
guess: the `0x80000000`–`0x81960000` hypervisor/TZ carve-outs are already
subtracted. U-Boot still prefers this internal tree for control data, so if
ABL does pass its own devicetree the resulting map is the same either way.

The node must be named `memory` with no unit address — `qcom_parse_memory()`
looks it up as `fdt_path_offset(fdt, "/memory")`.

## Build

CI only (no aarch64 toolchain and no WSL on the dev machine):
`.github/workflows/uboot.yml` clones the pinned U-Boot ref, applies the three
files above, builds with ccache and uploads `uboot-nx809j-boot.img`.

Locally, with a toolchain:

```sh
make qcom_nx809j_defconfig
make -j"$(nproc)" CROSS_COMPILE="ccache aarch64-linux-gnu-"
bash uboot/pack.sh u-boot.bin out/uboot-nx809j-boot.img
```

## Flash

`vbmeta_a` must be flashed with verification disabled, otherwise AVB rejects
the unsigned image. `vbmeta_noverify.img` is an avbtool image with
`--flags 2` (verification disabled), 256 bytes.

```sh
fastboot flash boot_a   out/uboot-nx809j-boot.img
fastboot flash vbmeta_a vbmeta_noverify.img
fastboot reboot
```

Restore stock Android with the backups taken before the first flash
(`boot_a.img`, `vbmeta_a.img`, `init_boot_a.img` — `init_boot_a` carries the
KernelSU patch, so it must go back too):

```sh
fastboot flash boot_a     backup/boot_a.img
fastboot flash vbmeta_a   backup/vbmeta_a.img
fastboot flash init_boot_a backup/init_boot_a.img
```

`fastboot boot` is not implemented by this bootloader; only flashing works.

## Status / next steps

* [ ] Confirm the video console: banner plus the `bdinfo`/`clk dump`/`part list`
      dump on the panel.
* [ ] Pin down the ABL splash geometry (assumed 1216x2688, the panel's native
      mode, 32bpp `a8r8g8b8`).
* [ ] Boot a kernel: stage `Image`, the DTB and the initramfs plus
      `/extlinux/extlinux.conf` into an unused GPT partition (e.g. `boot_b`)
      and `sysboot` from there, mirroring the OnePlus 15 `ex01_esp` flow.
* [ ] Runtime DT fixups so the kernel gets a usable `/memory` and
      `/reserved-memory` (U-Boot can patch the tree it hands over, which is a
      much better place for that than the kernel's static DTS).
