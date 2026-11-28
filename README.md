# Advan X1 Mainline Linux Port

Upstream-oriented mainline Linux port for the **Advan X1** (MediaTek **MT6789 /
Helio G99**, 8 GiB RAM, 1080×2460 120 Hz VESA DSC panel) maintained as a clean,
email-ready patch series on pristine upstream Linux (no kernel fork).

Boots at **Non-Secure EL2** via upstream U-Boot
([`advan-x1-uboot`](https://github.com/sysretq0/advan-x1-uboot), chainloaded
from stock LK via Loki Stage-1) using a SHA-256-verified **U-Boot FIT image**
(`advan-x1.itb`) and **`boot.scr`** (`bootflow scan -lb`).

- **Pinned upstream baseline:** [`666539eecdf4`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=666539eecdf4db623f5e3c3e55cf9f2dfad24aa4) (Linux v7.4-rc3/rc4)
- **Patch queue:** 134 upstream-ready patches across `patches/01-clk`..`07-dts`
- **Verification gates:** clean `git apply`, zero DTS drift, 0 `dtc` warnings, 0 `dt-validate` schema errors
- **Toolchain:** Google's pinned AOSP Clang (`clang-r614150`, matching `android-mainline`'s `bazel/constants.scl`) with `LLVM=1` on `x86_64` hosts/CI (`scripts/setup-google-clang.sh`, defaulting to `../toolchain/clang-r614150/` via `TOOLCHAIN_ROOT`, or `$GITHUB_WORKSPACE/toolchain/clang-r614150/` in CI). Non-`x86_64` hosts (such as `aarch64`) automatically default to host LLVM (`GOOGLE_CLANG=0`).
- **Initramfs PID 1:** Minimal dracut-shaped static `aarch64` C binary ([`initramfs/src/init.c`](initramfs/src/init.c)) with zero-mutation `/run/firmware` + `firmware_class.path` handoff, native `SYS_finit_module` (`insmod` + `modules.dep`) for curated non-essential `.ko` modules ([`config/initramfs-modules.txt`](config/initramfs-modules.txt)), automatic nested-GPT loop probing (`BLKSSZGET` + `LOOP_CONFIGURE` + `LO_FLAGS_PARTSCAN`, zero hardcoded offsets), and clean `switch_root` to `systemd` (leaving `fstab`, `configfs`/`debugfs`, USB gadget, runtime PM, and diagnostics to `systemd`).

## Current Status at a Glance

See **[`docs/hardware-status.md`](docs/hardware-status.md)** for the full
subsystem matrix, driver/DT mapping, and hardware verification evidence.

- **Working (hardware-verified):** full 8 GiB LPDDR4X RAM (~7.69 GiB usable,
  31 `no-map` carveouts), 8-core SMP + `mtk-cpufreq-hw`, 100% of MT6789 clock
  controllers & all 15 `M8` SCPSYS power domains, suspend-to-RAM (`s2idle` PSCI
  `0x01011f01` + SPM deep sleep + MIPI-TX/DSI/SMI resume), MT6366/MT6358 PMIC
  (regulators, keys, RTC, AUXADC, eFuse NVMEM, 4 PMIC thermal zones, vibrator),
  native `mediatek-drm` 120 Hz DSC display + `disp_iommu` (0 M4U faults) + PWM
  backlight + GNOME Mobile Wayland desktop, Mali-G57 MC2 GPU (`panfrost` +
  MFGPLL devfreq), Chipone ICNL9922C SPI touchscreen, UFS 2.2 storage, USB-C
  Host/OTG (`rt1711h` + `usb-role-switch` + VBUS boost) & ConfigFS gadget (NCM
  `usb0` `10.15.19.82/24` + ACM `/dev/ttyGS0`), SC89601A charger (18 W 9 V/2 A
  USB-PD + 2.0 A non-PD wall-charger fallback, verified 4%→92%),
  CW2217 fuel gauge, 13-zone LVTS thermal (+ STAGE3 HW thermal reboot `113.5 °C`
  & board NTC zones), watchdog + reboot-mode, pstore/ramoops, Wi-Fi STA/AP &
  native Bluetooth `hci0` (MT6789 ConnSys `btif_drv`/`wmt_drv`/`wlan_drv_gen4m_6789`
  /`hci_mtk_stp`), Gen95 CCCI cellular modem (4G LTE data `wwan0` + native VoLTE
  voice calls & IMS SMSoIP via `mtk-imsd` / `mtk_volte_rtp` / XFRM IPsec /
  ModemManager `mtk-ccci` IMS bridge), ASoC audio (`mt6789-mt6366`, FourSemi
  FS1599 amp with `-200 mB` cap, `mt6358-accdet`, UCM2 + PipeWire drop-ins),
  SCP RV55 sensor hub (5 IIO sensors + native `in_mount_matrix` auto-rotate),
  Awinic AW3641E rear camera LED flash/torch (`/sys/class/leds/white:flash`),
  and hardware JPEG encoder (`/dev/video0`, `jpgenc@17030000` + SMI
  `larb4`/`larb7` + M4U Region 1).
