# Upstream Deviations Audit — Advan X1 (MT6789) mainline port

> **Consolidation & Status Update (Post-`e204148` / Phase 3):**
> This document is the single canonical upstream deviation audit for `advan-x1-mainline`, superseding the historical `f3d8e93` baseline below as well as the later untracked `2b74552` drafts (`DEVIATION_AUDIT.md` and `upstream-deviation-review-compiled.md`).
>
> **Summary of Reconciled `2b74552` → Phase 3 Dispositions:**
> - **Resolved & Hardware-Verified in Phase 1 (`3b1f69f`, `console-ramoops-3b1f69f.log` — 88.67 s Wayland session, 0 GPU/Panfrost errors, clean `userrequested` reboot):**
>   - `M5` / `#0011` (GPU devfreq freeze): added `mfg0` `"alt"` (`CLK_TOP_MFG_REF_SEL`) + `CLK_TOP_MFGPLL` mux-park notifier (`01-clk/0002`, `dts/mt6789.dtsi`).
>   - `M4` / `B4-01`: removed duplicate board `i2c3_pins` override (`dts/mt6789-advan-x1.dts`).
>   - `M3` / `B4-04`: gated LVTS raw-zero `-EAGAIN` (4-retry cap → `-EIO`) and 2 ms start settle delay per-SoC in `struct lvts_data` (`04-peripherals/0008`–`0009`).
>   - `M2a` / `M6`: hardened CCCI (`04-peripherals/0025`–`0026`) — upstream `<linux/soc/mediatek/mtk_sip_svc.h>`, `arm_smccc_smc()` error checks, `md-mem0`-derived `md_img_size`, regulator checks, MD MTCMOS active-traffic guard, optional `mediatek,dpmaif`.
>   - `B2-08`: restored upstream `dev_dbg()` in `i2c-mt65xx` (`04-peripherals/0004`).
>   - `M13` / `B4-02`–`B4-06`: regenerated all 12 dummy-header patches, updated all 10 `sysretq0` YAML `maintainers:` lines across 7 binding patches, removed Q25 comment, updated `05-gpu/0002` commit body.
> - **Resolved & Hardware-Verified in Phase 2 (`e204148`, `console-ramoops-e204148.log` — 146.49 s Wayland session, 0 panics/deferred probes/Panfrost errors/swiotlb errors, 0 runtime M4U faults):**
>   - `M1` / `#0012` (`disp_iommu`): added MT6789 support to `drivers/iommu/mtk_iommu.c` (`03-soc-power/0014`, 34-bit IOVA / 35-bit PA with 32-bit TTBR) and enabled `disp_iommu` in `mt6789.dtsi` — completely eliminated `mediatek-disp-ovl: swiotlb buffer is full` on 8 GiB RAM (`larb0`/`larb1` bound, `ovl0`/`rdma0` in `iommu group 0`).
>   - `M10` (`watchdog@10007000`): added `syscon` + `simple-mfd` + `reboot-mode` (`offset = 0x24`, `mask = 0x0f`) in lockstep with `mediatek,mtk-wdt.yaml` (`04-peripherals/0006`).
>   - `M2b`: documented EL3 SiP SMC ABI & Loki handover contract in [`docs/ccci-sip-abi.md`](./ccci-sip-abi.md).
>   - `M5` / `B2-01` / `B2-04`: converged `clk-mt6789-mfgcfg.c` (`01-clk/0005`) with spinlock-guarded `MFG_CG_CON` hardware readback and documented the `MFG_TOP_CONFIG` reset + Panfrost `clk_prepare()` invariant.
> - **Implemented in Phase 3 (Hardware Verification Sweep + M4U Handover Polish):**
>   - **M4U Splash-Handover Polish (`06-display/0004`):** In `mtk_disp_ovl_probe()`, halt active bootloader splash DMA (`DISP_REG_OVL_SRC_CON = 0`, clear `INTEN`/`INTSTA`, `DISP_REG_OVL_EN = DISP_OVL_BYPASS_SHADOW`) as soon as `ovl0` probes so physical LK framebuffer (`0xfe007000..0xfffff000`) reads do not fault in `disp_iommu` during the 12 ms window before `mediatek-drm` binds and stops DSI VDO mode.
>   - **`M7` / `B3-01` / `B2-03` (`01-clk/0002`):** Verified via `boot-diag-e204148/last-clk_summary.log` — **0** clocks leaked HW-enabled with SW count 0; 12 of 19 vendor `mt6789-clkitg.dtsi` bring-up clocks are actively claimed by mainline drivers and 7 gate off cleanly; only the 3 `CLK_IS_CRITICAL` clocks (`scp_sel`, `ifrao_66mp_mclkp`, `ifrao_cpum`) stay enabled without a consumer. Refined rationale comments in `clk-mt6789-infra_ao.c`.
>   - **`M9` / `B3-05` (`dts/mt6789-advan-x1.dts`):** Verified via `last-gpio.log` (`gpio-85` panel & `gpio-107` touch both `out hi ACTIVE LOW` while running) and driver calls (`1`=assert physical LOW, `0`=deassert physical HIGH); documented gpiod active-low semantics vs stock raw-GPIO `0x00`.
>   - **`M11` / `D2.6` / `B8` (`initramfs/init`):** `nfc0` binds cleanly (`gpio-1` IRQ active transition observed `hi`→`lo`); fixed `VP-4.5` grep pattern (`st_nci` underscore) so `boot-diag` captures `/proc/interrupts` counts; live RF tag poll (`nfctool -d nfc0 -p`) remains an optional operator hardware test.
>   - **`M14` DTS (`dts/mt6789.dtsi` + `dts/mt6789-advan-x1.dts`):** Removed spurious `mediatek,pre-emphasis = <1>;` from `u2port0` (oracle-verified against `runtime_merged.dts:7876-7880` and `mp01` `phy-mtk-tphy.c:80-88`: stock sets `rev4 = BIT(28)`, NOT `rev6/pre-emphasis = GENMASK(31,30)`); confirmed `touchscreen-x/y-mm = 70 × 160` matches `panel-chipone-icnl9922c.c`.
>   - **`B3-02` (PWRAP):** Verified `mt6789_regs[]` (`INT_EN=0x450`, `INT_FLG=0x458`, `INT_CLR=0x45C`) against `mp01` `mtk-pmic-wrap.c:1041-1050` & upstream `mt8195_regs[]`/`pwrap_interrupt()` (+8/+12 bank stride prevents offset-0 alias IRQ storm).
>   - **`B3-04` (MIPI-TX):** Verified all cold-start constants (`MIPITX_PLL_CON4=0x00FF12E0`, `MIPITX_LANE_CON=0x3FFF0180/0080/0100`, `SW_CTRL_CON4`, `PLL_PWR`) against `mp01` `mediatek_v2/mtk_mipi_tx.c` N6/N4 D-PHY sequences (`L1903-1907`, `L2683-2687`, `L3128-3129`).
>   - **`B3-07` (CMA `alloc-ranges`):** Verified live placement (`0x68c00000..0x6cbfffff`) clear of `ramoops@48090000` and all modem/SCP `no-map` carveouts; updated `linux,cma` comment in `mt6789-advan-x1.dts`.
>   - **`B3-08` & `B3-10` (CCCI):** Verified `srclken-o1 = <0x200000>` & `power-flow-config = <0x12>` against `runtime_merged.dts:4686-4687` / `mp01` `md_sys1_platform.c:854-922, 1698` and documented both plus the `0x7274626e` (`"rtbn"`) CCIF0 SRAM tail footer in [`docs/ccci-sip-abi.md`](./ccci-sip-abi.md).
> - **Meta-Audit Errata Permanently Corrected (do NOT re-open):**
>   - `B3-06` (`ufsfde_ck` 7th UFS clock) is **confirmed present** in Advan stock `runtime_merged.dts:6162-6174` — preserve all 7 clocks and 3 resets.
>   - `M12` (charger `@6a`) is hardware-verified (`Reg[14]=0x64`, `0x6b` NACK) — preserve.
>   - `M14` YAML complaints (`ccif.yaml` 2-item `items:` and `ccci.yaml` `*-supply`) and `04-peripherals/0018` `panel: true` already conform to dtschema.
>   - `scripts/verify_patches.sh` is tracked in git (not gitignored).
> - **Remaining Open Items (Phase 4 — Pre-Upstream Deferred):**
>   - `M8` (8 unused power domains when camera/ISP/video are enabled), `B2-02` (DDP no-op stub), `B2-05` (`priv->dsc` NULL clear on stop).
>
> ---
>
> ## Historical Baseline (`f3d8e93`)
>
> **Audit type:** read-only. No tracked file, source, binary, or git state was modified.
**Report file:** this document is the sole new, untracked file.

