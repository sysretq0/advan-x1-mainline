# Advan X1 Mainline Linux Port

Upstream-oriented mainline Linux port for the **Advan X1** (MediaTek **MT6789 /
Helio G99**, 8 GiB RAM, 1080×2460 120 Hz VESA DSC panel) maintained as a clean,
email-ready patch series on pristine upstream Linux (no kernel fork).

Boots at **Non-Secure EL2** via upstream U-Boot
([`advan-x1-uboot`](https://github.com/sysretq0/advan-x1-uboot), chainloaded
from stock LK via Loki Stage-1) using a SHA-256-verified **U-Boot FIT image**
(`advan-x1.itb`) and **`boot.scr`** (`bootflow scan -lb`).

- **Pinned upstream baseline:** [`666539eecdf4`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=666539eecdf4db623f5e3c3e55cf9f2dfad24aa4) (Linux v7.4-rc3/rc4)
- **Patch queue:** 83 upstream-ready patches across `patches/01-clk`..`07-dts`
- **Verification gates:** clean `git apply`, zero DTS drift, 0 `dtc` warnings, 0 `dt-validate` schema errors
- **Toolchain:** Google's pinned AOSP Clang (`clang-r614150`, matching `android-mainline`'s `bazel/constants.scl`) with `LLVM=1` on `x86_64` hosts/CI (`scripts/setup-google-clang.sh`, cached under `toolchain/clang-r614150/`). Non-`x86_64` hosts (such as `aarch64`) automatically default to host LLVM (`GOOGLE_CLANG=0`).
- **Initramfs PID 1:** Minimal dracut-shaped static `aarch64` C binary ([`initramfs/src/init.c`](initramfs/src/init.c)) with zero-mutation `/run/firmware` + `firmware_class.path` handoff, native `SYS_finit_module` (`insmod` + `modules.dep`) for curated non-essential `.ko` modules ([`config/initramfs-modules.txt`](config/initramfs-modules.txt)) and extra kmods (`/lib/modules/extra/`), Option A native kernel nested-GPT loop probing (`BLKSSZGET` + `LOOP_CONFIGURE` + `LO_FLAGS_PARTSCAN`, zero hardcoded offsets), and clean `switch_root` to `systemd` (leaving `fstab`, `configfs`/`debugfs`, USB gadget, runtime PM, and diagnostics to `systemd`).

## Current Status at a Glance

See **[`docs/hardware-status.md`](docs/hardware-status.md)** for the full
subsystem matrix, driver/DT mapping, and hardware verification evidence.

- **Working (hardware-verified):** full 8 GiB LPDDR4X RAM (~7.69 GiB usable,
  31 `no-map` carveouts), 8-core SMP + `mtk-cpufreq-hw`, core/MMSYS/MFG clocks
  & SCPSYS power domains, MT6366/MT6358 PMIC (regulators, keys, RTC, AUXADC,
  vibrator), native `mediatek-drm` 120 Hz DSC display + `disp_iommu` (0 M4U
  faults) + PWM backlight + Wayland desktop, Mali-G57 MC2 GPU (`panfrost` +
  MFGPLL devfreq), Chipone ICNL9922C SPI touchscreen, UFS 2.2 storage, USB
  ConfigFS gadget (ACM `/dev/ttyGS0` + NCM `usb0`, including cold battery
  boot), SC89601A charger, CW2217 fuel gauge, 13-zone LVTS thermal, watchdog +
  reboot-mode, pstore/ramoops, Gen95 CCCI cellular modem (`mtk-ccci` + DPMAIF
  v1 + SMEM UL + `/dev/wwan0at0` + `wwan0` + ModemManager `mtk-ccci` plugin,
  live 4G LTE auto-connect & NTP sync verified), ASoC audio card
  (`mt6789-mt6366`, FourSemi FS1599 speaker amp `5-0034`, `mt6358-accdet`
  Headset Jack — probe/bind verified; UCM2 profiles included).
- **WIP / pending operator boot test (Phase A/B statically verified):**
  suspend-to-RAM (`s2idle` PSCI `0x01011f01` + SPM deep sleep + MIPI-TX/DSI
  cold-start resume & CRTC vblank drain fix), LVTS STAGE3 hardware thermal
  reboot (`113.5 °C`), CCCI 30 s HS1/HS2 timeout with FS-in-flight extension &
  DT-driven CCIF SRAM size, I²C `mt6789_compat` (`dma_sync=1`), hardened
  `/dev/mem` (`STRICT_DEVMEM` + `IO_STRICT_DEVMEM`), board NTC thermal zones
  (`ap-ntc-thermal` / `ltepa-ntc-thermal`), MT6366 DCXO `XO_NFC` clock buffer
  for ST21NFCD NFC (`nfc0` probes + IRQ 16 fires; live RF poll pending),
  MT6358/MT6366 PMIC eFuse NVMEM (`fixed-layout`) + 4 MT6366 PMIC thermal zones
  (`pmic-pmu/bk1/bk2/bk3-thermal`), ACCDET eFuse/moisture/CDD thresholds,
  FS1599 `-200 mB` LINEOUT mitigation, USB-C host/OTG/PD (`rt1711h`), live
  microSD card I/O test (`GPIO_ACTIVE_HIGH` CD), live audio playback/jack test,
  high-voltage charge pump.
- **Not yet started:** Wi-Fi / Bluetooth / GPS (MT6789 ConnSys/WMT & consys
  thermal), cameras & ISP, hardware video decode/encode (`vdec`/`venc`), SCP
  sensor hub (accel/gyro/mag/ALS) & SCP speaker DSP (`FS1599` Re25/F0),
  fingerprint reader.

