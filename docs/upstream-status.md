# Upstream Readiness & Patch Series Status

This document tracks the upstream patch queue layout, completed audit and
Linux 7.4 modernization work, and the remaining pre-LKML items for the
Advan X1 (MT6789) mainline Linux port.

- **Pinned upstream baseline:** `666539eecdf4db623f5e3c3e55cf9f2dfad24aa4` (Linux v7.4-rc3/rc4)
- **Patch author / Signed-off-by:** `sysretq0 <sysretq0@proton.me>` (standardized across all patches)
- **Automated gate:** `./scripts/verify_patches.sh` (clean `git apply`, zero DTS drift vs `dts/`, 0 `dtc` warnings, 0 `dt-validate` schema errors)

---

## 1. Patch Series Layout (125 Upstream-Targeted Patches)

| Directory | Count | Scope |
|---|---:|---|
| `patches/01-clk/` | 9 | MT6789 clock/reset bindings (including all 8 multimedia controllers), core clocks (`apmixedsys`, `topckgen`, `infra_ao` + MFGPLL mux-park notifier), `imp_iic_wrap`, `mmsys`, `mfgcfg`, multimedia subsystem drivers (`mdpsys`, `camsys_{main,rawa,rawb}`, `imgsys1`, `ipesys`, `vdecsys`, `vencsys` — 100% of MT6789 silicon clock gates), and Linux 7.4 `mtk_clk_simple_probe` conversions |
| `patches/02-pinctrl/` | 2 | MT6789 pinctrl binding and 188-pin pinctrl/EINT driver |
| `patches/03-soc-power/` | 22 | SCPSYS power domains (all 15 MT6789 domains including `CONN` with generic `GENPD_FLAG_NO_ATTACH_POWER_ON` and `M8` multimedia domains with `MTK_SCPD_KEEP_DEFAULT_OFF`), MT6789 PMIC wrapper (`mt6789_regs[]`), regulator coupler, MMSYS, disp-mutex (+ probe deferral), SMI (common + 2×1 sub-commons + larb sleep-ctrl restore), socinfo, disp IOMMU (`mtk-iommu`), and Linux 7.4 typed-allocator / PM-ops modernization |
| `patches/04-peripherals/` | 69 | SPI, I²C (`mt6789_compat`, `dma_sync=1`), eFuse, watchdog, LVTS thermal (+ per-SoC zero-MSR guard & STAGE3 hw reboot), MT6366 PMIC thermal + MT6358/MT6366 PMIC eFuse + MT6366 DCXO `XO_NFC` CCF clkbuf, DISP PWM, MT6358 long-press fix, MMC, SC89601A charger (+ 18W/3A PD unlock), CW2217 fuel gauge, Chipone ICNL9922C SPI touch, MT6789 AFE/MT6366 audio + `PCM 2` / `Hostless_Speech` voice crossbar + FourSemi FS1599 + MT6358 ACCDET (+ eFuse/moisture/CDD), UFS/USB PHY bindings + T-PHY wake/OTG, `tps65132` wakeup, CCCI/CCIF0/RPC/FS/Audio (`/dev/ccci_aud`)/WWAN AT/DPMAIF v1 + SMEM UL modem stack (+ suspend/resume restore & bring-up log cleanup), and MT6789 SCP RV55 sensorhub IIO binding & driver |
| `patches/05-gpu/` | 2 | Mali-G57 MC2 (`panfrost`) binding and compatible |
| `patches/06-display/` | 14 | MIPI-TX DSI PHY (+ s2idle cold-start resume), `mediatek-drm` pipeline + OVL0 splash DMA halt + CRTC vblank drain fix, DSI + hardware DSC 1.1, Chipone ICNL9922C panel binding & driver, disp-mutex bind deferral |
| `patches/07-dts/` | 7 | SoC/board binding, `mt6789.dtsi`, `mt6789-advan-x1.dts`, Linux 7.4 DTS/binding alignment, ConnSys/BTIF/Wi-Fi DT nodes, all 8 multimedia clock controllers + `M8` power domains, and SCP RV55 sensorhub |
| **Total (`patches/`)** | **125** | **Upstream submission series** (out-of-tree `patches-extra/` removed) |

---

## 2. Completed Upstream-Deviation Audit, 7.4 Modernization & Modem Wave 1

An end-to-end upstream deviation audit against stock Advan X1 firmware,
MT6789 SoC reference, and upstream Linux bindings (originally conducted at
`f3d8e93` and followed by the read-only ground-truth audit in
[`docs/advan-x1-mainline-ground-truth-audit.md`](advan-x1-mainline-ground-truth-audit.md))
has been remediated and verified:

