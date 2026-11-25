# Upstream Readiness & Patch Series Status

This document tracks the upstream patch queue layout, completed audit
remediation, and the remaining pre-LKML items for the Advan X1 (MT6789)
mainline Linux port.

- **Pinned upstream baseline:** `666539eecdf4db623f5e3c3e55cf9f2dfad24aa4` (Linux v7.4-rc3/rc4)
- **Patch author / Signed-off-by:** `sysretq0 <sysretq0@proton.me>` (standardized across all patches in `0d9a170`)
- **Automated gate:** `./scripts/verify_patches.sh` (clean `git apply`, zero DTS drift vs `dts/`, 0 `dtc` warnings, 0 `dt-validate` schema errors)

---

## 1. Patch Series Layout (71 Upstream Patches + 2 Optional)

| Directory | Count | Scope |
|---|---:|---|
| `patches/01-clk/` | 5 | MT6789 clock/reset bindings, core clocks (`apmixedsys`, `topckgen`, `infra_ao` + MFGPLL mux-park notifier), `imp_iic_wrap`, `mmsys`, `mfgcfg` |
| `patches/02-pinctrl/` | 2 | MT6789 pinctrl binding and 188-pin pinctrl/EINT driver |
| `patches/03-soc-power/` | 14 | SCPSYS power domains, MT6789 PMIC wrapper (`mt6789_regs[]`), regulator coupler, MMSYS, disp-mutex (+ probe deferral), SMI, socinfo, disp IOMMU (`mtk-iommu`) |
| `patches/04-peripherals/` | 37 | SPI, I²C (`mt8188_compat`), eFuse, watchdog, LVTS thermal (+ per-SoC zero-MSR guard), DISP PWM, MT6358 long-press fix, MMC, SC89601A charger, CW2217 fuel gauge, Chipone ICNL9922C SPI touch, MT6789 AFE/MT6366 audio + FourSemi FS1599 + MT6358 ACCDET, UFS/USB PHY bindings + T-PHY wake/OTG, `tps65132` wakeup, CCCI/CCIF0/RPC/FS/WWAN AT/DPMAIF v1 + SMEM UL modem stack |
| `patches/05-gpu/` | 2 | Mali-G57 MC2 (`panfrost`) binding and compatible |
| `patches/06-display/` | 8 | MIPI-TX DSI PHY, `mediatek-drm` pipeline + OVL0 splash DMA halt, DSI + hardware DSC 1.1, Chipone ICNL9922C panel binding & driver, disp-mutex bind deferral |
| `patches/07-dts/` | 3 | SoC/board binding, `mt6789.dtsi`, `mt6789-advan-x1.dts` |
| **Total (`patches/`)** | **71** | **Upstream submission series** |
| `patches-extra/` | 2 | Out-of-tree Chipone vendor TDDI fallback (`cts_core`). **Excluded from upstream submission.** |

---

## 2. Completed Upstream-Deviation Audit & Remediation

An end-to-end upstream deviation audit against stock Advan X1 firmware,
`mp01-kernel`, and upstream Linux bindings (originally conducted at `f3d8e93`;
full historical audit and 1,576-line execution plan are preserved in git
history at commit `0d9a170`) has been **100% remediated and hardware-verified**:

| Milestone | Commit(s) | Key Fixes & Verification |
|---|---|---|
| **Initial remediation sweep** | `f3d8e93`..`2b74552` | Fixed MT6789 `pwrap` register map (`P1`), resolved all 27 `dt-validate` schema errors (`D1.1`–`D1.13`), added CW2217 80-byte profile download (`D2.2`/`B1`), switched backlight PWM to stock `39385 ns` (`D2.5`), aligned `verify_patches.sh` on pinned baseline `666539ee`. |
| **Phase 1: Core correctness** | `3b1f69f` | Fixed live Mali-G57 MFGPLL devfreq freeze via `CLK_TOP_MFG_PLL_SEL` mux-park notifier (`#0011`), added `i2c3_pins` pinctrl (`M4`), replaced generic LVTS change with per-SoC zero-MSR guard (`M3`), hardened CCCI SMC/regulators/carveout (`M2a`/`M6`), demoted I²C chatter to `dev_dbg`, fixed series patch hygiene (`M13`). |
| **Phase 2: IOMMU, WDT, MFGCFG** | `e204148` | Enabled `disp_iommu` (`14016000.iommu`, eliminating `swiotlb` exhaustion `#0012`/`M1`), added `syscon-reboot-mode` on `10007000.watchdog` (`M10`), documented CCCI EL3 SiP ABI (`docs/ccci-sip-abi.md`, `M2b`), added spinlock + HW readback to `clk-mt6789-mfgcfg` (`M5`). |
| **Phase 3: Polish & HW verification** | `b1a3ae6` | Halted bootloader OVL0 splash DMA in `mtk_disp_ovl_probe()` → **0 M4U faults** and 0.8 ms DSI bind across full 198.6 s Wayland session; removed spurious `mediatek,pre-emphasis` from `u2port0` (`M14`); verified 0 HW-on clock leaks (`M7`), `GPIO 85/107` active-low resets (`M9`), and live `st_nci` IRQ 16 (`M11`); oracle-verified SCPSYS/CCIF0/clock constants (`B3-02/04/07/08/10`). |
| **Author cleanup & DSC fix** | `0d9a170` | Cleared `priv->dsc = NULL` in `mtk_dsc_stop()` (`B2-05`); standardized patch email to `sysretq0@proton.me` across all 60 `patches/`, 2 `patches-extra/`, and board DTS. |

---

## 3. Remaining Pre-Upstream Items (Deferred Tech Debt)

Only three structural items remain before splitting the series for LKML
submission:

1. **`M8` — Multimedia / Connectivity Power Domains (8 deferred):**
   `CONN`, `ISP`, `IPE`, `VDEC`, `VENC`, `CAM`, `CAM_RAWA`, and `CAM_RAWB` are
   omitted from `mt6789-pm-domains` until their corresponding clock controllers
   (`camsys`, `imgsys1`, `ipesys`, `vdecsys`, `vencsys`) are implemented in
   `mt6789.dtsi`.
2. **`B2-02` — `mtk_disp_ddp_comp_driver` No-Op Probe Stub:**
   `patches/06-display/0004` binds `dither`, `dsc`, and `postmask` via a
   minimal no-op component stub so SCPSYS `sync_state()` completes cleanly.
   Before upstreaming `mediatek-drm`, split these into proper upstream
   `mtk_ddp_comp` sub-drivers.
3. **`M2b` — CCCI Driver Subsystem Boundary:**
   `drivers/net/wwan/mtk_ccci/` (`patches/04-peripherals/0026`) stays under
   `drivers/net/wwan/` while Phase 5.2–5.4 WWAN AT and DPMAIF netdev support
   are being built. If submitted upstream prior to WWAN/DPMAIF completion,
   split the SoC modem boot + CCIF0 transport into `drivers/soc/mediatek/` (or
   `drivers/remoteproc/`) and keep `drivers/net/wwan/` strictly for the WWAN
   port/netdev layer (see [`docs/ccci-sip-abi.md`](ccci-sip-abi.md)).