## Scope, baselines and method

| Role | Artifact used |
|---|---|
| Target port | `/work/mainline/advan-x1-mainline` @ `f3d8e93bc9d05cdc4403dfb6091de3b8c6f17600` (branch `main`, clean worktree), 51 patches under `patches/*/*.patch` (11,274 added / 132 removed lines) |
| Clean upstream baseline | `/work/mainline/linux` @ `666539eecdf4db623f5e3c3e55cf9f2dfad24aa4` = **v7.4.0-rc3** (`Makefile:1-5`), branch `master`. `grep -ril mt6789` over the clean tree returns **0 files** — this is a new-SoC port, not a modification of existing MT6789 support. |
| Mature upstream MTK reference | `mt8186` / `mt8195` / `mt8188` / `mt8365` implementations in `arch/arm64/boot/dts/mediatek/` and `drivers/{clk,pinctrl,pmdomain,soc,memory,gpu/drm,phy,pwm}/mediatek/`; minimal MTK SoCs ignored |
| Vendor source | `/work/android-kernel/mp01-kernel`, `/work/android-kernel/mp01-kernel-modules`, `/work/android-kernel/motorola-kernel-modules`; mp01 DTS: `arch/arm64/boot/dts/mediatek/{mt6789.dts,mt6789-clkitg.dtsi,mt6789-disable-unused.dtsi,cust_mt6789_msdc.dtsi}` |
| Board hardware authority | `/work/firmware/advan_x1/dtb/runtime_merged.dts` (9,002 lines) and `dtbo_00.dts`; `/work/firmware/advan_x1/ondevice_dumps/` |
| Binary ground truth | 179 `.ko` in `/work/firmware/vendor_boot_unpack/recovery_root/lib/modules/` + 14 under `/work/scratch/` — all `vermagic=5.10.233-android12-9-00060-gbb5c47aa4fec` |

**Reproduction of line numbers:** the full 51-patch series was applied with `patch -p1 --forward` to a pristine `git archive` of the upstream HEAD into `/tmp/advan-audit/tree` (**0 failures**). All `arch/...`, `drivers/...`, `sound/...` and `Documentation/...` file paths and line numbers in this report refer to that post-patch tree. The two standalone DTS files in the repo (`dts/mt6789.dtsi`, `dts/mt6789-advan-x1.dts`) were **diffed against the applied tree: zero drift**. Every temporary artifact (including the applied tree, `dt-validate` logs and targeted disassembly) is under `/tmp/advan-audit/`.

**Checks already run:** `dtc` (cpp → dtc) compiles the board DTB with **zero warnings**; `dt-validate` against the patched bindings yields **27 unique schema errors** (raw log `/tmp/advan-audit/dtvalidate.out`); `checkpatch.pl` run on the patch *files* reports only context-line/`-- ` separator artefacts and is **not** a real finding.

**Evidence labels:** `mainline-ref` (clean upstream source/binding), `vendor-source` (mp01/Motorola/merged DTS), `binary-verified` (exact string/constant/import/call confirmed in a named `.ko` at symbol/offset), `binary-inferred` (control flow or register intent reconstructed from targeted disassembly).

---

## 1. Executive summary

| Subsystem | Severity | Deviation Type | Source / Binary Evidence | File |
|---|---|---|---|---|
| PMIC wrapper | **HIGH** | Framework/register-map violation — wrong `PWRAP_INT_EN` offset | `vendor-source` mp01 `mtk-pmic-wrap.c:1041-1050` (`0x450`) vs port `mt8195_regs` (`0x420`). Independently found twice. | `drivers/soc/mediatek/mtk-pmic-wrap.c:2307-2318,1094,2589` |
| Charger / power | **HIGH** (verify on HW) | Hardware quirk conflict — I²C address `0x6a` vs stock `0x6b` | `vendor-source` merged DTS `:5290-5292` (`@6b`) + `:5703-5705` (different chip at `0x6a`); `binary-verified` `upm6910_charger.ko` probe tries `0x6b` then `0x6a` | `arch/arm64/boot/dts/mediatek/mt6789-advan-x1.dts:302-317` |
| Fuel gauge | **HIGH** | Behavioral delta — mandatory vendor init/profile sequence dropped | `binary-verified` `cw2217_battery.ko:cw_init` + `vendor-source` Motorola `cw2217B_fuel_gauge_base.c:712-764`; stock `batinfo` blob unused | `drivers/power/supply/cw2217_battery.c:234-295`; `mt6789-advan-x1.dts:238-243` |
| DT bindings (all) | **HIGH** | Binding/schema violations — 27 `dt-validate` errors | `mainline-ref` + `dt-validate` (sound, clock, pinctrl, GPU, SMI, LVTS, UFS, USB, tphy, keys) | Multiple YAML + `mt6789.dtsi` / `mt6789-advan-x1.dts` (§4) |
| Display (DSC) | **MEDIUM** | Core-subsystem lifetime bug — stale pointer / UAF | `mainline-ref` source inspection; panel config is `static` in a `tristate` module | `drivers/gpu/drm/mediatek/mtk_dsi.c:1218-1219,1244-1252`; `panel-chipone-icnl9922c.c:35,527` |
| Thermal (LVTS) | **MEDIUM** | Generic driver regression — zone-driven `sensor_map` removed | `mainline-ref` `lvts_thermal.c:1330-1361` vs port `:1403-1470` | `drivers/thermal/mediatek/lvts_thermal.c:1403-1470` |
| Power domains | **MEDIUM** (latent) | Completeness — 9 of 15 domains have no data | `vendor-source` mp01 `mtk-scpsys-mt6789.c:29-222`; generic driver rejects `sta_mask == 0` | `drivers/pmdomain/mediatek/mt6789-pm-domains.h:23-124` |
| Audio | **MEDIUM** | Framework violation — non-volatile register hack + missing FE ops | `mainline-ref` `mt8186-afe-pcm.c:2570`; `mt8186-mt6366.c:646-656,1304-1313` | `sound/soc/mediatek/mt8186/mt8186-afe-pcm.c:2634-2655`; `mt6789-mt6366.c:55-115,181-186` |
| Clocks / power | **MEDIUM** (verify on HW) | Behavioral delta — vendor bring-up clocks not claimed | `binary-verified` `clk-bringup.ko` (`mediatek,clk-bring-up`, `clk_bring_up_probe`, `__bring_up_enable`); mp01 `mt6789-clkitg.dtsi:9-30` | `drivers/clk/mediatek/clk-mt6789-{infra_ao,topckgen}.c` |
| MMC / display / NFC | **MEDIUM** (verify on HW) | Hardware-quirk deltas — CD, reset polarities, clkbuf, backlight PWM | `vendor-source` merged DTS + mp01; `binary-verified` `st21nfc.ko`, `leds-mtk-pwm.c` formula | `mt6789-advan-x1.dts:190,222,266-274,950,36-42` |
| Memory map | **LOW** | Structural oddity — `memory` node == `no-map` ramoops carveout | `mainline-ref` memory/reserved-memory idiom | `mt6789-advan-x1.dts:64,71-86` |
| SoC macros / docs | **LOW** | Convention — MT8192 macro reuse; YAML example drift | `vendor-source` mp01 `scpsys.h:44-66`; self-inconsistent YAML vs DTS | `mt6789-pm-domains.h:44-120`; `mediatek,mt6789-pinctrl.yaml:138,140` |
| Process hygiene | **LOW** | Reproducibility — moving base ref, missing `verify_patches.sh` | `mainline-ref` build script/README read | `scripts/build.sh:23,53-57`; `README.md:58-59` |

---

## 2. Inventory of inspected Advan `.ko` binaries

