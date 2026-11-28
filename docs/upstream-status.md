# Upstream Readiness & Patch Series Status

This document tracks the upstream patch queue layout, completed audit and
Linux 7.4 modernization work, and the remaining pre-LKML items for the
Advan X1 (MT6789) mainline Linux port.

- **Pinned upstream baseline:** `666539eecdf4db623f5e3c3e55cf9f2dfad24aa4` (Linux v7.4-rc3/rc4)
- **Patch author / Signed-off-by:** `sysretq0 <sysretq0@proton.me>` (standardized across all patches)
- **Automated gate:** `./scripts/verify_patches.sh` (clean `git apply`, zero DTS drift vs `dts/`, 0 `dtc` warnings, 0 `dt-validate` schema errors)

---

## 1. Patch Series Layout (135 Upstream-Targeted Patches)

| Directory | Count | Scope |
|---|---:|---|
| `patches/01-clk/` | 9 | MT6789 clock/reset bindings (including all 8 multimedia controllers), core clocks (`apmixedsys`, `topckgen`, `infra_ao` + MFGPLL mux-park notifier), `imp_iic_wrap`, `mmsys`, `mfgcfg`, multimedia subsystem drivers (`mdpsys`, `camsys_{main,rawa,rawb}`, `imgsys1`, `ipesys`, `vdecsys`, `vencsys` — 100% of MT6789 silicon clock gates), and Linux 7.4 `mtk_clk_simple_probe` conversions |
| `patches/02-pinctrl/` | 2 | MT6789 pinctrl binding and 188-pin pinctrl/EINT driver |
| `patches/03-soc-power/` | 22 | SCPSYS power domains (all 15 MT6789 domains including `CONN` with generic `GENPD_FLAG_NO_ATTACH_POWER_ON` and `M8` multimedia domains with `MTK_SCPD_KEEP_DEFAULT_OFF`), MT6789 PMIC wrapper (`mt6789_regs[]`), regulator coupler, MMSYS, disp-mutex (+ probe deferral), SMI (common + 2×1 sub-commons + larb sleep-ctrl restore), socinfo, disp IOMMU (`mtk-iommu`), and Linux 7.4 typed-allocator / PM-ops modernization |
| `patches/04-peripherals/` | 76 | SPI, I²C (`mt6789_compat`, `dma_sync=1`), eFuse, watchdog, LVTS thermal (+ per-SoC zero-MSR guard & STAGE3 hw reboot), MT6366 PMIC thermal + MT6358/MT6366 PMIC eFuse + MT6366 DCXO `XO_NFC` CCF clkbuf, DISP PWM, MT6358 long-press fix, MMC, SC89601A charger (+ 18W/3A PD unlock), CW2217 fuel gauge, Chipone ICNL9922C SPI touch, MT6789 AFE/MT6366 audio + `PCM 2` / `Hostless_Speech` voice crossbar + FourSemi FS1599 + MT6358 ACCDET (+ eFuse/moisture/CDD), UFS/USB PHY bindings + T-PHY wake/OTG, `tps65132` wakeup, split CCCI SoC core (`drivers/soc/mediatek/mtk_ccci/`, `CONFIG_MTK_CCCI_SOC`) + `auxiliary_bus` WWAN AT/DPMAIF v1 data plane (`drivers/net/wwan/mtk_ccci/`, `CONFIG_MTK_CCCI`), MT6789 SCP RV55 sensorhub IIO binding & driver, AW3641E LED flash/torch, MT6789 JPEG encoder (`mtk-jpeg`), and firmware-free stateless H.264 hardware video decoder (`mtk-vdec-mt6789`) |
| `patches/05-gpu/` | 2 | Mali-G57 MC2 (`panfrost`) binding and compatible |
| `patches/06-display/` | 14 | MIPI-TX DSI PHY (+ s2idle cold-start resume), `mediatek-drm` pipeline + OVL0 splash DMA halt + CRTC vblank drain fix, DSI + hardware DSC 1.1, Chipone ICNL9922C panel binding & driver, disp-mutex bind deferral |
| `patches/07-dts/` | 10 | SoC/board binding, `mt6789.dtsi`, `mt6789-advan-x1.dts`, Linux 7.4 DTS/binding alignment, ConnSys/BTIF/Wi-Fi DT nodes, all 8 multimedia clock controllers + `M8` power domains, SCP RV55 sensorhub, AW3641E flash, JPEG encoder, and stateless H.264 VDEC |
| **Total (`patches/`)** | **135** | **Upstream submission series** (out-of-tree `patches-extra/` removed) |

---

## 2. Completed Milestones & Release-Grade Audit

An end-to-end upstream deviation audit against stock Advan X1 firmware,
MT6789 SoC reference, and upstream Linux bindings (originally conducted at
`f3d8e93` and archived in
[`docs/archive/advan-x1-mainline-ground-truth-audit.md`](archive/advan-x1-mainline-ground-truth-audit.md))
has been remediated and verified:

