# Phase B backlog — deferred modernization items

Phase A (`074d43d..c39ac03`) is remediation only. The items below were
investigated with Tier A evidence and deliberately deferred because a correct
fix needs a new upstream-able driver/binding or a build-order refactor, not a
stable tweak. Each entry carries the exact evidence needed to resume.

## D2-22 — MT6366 XO_NFC clock buffer (NFC power)

- Stock `runtime_merged.dts:4325-4336`:
  `mt6366_clock_buffer` compat `mediatek,clock_buffer`, `xo-mode-num=<4>`,
  `xo-buf-name = "XO_SOC","XO_WCN","XO_NFC","XO_CEL","RSV1","RSV2","XO_EXT"`,
  `xo-buf-allow-control[2]=1` (XO_NFC), `mediatek,enable`.
- Stock `runtime_merged.dts:4485-4492`: `clock_buffer_ctrl` compat
  `mediatek,clock_buffer_ctrl`, `pmif=<0x54 0>`, `mediatek,enable`.
- Stock `mtk-clkbuf-dcxo-6366.c:79-86`: XO_NFC uses `XO_EXTBUF3_EN_M`
  (`DCXO_CW00` bit 8), `MT6366_XO_STATIC_AUXOUT_ADDR` (`DCXO_CW19` = `0x7b6`)
  bit 0, `xo_en_auxout_sel=6`.
- Stock NFC node `runtime_merged.dts:5241-5246` has **no** `clocks` property;
  XO_NFC is enabled board-wide by the clock-buffer driver, not by the NFC
  client. Upstream `drivers/nfc/st-nci/` has zero clock code and its binding
  (`st,st-nci.yaml`) forbids extra properties — a DT `clocks` property on
  `nfc@8` would fail `dtbs_check` and be ignored by the driver.
- Phase A verified mainline `nfc@8` DT is otherwise correct (`&pio` IRQ GPIO1
  level-high, `nfc_pins`, reset GPIO106), and `CONFIG_NFC_ST_NCI_I2C=y`.
- **Phase B:** add a mainline MT6366 clock-buffer driver/binding (regmap via
  `mt6397-core`) that enables DCXO index 2 at probe; no NFC DT change needed.

## D2-09 remainder — MT6366 PMIC + consys thermal zones

- Stock `runtime_merged.dts:728-736`: `mt6366_temp` compat
  `mediatek,mt6366-pmic-temp`, PMIC AUXADC channels 5-8, `nvmem-cells` to
  `mt6366_e_data`; zones `pmic6366_pmu/bk1/bk2/bk3` at `:892-921`.
- Stock `runtime_merged.dts:7391-7407`: `consys@18002000` compat
  `mediatek,mt6789-consys` with `#thermal-sensor-cells=<0>`; zone `consys`.
- Neither driver exists upstream. `mp01-kernel/drivers/thermal/mediatek/pmic_temp.c`
  derives each zone from **per-unit efuse** slope/intercept (`o_slope`,
  `o_slope_sign`, `o_vts`, `degc_cali` read via nvmem), not a fixed table —
  a generic-adc-thermal substitute would report wrong temperatures.
- **Phase B:** port `pmic_temp` + MT6366 efuse nvmem binding; add consys sensor
  driver. Do not add placeholder zones.

## D2-17 remainder — ACCDET efuse / moisture fallback

- `mt6358-accdet.ko` logs: `"accdet no 4-key-thrsh dts, use efuse"`,
  `"moisture_efuse efuse=0x%x,moisture_offset=%d mv"`,
  `"moisture_vdd efuse=0x%x, moisture_vdd_offset=%d mv"`,
  `"headset-three-key-threshold-CDD"`, `"mt63xx-accdet-efuse"`.
- Stock `runtime_merged.dts:3785-3812`: PMIC efuse nvmem
  (`mediatek,mt6358-efuse`, child `mt6366_e_data` at `reg=<0x40 0x0c>`),
  `nvmem = <0x4d>`, `nvmem-names = "mt63xx-accdet-efuse"`.
- Phase A added the CDD key array (`<0 121 192 600>`) and parser but not the
  efuse/moisture paths (no mainline PMIC efuse nvmem node, no nvmem consumer
  in patch 0036).
- **Phase B:** add PMIC efuse nvmem node + `devm_nvmem_device_get` /
  `nvmem_device_read` parsing and moisture compensation; then wire CDD
  selection to accessory/jack-type detection.

## D2-15 remainder — FS1599 Re25/F0

- See `docs/audio-speaker-protection.md`. Needs SCP audio task, Audio IPI and
  `fs1599.fsm`; Phase A caps LINEOUT at −200 mB instead.

## D2/D6 — CCIF SRAM size / reset version DT-driven

- Patch `0026` hardcodes `MTK_CCIF_SRAM_SIZE 512` and CCIF0 reset version 0
  (`RST3_SET BIT(8)` at `0x150`); stock `runtime_merged.dts:4662` has
  `mediatek,sram_size=<0x200>` and no reset-version property (implies 0).
- A DT-driven conversion was attempted but overlaps hunks in patches `0027`
  and `0028`; it is deliberately deferred to avoid destabilising the
  validated apply order. The fallback semantics are already correct (512/0),
  so this is provenance/cleanliness, not a functional risk.
- **Phase B:** add `mediatek,ccif-sram-size` / `mediatek,ccif-hw-reset-ver`
  to binding `0025`, parse in `0026` with 512/0 defaults, then regenerate
  `0027`/`0028` against the shifted context.

## D-17 / D3-16 — USB2 PHY calibration and microSD VCORE

- Stock `runtime_merged.dts:7871-7885` has `rev4=1`, `rx_sqth=5` and an
  `intr_cal` efuse mask `0x1f` on `u2port0`; mainline `dtsi:1025-1032` omits
  them. No USB failure is currently attributed to this — defer.
- Stock `runtime_merged.dts:6233-6234` has `req-vcore = <650000>` on MSDC1;
  mainline omits it. microSD CD polarity was corrected in Phase A; test card
  I/O first, add the vote only if SDR104 shows voltage droop.

## F01 / F06 / F07 — provenance comments in existing commits

- `3b1f69f` / `docs/ccci-bringup-plan.md` (Zinwa Q25 cross-checks), `ce06c2e`
  (Motorola `chipone_tddi_mmi_v2`), `7a2c78a` (Motorola `ti,tps65132` boards)
  cite sibling ODMs in commit bodies/docs.
- Rewriting those messages would require rebasing signed history. Phase A
  records the authority rule in `docs/upstream-status.md` §4 instead; the
  code itself was already re-verified against Tier A.