All 193 candidate modules were triaged at metadata level (`readelf -p .modinfo`, `strings -a -t x`, `nm -u`); only a handful needed targeted disassembly. Paths: `M` = mp01 kernel/modules, `R` = Motorola kmods, `A`/`I` = applied port board DTS / SoC DTSI.

| Module | Matching Subsystem | Closest Source Match | Key Advan-Specific Deltas |
|---|---|---|---|
| `clk-mt6789.ko` + 10 `clk-mt6789-{audiosys,bus,cam,disp_dsc,i2c,img,impen,mfgcfg,mmsys,vcodec}.ko` | Clock (topckgen/apmixed/infra_ao + sub-blocks) | M `drivers/clk/mediatek/clk-mt6789*.c` | One `.ko` per block, all depending on `clk-common`; port folds these into built-in `COMMON_CLK_MT6789` drivers with identical tables. AFE/cam/img/vdec/venc block clocks have no mainline consumer. |
| `clk-common.ko`, `clkbuf.ko`, `clk-bringup.ko`, `clk-{chk,dbg,fmeter}-mt6789.ko`, `clk-disable-unused.ko` | Clock infra / DCXO buffer / debug | M `drivers/clk/mediatek/{clk-common*,clkbuf*,clk-bringup,clk-fmeter*}.c`; `clkbuf.ko` has no source match | `clkbuf.ko` reads `mediatek,clkbuf-*` / `mediatek,xo-buf-*` and is imported by `st21nfc.ko` (`clk_buf_ctrl`) — no clkbuf node in `A`/`I`. `clk-bringup.ko` enables 19 clocks; `clk-disable-unused.ko` alias `mediatek,clk-disable-unused`. |
| `pinctrl-mt6789.ko`, `pinctrl-mtk-v2.ko` | Pinmux / pinconf | M `drivers/pinctrl/mediatek/pinctrl-mt6789.c`, `pinctrl-mtk-common-v2.c` | Data-only; imports `mtk_paris_pinctrl_probe`, `mtk_pinconf_adv_drive_*`; no DT props. Board GPIO assignments live only in `A` (panel RST 85, LDO18 150, TCPC IRQ 13, touch IRQ 9/RST 107). |
| `mediatek-drm.ko`, `mtk_panel_ext.ko`, `panel-fhdp-icnl9922-dsi-vdo-boe678-120hz.ko`, `hardware_info.ko` | DRM/KMS + panel | M `drivers/gpu/drm/mediatek/mediatek_v2/*`; panel driver is Advan/HDplus-only | Panel compat `hdplus,icnl9922c,120hz` (port uses `chipone,icnl9922c`); 1080×2460, 60/90/120 Hz, I²C panel-ID probe; vendor `lcm_prepare` = `udelay(2000)` @ `0xb08` + 9×`udelay(1000)` @ `0xc44..0xca4` (binary-verified). |
| `chipone-tddi.ko`, `gf_tee.ko` | Touch / fingerprint | R `drivers/input/touchscreen/chipone_tddi_*` | `version=v3.4.4`; ~20 `chipone,*` factory-test props + `tpd-*`, `lcm-resolution`, SPI-NOR `Macronix,MX25R2035F` firmware; vendor probe rewrites firmware, port instead bundles 71,792-byte blob in initramfs. |
| `mtk-kpd.ko`, `mt6358-regulator.ko`, `mt6397.ko`, `rtc-mt6397.ko`, `mt6358_battery.ko`, `pmic_lbat_service.ko`, `pmic_lvsys_notify.ko` | PMIC / keys / battery | M `drivers/input/keyboard/mtk-kpd.c`, `drivers/regulator/mt6358-regulator.c`, `drivers/mfd/mt6397-*`, `drivers/rtc/rtc-mt6397.c` | Vendor props `mediatek,key-debounce-ms`/`hw-map-num`/`hw-init-map`/`use-extend-type`, `mediatek,oc-irq-enable-delay-ms`, `mediatek,mt6363-lvsys-notify` absent from `A`/`I`; port uses mainline matrix-keypad + regulator bindings. |
| `cw2217_battery.ko` | Fuel gauge (Cellwise CW2217) | R `drivers/power/cw2217b_fg_mmi/cw2217B_fuel_gauge_base.c`; port `drivers/power/supply/cw2217_battery.c` | Compat `cellwise,cw221X` (port `cellwise,cw2217`); 80-byte `batinfo` profile in stock DT is not consumed. `cw_init` sequence binary-verified (§4 D2.2 / §5 B1). |
| `upm6910_charger.ko`, `sc854x_charger.ko`, `mtk_charger_framework.ko`, `mtk_chg_type_det.ko`, `mtk_pd_charging.ko`, `mtk_pep{20,50}.ko`, `tcpc_rt1711h.ko`, `rt_pd_manager.ko` | Charging / USB-PD | M `drivers/power/supply/mtk_*`, vendor-only for `upm6910`/`sc854x` | `upm6910_charger.ko` is really **Southchip SC89601A/S** (`"SC89601A Reg[%.2x]"`, `"SC89601S Reg[%.2x]"`); 13 `upm,upm6910,*` thresholds; port adds `southchip,sc89601a` to `bq25890_charger.c` instead. `sc854x` secondary charger has no port node. |
| `i2c-mt65xx.ko`, `spi-mt65xx.ko`, `mtk-mmc*.ko`, `mtk_wdt.ko` | Bus / storage / watchdog | M `drivers/{i2c/busses/i2c-mt65xx,spi/spi-mt65xx,mmc/host/mtk-mmc,watchdog/mtk_wdt}.c` | Compat `mediatek,mt6789-*` present in `I`; vendor tuning props (`mediatek,clk_div_ctrl`, `mediatek,hs200-cmd-int-delay`, `mediatek,rg_dfd_timeout`, …) absent from `A`/`I`. |
| `mtk-scpsys*.ko`, `mtk-pm-domain-disable-unused.ko`, `mtk-smi*.ko`, `mtk-dvfsrc*.ko`, `mtk-emi.ko` | Power domains / SMI / DVFSRC | M `drivers/soc/mediatek/mtk-scpsys*.c`, `drivers/memory/mtk-smi.c`, `drivers/soc/mediatek/mtk-dvfsrc*.c` | `mediatek,scpsys-bring-up`, `mediatek,mt6789-dvfsrc`, SMI debug props absent; port uses generic `mtk-pm-domains` with only 6 domain data sets. |
| `phy-mtk-tphy.ko`, `phy-mtk-ufs.ko`, `mtk-pmic-wrap.ko`, `mt6375*.ko`, `rt4831a_drv.ko`, `rt5133-regulator.ko` | PHY / PMIC wrap / sub-PMIC / bias | M `drivers/phy/mediatek/*`, `drivers/soc/mediatek/mtk-pmic-wrap.c`, `drivers/mfd/mt6375.c` | PHY eye-tuning props mostly absent; `mediatek,mt6375*`, `mediatek,gate-ic-i2c` (`rt4831a`), `richtek,rt5133` have no port nodes. `mt6789-pwrap` compat present in `I` but with wrong register map (§3, P1). |
| `st21nfc.ko` | NFC | M `drivers/misc/mediatek/nfc/st21nfc/st21nfc.c`; R `drivers/nfc/st21nfc/st21nfc.c` | Requires `mediatek,nfc-gpio-v2` node + `gpio-{irq,pidle,rst}-std` + `mediatek,irq_nfc-eint` + clkbuf; port uses mainline `st,st21nfcb-i2c`. |
| `snd-soc-fs1599.ko` (scratch) | Audio codec | none | Compat `foursemi,fs15xx`, props `fsm,{irq,rst}-gpio`; not wired in port (uses MT6366 codec). |
| `leds-mtk*.ko`, `pwm-mtk-disp.ko`, `leds-mtk-disp.ko` | Backlight / LED | M `drivers/leds/leds-mtk*.c`, `drivers/pwm/pwm-mtk-disp.c` | Vendor `max-hw-brightness` + `led_pwm_set()` `/4` duty cap; port uses generic `pwm-backlight` (§5, D2). |
| Grouped (≈140 remaining) | Debug/telemetry (~30), connectivity (~14), display/DMA companions (~12), other power/thermal (~18), storage/UFS (~10), misc board/PMIC (~25) | Mostly M | Vendor-only ioctls/daemons (`aee_*`, `mrdump`, `mmprofile`, `tinysys-*`, `mcDrvModule`, `mtk_mmdvfs*`, …). Their absence is an intentional mainline decoupling, not a board-quirk regression. |