- **WIP / pending live verification:** 2G/3G CS voice fallback (`ccci_audd`),
  live microSD card I/O test (`GPIO_ACTIVE_LOW` CD), and firmware-free
  stateless H.264 hardware video decoder (`mtk-vdec-mt6789` on
  `vdec@16000000`).
- **On hold / not yet started:** NFC (`st21nfc` — on hold; `nfc0` probes and
  IRQ 16 fires, RF polling deferred), GPS (`gps_drv_stp` over ConnSys), cameras
  & ISP (`camsys` clocks/PDs ready), HEVC/VP9 VDEC & hardware video encode
  (`venc`), SCP speaker DSP (`FS1599` Re25/F0), fingerprint reader.

## Documentation

| Document | Purpose |
|---|---|
| [`docs/hardware-status.md`](docs/hardware-status.md) | Authoritative matrix of what works, what is WIP, what is not yet started, and next milestones. |
| [`docs/upstream-status.md`](docs/upstream-status.md) | 131-patch series layout, completed upstream-deviation & 7.4 modernization summary, Tier A provenance, and remaining pre-LKML items. |
| [`docs/modem-full-bringup.md`](docs/modem-full-bringup.md) | Vendor-grounded full modem architecture (Internet → Call → SMS → VoLTE) and Wave 1/2 implementation map. |
| [`docs/ccci-bringup-plan.md`](docs/ccci-bringup-plan.md) | Gen95 cellular modem ground-truth spec (memory map, CCIF0, SCP CCISM, RO NVRAM FS RPC, DPMAIF v1). |
| [`docs/ccci-sip-abi.md`](docs/ccci-sip-abi.md) | EL3 BL31 `MTK_SIP_KERNEL_CCCI_CONTROL` SMC ABI and Loki/LK modem handover contract. |
| [`docs/audio-speaker-protection.md`](docs/audio-speaker-protection.md) | FourSemi FS1599 speaker hardware protection analysis and `-200 mB` LINEOUT mitigation. |
| [`docs/phase-b-backlog.md`](docs/phase-b-backlog.md) | Phase B driver/binding status and deferred hardware items with Tier A evidence. |
| [`docs/7.4-modernization-plan.md`](docs/7.4-modernization-plan.md) | Linux 7.4 API modernization execution record and verification gates. |

## Repository Layout

- `patches/` — 134 email-ready patches organized by upstream subsystem
  (`01-clk`, `02-pinctrl`, `03-soc-power`, `04-peripherals`, `05-gpu`,
  `06-display`, `07-dts`), applied onto pristine upstream Linux at build time.
- `dts/` — `mt6789.dtsi` and `mt6789-advan-x1.dts` (kept byte-identical to
  `patches/07-dts/0002` and `0003` via `scripts/verify_patches.sh`).
- `config/` — kernel config fragments (`advan-x1-p1.configfrag` + opt-in
  `extra/` fragments such as `devmem-debug.configfrag`).
- `boot/` — U-Boot `boot.cmd.in`, plain-text `uEnv.txt.in` runtime override
  template, and optional `extlinux.conf.example`.
