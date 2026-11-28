# Advan X1 Mainline Linux Port

Release-grade, upstream-oriented mainline Linux port for the **Advan X1**
(MediaTek **MT6789 / Helio G99**, 8 GiB RAM, 1080×2460 120 Hz VESA DSC panel)
maintained as a clean, email-ready patch series on pristine upstream Linux (no
kernel fork).

Boots at **Non-Secure EL2** via upstream U-Boot
([`advan-x1-uboot`](https://github.com/sysretq0/advan-x1-uboot), chainloaded
from stock LK via [Loki](https://github.com/sysretq0/loki) EL3+EL2) using a
SHA-256-verified **U-Boot FIT image** (`advan-x1.itb`) and **`boot.scr`**
(`bootflow scan -lb`).

- **Pinned upstream baseline:** [`666539eecdf4`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=666539eecdf4db623f5e3c3e55cf9f2dfad24aa4) (Linux v7.4-rc3/rc4)
- **Patch queue:** 134 upstream-ready patches across `patches/01-clk`..`07-dts`
- **Kernel config:** Release-grade Linux/mobile distro baseline (UCLAMP, MGLRU, PSI, Landlock, AppArmor, Yama, Lockdown, hardened allocator/usercopy, BBR, FQ_CODEL/CAKE, full inet nftables/xtables, WireGuard, zram/zswap, compressed firmware, BT LE/RFCOMM/BNEP, USB tethering/WWAN, HID peripherals)
- **Verification gates:** clean `git apply`, zero DTS drift, 0 `dtc` warnings, 0 `dt-validate` schema errors
- **Toolchain:** Google's pinned AOSP Clang (`clang-r614150`, matching `android-mainline`'s `bazel/constants.scl`) with `LLVM=1` on `x86_64` hosts/CI (`scripts/setup-google-clang.sh`). Non-`x86_64` hosts (such as `aarch64`) automatically default to host LLVM (`GOOGLE_CLANG=0`).
- **Initramfs PID 1:** Minimal dracut-shaped static `aarch64` C binary ([`initramfs/src/init.c`](initramfs/src/init.c)) with zero-mutation `/run/firmware` + `firmware_class.path` handoff, native `SYS_finit_module` (`insmod` + `modules.dep`) for curated `.ko` modules ([`config/initramfs-modules.txt`](config/initramfs-modules.txt)), automatic nested-GPT loop probing (`BLKSSZGET` + `LOOP_CONFIGURE` + `LO_FLAGS_PARTSCAN`, zero hardcoded offsets), and clean `switch_root` to `systemd`.

## Port Status (Release-Grade / Daily-Driver Complete)

See **[`docs/hardware-status.md`](docs/hardware-status.md)** for the full
subsystem matrix, driver/DT mapping, and hardware verification evidence.

- **Working (hardware-verified):**
  - **Core SoC, Power & Suspend:** full 8 GiB LPDDR4X RAM (~7.69 GiB usable,
    31 `no-map` carveouts), 8-core SMP + `mtk-cpufreq-hw`, 100% of MT6789 clock
    controllers & all 15 `M8` SCPSYS power domains, tuned `s2idle`
    suspend-to-RAM (PSCI `0x01011f01` + SPM deep sleep + MIPI-TX/DSI/SMI
    restore + WoWLAN-gated Wi-Fi IRQ wake + `InhibitDelayMaxSec=5` + hardened
    GNOME Mobile lockscreen PIN/resume flow), MT6366/MT6358 PMIC (regulators,
    keys, RTC, AUXADC, eFuse NVMEM, 4 PMIC thermal zones, vibrator), 13-zone
    LVTS thermal (+ STAGE3 HW reboot `113.5 °C` & board NTC), watchdog +
    reboot-mode, pstore/ramoops, and Loki persistent EL2/EL3 debug console.
  - **Display, GPU, Touch & Media:** native `mediatek-drm` 120 Hz DSC 1.1
    display + `disp_iommu` (0 M4U faults) + PWM backlight + GNOME Mobile
    Wayland desktop, Mali-G57 MC2 GPU (`panfrost` + MFGPLL devfreq), Chipone
    ICNL9922C SPI touchscreen, Awinic AW3641E rear LED flash/torch, hardware
    JPEG encoder (`/dev/video0`, `jpgenc@17030000`), and firmware-free
    stateless H.264 hardware video decoder (`/dev/video1`, `mtk-vdec-mt6789`,
    verified 170/170 frames 100% bit-exact up to 1080p, 0 IOMMU faults).
  - **Connectivity & Modem:** Wi-Fi STA/SoftAP Hotspot & native BlueZ
    Bluetooth `hci0` (MT6789 ConnSys `btif_drv`/`wmt_drv`/`wlan_drv_gen4m_6789`
    /`hci_mtk_stp` with post-suspend stability & P2P slot protection), Gen95
    CCCI cellular modem (4G LTE data `wwan0` + native VoLTE voice calls & IMS
    SMSoIP via `mtk-imsd` / `mtk_volte_rtp` / XFRM IPsec / ModemManager
    `mtk-ccci` IMS bridge).
  - **Storage, USB-C, Charging, Audio & Sensors:** UFS 2.2 storage, USB-C
    Host/OTG (`rt1711h` + `usb-role-switch` + VBUS boost) & ConfigFS gadget
    (NCM `usb0` `10.15.19.82/24` + ACM `/dev/ttyGS0`), SC89601A charger (18 W
    9 V/2 A USB-PD + 2.0 A non-PD fallback, verified 4%→92%), CW2217 fuel
    gauge, ASoC audio (`mt6789-mt6366`, FourSemi FS1599 amp with `-200 mB` cap,
    `mt6358-accdet`, UCM2 + PipeWire drop-ins), and SCP RV55 sensor hub (5 IIO
    sensors + native `in_mount_matrix` auto-rotate).
- **Optional / deferred hardware:** 2G/3G CS voice fallback live confirmation
  (`ccci_audd`, 4G VoLTE works), physical microSD card I/O test (`GPIO_ACTIVE_LOW`
  CD verified), NFC RF polling (`nfc0` probes, on hold), GPS (`gps_drv_stp`),
  cameras & ISP (`camsys` clocks/PDs ready), HEVC/VP9 VDEC & hardware VENC,
  SCP speaker DSP, and fingerprint reader.

## Documentation

| Document | Purpose |
|---|---|
| [`docs/hardware-status.md`](docs/hardware-status.md) | Authoritative hardware support matrix, driver/DT mapping, and verification evidence. |
| [`docs/upstream-status.md`](docs/upstream-status.md) | 134-patch upstream series layout, release-grade milestones, Tier A provenance, and `M2b` pre-LKML note. |
| [`docs/modem-full-bringup.md`](docs/modem-full-bringup.md) | Vendor-grounded full modem architecture (4G LTE data, VoLTE, IMS SMS, CS voice). |
| [`docs/ccci-sip-abi.md`](docs/ccci-sip-abi.md) | EL3 BL31 `MTK_SIP_KERNEL_CCCI_CONTROL` SMC ABI and Loki/LK modem handover contract. |
| [`docs/audio-speaker-protection.md`](docs/audio-speaker-protection.md) | FourSemi FS1599 speaker hardware protection analysis and `-200 mB` LINEOUT mitigation. |
| [`docs/archive/`](docs/archive/) | Historical bring-up plans and completed audits (ground-truth audit, 7.4 modernization, Phase B backlog, CCCI bring-up plan). |

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