---

## 3. Upstream & MediaTek framework violations

### P1 — PMIC wrapper: wrong register map for MT6789 [HIGH]

- **Location:** `drivers/soc/mediatek/mtk-pmic-wrap.c:2307-2318`, `pwrap_mt6789` (`.regs = mt8195_regs` at `:2308`, `.int_en_all = 0x180000` at `:2311`, `.caps = PWRAP_CAP_ARB` at `:2315`); write site `:2589` (`pwrap_writel(wrp, wrp->master->int_en_all, PWRAP_INT_EN)`); offending array `:1094` (`[PWRAP_INT_EN] = 0x420`).
- **Observed in Port:** `0x180000` is written to MMIO `0x420`; because `PWRAP_CAP_INT1_EN` is not set, `0x450` is never programmed.
- **Expected Upstream / Reference Behavior:** mp01 `drivers/soc/mediatek/mtk-pmic-wrap.c:1041-1050` defines a dedicated `mt6789_regs[]` with `[PWRAP_INT_EN] = 0x450`, and its `pwrap_mt6789` (`:2518-2529`) uses it with the identical mask/caps. The `enum pwrap_regs` index of `PWRAP_INT_EN` is 37 in both trees, so the tables are directly comparable; WACS2/SWINF offsets agree, only the interrupt-enable region differs.
- **Evidence & Confidence:** `vendor-source` (mp01 file/lines above), `mainline-ref` (port file/lines above). Independently reproduced by two subsystem audits. High confidence in the code defect; hardware impact needs validation.
- **Impact:** PMIC-to-SoC interrupt enable is not programmed; PMIC keys, RTC, charger and fuel-gauge wake events can be lost, and an unrelated wrapper register at `0x420` is clobbered on every `pwrap_init()`.
- **Suggested Direction:** `port hardware quirk cleanly` + `verify on hardware` — add an upstream-style `mt6789_regs[]` carrying the vendor offsets (at minimum `PWRAP_INT_EN = 0x450`) and point `pwrap_mt6789` at it.

### P2 — Generic LVTS driver changed to enable sensors before zone registration [MEDIUM]

- **Location:** `drivers/thermal/mediatek/lvts_thermal.c:1403-1434` (`lvts_ctrl_start()`): `sensor_map |= sensor_bitmap[i]` at `:1419-1420` for every valid sensor, MSRCTL1/MONCTL0 writes and `usleep_range(2000, 3000)` at `:1423-1434`, all **before** the zone-registration loop at `:1436`.
- **Observed in Port:** all sensors marked valid in `lvts_ctrl_data` are enabled in hardware regardless of whether a thermal zone binds to them; upstream's zone-driven accumulation was removed.
- **Expected Upstream / Reference Behavior:** clean tree `drivers/thermal/mediatek/lvts_thermal.c:1330-1361` assigns `sensor_map` **inside** the per-sensor loop after `thermal_zone_device_register*()`, with the comment "The initialization of the thermal zones give us which sensor point to enable…".
- **Evidence & Confidence:** `mainline-ref` (exact line ranges). High confidence in the divergence.
- **Impact:** generic LVTS semantics change for every SoC compiled with this driver: a sensor without a DT zone is still powered and sampled (power waste, unhandled over-temperature). On this board all 13 sensors have zones (`mt6789.dtsi:1164-1498`), so immediate impact is limited.
- **Suggested Direction:** `refactor to upstream MTK idiom` — restore register-zone-first ordering and zone-driven `sensor_map`; if early readings are needed, use an explicit SoC quirk, not a generic change.

### P3 — AFE `AFE_AUD_PAD_TOP` forced non-volatile to mask an `-EBUSY` [MEDIUM]

- **Location:** `sound/soc/mediatek/mt8186/mt8186-afe-pcm.c:2634-2642` (`mt6789_is_volatile_reg()`: `case AFE_AUD_PAD_TOP: return false;`) and `:2646-2655` (`mt6789_afe_regmap_config`).
- **Observed in Port:** `AFE_AUD_PAD_TOP (0x0e40)` is removed from the volatile set (replaced by `AFE_AGENT_ON`) to stop DAPM supply-widget creation from returning regmap `-EBUSY` while the AFE is runtime-suspended (commit `f3d8e93`).
- **Expected Upstream / Reference Behavior:** clean tree `mt8186-afe-pcm.c:2570` keeps `AFE_AUD_PAD_TOP` volatile; `mt8186-dai-adda.c:415-416` creates a DAPM `SUPPLY_S` on it, `:207-209` writes `0x39`/`0x31`; `mt8186-reg.h:1697-1701` defines live bits (`AUD_PAD_TOP_MON` bit 15, `FIFO_RSP` bits 7:4) that must not be cached.
- **Evidence & Confidence:** `mainline-ref` (exact files/lines). High confidence in the divergence; the runtime-PM root cause remains unfixed.
- **Impact:** regmap serves the DAPM widget a cached handshake register; live monitor bits can be stale and writes applied on top of a stale image.
- **Suggested Direction:** `refactor to upstream MTK idiom` — restore volatile handling and fix the root cause by resuming AFE runtime PM / power domain / clocks around card DAPM sync (an earlier `pm_runtime_resume_and_get()` wrapper existed in `50f8669`).

### P4 — MT6789 audio card omits common FE ops and PCM constraints [MEDIUM]

- **Location:** `sound/soc/mediatek/mt8186/mt6789-mt6366.c:55-115` (FE links `Playback_1/2/3`, `Capture_1/2` have no `.ops`), `:181-186` (`mt6789_mt6366_pdata` sets no `pcm_constraints`/`num_pcm_constraints`/`num_jacks`).
- **Observed in Port:** no `mtk_soundcard_common_playback_ops` / `..._capture_ops`, so `mtk_soundcard_startup()` and its `snd_pcm_hw_constraint_list()` are never invoked. The card still registers (`ALSA device list: #1: mt6789-mt6366`).
- **Expected Upstream / Reference Behavior:** `mt8186-mt6366.c:646-656` sets `.ops = &mtk_soundcard_common_playback_ops` on FE links; `:1304-1313` sets `.pcm_constraints = mt8186_pcm_constraints`, `.num_pcm_constraints`, `.num_jacks`. Generic implementation: `common/mtk-soundcard-driver.c:137-186,225-385`.
- **Evidence & Confidence:** `mainline-ref`. High confidence in the omission; impact is userspace-dependent.
- **Impact:** no rate/channel constraints are advertised; unsupported formats can pass `hw_params()` and fail later in the AFE/DAI path.
- **Suggested Direction:** `refactor to upstream MTK idiom` + `verify on hardware` (playback/capture at non-default rates) — add the common FE ops and an MT6789-appropriate constraints table.

### P5 — Partial power-domain data set [MEDIUM, latent]

- **Location:** `drivers/pmdomain/mediatek/mt6789-pm-domains.h:23-124` (`scpsys_domain_data_mt6789[]`), `include/dt-bindings/power/mt6789-power.h:5-19` (15 domains), array size/`:127` (`num_domains = 12`).
- **Observed in Port:** only `MFG0`-`MFG3`, `DISP`, `AUDIO` have data. `MD`(0), `CONN`(1), `ISP`(6), `IPE`(7), `VDEC`(8), `VENC`(9) are zero-initialized (`sta_mask == 0`); `CAM`(12), `CAM_RAWA`(13), `CAM_RAWB`(14) exceed `num_domains`. Vendor subdomain hierarchy is absent.
- **Expected Upstream / Reference Behavior:** mp01 `drivers/soc/mediatek/mtk-scpsys-mt6789.c:29-222` defines all 14 domains with correct `sta_mask`/`ctl_offs`/bus protection (e.g. `MD` BIT(0)/0x300, `ISP` BIT(13)/0x334, `CAM` BIT(23)/0x35C). Generic `drivers/pmdomain/mediatek/mtk-pm-domains.c:868-875` rejects a node whose id is `>= num_domains` or whose `sta_mask == 0`.
- **Evidence & Confidence:** `vendor-source` + `mainline-ref`. High confidence.
- **Impact:** no active breakage — current DTS references only the six populated domains — but camera/ISP/video enablement will fail at genpd registration, and vendor subdomain wiring is lost.
- **Suggested Direction:** `port hardware quirk cleanly` when those consumers are enabled.

