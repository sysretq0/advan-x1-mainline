# Advan X1 (MT6789 / Helio G99) — Hardware Support & Bring-Up Status

**Last hardware-verified commit:** `b1a3ae6` (198.6 s Wayland session, clean reboot; current HEAD `0d9a170` adds only `priv->dsc = NULL` cleanup and patch email update)  
**Upstream kernel baseline:** Linux v7.4-rc3/rc4 (`666539eecdf4db623f5e3c3e55cf9f2dfad24aa4`)  
**Boot chain:** Stock LK → Loki Stage-1 (EL1) → [`advan-x1-uboot`](https://github.com/sysretq0/advan-x1-uboot) (EL2) → U-Boot FIT (`advan-x1.itb`)

Status legend:
- **Working** — Hardware-verified on ADVAN X1 via pstore/ramoops and/or live session testing.
- **WIP** — Driver/DT present and probing, or partially brought up; missing feature, routing, or end-to-end functional test.
- **Not started** — Hardware exists on the board (`runtime_merged.dts`), not yet implemented in mainline DTS/drivers.

---

## Summary Matrix

| Subsystem | Hardware / IC | Mainline Driver / DT Node | Status | Notes |
|---|---|---|---|---|
| **CPU / SMP** | MT6789 (2×A76 + 6×A55) | PSCI + `mtk-cpufreq-hw` | **Working** | 8 CPUs online (`2.0 GHz` little, `2.2 GHz` big) |
| **RAM & pstore** | 8 GiB LPDDR4X (`7.69 GiB` usable in stock) | `memory@40000000` + 31 `no-map` carveouts, `ramoops@48090000` | **WIP** | Full ~7.69 GiB map + U-Boot `ARCH_FIXUP_FDT_MEMORY=n` landed; pending HW boot check (was ~3.88 GiB on `b1a3ae6`); ramoops works |
| **Clocks** | `apmixedsys`, `topckgen`, `infra_ao`, `imp_iic_wrap`, `mmsys`, `mfgcfg` | `clk-mt6789-*` (`patches/01-clk/`) | **Working** | 0 HW-on clock leaks; `clk_disable_unused` clean |
| **Power Domains** | SCPSYS (`md`, `mfg0..3`, `disp`, `audio`) | `mtk-pm-domains` (`10006000.syscon`) | **Working** | `mfg0/1` regulator-coupled; 0 deferred probes |
| **Pinctrl / GPIO / EINT** | 188 pins + EINT | `pinctrl-mt6789` (`10005000.pinctrl`) | **Working** | Verified pinmux, active-low resets, EINT IRQs |
| **PMIC & Regulators** | MT6366 (MT6358 family) | `mtk-pmic-wrap` (`10026000.pwrap`) + `mt6358-regulator` | **Working** | MT6789 pwrap register map; regulators, RTC, AUXADC |
| **Display (DRM/KMS)** | Chipone ICNL9922C 1080×2460 120 Hz DSC 1.1 | `mediatek-drm` + `panel-chipone-icnl9922c` + `tps65132` | **Working** | Full DDP + DSC 1.1; 0.8 ms LK handover; DPMS off/on; Wayland |
| **Display IOMMU (M4U)** | MT6789 `disp_iommu` + SMI `larb0`/`larb1` | `mtk-iommu` (`14016000.iommu`) + `mtk-smi` | **Working** | **0 M4U faults**, **0 swiotlb errors** across full session |
| **Backlight** | MT6789 DISP PWM | `pwm-mtk-disp` (`1100e000.pwm`) + `pwm-backlight` | **Working** | `39385 ns` (25.4 kHz), 113-step curve, full 0–100% range |
| **3D GPU** | Arm Mali-G57 MC2 | `panfrost` (`13000000.gpu`) | **Working** | Live MFGPLL devfreq (mux-park notifier), 0 GPU faults |
| **Touchscreen** | Chipone ICNL9922C SPI TDDI | `chipone-icnl9922c` (`spi0.0`) | **Working** | 71,792 B SRAM boot (`fwver 0x0104`), `drm_panel_follower` sync |
| **Internal Storage** | UFS 2.2 | `ufshcd-mtk` (`11270000.ufshci`) | **Working** | 7 clocks, 3 resets, `vemc` supply, Debian rootfs mount |
| **USB Gadget (Device)** | MUSB + MT6789 USB2 T-PHY | `musb-hdrc` (`11200000.usb`) + `phy-mtk-tphy` (`11f40000.t-phy`) | **Working** | ConfigFS ACM (`/dev/ttyGS0`) + NCM (`usb0`); cold battery boot fixed |
| **Charging (Standard)** | Southchip SC89601A | `bq25890` (`&i2c5` / `4-006a`) | **Working** | USB charging + status; D+/D- gadget coexistence fixed |
| **Battery Fuel Gauge** | CellWise CW2217 | `cw2217` (`&i2c0` / `0-0064`) | **Working** | 80-byte board profile download; live V/I/SOC/temp/cycles |
| **Thermal** | MT6789 LVTS (13 zones) | `mtk-lvts-thermal` (`1100b000`) | **Working** | eFuse calibration + V5 manual RCK; all 13 zones reading |
| **Watchdog & Reboot** | MT6789 TOPRGU | `mtk-wdt` (`10007000`) + `syscon-reboot-mode` | **Working** | Clean `reboot: Restarting system`; bootloader/recovery modes |
| **Keys & Haptics** | Power/Vol keys + vibrator | `mtk-pmic-keys`, `mt6779-keypad`, `regulator-haptic` | **Working** | HW long-press reset fixed; `vibr` regulator haptic registered |
| **Buses (I²C / SPI / UART)** | `i2c0..i2c7`, `spi0`, `uart0` | `i2c-mt65xx` (`mt8188_compat`), `spi-mt65xx`, `8250_mtk` | **Working** | Earlycon on `uart0`; all board I²C/SPI clients active |
| **Cellular Modem** | MT6789 Gen95 Modem (`md1img`) | `mtk-ccci` (`d0cf000.modem`, `drivers/net/wwan/mtk_ccci/`) | **WIP** | MD BootROM passes (`0x5443000c/0x53320000`); HS1 timeout next |
| **Audio** | MT6789 AFE + MT6366 codec (+ ext amp) | `mt6789-mt6366-sound` (`11210000.audio-controller`) | **WIP** | ALSA card `#1` probes; `speaker_amp@34` & `accdet` not wired |
| **NFC** | STMicro ST21NFCD | `st-nci-i2c` (`&i2c3` / `nfc@8`) | **WIP** | `nfc0` registers + IRQ 16 fires; live RF tag poll pending |
| **USB-C Host / OTG / PD** | Richtek RT1711H TCPC | `rt1711h` (`&i2c5` / `4-004e`) | **WIP** | Sink/device detection works; host VBUS boost & PD swap not wired |
| **microSD Card** | MT6789 MSDC1 | `mtk-msdc` (`11240000.mmc`, `GPIO 7` CD) | **WIP** | Controller + CD GPIO probe cleanly; live card I/O test pending |
| **Fast Charging** | SC8547 charge pump (`i2c7@6a`) + PE | — | **WIP** | Standard SC89601A charging works; HV charge pump not implemented |
| **Wi-Fi / Bluetooth / GPS** | MT6789 ConnSys (`consys@18002000`) | — | **Not started** | No mainline MT6789 ConnSys/WMT driver |
| **Cameras & Flash** | `camsys` / `imgsensor` / `aw3641e` / `lm3643` | — | **Not started** | Needs `camsys`/`imgsys` clocks, `M8` power domains, V4L2 stack |
| **Video Codec (HW)** | `vdecsys` / `vencsys` / `mdpsys` | — | **Not started** | Needs multimedia clock controllers & `M8` power domains |
| **Sensors (IMU/ALS/Prox)** | SCP RV55 sensor hub (`scp`) | — | **Not started** | Needs `mtk-scp` remoteproc + IIO sensor bridge |
| **Fingerprint** | Goodix (`goodix-fp` SPI5) / FocalTech | — | **Not started** | Not wired in DTS or driver stack |
| **Suspend-to-RAM (S3)** | SPM deep sleep | — | **Not started** | Runtime PM & DPMS work; full S3 suspend/resume unvalidated |

---

## 1. Working (Hardware-Verified)

Verified on physical ADVAN X1 hardware through `3b1f69f` → `e204148` → `b1a3ae6`
(198.63 s interactive Wayland session, `deferred=none`, 0 M4U faults, 0 Panfrost
faults, clean `reboot: Restarting system`):

- **Core SoC, SMP & CPUfreq:** All 8 cores online (`policy0`: 6× Cortex-A55
  500–2000 MHz, `policy6`: 2× Cortex-A76 725–2200 MHz via `mtk-cpufreq-hw`),
  SoC identity exported via `mtk-socinfo` (`MediaTek Helio G99 / MT6789`).
- **Clocks & Power Domains:** All active clock controllers (`apmixedsys`,
  `topckgen`, `infra_ao`, `imp_iic_wrap`, `mmsys`, `mfgcfg`) operate without
  `clk_ignore_unused` or `pd_ignore_unused` and with zero leaked HW-on clocks
  in `clk_summary`. SCPSYS manages `md`, `mfg0`–`mfg3`, `disp`, and `audio`
  power domains with `mfg0` (`vsram_gpu`) and `mfg1` (`vgpu`) regulator-coupled.
- **Display, IOMMU & Backlight:** Full `mediatek-drm` DDP pipeline
  (`OVL0 → RDMA0 → COLOR0 → CCORR0 → AAL0 → GAMMA0 → POSTMASK0 → DITHER0 →
  DSC0 → DSI0`) drives the Chipone ICNL9922C 1080×2460 4-lane VESA DSC 1.1
  panel (60/90/120 Hz) powered by `tps65132` (`i2c6@0x3e`). Active LK splash
  DMA is halted cleanly in `mtk_disp_ovl_probe()`, giving 0.8 ms DSI bind and
  **zero M4U faults** on `disp_iommu` (`14016000.iommu`, `larb0`/`larb1`) with
  zero `swiotlb` buffer exhaustion. DPMS screen off/on and `disp_pwm0`
  backlight (`39385 ns` / 25.4 kHz, 113 levels) work reliably.
- **3D GPU (Panfrost):** Mali-G57 MC2 (`13000000.gpu`, id `0x9093`) runs
  accelerated Wayland/GNOME sessions with live `MFGPLL` devfreq scaling
  protected by a `CLK_TOP_MFG_PLL_SEL` mux-park notifier and spinlock-guarded
  `clk-mt6789-mfgcfg` HW readback.
- **Touchscreen:** Clean mainline `chipone-icnl9922c` SPI driver (`spi0.0`,
  `patches/04-peripherals/0019`) boots the 71,792-byte volatile SRAM firmware
  (`fwid=0x9965`, `fwver=0x0104`), reports `ABS_MT_TOUCH_MAJOR` and physical
  mm dimensions, and synchronizes reset/sleep via `drm_panel_follower`.
- **Storage:** UFS 2.2 (`11270000.ufshci`) mounts the Debian rootfs with full
  7-clock + 3-reset control and `mt6366_vemc_reg` supply (no regulator late
  cleanup dropouts).
- **USB Gadget & Power:** MUSB (`11200000.usb`) + MT6789 USB2 T-PHY
  (`11f40000.t-phy`) with cold-boot wakeup and OTG session forcing enumerates
  ConfigFS ACM (`/dev/ttyGS0`) and NCM (`usb0`, `10.15.19.82/24`) even when
  booted unplugged on battery. Southchip SC89601A (`bq25890`, `4-006a`) and
  CellWise CW2217 (`0-0064`, 80-byte profile programmed) report live battery
  telemetry without interfering with USB D+/D-.
- **PMIC, Thermal, Watchdog & Input:** MT6366/MT6358 via MT6789 `pwrap`
  register map provides regulators, RTC (`rtc0`), AUXADC (`iio:device0`),
  `mtk-pmic-keys` (with fixed HW long-press reset), `mt6779-keypad`, and
  `regulator-haptic` vibrator. All 13 LVTS thermal zones read cleanly with
  eFuse calibration and V5 manual RCK; `mtk-wdt` + `syscon-reboot-mode` handle
  watchdogs and clean reboots.

---

## 2. Work in Progress (Partially Working / Pending Verification)

### 2.1 Full 8 GiB RAM Exposure (Implemented, Pending HW Verification)
- **What was previously verified (`b1a3ae6`):** Linux booted cleanly with
  35-bit PA / 34-bit IOVA and 896 KiB persistent pstore/ramoops (`0x48090000`),
  seeing **3,975 MiB (~3.88 GiB / 4 GiB)** (`Memory: 3796964K/4070400K available`)
  because `advan-x1-uboot` (`CONFIG_ARCH_FIXUP_FDT_MEMORY=y`, 2 banks)
  overwrote `memory@61000000` with two conservative windows (`240 MiB` +
  `3,735 MiB`).
- **What just landed:**
  - `advan-x1-uboot`: disabled `CONFIG_ARCH_FIXUP_FDT_MEMORY` so U-Boot stops
    clobbering the kernel FIT DTB `/memory` node (matching upstream MediaTek
    U-Boot boards), while expanding U-Boot's own clean high-DRAM banks to 3
    banks (~5.23 GiB for UEFI) with zero secure holes mapped.
  - `advan-x1-mainline`: replaced the 2 narrow memory nodes with full 8 GiB
    `memory@40000000` (`0x40000000..0x23fffffff`) plus all **31 stock
    `RES[0..30]` `no-map` carveouts** (`313.55 MiB`, exact `diff = 0x0` vs
    `pl_lk.txt` → **7,878.45 MiB / ~7.69 GiB** usable RAM).
- **Next step:** Flash rebuilt U-Boot (`lk_b` / inactive slot via Loki) +
  rebuilt mainline FIT image and confirm `Memory: ~7.5G / ~7.69G` and
  `0` SErrors/M4U faults on hardware.

### 2.2 Cellular Modem (Gen95 CCCI — Phase 5)
- **What works now:** Phase 5.1 (9 Advan X1 `no-map` reserved-memory carveouts,
  MD power domain with `srclken-o1`/`power-flow-config`, `modem` DT node) is
  complete. Phase 5.2 core (`drivers/net/wwan/mtk_ccci/`,
  `patches/04-peripherals/0026`) parses LK `ccci_tag_mem` (`tags=27`,
  `md1_verified=1`), reads `md1rom` (`0xdc000000`) and `md1dsp` (`0xe3200000`)
  after Loki EMI MPU remap, cycles MD MTCMOS safely, writes the CCIF0 SRAM
  `"rtbn"` footer, invokes `MTK_SIP_KERNEL_CCCI_CONTROL` (`MD_KERNEL_BOOT_UP`),
  and gets **MD BootROM pass** (`boot_status=0x5443000c / 0x53320000`).
- **Current blocker:** CCIF0 **HS1 handshake times out at 5 s** (`hs1=0`,
  `AP_CCIF0` `rch=0`).
- **Remaining:** Finish Phase 5.2 (HS1/HS2 + Emulated SCP CCISM ring at
  `0x8e810000` so MD stays alive >120 s), Phase 5.3 (read-only ext4 + volatile
  tmpfs NVRAM FS RPC + `WWAN_PORT_AT`), Phase 5.4 (DPMAIF v1 `wwan0` IP data
  plane), Phase 5.5 (ModemManager). Full hardware spec:
  [`docs/ccci-bringup-plan.md`](ccci-bringup-plan.md) &
  [`docs/ccci-sip-abi.md`](ccci-sip-abi.md).

### 2.3 Audio (`mt6789-mt6366-sound`)
- **What works now:** `11210000.audio-controller` (MT6789 AFE), `mt6358-sound`
  PMIC codec, and `mt6789-mt6366-sound` machine driver probe cleanly and
  register ALSA card `#1: mt6789-mt6366` with PCM playback/capture devices.
- **What's missing / unverified:**
  - Stock DT routes the loudspeaker through an external I²S amplifier
    (`speaker_amp@34` on `i2c6`, `mediatek,spk-i2s = <3 0>`), which is not yet
    identified/wired in mainline DT.
  - Headset jack insertion/button detection (`mt6358-accdet`) is not yet wired.
  - End-to-end playback (earpiece/headphone/speaker) and microphone capture
    require live hardware verification and UCM2 profiles.

### 2.4 NFC (`st,st21nfcb-i2c`)
- **What works now:** `nfc@8` on `&i2c3` registers `nfc0` via mainline
  `st-nci-i2c`, configures IRQ `GPIO 1` and reset `GPIO 106`, and fires
  hardware IRQ 16 (`mt-eint 1 Level st_nci`, `count=1` on probe).
- **What's pending:** Live RF tag discovery (`nfctool -d nfc0 -p` / `neard`) to
  confirm PMIC DCXO `XO_NFC` clock buffer remains available under active RF
  polling without the vendor `clkbuf` driver.

### 2.5 USB-C Host / OTG / Power Delivery
- **What works now:** `rt1711h` (`&i2c5` / `4-004e`) registers `port0` in
  sink/device mode; MUSB gadget (ACM + NCM) works on wall and battery boots.
- **What's missing:** Wiring a real `vbus-supply` (charger OTG boost), dual-role
  power/data switching (`DR_SWAP` / `PR_SWAP`), and host-mode peripheral tests
  (USB flash drive / HID).

### 2.6 microSD Card & Fast Charging
- **microSD (`11240000.mmc`):** Controller and CD `GPIO 7` (`in hi ACTIVE LOW`
  when empty) probe cleanly; live read/write verification with a physical SD
  card inserted is pending.
- **Fast Charging:** Standard SC89601A charging works; high-voltage charge pump
  (`sc8547` on stock `i2c7@0x6a`) and Pump Express negotiation are not
  implemented.

---

## 3. Not Yet Started / Unsupported

The following hardware blocks exist in the stock Advan X1 DT
(`runtime_merged.dts`) and have no mainline implementation yet:

1. **Wi-Fi, Bluetooth & GPS (`consys@18002000`, `wifi@18000000`, `gps@18c00000`):**
   MT6789 integrated ConnSys/WMT subsystem has no upstream Linux driver.
2. **Cameras, ISP & LED Flash (`camsys`, `imgsensor` on `i2c2`/`i2c4`/`i2c8`/`i2c9`,
   `aw3641e` / `lm3643`):** Requires multimedia clock controllers (`camsys`,
   `imgsys1`, `ipesys`), the deferred `M8` power domains, and V4L2/CSI-2 sensor
   drivers.
3. **Hardware Video Decode / Encode & MDP (`vdecsys`, `vencsys`, `mdpsys`):**
   Requires multimedia clock controllers, `M8` power domains, and V4L2 stateless
   codec support.
4. **Sensors (Accelerometer, Gyroscope, Magnetometer, ALS/Proximity):** Routed
   through the MT6789 SCP RV55 coprocessor on stock firmware; requires
   `mtk-scp` remoteproc and an IIO sensor bridge.
5. **Fingerprint Reader (`goodix-fp` on SPI5 / `focaltech,fp`):** Not wired in
   mainline DT or userspace.
6. **Suspend-to-RAM (Deep Sleep S3):** Runtime PM works across active
   subsystems, but full system S3 suspend/resume (SPM sleep path and wakeup
   sources) has not been brought up or tested.

---

## 4. Next Milestones

1. **Full 8 GiB RAM Map:** Add remaining stock `RES[0..30]` `no-map` carveouts
   and expand `advan-x1-uboot` + mainline DTS DRAM windows from ~3.88 GiB to
   the full ~7.69 GiB usable RAM.
2. **Modem Phase 5.2–5.4:** Resolve CCIF0 HS1 timeout, bring up Emulated SCP
   CCISM ring, expose `WWAN_PORT_AT` + RO NVRAM FS RPC, and bring up DPMAIF v1
   `wwan0` LTE data.
3. **Audio & Peripheral Polish:** Identify and wire `speaker_amp@34` +
   `mt6358-accdet`, validate analog audio + UCM2, verify live NFC RF polling,
   test physical microSD I/O, and wire USB-C host/OTG mode.
4. **Upstream Submission Prep:** Clear remaining pre-LKML items (`M8`, `B2-02`,
   `M2b` boundary) tracked in [`docs/upstream-status.md`](upstream-status.md).
