# Advan X1 (MT6789 / Helio G99) — Hardware Support & Bring-Up Status

**Latest hardware-verified baseline:** `3aa67e2` / `995622f`+ (Linux 7.4-rc4 modernized kernel, full 7.69 GiB RAM, live Wi-Fi STA + 4G LTE + VoLTE voice calls, SCP RV55 IIO sensors + native `mount-matrix` auto-rotate, AW3641E rear LED flash/torch, hardware JPEG encoder `/dev/video0`, SC89601A 2.0 A non-PD charging, audio playback, `s2idle` suspend-to-RAM survival, clean shutdown). All 100% of MT6789 clock controllers and all 15 `M8` SCPSYS power domains are wired; WireGuard/nftables desktop kmods (`c30396e`) and the MT6631 Bluetooth `HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2` + factory `BT_Addr` fix are landed in-tree.  
**Upstream kernel baseline:** Linux v7.4-rc3/rc4 (`666539eecdf4db623f5e3c3e55cf9f2dfad24aa4`)  
**Boot chain:** Stock LK → Loki Stage-1 (EL1) → [`advan-x1-uboot`](https://github.com/sysretq0/advan-x1-uboot) (EL2) → U-Boot FIT (`advan-x1.itb`)

Status legend:
- **Working** — Hardware-verified on ADVAN X1 via pstore/ramoops and/or live session testing.
- **WIP** — Driver/DT present and probing, or partially brought up; missing feature, routing, or end-to-end functional test.
- **On hold** — Intentionally paused to prioritize higher-value subsystems.
- **Not started** — Hardware exists on the board (`runtime_merged.dts`), not yet implemented in mainline DTS/drivers.

---

## Summary Matrix

| Subsystem | Hardware / IC | Mainline Driver / DT Node | Status | Notes |
|---|---|---|---|---|
| **CPU / SMP** | MT6789 (2×A76 + 6×A55) | PSCI + `mtk-cpufreq-hw` | **Working** | 8 CPUs online (`2.0 GHz` little, `2.2 GHz` big) |
| **RAM & pstore** | 8 GiB LPDDR4X (`7.69 GiB` usable) | `memory@40000000` + 31 `no-map` carveouts, `ramoops@48090000` | **Working** | Full `7714360K/8388608K` (~7.69 GiB) verified on HW; ramoops works |
| **Clocks** | All MT6789 CCF controllers (`apmixedsys`, `topckgen`, `infra_ao`, `imp_iic_wrap`, `mmsys`, `mfgcfg`, `mdpsys`, `camsys_*`, `imgsys1`, `ipesys`, `vdecsys`, `vencsys`) | `clk-mt6789-*` (`patches/01-clk/`) | **Working** | 100% of MT6789 silicon clock gates mapped; 0 HW-on clock leaks; `clk_disable_unused` clean |
| **Power Domains** | All 15 SCPSYS domains (`md`, `conn`, `mfg0..3`, `disp`, `audio`, `isp`, `ipe`, `vdec`, `venc`, `cam`, `cam_rawa`, `cam_rawb`) | `mtk-pm-domains` (`10006000.syscon`) | **Working** | Full `M8` hierarchy wired; `mfg0/1` regulator-coupled; `conn` uses generic `GENPD_FLAG_NO_ATTACH_POWER_ON`; multimedia domains `KEEP_DEFAULT_OFF` |
| **Suspend-to-RAM** | SPM deep sleep + MIPI-TX/SMI restore | PSCI `0x01011f01` `s2idle` | **Working** | Survives full suspend/resume cycle on hardware; post-suspend cellular reconnect hardened in Wave 1 |
| **Pinctrl / GPIO / EINT** | 188 pins + EINT | `pinctrl-mt6789` (`10005000.pinctrl`) | **Working** | Verified pinmux, active-low resets, EINT IRQs |
| **PMIC & Regulators** | MT6366 (MT6358 family) | `mtk-pmic-wrap` (`10026000.pwrap`) + `mt6358-regulator` + `mt6358-efuse` + `mt6366-thermal` | **Working** | MT6789 pwrap register map; regulators, RTC, AUXADC, PMIC eFuse, 4 PMIC thermal zones |
| **Display (DRM/KMS)** | Chipone ICNL9922C 1080×2460 120 Hz DSC 1.1 | `mediatek-drm` + `panel-chipone-icnl9922c` + `tps65132` | **Working** | Full DDP + DSC 1.1; 0.8 ms LK handover; DPMS off/on; s2idle resume; GNOME Mobile Wayland |
| **Display IOMMU (M4U)** | MT6789 `disp_iommu` + SMI `larb0`/`larb1` + 2×1 sub-commons | `mtk-iommu` (`14016000.iommu`) + `mtk-smi` | **Working** | **0 M4U faults**, **0 swiotlb errors**; sub-common regs restored on resume |
| **Backlight** | MT6789 DISP PWM | `pwm-mtk-disp` (`1100e000.pwm`) + `pwm-backlight` | **Working** | `39385 ns` (25.4 kHz), 113-step curve, full 0–100% range |
| **3D GPU** | Arm Mali-G57 MC2 | `panfrost` (`13000000.gpu`) | **Working** | Live MFGPLL devfreq (mux-park notifier), 0 GPU faults |
| **Touchscreen** | Chipone ICNL9922C SPI TDDI | `chipone-icnl9922c` (`spi0.0`) | **Working** | 71,792 B SRAM boot (`fwver 0x0104`), `drm_panel_follower` sync |
| **Internal Storage** | UFS 2.2 | `ufshcd-mtk` (`11270000.ufshci`) | **Working** | 7 clocks, 3 resets, `vemc` supply, Linux root mount |
| **USB Gadget (Device)** | MUSB + MT6789 USB2 T-PHY | `musb-hdrc` (`11200000.usb`) + `phy-mtk-tphy` (`11f40000.t-phy`) | **Working** | ConfigFS NCM (`usb0`, `10.15.19.82/24` via `systemd-networkd`) + ACM (`/dev/ttyGS0`) via `advan-x1-usb-gadget.service` |
| **Charging (Standard)** | Southchip SC89601A | `bq25890` (`&i2c5` / `4-006a`) | **Working** | USB charging + status; D+/D- gadget coexistence fixed; absolute 4.3 V/8.2 V VINDPM + 2.0 A non-PD wall adapter fallback verified live (+1.75 A) |
| **Battery Fuel Gauge** | CellWise CW2217 | `cw2217` (`&i2c0` / `0-0064`) | **Working** | 80-byte board profile download; live V/I/SOC/temp/cycles |
| **Thermal** | MT6789 LVTS (13 zones) + Board NTC + MT6366 PMIC (4 zones) | `mtk-lvts-thermal` (`1100b000`), `generic-adc-thermal`, `mt6366_thermal` | **Working** | eFuse calibration, V5 manual RCK, STAGE3 HW reboot (`113.5 °C`) |
| **Watchdog & Reboot** | MT6789 TOPRGU | `mtk-wdt` (`10007000`) + `syscon-reboot-mode` | **Working** | Clean `reboot: Restarting system`; bootloader/recovery modes |
| **Keys & Haptics** | Power/Vol keys + vibrator | `mtk-pmic-keys`, `mt6779-keypad`, `regulator-haptic` | **Working** | HW long-press reset fixed; `vibr` regulator haptic registered |
| **Buses (I²C / SPI / UART)** | `i2c0..i2c7`, `spi0`, `uart0` | `i2c-mt65xx` (`mt6789_compat`, `dma_sync=1`), `spi-mt65xx`, `8250_mtk` | **Working** | Earlycon on `uart0`; all board I²C/SPI clients active |
| **Audio** | MT6789 AFE + MT6366 + FourSemi FS1599 + ACCDET | `mt6789-mt6366-sound`, `fs1599`, `mt6358-accdet` + UCM2 + PipeWire drop-ins | **Working** | Playback verified; `-200 mB` LINEOUT cap for FS1599; PipeWire/WirePlumber AFE latency drop-ins packaged; `PCM 2` / `VoiceCall.conf` wired for CS calls |
| **Cellular Modem — Data** | MT6789 Gen95 Modem (`md1img`) | `mtk-ccci` (`d0cf000.modem`) + DPMAIF v1 + ModemManager `mtk-ccci` | **Working** | Live 4G LTE auto-connect (`wwan0` + `/dev/wwan0at0`), IPv4/IPv6 + post-suspend DPMAIF restore & `+CGACT?` reconnect in Wave 1 |
| **Cellular Modem — Voice / SMS** | MT6789 Gen95 Modem + AFE `PCM 2` | `/dev/ccci_aud`, `ccci_audd`, `Hostless_Speech` (`hw:*,5`), UCM2 `VoiceCall.conf`, MM plugin + `mtk-imsd` SMSoIP | **WIP** | VoLTE voice calls verified live; IMS SMS path (`mtk-imsd` + `MMIfaceModemMessaging` bridge, SMSC `+CSCA?`, DBus draft/delete & PDU/RP-DATA verified) + CS `ccci_audd` wired, awaiting live carrier MO/MT SMS & CS call confirmation |
| **Cellular Modem — VoLTE/IMS** | MT6789 Gen95 IMS channels | `mtk-ccci` + `mtk-imsd` + `mtk_volte_rtp` + XFRM IPsec + MM `MMIfaceModemVoice` IMS bridge | **Working** | Native VoLTE SIP/IPsec + RTP/AMR-WB (`pw-cat`) bridged natively into ModemManager & GNOME Calls (`DIAL`/`ANSWER`/`HANGUP`/`DTMF` + incoming `INVITE`), fast Airplane Mode recovery (`AT+CFUN?` + URC wake), verified live on XL (`51011`) |
| **USB-C Host / OTG / PD** | Richtek RT1711H TCPC + SC89601A VBUS boost | `rt1711h` (`4-004e`) + `charger_vbus` + `usb-role-switch` (`usb0`) | **WIP** | Dual-role VBUS boost & OF graph role-switch wired; bidirectional charging works, host data enumeration still WIP |
| **microSD Card** | MT6789 MSDC1 | `mtk-msdc` (`11240000.mmc`, `GPIO 7` `GPIO_ACTIVE_LOW` CD) | **WIP** | Controller + CD polarity fixed (`2abd89e`); live card I/O test pending |
| **Fast Charging** | SC8547 charge pump (`i2c7@6a`) + PE | — | **WIP** | Standard SC89601A charging works (4%→92% verified on `3aa67e2`); HV charge pump not implemented |
| **NFC** | STMicro ST21NFCD | `st-nci-i2c` (`&i2c3` / `nfc@8`) + `mt6366-clkbuf` (`XO_NFC`) | **On hold** | `nfc0` probes + IRQ 16 fires; intentionally paused to prioritize modem & core features |
| **Wi-Fi (MT6789 ConnSys)** | MT6789 `SOC2_2` + `MT6631` A-die | `btif_drv` + `wmt_drv` + `wlan_drv_gen4m_6789` + `mtk-wmt-init` | **Working** | Live WPA2/WPA3 STA (`wlan0`) + 4G LTE coexistence verified; Loki LK `consys_init()` opens EMI MPU Regions 27 & 18; `15-mtk-wifi.conf` keeps NM on `wlan0` |
| **Bluetooth (MT6789 ConnSys)** | MT6789 `SOC2_2` + `MT6631` A-die | `hci_mtk_stp` (native BlueZ `hci0` over STP/WMT) | **Working** | Live verified on `b20bb73`: `hci0` passes full kernel `hci_init1..4` + `le_init4` (`HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2`), programs factory `BT_Addr` (`00:0c:e7:55:57:1a`) via `0xfc1a`, and executes LE scan/PHY commands cleanly |
| **GPS (MT6789 ConnSys)** | MT6789 `SOC2_2` | — | **Not started** | `gps_drv_stp` / NMEA bridge not yet wired |
| **LED Torch / Flash** | Awinic AW3641E (`GPIO 99` ENM, `GPIO 36` ENF) | `leds-sgm3140` (`awinic,aw3641e` → `/sys/class/leds/white:flash`) | **Working** | Live verified on `3aa67e2`: torch (`brightness` `1→0`) and strobe (`flash_strobe`, `250 ms` timeout, `root:feedbackd`) |
| **Cameras & ISP** | `camsys` / `imgsensor` | `clk-mt6789-{cam,img}` + `M8` PDs ready | **Not started** | Clocks & `M8` PDs wired; needs V4L2/CSI-2 sensor & ISP drivers |
| **JPEG Encoder (HW)** | MT6789 `jpgenc@17030000` (`vencsys` + `larb7`) | `mtk-jpeg` (`mediatek,mt6789-jpgenc`) + `larb4`/`larb7` M4U Region 1 | **Working** | Live verified on `3aa67e2`: `/dev/video0` (`mtk-jpeg-enc`) 640×480 YUYV→JPEG pixel-accurate encode, GIC 375 IRQ, 0 IOMMU faults; boot auto-load fixed in `995622f` |
| **Video Codec (H.264/HEVC/VP9)** | `vdecsys` / `vencsys` (`larb4`/`larb7`) | Clocks, `M8` PDs, SMI `larb4`/`larb7` ready | **Not started** | MT6789 uses Android closed-source `mediatek-vcu` (`vpud`), not ChromeOS SCP firmware; requires open-source stateless/VCU codec driver |
| **Sensors (IMU/ALS/Prox)** | SCP RV55 sensor hub (`icm-42607-p`, `af6133e`, `STK3335-X`, `stk3a5x`) | `mtk-scp-sensorhub` (`10724000.sensorhub` → Linux IIO) | **Working** | Live verified on `3aa67e2`: all 5 IIO sensors (`iio:device4..8`) + `iio-sensor-proxy` + native kernel `in_mount_matrix` (`-1,0,0;0,-1,0;0,0,1`) + 0 deprecated-wq warnings |
| **Fingerprint** | Goodix (`goodix-fp` SPI5) / FocalTech | — | **Not started** | Not wired in DTS or driver stack |

---

## 1. Working (Hardware-Verified)

- **Core SoC, SMP & CPUfreq:** All 8 cores online (`policy0`: 6× Cortex-A55
  500–2000 MHz, `policy6`: 2× Cortex-A76 725–2200 MHz via `mtk-cpufreq-hw`),
  SoC identity exported via `mtk-socinfo` (`MediaTek Helio G99 / MT6789`).
- **Clocks, Power Domains & Suspend-to-RAM:** All 14 MT6789 clock controllers
  (`apmixedsys`, `topckgen`, `infra_ao`, `imp_iic_wrap`, `mmsys`, `mfgcfg`,
  `mdpsys`, `camsys_{main,rawa,rawb}`, `imgsys1`, `ipesys`, `vdecsys`,
  `vencsys`) and all 15 SCPSYS power domains (`md`, `conn`, `mfg0`–`mfg3`,
  `disp`, `audio`, `isp`, `ipe`, `vdec`, `venc`, `cam`, `cam_rawa`, `cam_rawb`)
  are wired without `clk_ignore_unused` or `pd_ignore_unused` bootargs. `mfg0`
  (`vsram_gpu`) and `mfg1` (`vgpu`) are regulator-coupled, `conn` defers
  power-on cleanly via generic `GENPD_FLAG_NO_ATTACH_POWER_ON`, and multimedia
  domains stay `MTK_SCPD_KEEP_DEFAULT_OFF` until consumers claim them. Full
  `s2idle` suspend/resume survives on hardware with MIPI-TX/DSI cold-start
  resume and SMI 2×1 sub-common register restoration.
- **Wi-Fi & Bluetooth (MT6789 ConnSys):** `btif_drv` + `wmt_drv` +
  `wlan_drv_gen4m_6789` + `hci_mtk_stp` + `mtk-wmt-init` provide live WPA2/WPA3
  STA (`wlan0`) coexisting with 4G LTE (`wwan0`), hardware SoftAP (`start_ap`),
  and native Linux Bluetooth `hci0` (`MTK SOC2_2 #1`, BT 5.4, factory `BT_Addr`
  programmed via `0xfc1a`, `HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2`). Loki
  LK configures EMI MPU Regions 27 & 18 via `MTK_SIP_BL_EMIMPU_CONTROL`
  (`0x82000415`) at boot.
- **Display, IOMMU & Backlight:** Full `mediatek-drm` DDP pipeline
  (`OVL0 → RDMA0 → COLOR0 → CCORR0 → AAL0 → GAMMA0 → POSTMASK0 → DITHER0 →
  DSC0 → DSI0`) drives the Chipone ICNL9922C 1080×2460 4-lane VESA DSC 1.1
  panel (60/90/120 Hz) powered by `tps65132` (`i2c6@0x3e`). Active LK splash
  DMA is halted cleanly in `mtk_disp_ovl_probe()`, giving 0.8 ms DSI bind and
  **zero M4U faults** on `disp_iommu` (`14016000.iommu`, `larb0`/`larb1`) with
  zero `swiotlb` buffer exhaustion. DPMS screen off/on and `disp_pwm0`
  backlight (`39385 ns` / 25.4 kHz, 113 levels) work reliably.
- **3D GPU (Panfrost):** Mali-G57 MC2 (`13000000.gpu`, id `0x9093`) runs
  accelerated GNOME Mobile Wayland sessions with live `MFGPLL` devfreq scaling
  protected by a `CLK_TOP_MFG_PLL_SEL` mux-park notifier and spinlock-guarded
  `clk-mt6789-mfgcfg` HW readback.
- **Touchscreen:** Clean mainline `chipone-icnl9922c` SPI driver (`spi0.0`)
  boots the 71,792-byte volatile SRAM firmware (`fwid=0x9965`, `fwver=0x0104`),
  reports `ABS_MT_TOUCH_MAJOR` and physical mm dimensions, and synchronizes
  reset/sleep via `drm_panel_follower`.
- **Audio:** MT6789 AFE + MT6366 PMIC codec + FourSemi FS1599 Class-K analog
  speaker amp (`i2c6@0x34`, capped at `Lineout Volume 8` / `-200 mB`) +
  `mt6358-accdet` headset jack (`Headphone Jack` / `Headset Mic Jack` ALSA
  kcontrols created via `snd_soc_card_jack_new_pins()`). UCM2 `HiFi.conf` +
  `VoiceCall.conf` and PipeWire/WirePlumber AFE latency drop-ins are packaged
  in `advan-x1-support.deb`.
- **Cellular Modem (4G LTE Data & Native VoLTE):** `mtk-ccci` + DPMAIF v1 +
  SMEM UL + read-only NVRAM overlay `ccci_fsd`/`ccci_rpcd` + native ModemManager
  `libmm-plugin-mtk-ccci.so` provide automatic 4G LTE connectivity (`wwan0` +
  `/dev/wwan0at0`). Native VoLTE (`mtk-imsd` + `mtk_volte_rtp` + XFRM IPsec +
  `MMIfaceModemVoice` IMS bridge on `wwan1`) is live-verified for voice calls
  in GNOME Calls on XL (`51011`), with fast Airplane Mode recovery (`AT+CFUN?`
  + URC wakeup).
- **Sensors, LED Flash & Hardware JPEG Encoder:** SCP RV55 sensor hub
  (`10724000.sensorhub`) exposes all 5 IIO sensors (`accel`, `gyro`, `magn`,
  `light`, `proximity`) with native kernel `in_mount_matrix` (`-1,0,0;0,-1,0;0,0,1`)
  driving `iio-sensor-proxy` auto-rotate. Awinic AW3641E (`/sys/class/leds/white:flash`)
  provides torch and `250 ms` flash strobe. Hardware JPEG encoder
  (`17030000.jpeg-encoder` → `/dev/video0`, `mtk-jpeg-enc` over `vencsys` +
  SMI `larb7` + `disp_iommu` Region 1) is live-verified for pixel-accurate
  640×480 YUYV→JPEG encoding with 0 IOMMU faults.
- **Storage, USB Gadget & Power:** UFS 2.2 (`11270000.ufshci`), MUSB
  (`11200000.usb`) + MT6789 USB2 T-PHY (`11f40000.t-phy`) composite ConfigFS
  gadget (NCM `usb0` `10.15.19.82/24` + ACM `/dev/ttyGS0`), Southchip SC89601A
  (`bq25890`, `4-006a`, including 2.0 A non-PD wall-charger fallback under
  4.3 V absolute VINDPM verified 4%→92%), CellWise CW2217 (`0-0064`),
  MT6366/MT6358 PMIC (regulators, RTC, AUXADC, eFuse, thermal, keys, vibrator),
  13-zone LVTS thermal (+ STAGE3 HW reboot), and `mtk-wdt` + `syscon-reboot-mode`.

---

## 2. Work in Progress / On Hold

1. **SMS (IMS + CS Fallback) & CS Voice (`ccci_audd`):**
   - IMS SMS (`mtk-imsd` SIP `MESSAGE` + `application/vnd.3gpp.sms` + ModemManager
     `MMIfaceModemMessaging` bridge, SMSC `+CSCA?` auto-refresh, DBus draft/delete)
     and CS voice (`ccci_audd` + `Hostless_Speech` + `VoiceCall.conf`) are fully
     wired; live over-the-air MO/MT SMS and 2G/3G CS call confirmation remain.
   - See [`docs/modem-full-bringup.md`](modem-full-bringup.md).
2. **USB-C Host / OTG / Power Delivery:** Bidirectional USB-C power works; host
   role data enumeration (`rt1711h` + `usb-role-switch` + `musb-hdrc`) is WIP.
3. **microSD Card (`11240000.mmc`):** `GPIO 7` CD polarity corrected to
   `GPIO_ACTIVE_LOW` (`2abd89e`); live physical card I/O test pending.
4. **Fast Charging:** Standard SC89601A charging works; SC8547 HV charge pump
   (`i2c7@0x6a`) is not implemented.
5. **NFC (`st,st21nfcb-i2c` — on hold):** `nfc0` probes with `XO_NFC` clkbuf
   and IRQ 16 fires; live RF polling is intentionally paused.

---

## 3. Not Yet Started / Unsupported

1. **GPS (`gps@18c00000`):** `gps_drv_stp` / NMEA bridge over ConnSys WMT not yet wired.
2. **Cameras & ISP (`camsys`, `imgsensor`):**
   Multimedia clock controllers (`camsys_*`, `imgsys1`, `ipesys`) and `M8`
   power domains are wired; V4L2/CSI-2 sensor and ISP drivers are not yet implemented.
3. **Hardware Video Decode / Encode & MDP (`vdecsys`, `vencsys`, `mdpsys`):**
   Multimedia clock controllers, `M8` PDs, and SMI `larb4`/`larb7` are wired
   (with hardware JPEG encoding working on `/dev/video0`); H.264/HEVC/VP9
   `vdec`/`venc` (Android `vpud`) and MDP drivers are not yet implemented.
4. **Fingerprint Reader (`goodix-fp` / `focaltech,fp`):** Not wired in mainline
   DT or userspace.

---

## 4. Next Milestones

1. **Finalize Remaining WIP Subsystems:** Live-verify native BlueZ Bluetooth
   (`hci_mtk_stp` after `HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2`), live
   MO/MT SMS over IMS, physical microSD I/O, and USB-C OTG host data enumeration.
2. **Audio & Peripheral Polish:** Verify headset jack switching (`mt6358-accdet`),
   CS voice fallback (`ccci_audd`), and optional SC8547 HV fast charging.
3. **Upstream Submission Prep:** Clear the last pre-LKML structural boundary
   (`M2b` CCCI split; `M8` is complete) tracked in
   [`docs/upstream-status.md`](upstream-status.md).