### P6 — MT8192 bus-protection macros reused for MT6789 [LOW]

- **Location:** `drivers/pmdomain/mediatek/mt6789-pm-domains.h:44-62,97-104,117-120`, macros from `include/linux/soc/mediatek/infracfg.h:128-139`.
- **Observed in Port:** MFG1/DISP/AUDIO protection uses `MT8192_TOP_AXI_PROT_EN_*` names.
- **Expected Upstream / Reference Behavior:** upstream convention is SoC-named macros; mp01 `drivers/soc/mediatek/scpsys.h:44-66` defines `MT6789_TOP_AXI_PROT_EN_*`. **Numeric values are identical** (vendor `BIT(21)`, `BIT(5)|BIT(6)`, `BIT(21)|BIT(22)`, `BIT(7)`, `BIT(4)` and SET/CLR/STA1 offsets `0x2a0/0x2a4/0x228`, `0x2a8/0x2ac/0x258`, `0x2d4/0x2d8/0x2ec`, `0x714/0x718/0x724` all match).
- **Evidence & Confidence:** `vendor-source` + `mainline-ref`. High confidence that this is naming only, not a functional bug.
- **Impact:** maintenance hazard — a future change to shared MT8192 macros silently changes MT6789 behaviour.
- **Suggested Direction:** `refactor to upstream MTK idiom` — define `MT6789_*` macros.

### P7 — DSC configuration pointer outlives the panel module [MEDIUM]

- **Location:** `drivers/gpu/drm/mediatek/mtk_dsi.c:1218-1219` (`mtk_dsi_host_attach()` caches `dsi->dsc = device->dsc`, `dsi->hs_rate = device->hs_rate`), `:1244-1252` (`mtk_dsi_host_detach()` only does `component_del()` + `drm_bridge_remove()`, never clears them); `drivers/gpu/drm/mediatek/mtk_ddp_comp.c:223-228` (`mtk_dsc_set_dsc()` caches the same pointer); `drivers/gpu/drm/panel/panel-chipone-icnl9922c.c:35` (`static struct drm_dsc_config icnl9922c_dsc`, handed out at `:527`), Kconfig `:131` (`tristate`).
- **Observed in Port:** host and DDP comp retain raw pointers into a module-local DSC config; `icnl9922c_remove()` (`:539-545`) calls `mipi_dsi_detach()` but nothing invalidates the cached copies, which are dereferenced at `mtk_dsi.c:836,846-849,495-496,1157-1160` and `mtk_ddp_comp.c:80,111-113,127,162-229`.
- **Expected Upstream / Reference Behavior:** upstream `mtk_dsi` has no DSC pointer; the generic panel pattern (e.g. `panel-ilitek-ili9882t.c:943`) keeps the DSC config attached to the `mipi_dsi_device`, not to the host.
- **Evidence & Confidence:** `mainline-ref` (source inspection of all four files; verified no `dsc = NULL` in detach). High confidence in the code fact.
- **Impact:** after `rmmod panel-chipone-icnl9922c` or driver unbind, a subsequent modeset reads freed module memory → potential crash or garbage DSC programming; clean reload is blocked.
- **Suggested Direction:** `refactor to upstream MTK idiom` — clear `dsi->dsc`/`dsi->hs_rate` in `mtk_dsi_host_detach()`, invalidate the DDP-comp copy, or make the config `const` and owned by a long-lived object.

---

## 4. DTS & binding mismatches

### D1 — 27 binding-schema validation failures [HIGH]

- **Location / Observed / Expected:** the table below enumerates every unique error; all reproduce in `/tmp/advan-audit/dtvalidate.out`. The upstream baseline for each schema is `/work/mainline/linux/Documentation/devicetree/bindings/`.
- **Evidence & Confidence:** `mainline-ref` + `dt-validate` against the patched binding set. Definitive. `dtc` itself is clean (zero warnings) — this is a schema-layer failure, so `make dt_binding_check` in the series will fail.
- **Impact:** patches cannot pass upstream binding checks; several nodes additionally have real functional consequences (missing PD/IOMMU, wrong compat branch).
- **Suggested Direction:** per row below (all `refactor to upstream MTK idiom` unless noted).

| # | Location (post-patch tree) | Violation | Direction |
|---|---|---|---|
| 1 | `Documentation/devicetree/bindings/sound/mediatek,mt6789-mt6366-sound.yaml:17-18,61`; DTS `mt6789-advan-x1.dts:98-129` | `additionalProperties: false` composed with `allOf: [$ref: sound-card-common.yaml#]` rejects the binding's own required `model` and `audio-routing` | use `unevaluatedProperties: false` |
| 2 | same binding `:24-28`; DTS `:126-128` | local `audio-routing` redefinition conflicts with the common schema; 3-pair routing rejected as too long | drop the local redefinition |
| 3 | `bindings/clock/mediatek,mt6789-clock.yaml:18-23`; DTS `mt6789.dtsi:849,854,861,867` | `compatible` declared as single-value `enum`, but four `imp_iic_wrap_*` nodes use 2-string compat (`…, "syscon"`) | use `items:` + `const: syscon` like `mediatek,mt6789-sys-clock.yaml:22-28` |
| 4 | `bindings/pinctrl/mediatek,mt6789-pinctrl.yaml:81-93`; `include/dt-bindings/pinctrl/mt65xx.h:19-26` | pull enums stop at `203`; header defines `MTK_PULL_SET_RSEL_000..111` = 200–207 | extend both enums to 200–207 |
| 5 | `bindings/display/mediatek/mediatek,{dither,dsc,postmask}.yaml`; DTS `mt6789.dtsi:1118-1138` | `power-domains` newly required; `postmask0`, `dither0`, `dsc0` lack it (all sibling display nodes have `MT6789_POWER_DOMAIN_DISP`) | add the property to the three nodes |
| 6 | `bindings/display/mediatek/mediatek,{ovl,rdma}.yaml`; DTS `mt6789.dtsi:1070-1084` | `iommus` required but `ovl0`/`rdma0` omit it (stock DTS supplies `iommus`, e.g. `runtime_merged.dts:7064`) | add correct larb/master IDs or relax the requirement |
| 7 | `bindings/gpu/arm,mali-bifrost.yaml:24,233`; DTS `mt6789.dtsi:610-613` | `mediatek,mt6789-mali` added to the 2-string bifrost branch, but DTS uses 3 strings (`…, "mediatek,mt8186-mali", "arm,mali-valhall-jm"`) | add a 3-string `oneOf` variant |
| 8 | DTS `mt6789.dtsi:1038,1049,1060` | SMI nodes carry bogus fallbacks `"mediatek,smi-common"` / `"mediatek,smi-larb"`; patched schemas and upstream DTS use a single SoC-specific string | drop the fallbacks |
| 9 | `bindings/thermal/mediatek/mediatek,lvts-thermal.yaml:47-57`; DTS `mt6789.dtsi:592-593,811-813` | binding permits two named cells; port intentionally uses one (see §7 — **not a bug**); schema must stay consistent with the port's `maxItems: 1` rule | keep schema/DTS aligned |
| 10 | DTS `mt6789-advan-x1.dts:686-700` | `keys` children named `powerkey`/`homekey`; upstream pattern requires `power`/`home` (`mt6358.dtsi:347,351`) | rename children |
| 11 | DTS `mt6789.dtsi:773-795` (`ufshci`) | 4 errors: `phys` required; `resets`/`reset-names` unevaluated; `clocks`/`clock-names` exceed the non-`mt8195` branch's `maxItems: 1` | provide `phys`/`phy-names`, reduce clocks, or extend the upstream binding with justification |
| 12 | DTS `mt6789.dtsi:834-846` (`usb0`) | bare `"mediatek,mtk-musb"` fails `mediatek,musb.yaml:17-23` (requires SoC-specific first compat) | add SoC compat to DTS and binding |
| 13 | DTS `mt6789.dtsi:816-832` (`u2phy0`) | bare `"mediatek,generic-tphy-v2"` fails `mediatek,tphy.yaml:69-85` (requires 2 strings) | add `mediatek,mt*-tphy` first compat |

