# Phase B backlog & modernization progress

Phase A (`074d43d..8a923d3`) addressed stable Tier A remediations and review
corrections. Phase B implements the upstream-able drivers and bindings below
from Tier A evidence; items requiring out-of-tree DSP/connectivity firmware
or live hardware efuse readback remain tracked as deferred.

## D2-22 — MT6366 XO_NFC clock buffer (NFC power) — **IMPLEMENTED (Phase B)**

- Stock `runtime_merged.dts:4325-4336`, `mtk-clkbuf-dcxo-6366.c:79-86`, and
  `clkbuf.ko`: `XO_NFC` (DCXO index 2, `EXTBUF3`) uses `DCXO_CW00` (`0x788`)
  mode bits `[7:6]` + `en_m` bit `8`, verified via `DCXO_CW18/19`
  (`0x7b4/0x7b6`, `auxout_sel=6`, bit `0`).
- **Implemented:** patches `04-peripherals/0041` (`mediatek,mt6366-clkbuf.yaml`
  + `mediatek,mt6397.yaml`) and `0042` (`drivers/soc/mediatek/mtk-mt6366-clkbuf.c`
  + `mt6397-core.c` MFD cell), wired under `pmic` in `mt6789-advan-x1.dts`
  with `mediatek,xo-buf-enable = <2>;` (no `clocks` property added to `nfc@8`,
  keeping `st,st-nci.yaml` valid) and `CONFIG_MTK_MT6366_CLKBUF=y`.

## D2-09 remainder — MT6366 PMIC thermal (**IMPLEMENTED**) + consys thermal (**DEFERRED**)

- **MT6366 PMIC thermal — IMPLEMENTED (Phase B):**
  - Patches `04-peripherals/0043` (`mediatek,mt6358-efuse.yaml`,
    `mediatek,mt6366-pmic-temp.yaml`, `mediatek,mt6397.yaml`), `0044`
    (`drivers/nvmem/mt6358-efuse.c` implementing `reg_v1` OTP readback), and
    `0045` (`drivers/thermal/mediatek/mt6366_thermal.c` implementing stock
    `mt6366_get_cali_data()` / `pmic_raw_to_temp()` on raw 12-bit
    `mt6359-auxadc` channels `CHIP_TEMP/VCORE_TEMP/VPROC_TEMP/VGPU_TEMP`).
  - Wired `pmic_efuse` (`nvmem-layout` `thermal-cali@40`), `mt6366_temp`, and
    thermal zones `pmic-pmu/bk1/bk2/bk3-thermal` in `mt6789-advan-x1.dts`;
    enabled `CONFIG_NVMEM_MT6358_EFUSE=y` and `CONFIG_MTK_MT6366_THERMAL=y`.
- **Consys thermal (`consys@18002000`) — DEFERRED:**
  - Stock `runtime_merged.dts:7391-7407` binds `mediatek,mt6789-consys`.
  - Requires powering and initializing the MT6789 ConnSys/WMT connectivity
    subsystem (Wi-Fi/BT/GPS firmware bring-up, out of scope for Phase B).

## D2-17 remainder — ACCDET efuse / moisture / CDD — **IMPLEMENTED (Phase B)**

- Stock `mt6358-accdet.ko` and `mp01` `mt6358-accdet.c` read MT6366 PMIC
  eFuse rows `115..119` (`0xe6..0xee`) for `auxadc_offset`, `four-key`
  fallback, `moisture_vdd_offset`, and `moisture_offset`, and carry
  `headset-three-key-threshold-CDD = <0 121 192 600>`.
- **Implemented:** patches `04-peripherals/0043` (`nvmem-cells` in
  `mediatek,mt6358-accdet.yaml`) and `0046` (`mt6358-accdet.c` eFuse loader,
  `auxadc_offset` subtraction in button detection, 4-key eFuse fallback, and
  `mediatek,three-key-threshold-cdd` selection), wired to `pmic_efuse` cells
  `@e6/@e8/@ec/@ee` in `mt6789-advan-x1.dts`.

## D2/D6 — CCIF SRAM size / reset version DT-driven — **IMPLEMENTED (Phase B)**

- **Implemented:** patch `04-peripherals/0040` adds optional
  `mediatek,ccif-sram-size` (default 512) and `mediatek,ccif-hw-reset-ver`
  (default 0) to `mediatek,mt6789-ccif.yaml` and parses them in
  `mtk_ccci_parse_ccif()` with bounds validation; `mt6789.dtsi` sets
  `mediatek,ccif-sram-size = <0x200>;` on `ccif0` (matching stock
  `runtime_merged.dts:4662`).

## D2-15 remainder — FS1599 Re25/F0 — **DEFERRED (Vendor SCP DSP required)**

- See `docs/audio-speaker-protection.md`. Needs SCP audio task
  (`scp_rv55.bin`), Audio IPI, and closed-source `fs1599.fsm` (note stock
  `runtime_merged.dts:6099` sets `snd_scp_audio` `status = "disabled"`).
  Phase A caps LINEOUT at −200 mB instead.

## D-17 / D3-16 — USB2 PHY calibration and microSD VCORE — **DEFERRED (Live HW check)**

- Stock `runtime_merged.dts:6128-6131,7871-7885` references `u2_phy_data` at
  downstream `mediatek,devinfo` offset `0x1b0` (`intr_cal` mask `0x1f`), plus
  `rev4=1` and `rx_sqth=5`. Because downstream `devinfo` ATAG offsets are
  shifted relative to physical MMIO `efuse@11c10000` (e.g. LVTS `0x1b4` ->
  MMIO `0x1a4`), physical MMIO readback on live hardware is required before
  wiring `nvmem-cells = <&u2_phy_data>` on `u2port0`.
- Stock `runtime_merged.dts:6233-6234` has `req-vcore = <650000>` on MSDC1.
  microSD CD polarity was corrected in Phase A; test card I/O first and add
  an OPP/DVFSRC vote only if SDR104 shows voltage droop.

## F01 / F06 / F07 — provenance comments in existing commits — **CLOSED**

- `3b1f69f` / `docs/ccci-bringup-plan.md` (Zinwa Q25 cross-checks), `ce06c2e`
  (Motorola `chipone_tddi_mmi_v2`), `7a2c78a` (Motorola `ti,tps65132` boards)
  cite sibling ODMs in commit bodies/docs.
- Rewriting those messages would require rebasing signed history. Authority
  hierarchy is documented in `docs/upstream-status.md` §4; all code has been
  re-verified against Tier A.