## Documentation

| Document | Purpose |
|---|---|
| [`docs/hardware-status.md`](docs/hardware-status.md) | Authoritative matrix of what works, what is WIP, what is not yet started, and next milestones. |
| [`docs/upstream-status.md`](docs/upstream-status.md) | 82-patch series layout, completed upstream-deviation remediation summary, Tier A provenance, and remaining pre-LKML items. |
| [`docs/phase-b-backlog.md`](docs/phase-b-backlog.md) | Phase B modernization status (implemented upstream-able drivers/bindings and remaining deferred items with Tier A evidence). |
| [`docs/audio-speaker-protection.md`](docs/audio-speaker-protection.md) | FourSemi FS1599 speaker hardware protection analysis and Phase A `-200 mB` LINEOUT mitigation. |
| [`docs/ccci-bringup-plan.md`](docs/ccci-bringup-plan.md) | Phase 5 Gen95 cellular modem ground-truth spec (memory map, CCIF0, SCP CCISM, RO NVRAM FS RPC, DPMAIF v1) and progress. |
| [`docs/ccci-sip-abi.md`](docs/ccci-sip-abi.md) | EL3 BL31 `MTK_SIP_KERNEL_CCCI_CONTROL` SMC ABI and Loki/LK modem handover contract. |

## Repository Layout

- `patches/` — 83 email-ready patches organized by upstream subsystem
  (`01-clk`, `02-pinctrl`, `03-soc-power`, `04-peripherals`, `05-gpu`,
  `06-display`, `07-dts`), applied onto pristine upstream Linux at build time.
- `dts/` — `mt6789.dtsi` and `mt6789-advan-x1.dts` (kept byte-identical to
  `patches/07-dts/0002` and `0003` via `scripts/verify_patches.sh`).
- `config/` — kernel config fragments (`advan-x1-p1.configfrag` + opt-in
  `extra/` fragments such as `devmem-debug.configfrag`).
- `boot/` — U-Boot `boot.cmd.in`, plain-text `uEnv.txt.in` runtime override
  template, and optional `extlinux.conf.example`.
- `tools/` — `pack_uboot_boot.py` (FIT + `boot.scr` + `<=32 MiB` single-space
  or automatic 2-space split packager), `modemmanager-mtk-ccci/` (native MM
  1.24 plugin), `mtk-wwan-connect/`, and `ucm2/` (ALSA UCM2 profiles).
- `scripts/` — `verify_patches.sh` (clean-tree apply, DTS sync, `dtc`,
  `dt-validate`), `build.sh`, and `fetch_ci_bundle.sh`.
- `initramfs/` — standalone BusyBox initramfs with USB ACM (`ttyGS0`) + NCM
  gadget, `root=PARTLABEL=...` / `root=UUID=...` / `root=/dev/...` support,
  nested-GPT `xnu_data` fallback, and boot-time hardware diagnostics.
- `docs/` — hardware support status, upstream readiness, and CCCI specs.
- `legacy/` — pre-redo hack artifacts kept as reference. **Nothing here ships.**

## U-Boot Boot Architecture (<=32 MiB Single-Space or 2-Space Split)

`tools/pack_uboot_boot.py` targets a **32 MiB** Space 1 budget by default:

1. **Single-space mode (`<= 32 MiB`, default)**:
   - `gzip` FIT (`advan-x1.itb`): ~22.7 MiB (`Image.gz` + DTB + `initramfs.cpio.gz`)
   - `lzma` FIT (`--compression lzma`, 1 MiB dict): ~17.5 MiB (fits inside a
     32 MiB `esp` partition sharing space with existing `/EFI/` bootloaders)
   - `space1/` contains `/boot.scr`, `/uEnv.txt`, and `/advan-x1.itb` — drop
     directly onto a FAT32/ext4 boot partition (`linux_boot` 48 MiB or `esp`
     32 MiB) or flash `boot-space1.img`.
2. **Automatic 2-space split mode (`> 32 MiB`, or `--mode split`)**:
   - **Space 1 (`space1/`, ~4 KiB)**: `/boot.scr` + `/uEnv.txt` on the small
     boot/ESP partition (`linux_boot` or `esp`).
   - **Space 2 (`space2/`, overflow when `> 32 MiB`)**: `/advan-x1.itb` or
     `/boot/advan-x1.itb` on a second flat FAT32/ext4 partition (default
     `linux_boot` 48 MiB, fallback `esp` / `boot`). `boot.scr` discovers Space 2
     automatically by GPT partition label.
   - **Rootfs**: defaults to the nested-GPT Debian rootfs on `xnu_data`;
     other partitions are left untouched unless targeted via
     `extra_bootargs=root=...` in `/uEnv.txt`.

Runtime kernel cmdline overrides (e.g. `extra_bootargs=noswitch` or
`extra_bootargs=root=PARTLABEL=rootfs rootfstype=ext4`) can be edited in plain
text in `/uEnv.txt` on Space 1 without rebuilding `boot.scr` or the FIT image.

## Verification & Policy

Run the full clean-tree verification gate before every commit:

```bash
./scripts/verify_patches.sh
```

- Every peripheral address, IRQ, clock ID, and display parameter must trace to
  Advan X1 stock artifacts (`runtime_merged.dts` / stock `.ko` / `mp01-kernel`)
  — no guesses borrowed from unrelated boards.
- Hardware support claims require device telemetry; unverified paths stay
  recorded as WIP or unverified.
- All commits must pass `verify_patches.sh` + `checkpatch.pl` and be GPG-signed.