### D2 — Board hardware-quirk deltas vs stock / mp01

#### D2.1 — Charger I²C address conflict [HIGH — verify on hardware]

- **Location:** `arch/arm64/boot/dts/mediatek/mt6789-advan-x1.dts:302-317`, node `charger@6a` (`reg = <0x6a>`, compat `"southchip,sc89601a", "ti,bq25890"`). Changed by port commit `f6183d7`.
- **Observed in Port:** SC89601A at `0x6a` on `i2c5`.
- **Expected Upstream / Reference Behavior (conflicting evidence, both authoritative):**
  - Stock DTS `runtime_merged.dts:5290-5319`: `upm6910_chg@6b` with `upm,upm6910_charger` and `sc,sc89601a,*` tuning values that match the port's `ti,*` values after mA→µA conversion. A **different** chip (`sc854x-charger@6A`, `"sc,sc8547-standalone"`) occupies `0x6a` at `runtime_merged.dts:5703-5705`.
  - `binary-verified`: `upm6910_charger_probe` loads `#0x6b` at `0x12e0`, probes ID reg `0xb` at `0x1320`, and on failure logs `"[upm6910]:%s: No device found, check new i2c address"` (`strings` offset `0x70b1`) before switching to `#0x6a` at `0x13b0`. The `0x6a` path is a fallback for another board revision; the port's commit interprets it as the primary address.
- **Evidence & Confidence:** `vendor-source` + `binary-verified` (`upm6910_charger.ko`, `upm6910_charger_probe+0xa8/+0x178`, offsets above). Definite code divergence; which address this specific unit responds to is `verify on hardware` (probe with both addresses and check `SC89601A Reg[14]`).
- **Impact:** if the unit is at `0x6b`, the charger never probes → no charging; if at `0x6a`, the port is correct and the stock DTB describes a different board revision.
- **Suggested Direction:** `verify on hardware`, then `port hardware quirk cleanly` — set the address that answers the chip-ID read; keep the `southchip,sc89601a` compatible (correctly added in `drivers/power/supply/bq25890_charger.c:24,30,41,49,1326,1655`).

#### D2.2 — Fuel gauge drops mandatory vendor profile sequence [HIGH]

- **Location:** `drivers/power/supply/cw2217_battery.c:234-295` (`cw2217_probe()`, especially `:267-275` mode handling and `:281` `devm_power_supply_register()`), `:29-30` (only `MODE_ACTIVE 0x00` / `MODE_SLEEP 0xf0` defined); DTS `mt6789-advan-x1.dts:238-243`.
- **Observed in Port:** reads CHIP_ID (`0x00`) requiring `0xa0`; only if MODE_CONFIG (`0x08`) is exactly `0xf0` writes `0x00` and waits 20 ms; registers the power supply. No restart pulse, profile write, config-update flag or IC-ready poll.
- **Expected Upstream / Reference Behavior:** Motorola `drivers/power/cw2217b_fg_mmi/cw2217B_fuel_gauge_base.c:712-764` (`cw_config_start_ic()`), `:221-273`, `:935-974`: write `0x30` to `0x08` + 20 ms, `0xf0` + 10 ms, write 80 bytes to `0x10..0x5f`, write `0x80` to `REG_SOC_ALERT 0x0b`, `0x00` to `REG_GPIO_CONFIG 0x0a`, then `0x30`/`0x00` to `0x08` and poll `REG_IC_STATE 0xa7` for `0x0c`; verify read-back. Stock DTS `runtime_merged.dts:5108-5117` carries the 80-byte `batinfo` profile, which the port DTS drops.
- **Evidence & Confidence:** `vendor-source` (file/lines above + merged DTS) and `binary-verified` — `cw2217_battery.ko:cw_init` (symbol offset `0x760`): `0x868` `mov w8,#0x30` → write `0x08` @ `0x894`; `0x8a8` `#0xf0` → write `0x08` @ `0x8c0`; `0x8e4..0x900` loop `cmp x22,#0x50` writing `0x10 + i`; `0x908` `#0x80` → write `0x0b` @ `0x934`; `0x928..0x93c` zero to `0x0a`; `0xa60`/`0xa9c` `0x30`/`0x00` to `0x08`; `0xae0` reads `0xa7`. High confidence.
- **Impact:** SOC, `CHARGE_FULL`/SOH, cycle count and current integration can be wrong on cold boot or after the gauge loses its profile.
- **Suggested Direction:** `port hardware quirk cleanly` — expose `batinfo` (80 bytes) in DT and implement the RESTART→SLEEP→profile→flag→ACTIVE→IC-ready sequence with 20/10 ms delays.

#### D2.3 — MSDC1 card-detect polarity inverted [MEDIUM — verify on hardware]

- **Location:** `mt6789-advan-x1.dts:190` — `cd-gpios = <&pio 7 GPIO_ACTIVE_LOW>`, no `cd-debounce-delay-ms`.
- **Observed in Port:** active-low card detect (changed by commit `f6183d7` "to avoid false card-present CMD8 retries when empty").
- **Expected Upstream / Reference Behavior:** stock `runtime_merged.dts:6228-6229` has `cd-debounce-delay-ms = <0>` and `cd-gpios = <pio 7 0x00>` (`0x00` = `GPIO_ACTIVE_HIGH`); mp01 `cust_mt6789_msdc.dtsi:51-52` says `GPIO_ACTIVE_HIGH` explicitly.
- **Evidence & Confidence:** `vendor-source` (numeric flags unambiguous) vs port commit rationale (no telemetry cited). `verify on hardware`.
- **Impact:** if the switch grounds the line on insertion (typical), the port is correct; if it drives high, SD detection is inverted and the card never initialises.
- **Suggested Direction:** `verify on hardware` (read `cd-gpios` state with/without card), then align polarity and add `cd-debounce-delay-ms = <0>`.

#### D2.4 — Panel / touch reset GPIO flags inverted [MEDIUM — verify on hardware]

- **Location:** `mt6789-advan-x1.dts:950` (panel `reset-gpios = <&pio 85 GPIO_ACTIVE_LOW>`), `:222` (touch `reset-gpios = <&pio 107 GPIO_ACTIVE_LOW>`).
- **Observed in Port:** both reset lines active-low; `panel-chipone-icnl9922c.c:384-387,508` uses `GPIOD_OUT_HIGH` then `1→0`; `chipone_icnl9922c.c:231-235,717` uses `GPIOD_OUT_LOW` then `0→1→0`.
- **Expected Upstream / Reference Behavior:** stock `runtime_merged.dts:7024` panel `reset-gpios = <pio 85 0x00>`, `:1310` touch reset GPIO 107 flags `0x00` (both active-high); port commits `8334c51`/`f3d8e93` deliberately chose standard `GPIO_ACTIVE_LOW` gpiod polarity with 10 ms assert / 40 ms deassert.
- **Evidence & Confidence:** `vendor-source` (numeric flags) + `binary-inferred` (driver gpiod intent). The numeric difference is definitive; whether the mainline driver's logical inversion compensates is hardware-dependent.
- **Impact:** if the panel/touch reset is truly active-high, the devices never leave reset (black screen / dead touch).
- **Suggested Direction:** `verify on hardware` (scope the reset lines during probe), then align flags if the driver's logical sense does not already invert.

#### D2.5 — Backlight PWM period and duty mapping diverge [MEDIUM — verify on hardware]

- **Location:** `mt6789-advan-x1.dts:36-42` — `pwms = <&disp_pwm0 0 500000>`, `brightness-levels = <0 1024 2048 4096 8192 16384 32768 65535>`, `default-brightness-level = <5>`.
- **Observed in Port:** 500,000 ns (2 kHz) period; max level maps to 100 % duty; default ≈50 %.
- **Expected Upstream / Reference Behavior:** stock `runtime_merged.dts:2134-2141` uses `0x99d9` = 39,385 ns (≈25.4 kHz) and `max-hw-brightness = <255>`; mp01 `drivers/leds/leds-mtk-pwm.c:47-65` computes `duty = period * brightness / 4 / max_hw_brightness`, so stock max is ≈25 % duty. Port PWM *data* (`pwm-mtk-disp.c:287-294`) matches vendor exactly — only the DTS policy differs.
- **Evidence & Confidence:** `binary-verified` (period from stock DTB + duty formula from vendor source) + `mainline-ref` (generic `pwm-backlight` semantics). Numbers are definite; electrical consequence requires hardware.
- **Impact:** ≈12.7× lower frequency and ≈4× higher maximum duty than stock — possible flicker, different brightness curve, or over-driving LEDs.
- **Suggested Direction:** `verify on hardware` (measure luminance/current per level), then align period to 39,385 ns and scale levels to ≈25 % max duty (or implement the `/4` vendor scaling).