| Milestone | Commit(s) | Key Fixes & Verification |
|---|---|---|
| **Initial remediation sweep** | `f3d8e93`..`2b74552` | Fixed MT6789 `pwrap` register map (`P1`), resolved all 27 `dt-validate` schema errors (`D1.1`–`D1.13`), added CW2217 80-byte profile download (`D2.2`/`B1`), switched backlight PWM to stock `39385 ns` (`D2.5`), aligned `verify_patches.sh` on pinned baseline `666539ee`. |
| **Phase 1–3: Core, IOMMU, Display** | `3b1f69f`..`0d9a170` | Fixed Mali-G57 MFGPLL devfreq freeze (`CLK_TOP_MFG_PLL_SEL` mux-park notifier), enabled `disp_iommu` (0 M4U faults / 0 `swiotlb` exhaustion), halted bootloader OVL0 splash DMA, added `syscon-reboot-mode`, cleared `priv->dsc = NULL` in `mtk_dsc_stop()`, standardized patch identity to `sysretq0@proton.me`. |
| **Phase A/B & Suspend-to-RAM** | `074d43d`..`2abd89e` | Added LVTS STAGE3 hw thermal reboot, 30 s CCCI HS1/HS2 timeout, MT6366 `XO_NFC` clkbuf, MT6358 eFuse NVMEM, MT6366 PMIC thermal, ACCDET eFuse/CDD, SMI 2×1 sub-common restore, MIPI-TX/DSI s2idle resume (hardware suspend/resume survived), and hardware-verified microSD CD polarity (`GPIO_ACTIVE_LOW` in `2abd89e`). |
| **Linux 7.4 Modernization** | `d3e3b51`..`f1cc338` | Adopted 7.4 typed allocators (`kzalloc_obj`/`devm_kzalloc_obj`), `DEFINE_SIMPLE_DEV_PM_OPS`/`DEFINE_RUNTIME_DEV_PM_OPS`, `<linux/cleanup.h>` guards, `mtk_clk_simple_probe`, strict `__le*` sparse annotations in `mtk_ccci`, and dropped the out-of-tree `patches-extra/` vendor TDDI driver. Note: `01-clk/0002` (`topckgen`) was intentionally left on its existing helpers (`f1cc338` dropped `01-clk/0008` to avoid style-only churn on already-upstream patterns; commit `d3e3b51`'s historical message predates that drop). |
| **Modem Wave 1 & Debug Cleanup** | `f492bf9` | Added `/dev/ccci_aud` speech mailbox, DPMAIF suspend/resume register restore, `PCM 2` / `Hostless_Speech` AFE voice crossbar, UCM2 `VoiceCall.conf`, PipeWire latency packaging, ModemManager IPv4/IPv6/`+CGACT?`/voice/SMS AT init, and stripped bring-up debug spam/services. |
| **VoLTE/IMS, ConnSys Wi-Fi/BT, Full Clocks & M8** | `e505c0f`..HEAD | Added `ccci_audd`, native VoLTE/IMS + RTP/AMR-WB stack, ConnSys Wi-Fi (`wlan0` verified) + native BlueZ `hci_mtk_stp`, all 8 remaining MT6789 multimedia clock controllers (57 gates bit-exact from Tier-A stock `.ko`s), all 7 remaining `M8` power domains (`MTK_SCPD_KEEP_DEFAULT_OFF`), and replaced the `drivers/pmdomain/core.c` compatible string check with generic `GENPD_FLAG_NO_ATTACH_POWER_ON` / `MTK_SCPD_NO_ATTACH_POWER_ON`. |

---

## 3. Remaining Pre-Upstream Items (Deferred Tech Debt)

Only one structural item remains before splitting the series for LKML submission (`M8` is now complete):

1. **`M2b` — CCCI Driver Subsystem Boundary:**
   `drivers/net/wwan/mtk_ccci/` stays under `drivers/net/wwan/` while full
   modem bring-up (data, CS voice, SMS, VoLTE) matures. Before LKML submission,
   split the SoC modem boot + CCIF0 transport into `drivers/soc/mediatek/` (or
   `drivers/remoteproc/`) and keep `drivers/net/wwan/` strictly for the WWAN
   port/netdev layer (see [`docs/ccci-sip-abi.md`](ccci-sip-abi.md)).

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
[`docs/advan-x1-mainline-ground-truth-audit.md`](advan-x1-mainline-ground-truth-audit.md).