- `tools/` — `pack_uboot_boot.py` (FIT + `boot.scr` + `<=32 MiB` single-space
  or automatic 2-space split packager), `ccci_fsd/` & `ccci_rpcd/` (read-only
  NVRAM overlay modem daemons), `modemmanager-mtk-ccci/` (ModemManager plugin
  built against target Debian sid/forky `modemmanager`), `mtk-wwan-connect/`
  (standalone WWAN fallback), `ucm2/` (ALSA UCM2 HiFi + VoiceCall profiles),
  and `pipewire/` (PipeWire/WirePlumber MT6789 AFE latency drop-ins).
- `rootfs/advan-x1-support/` — packaged device defaults (USB ConfigFS gadget
  service + `systemd-networkd` `usb0` config, `zram-generator.conf`, journald,
  logind, udev rules, GNOME Mobile gschema overrides).
- `scripts/` — `verify_patches.sh` (clean-tree apply, DTS sync, `dtc`,
  `dt-validate`), `build.sh`, `build-initramfs.sh`, `build-userspace-tools.sh`,
  `build-gnome-mobile.sh`, `setup-google-clang.sh`, and `fetch_ci_bundle.sh`.
- `initramfs/` — minimal static C PID 1 initramfs with standard `root=`
  resolution (`PARTLABEL=`, `PARTUUID=`, `UUID=`, `LABEL=`, `/dev/...`,
  `/dev/disk/by-*`), nested-GPT loop attach, and BusyBox rescue shell on
  failure / `rd.break` / `noswitch`.
- `docs/` — hardware support status, upstream readiness, audio, and modem specs.
- `legacy/` — historical pre-redo artifacts kept as reference. **Nothing here ships.**

## U-Boot Boot Architecture (<=32 MiB Single-Space or 2-Space Split)

`tools/pack_uboot_boot.py` targets a **32 MiB** Space 1 budget by default:

1. **Single-space mode (`<= 32 MiB`, default)**:
   - `gzip` FIT (`advan-x1.itb`): ~22.7 MiB (`Image.gz` + DTB + `initramfs.cpio.gz`)
   - `lzma` FIT (`--compression lzma`, 1 MiB dict): ~17.5 MiB (fits inside a
     32 MiB `esp` partition sharing space with existing `/EFI/` bootloaders)
   - `space1/` contains `/boot.scr`, `/uEnv.txt`, and `/advan-x1.itb` — drop
     directly onto a FAT32/ext4 boot partition (`linux_boot` or `esp`) or flash
     `boot-space1.img`.
2. **Automatic 2-space split mode (`> 32 MiB`, or `--mode split`)**:
   - **Space 1 (`space1/`, ~4 KiB)**: `/boot.scr` + `/uEnv.txt` on the small
     boot/ESP partition (`linux_boot` or `esp`).
   - **Space 2 (`space2/`, overflow when `> 32 MiB`)**: `/advan-x1.itb` or
     `/boot/advan-x1.itb` on a second flat FAT32/ext4 partition (default
     `linux_boot`, fallback `esp` / `boot`). `boot.scr` discovers Space 2
     automatically by GPT partition label.
   - **Root filesystem**: specify your target Linux partition via
     `extra_bootargs=root=...` in `/uEnv.txt` on Space 1 (e.g. `root=UUID=...`,
     `root=PARTUUID=...`, or `root=PARTLABEL=linux`), or label your Linux
     partition `linux` / `root` for automatic discovery. Both direct
     filesystems and nested-GPT OS disk images are supported.

Runtime kernel cmdline overrides (e.g. `extra_bootargs=noswitch` or
`extra_bootargs=root=PARTLABEL=linux rootfstype=ext4`) can be edited in plain
text in `/uEnv.txt` on Space 1 without rebuilding `boot.scr` or the FIT image.

## Verification & Policy

Run the full clean-tree verification gate before every commit:

```bash
./scripts/verify_patches.sh
```

- Every peripheral address, IRQ, clock ID, and display parameter must trace to
  Advan X1 stock artifacts (`runtime_merged.dts` / stock `.ko` / MT6789 SoC
  reference) — no guesses borrowed from unrelated boards.
- Hardware support claims require device telemetry; unverified paths stay
  recorded as WIP or unverified.
- All commits must pass `verify_patches.sh` + `checkpatch.pl` and be GPG-signed.