#### D2.6 — NFC clock-buffer infrastructure absent [MEDIUM — verify on hardware]

- **Location:** `mt6789-advan-x1.dts:266-274` (`st,st21nfcb-i2c` node only); no `mediatek,clock_buffer` / `mediatek,nfc-gpio-v2` node anywhere in `A`/`I`.
- **Observed in Port:** mainline `st-nci` binding with `reset-gpios`/`interrupts`; mainline `drivers/nfc/st-nci/i2c.c:273` accepts the compat (pstore shows `nfc0` registered).
- **Expected Upstream / Reference Behavior (vendor):** `binary-verified` — `st21nfc.ko` strings `gpio-irq-std`, `gpio-pidle-std`, `gpio-rst-std`, `mediatek,irq_nfc-eint`, `mediatek,nfc-gpio-v2`, `"cannot find mediatek,nfc-gpio-v2 in DTS"`; imports `clk_buf_ctrl`, `clk_buf_hw_ctrl`, `of_get_named_gpio_flags`. `clkbuf.ko` compatibles include `mediatek,clock_buffer`, `mediatek,clkbuf-*`, `mediatek,xo-buf-*`, `mediatek,mt6789`.
- **Evidence & Confidence:** `binary-verified` (strings/imports) + `mainline-ref` (valid mainline binding). Definite omission; actual need depends on whether the NFC clock is already enabled by the bootloader.
- **Impact:** if the DCXO/NFC buffer is not otherwise on, NFC may fail despite registering.
- **Suggested Direction:** `verify on hardware` (NFC detect/poll), then port only the required clkbuf node cleanly if needed.

#### D2.7 — Redundant `memory` node overlapping ramoops [LOW]

- **Location:** `mt6789-advan-x1.dts:64` (CMA `alloc-ranges = <0 0x40000000 0 0xc0000000>`), `:71-86` (`ramoops@48090000` with `no-map` and a `memory@48090000` node describing the **same** 896 KiB range).
- **Observed in Port:** a `device_type = "memory"` node exactly equals a `no-map` reserved-memory carveout; CMA's range encloses it.
- **Expected Upstream / Reference Behavior:** upstream uses one `memory` node with `reserved-memory` carving sub-ranges (stock `runtime_merged.dts:460-461` is a single `reg = <0 0x40000000 0x2 0>`).
- **Evidence & Confidence:** `mainline-ref` + numeric interval equality. Verified structural oddity; impact depends on memblock ordering.
- **Impact:** possible double accounting / CMA allocating into the ramoops window; at minimum a confusing memory map.
- **Suggested Direction:** `verify on hardware` (`/proc/meminfo`, `iomem`, pmsg survival), then consolidate to a single `memory` node.

### D3 — Bootloader-leaked clocks and power domains not explicitly disabled [MEDIUM]

- **Location:** `mt6789-advan-x1.dts:23-25` (`chosen` has only `stdout-path`, no `bootargs`); no `mediatek,clk-disable-unused` / `mediatek,scpsys-disable-unused` nodes anywhere.
- **Observed in Port:** no `clk_ignore_unused`/`pd_ignore_unused` and no disable-unused nodes.
- **Expected Upstream / Reference Behavior:** mp01 `mt6789-disable-unused.dtsi:7-60+` defines dozens of such nodes; `binary-verified`: `clk-disable-unused.ko` alias `mediatek,clk-disable-unused` (modinfo offset `0x0c`), `mtk-pm-domain-disable-unused.ko` alias `mediatek,scpsys-disable-unused`. Vendor `clk-bringup.ko` (`mediatek,clk-bring-up`) enables 19 clocks; the port marks only 3 `CLK_IS_CRITICAL` (`clk-mt6789-infra_ao.c:122,162`, `clk-mt6789-topckgen.c:520`), while ~18 vendor bring-up clocks (CCIF/DEBUGSYS/BTIF/DPMAIF/ATB/PWRAP_ULPOSC group) have no consumer.
- **Evidence & Confidence:** `vendor-source` (mp01 DTS) + `binary-verified` (`.ko` aliases/symbols). Definite omission; power impact requires measurement.
- **Impact:** with no `ignore_unused`, `clk_disable_unused` will gate unclaimed clocks at late init; conversely, bootloader-enabled power domains with no driver stay on — measurable idle-power/thermal regression.
- **Suggested Direction:** `verify on hardware` (compare with `clk_ignore_unused`, measure idle power), then `port hardware quirk cleanly` by marking only the genuinely required clocks `CLK_IS_CRITICAL` and adding minimal disable-unused nodes for blocks with no mainline consumer.

### D4 — Binding example drift [LOW]

- **Location:** `bindings/pinctrl/mediatek,mt6789-pinctrl.yaml:138,140` — example uses `gpio-ranges = <&pio 0 0 189>` and `interrupts = <GIC_SPI 212 …>`.
- **Observed in Port:** actual DTS uses `188` (`mt6789.dtsi:375`) and `GIC_SPI 244` (`:378`); vendor mp01 `mt6789.dts:2303+` also uses 188.
- **Evidence & Confidence:** `mainline-ref` (same-tree YAML vs DTS) + `vendor-source`. High confidence, documentation only.
- **Suggested Direction:** `refactor to upstream MTK idiom` — correct the example.

---

## 5. Behavioral deltas vs vendor / binary ground truth

| ID | Subsystem | Delta | Evidence | Confidence | Direction |
|---|---|---|---|---|---|
| B1 | Fuel gauge | 80-byte profile download missing | `cw2217_battery.ko:cw_init`, Motorola source, stock `batinfo` | `binary-verified` + `vendor-source` | `port hardware quirk cleanly` (D2.2) |
| B2 | Charger | I²C address `0x6a` vs stock `0x6b`; secondary `sc854x` charger node absent | `upm6910_charger.ko` probe fallback; merged DTS `:5290/:5703` | `binary-verified` + `vendor-source`, HW outcome open | `verify on hardware` (D2.1) |
| B3 | MMC | CD polarity inverted vs stock/mp01 | merged DTS `:6229`, mp01 `cust_mt6789_msdc.dtsi:52` | `vendor-source` | `verify on hardware` (D2.3) |
| B4 | Panel/touch | Reset GPIO flags inverted vs stock | merged DTS `:7024,:1310` | `vendor-source` + `binary-inferred` | `verify on hardware` (D2.4) |
| B5 | Backlight | 2 kHz/100 % vs 25.4 kHz/25 % | stock DTB `:2134`, `leds-mtk-pwm.c:47-65` | `binary-verified` + `vendor-source` | `verify on hardware` (D2.5) |
| B6 | Clocks | 19 vendor bring-up clocks unclaimed | `clk-bringup.ko`, mp01 `mt6789-clkitg.dtsi:9-30` | `binary-verified` + `vendor-source` | `verify on hardware`, then selectively `CLK_IS_CRITICAL` (D3) |
| B7 | Power | disable-unused nodes absent | `clk-disable-unused.ko`, `mtk-pm-domain-disable-unused.ko`, mp01 DTSI | `binary-verified` + `vendor-source` | `verify on hardware` (D3) |
| B8 | NFC | clkbuf / `mediatek,nfc-gpio-v2` absent | `st21nfc.ko`, `clkbuf.ko` strings/imports | `binary-verified` | `verify on hardware` (D2.6) |
| B9 | Touch | vendor SPI-NOR firmware rewrite absent; port bundles 71,792-byte FW in initramfs and syncs via `drm_panel_follower` | `chipone-tddi.ko` (`Macronix,MX25R2035F`); commits `50f8669`/`f3d8e93` | `binary-verified` + `vendor-source` | **Intentional cleanup** — no action |
| B10 | Panel | panel init command bytes, DSC PPS, all 60/90/120 Hz timings, 852 MHz hs_rate, D-PHY values, ULPS/handover and OVL0/RDMA0 quirks | `panel-fhdp-…120hz.ko` `.rodata` @ `0x30805`, `.data`+`0xb5c` etc.; commits `7e8db92`,`a15a79d`,`93e4b56`,`4c1800f`,`bfb654a`,`8c3bcae` | `binary-verified` | **Verified correct** — no action |
| B11 | Thermal | LVTS eFuse `0x1a4/0x34` and V5 manual RCK are hardware-validated fixes (see §7) | commit `b553e64`/`c2fdf5b` live telemetry; mp01 `mtk_lvts_tc_mt6893.c` constants | `vendor-source` + hardware telemetry | **Intentional cleanup** — no action |
| B12 | Audio/I²C/SPI | I2C `mt8188_compat`, SPI `mt6765_compat`, MT6358 long-press bits, AFE power/clock adaptation | commits `612ecef`, `9cd7a45`, `59ee63f` | `vendor-source` + hardware telemetry | **Intentional cleanups** — no action |

