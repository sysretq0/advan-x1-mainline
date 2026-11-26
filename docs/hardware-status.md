# Advan X1 (MT6789 / Helio G99) — Hardware Support & Bring-Up Status

**Latest hardware-verified baseline:** `f1cc338` (7.4 modernized kernel, full 7.69 GiB RAM, live 4G LTE auto-connect + NTP sync, audio playback, `s2idle` suspend-to-RAM survival, clean shutdown). Modem Wave 1 + userspace cleanup commits (`f492bf9`+) are CI-verified and awaiting live operator testing in order **Internet → Call → SMS → VoLTE**.  
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
| **Clocks** | `apmixedsys`, `topckgen`, `infra_ao`, `imp_iic_wrap`, `mmsys`, `mfgcfg` | `clk-mt6789-*` (`patches/01-clk/`) | **Working** | 0 HW-on clock leaks; `clk_disable_unused` clean |
| **Power Domains** | SCPSYS (`md`, `mfg0..3`, `disp`, `audio`) | `mtk-pm-domains` (`10006000.syscon`) | **Working** | `mfg0/1` regulator-coupled; 0 deferred probes |
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
| **Charging (Standard)** | Southchip SC89601A | `bq25890` (`&i2c5` / `4-006a`) | **Working** | USB charging + status; D+/D- gadget coexistence fixed |
| **Battery Fuel Gauge** | CellWise CW2217 | `cw2217` (`&i2c0` / `0-0064`) | **Working** | 80-byte board profile download; live V/I/SOC/temp/cycles |
| **Thermal** | MT6789 LVTS (13 zones) + Board NTC + MT6366 PMIC (4 zones) | `mtk-lvts-thermal` (`1100b000`), `generic-adc-thermal`, `mt6366_thermal` | **Working** | eFuse calibration, V5 manual RCK, STAGE3 HW reboot (`113.5 °C`) |
| **Watchdog & Reboot** | MT6789 TOPRGU | `mtk-wdt` (`10007000`) + `syscon-reboot-mode` | **Working** | Clean `reboot: Restarting system`; bootloader/recovery modes |
| **Keys & Haptics** | Power/Vol keys + vibrator | `mtk-pmic-keys`, `mt6779-keypad`, `regulator-haptic` | **Working** | HW long-press reset fixed; `vibr` regulator haptic registered |
| **Buses (I²C / SPI / UART)** | `i2c0..i2c7`, `spi0`, `uart0` | `i2c-mt65xx` (`mt6789_compat`, `dma_sync=1`), `spi-mt65xx`, `8250_mtk` | **Working** | Earlycon on `uart0`; all board I²C/SPI clients active |
| **Audio** | MT6789 AFE + MT6366 + FourSemi FS1599 + ACCDET | `mt6789-mt6366-sound`, `fs1599`, `mt6358-accdet` + UCM2 + PipeWire drop-ins | **Working** | Playback verified; `-200 mB` LINEOUT cap for FS1599; PipeWire/WirePlumber AFE latency drop-ins packaged; `PCM 2` / `VoiceCall.conf` wired for CS calls |
| **Cellular Modem — Data** | MT6789 Gen95 Modem (`md1img`) | `mtk-ccci` (`d0cf000.modem`) + DPMAIF v1 + ModemManager `mtk-ccci` | **Working** | Live 4G LTE auto-connect (`wwan0` + `/dev/wwan0at0`), IPv4/IPv6 + post-suspend DPMAIF restore & `+CGACT?` reconnect in Wave 1 |
| **Cellular Modem — Voice / SMS** | MT6789 Gen95 Modem + AFE `PCM 2` | `/dev/ccci_aud`, `Hostless_Speech` (`hw:*,5`), UCM2 `VoiceCall.conf`, MM plugin | **WIP** | Wave 1 kernel/AT/UCM2 wired; awaiting live call/SMS verification and Wave 2 speech-mailbox helper (`docs/modem-full-bringup.md`) |
| **Cellular Modem — VoLTE/IMS** | MT6789 Gen95 IMS channels | `mtk-ccci` channel drains wired | **Not started** | Needs open-source SIP/IMS + XFRM IPsec userspace stack (Stage 4) |
| **USB-C Host / OTG / PD** | Richtek RT1711H TCPC + SC89601A VBUS boost | `rt1711h` (`4-004e`) + `charger_vbus` + `usb-role-switch` (`usb0`) | **WIP** | Dual-role VBUS boost & OF graph role-switch wired; bidirectional charging works, host data enumeration still WIP |
| **microSD Card** | MT6789 MSDC1 | `mtk-msdc` (`11240000.mmc`, `GPIO 7` `GPIO_ACTIVE_LOW` CD) | **WIP** | Controller + CD polarity fixed (`2abd89e`); live card I/O test pending |
| **Fast Charging** | SC8547 charge pump (`i2c7@6a`) + PE | — | **WIP** | Standard SC89601A charging works; HV charge pump not implemented |
| **NFC** | STMicro ST21NFCD | `st-nci-i2c` (`&i2c3` / `nfc@8`) + `mt6366-clkbuf` (`XO_NFC`) | **On hold** | `nfc0` probes + IRQ 16 fires; intentionally paused to prioritize modem & core features |
| **Wi-Fi / Bluetooth / GPS** | MT6789 ConnSys (`consys@18002000`) | — | **Not started** | No mainline MT6789 ConnSys/WMT driver |
| **Cameras & Flash** | `camsys` / `imgsensor` / `aw3641e` / `lm3643` | — | **Not started** | Needs `camsys`/`imgsys` clocks, `M8` power domains, V4L2 stack |
| **Video Codec (HW)** | `vdecsys` / `vencsys` / `mdpsys` | — | **Not started** | Needs multimedia clock controllers & `M8` power domains |
| **Sensors (IMU/ALS/Prox)** | SCP RV55 sensor hub (`scp`) | — | **Not started** | Needs `mtk-scp` remoteproc + IIO sensor bridge |
| **Fingerprint** | Goodix (`goodix-fp` SPI5) / FocalTech | — | **Not started** | Not wired in DTS or driver stack |