| Milestone | Commit(s) | Key Fixes & Verification |
|---|---|---|
| **Initial remediation sweep** | `f3d8e93`..`2b74552` | Fixed MT6789 `pwrap` register map (`P1`), resolved all 27 `dt-validate` schema errors (`D1.1`–`D1.13`), added CW2217 80-byte profile download (`D2.2`/`B1`), switched backlight PWM to stock `39385 ns` (`D2.5`), aligned `verify_patches.sh` on pinned baseline `666539ee`. |
| **Phase 1–3: Core, IOMMU, Display** | `3b1f69f`..`0d9a170` | Fixed Mali-G57 MFGPLL devfreq freeze (`CLK_TOP_MFG_PLL_SEL` mux-park notifier), enabled `disp_iommu` (0 M4U faults / 0 `swiotlb` exhaustion), halted bootloader OVL0 splash DMA, added `syscon-reboot-mode`, cleared `priv->dsc = NULL` in `mtk_dsc_stop()`, standardized patch identity to `sysretq0@proton.me`. |
| **Phase A/B & Suspend-to-RAM** | `074d43d`..`2abd89e` | Added LVTS STAGE3 hw thermal reboot, 30 s CCCI HS1/HS2 timeout, MT6366 `XO_NFC` clkbuf, MT6358 eFuse NVMEM, MT6366 PMIC thermal, ACCDET eFuse/CDD, SMI 2×1 sub-common restore, MIPI-TX/DSI s2idle resume, and hardware-verified microSD CD polarity (`GPIO_ACTIVE_LOW` in `2abd89e`). |
| **Linux 7.4 Modernization** | `d3e3b51`..`f1cc338` | Adopted 7.4 typed allocators (`kzalloc_obj`/`devm_kzalloc_obj`), `DEFINE_SIMPLE_DEV_PM_OPS`/`DEFINE_RUNTIME_DEV_PM_OPS`, `<linux/cleanup.h>` guards, `mtk_clk_simple_probe`, strict `__le*` sparse annotations in `mtk_ccci`, and dropped the out-of-tree `patches-extra/` vendor TDDI driver. |
| **Modem Wave 1 & VoLTE/IMS** | `f492bf9`..`e505c0f` | Added `/dev/ccci_aud`, DPMAIF suspend/resume restore, `PCM 2` / `Hostless_Speech` AFE voice crossbar, UCM2 `VoiceCall.conf`, PipeWire drop-ins, `ccci_audd`, native VoLTE/IMS + RTP/AMR-WB (`mtk-imsd`/`mtk_volte_rtp`), IMS SMSoIP, and fast Airplane Mode recovery. |
| **ConnSys, Sensors, JPEG & VDEC H.264** | `e505c0f`..`e7165ed` | Added ConnSys Wi-Fi STA/SoftAP + native BlueZ `hci_mtk_stp`, all 8 multimedia clock controllers + `M8` power domains, generic `GENPD_FLAG_NO_ATTACH_POWER_ON`, SCP RV55 IIO sensors + `in_mount_matrix`, AW3641E torch/flash, `mtk-jpeg` (`/dev/video0`), and firmware-free stateless H.264 VDEC (`/dev/video1`, 170/170 bit-exact). |
| **Suspend Polish, Distro Config & `M2b` CCCI Split** | `faea59b`..HEAD | Fixed 0 s `s2idle` ConnSys Event `0x07` wakeup (WoWLAN-gated `irq_wake` + N9 FW suspend notify + non-waking `CONN_LOG_IRQ`), P2P `u4DeviceNum` resync, unmanaged `p2p-dev-wlan0`, `InhibitDelayMaxSec=5`, GNOME Mobile `authPrompt.js`/`unlockDialog.js` lockscreen PIN/dispose fixes, full Debian/Mobian/Fedora/Ubuntu release-grade kernel config, and completed the `M2b` CCCI subsystem split (`drivers/soc/mediatek/mtk_ccci/` + `auxiliary_bus` `drivers/net/wwan/mtk_ccci/`). |

---

## 3. Pre-Upstream Structural Status

**All pre-LKML structural boundaries are resolved (0 deferred items):**

- **`M8` (Power Domains & Multimedia Clocks):** Complete — all 15 SCPSYS domains and 100% of MT6789 silicon clock gates wired; generic `GENPD_FLAG_NO_ATTACH_POWER_ON` replaces custom compatible checks.
- **`M2b` (CCCI Subsystem Boundary):** Complete (`patches/04-peripherals/0076`) — SoC modem boot, EL3 SiP SMC control, CCIF0 ringbuffer transport, and RPC/FS/Audio services live in `drivers/soc/mediatek/mtk_ccci/` (`CONFIG_MTK_CCCI_SOC`, `mtk-ccci-soc`, header `include/linux/soc/mediatek/mtk_ccci.h`), while `drivers/net/wwan/mtk_ccci/` (`CONFIG_MTK_CCCI`, `mtk_ccci_wwan`) is strictly an `auxiliary_bus` WWAN AT port + DPMAIF v1 network driver (see [`docs/ccci-sip-abi.md`](ccci-sip-abi.md)).

---

## 4. Tier A Provenance Note

The authoritative board ground truth is Advan X1 stock firmware (live Android
hardware oracle, stock merged DTB `runtime_merged.dts`, stock prebuilt `.ko`
modules). Sibling MT6789 sources are used only for SoC architecture / wire
protocols and are **not** board authority. Key Tier A reconciliations:

- **F11:** MT6789 I²C `dma_sync=1` from stock `mt6983_compat` (was
  `mt8188_compat` with `0`) — `patches/04-peripherals/0004`.
- **D-03:** Unused/fabricated `vmddr` regulator removed (absent on stock) —
  `patches/07-dts/0003`.
- **D2-20:** microSD card-detect (`GPIO 7`) initially restored to stock DT
  `GPIO_ACTIVE_HIGH` in Phase A, then flipped back to **`GPIO_ACTIVE_LOW`** in
  `2abd89e` after live hardware verification confirmed inverted physical CD
  polarity (`in hi` when empty, low when card inserted).
- **D-13:** SPI register windows narrowed to stock `0x100` —
  `patches/07-dts/0002`.
- **F05:** `PWRAP_INT_EN=0x450` verified against stock `mtk-pmic-wrap.ko`.

Full historical evidence is in
[`docs/archive/advan-x1-mainline-ground-truth-audit.md`](archive/advan-x1-mainline-ground-truth-audit.md).