---

## 6. Process / reproducibility notes [LOW]

- **PROC-1:** `scripts/build.sh:23` defaults `BASE=${BASE:-master}` — a moving ref. The exact upstream base (`666539ee`) is not recorded anywhere in the repo. As `master` advances, this exact series can fail or apply with fuzz. `Suggested: pin BASE to an immutable SHA.`
- **PROC-2:** `README.md:58-59` claims `verify_patches.sh + checkpatch.pl` enforcement, but no `verify_patches.sh` exists and `scripts/build.sh:53-57,63` applies patches with `git apply --whitespace=nowarn`. All 51 patches do carry `Signed-off-by:` and the 8 most recent commits are GPG-signed (`%G? = G`); six older history commits lack a body signoff. This is a process claim mismatch, **not** an upstream-code deviation.

---

## 7. Intentional mainline cleanups & verified-correct behavior (do NOT report as bugs)

Each item below was checked against vendor source, stock DTB, or `.ko` evidence and is a *correct* adaptation to upstream conventions or a hardware-validated fix:

1. **No generic-helper reinvention.** All five clock drivers use upstream `mtk_alloc_clk_data`, `mtk_clk_register_plls/factors/muxes/composites/gates`, `mtk_clk_pdev_probe`, `mtk_clk_simple_probe`, `of_clk_add_hw_provider` with `clk_hw_onecell_data`; pinctrl uses `mtk_paris_pinctrl_probe` / `pm_sleep_ptr`; PM domains use generic `mtk-pm-domains.c` (`mtk-pm-domains.c:1292-1294` has the MT6789 entry). This matches mature mt8186/mt8195 idiom.
2. **DT-binding headers are byte-exact vendor copies:** `include/dt-bindings/clock/mt6789-clk.h` (377 `CLK_*` macros) and `power/mt6789-power.h` (16 macros) have zero differences vs mp01 and `docs/reference/mt6789-clk.h-volla-vendor`.
3. **Clock data is numerically correct:** all 14 PLL offsets (incl. the `usbpll` special case), MMSYS gate shifts, SMI port table (`mtk-smi.c:286+` vs vendor `port_in_larb_gen2`), and all 137 referenced parent names resolve. `f26m` vs vendor `f26m_ck` is a consistent rename.
4. **LVTS eFuse offset `0x1a4/0x34`** is correct, despite differing from stock `0x1b4/0x2f8`: commit `b553e64` documents live telemetry proving the stock values are *logical atag,devinfo indices*, not physical `0x11c10000` MMIO offsets; using them shifted calibration by 16 bytes and caused instant >250 °C critical shutdowns. `c2fdf5b` adds V5 manual RCK (Auto-RCK glitches on TS2_2) with constants matching mp01. **Not a deviation.**
5. **SC89601A silicon / chip ID 4** is correct: port adds `southchip,sc89601a` to `bq25890_charger.c:24,30,41,49,1326,1655`; stock dmesg (`ondevice_dumps/dmesg.txt:6147`) shows `SC89601A Reg[14] = 0x64`; `F_PN = REG_FIELD(0x14,3,5)` → `(0x64>>3)&7 == 4`. CW2217 shunt `5000` µΩ and the current-scaling math match vendor `*160/sense_r_mohm/100` mA. (Only the *address* remains disputed — §4 D2.1.)
6. **Pinctrl 8th region `"eint"`** (`mt6789.dtsi:364,372`) is required by mainline `pinctrl-mtk-common-v2.c:388` (`nbase = count_reg_names - soc->nbase_names`); it folds the vendor's separate `apirq@1000b000` node into the pinctrl node. Duplicate `i2c3_pins` label is legal dtc and intentional.
7. **Panel/touch decoupling:** generic `drm_panel` replaces vendor `mtk_panel_ext` (vendor-only ioctls dropped); stock `bias-gpios` 108/109 map to `ti,tps65132` `vsp/vsn-supply` with `GPIO_ACTIVE_HIGH` (`runtime_merged.dts:7025`); stock `ldo18-gpios` 150 and panel reset GPIO 85 are wired. The vendor compatible `hdplus,icnl9922c,120hz` is intentionally replaced by `chipone,icnl9922c`.
8. **Touch firmware:** 71,792-byte `initramfs/lib/firmware/chipone/icnl9922c.bin`, 1000-byte DRW chunks (MTK SPI DMA 1024-byte limit), SFCTRL SRAM CRC, strict TCS CRC checks, and `drm_panel_follower` sync with panel GPIO85 are deliberate robustness fixes.
9. **NFC via mainline `st,st21nfcb-i2c`** is valid (`drivers/nfc/st-nci/i2c.c:273`, `net/nfc/st,st-nci.yaml:13-17`); IRQ GPIO 1 and reset GPIO 106 flags match stock (`:7788-7791`). Vendor `mediatek,nfc-gpio-v2` / clkbuf are intentionally not used *provided* the clock is otherwise available (§4 D2.6).
10. **Display register quirks preserved:** MIPI-TX PLL handover (`phy-mtk-mipi-dsi-mt8183.c:40-199`), D-PHY timings (`timcon0=0x090f0808`, `timcon1=0x0f2d0d24` at 852 MHz), ULPS exit, shadow bypass (`0x6`), `DIS_EOT` omission, OVL0/RDMA0 FIFO/shadow fixes and `bgclr_in_off` all match live stock registers and vendor source. The `pcw = div_u64(...<<24, 26000000)` expression is verbatim upstream, not a truncation bug.
11. **Bus/compat mapping:** SPI `mt6765_compat` is equivalent to vendor `mt6789_compat` after mainline removed `must_rx`; I²C `mt8188_compat` (`dma_sync = 0`) is a documented fix for `tps65132 -ETIMEDOUT`; MT6358 long-press key bits are a hardware-verified fix (`9cd7a45`); AFE `power-domains = <&spm MT6789_POWER_DOMAIN_AUDIO>` and optional reset are intentional MT6789 adaptations.
12. **Vendor-only subsystems intentionally absent:** `aee_*`, `mrdump`, `mmprofile`, `tinysys-*`, `mtk_mmdvfs*`, `mcDrvModule`, `cmdq`, DVFSRC, sub-PMIC `mt6375`, RT5133, `sc854x`, `rg_dfd_timeout`, PHY eye-tuning props, I²C/MMC tuning props. These are mainline decouplings or out-of-scope peripherals, not board-quirk regressions — except where a specific hardware quirk was shown to be required (§4/§5).

---

## 8. Limitations / explicitly unverified

- Reset index-map bit positions in `clk-mt6789-infra_ao.c:174-180` could **not** be verified — no MT6789 reset-controller source exists in the vendor tree and `clk-mt6789.ko` strings contain no matching names. **No finding asserted.**
- `MT6789_MMSYS_SW0_RST_B = 0x160`, `MT6789_MUTEX_MOD_DISP_DSC_WRAP0_CORE0 = 13` and mmsys routing offsets were checked only for internal consistency. **No finding asserted.**
- Whether the port's charger address (`0x6a`) and the reset/CD polarities match *this specific unit* requires on-device measurement; the report states both sides of the evidence rather than guessing.
- No hardware boot, flash, or runtime test was performed — this is a source/binary audit only.