---

## 1. Working (Hardware-Verified)

- **Core SoC, SMP & CPUfreq:** All 8 cores online (`policy0`: 6× Cortex-A55
  500–2000 MHz, `policy6`: 2× Cortex-A76 725–2200 MHz via `mtk-cpufreq-hw`),
  SoC identity exported via `mtk-socinfo` (`MediaTek Helio G99 / MT6789`).
- **Clocks, Power Domains & Suspend-to-RAM:** All active clock controllers
  (`apmixedsys`, `topckgen`, `infra_ao`, `imp_iic_wrap`, `mmsys`, `mfgcfg`)
  operate without `clk_ignore_unused` or `pd_ignore_unused` and with zero leaked
  HW-on clocks in `clk_summary`. SCPSYS manages `md`, `mfg0`–`mfg3`, `disp`, and
  `audio` power domains with `mfg0` (`vsram_gpu`) and `mfg1` (`vgpu`)
  regulator-coupled. Full `s2idle` suspend/resume survives on hardware with
  MIPI-TX/DSI cold-start resume and SMI 2×1 sub-common register restoration.
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
- **Cellular Modem (4G LTE Data):** `mtk-ccci` + DPMAIF v1 + SMEM UL +
  read-only NVRAM overlay `ccci_fsd`/`ccci_rpcd` + native ModemManager
  `libmm-plugin-mtk-ccci.so` provide automatic 4G LTE connectivity (`wwan0` +
  `/dev/wwan0at0`).
- **Storage, USB Gadget & Power:** UFS 2.2 (`11270000.ufshci`), MUSB
  (`11200000.usb`) + MT6789 USB2 T-PHY (`11f40000.t-phy`) composite ConfigFS
  gadget (NCM `usb0` `10.15.19.82/24` + ACM `/dev/ttyGS0`), Southchip SC89601A
  (`bq25890`, `4-006a`), CellWise CW2217 (`0-0064`), MT6366/MT6358 PMIC
  (regulators, RTC, AUXADC, eFuse, thermal, keys, vibrator), 13-zone LVTS
  thermal (+ STAGE3 HW reboot), and `mtk-wdt` + `syscon-reboot-mode`.

---

## 2. Work in Progress / On Hold

1. **Full Modem (Wave 1 → Wave 2, strict order: Internet → Call → SMS → VoLTE):**
   - Post-suspend DPMAIF resume + IPv4/IPv6/`+CGACT?` reconnect, CS voice
     `PCM 2` crossbar + `/dev/ccci_aud` + `VoiceCall.conf`, and 3GPP PDU SMS
     init are implemented in Wave 1 and awaiting live hardware verification.
   - Wave 2 will add the lightweight `ccci-aud-helper` speech-mailbox trigger
     for live call audio; Stage 4 VoLTE/IMS follows once Stages 1–3 are solid.
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

1. **Wi-Fi, Bluetooth & GPS (`consys@18002000`, `wifi@18000000`, `gps@18c00000`):**
   MT6789 integrated ConnSys/WMT subsystem has no upstream Linux driver.
2. **Cameras, ISP & LED Flash (`camsys`, `imgsensor`, `aw3641e` / `lm3643`):**
   Requires multimedia clock controllers (`camsys`, `imgsys1`, `ipesys`), the
   deferred `M8` power domains, and V4L2/CSI-2 sensor drivers.
3. **Hardware Video Decode / Encode & MDP (`vdecsys`, `vencsys`, `mdpsys`):**
   Requires multimedia clock controllers, `M8` power domains, and V4L2 stateless
   codec support.
4. **Sensors (Accelerometer, Gyroscope, Magnetometer, ALS/Proximity):** Routed
   through the MT6789 SCP RV55 coprocessor on stock firmware; requires
   `mtk-scp` remoteproc and an IIO sensor bridge.
5. **Fingerprint Reader (`goodix-fp` / `focaltech,fp`):** Not wired in mainline
   DT or userspace.

---

## 4. Next Milestones

1. **Full Modem Hardware Verification (Wave 1 → Wave 2):** Verify post-suspend
   4G LTE internet, CS voice call (`PCM 2` + `/dev/ccci_aud` + `ccci-aud-helper`),
   and 3GPP PDU SMS on hardware, then evaluate Stage 4 VoLTE/IMS.
2. **Audio & Peripheral Polish:** Confirm zero-dropout playback with packaged
   PipeWire/WirePlumber drop-ins, verify headset jack switching, test physical
   microSD I/O, and complete USB-C OTG host data enumeration.
3. **Upstream Submission Prep:** Clear remaining pre-LKML items (`M8`, `B2-02`,
   `M2b` boundary) tracked in [`docs/upstream-status.md`](upstream-status.md).
