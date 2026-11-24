# MT6789 (Advan X1) Mainline Port — Remediation & Implementation Plan

> **Current Status (Post-`3b1f69f` / Phase 2):**
> The historical `f3d8e93` Phases 0–5 plan below is **100% complete**. The follow-up post-`2b74552` deviation remediation tracks:
> - **Phase 1 (`WP-1.0`–`WP-1.5`, commit `3b1f69f`):** **COMPLETE & HARDWARE-VERIFIED** (`console-ramoops-3b1f69f.log` — 88.67 s Wayland session, 0 Panfrost/GPU errors, clean `userrequested` reboot, all 13 LVTS zones healthy, `nfc0` & CCCI probed cleanly).
> - **Phase 2 (`WP-2.1`–`WP-2.4` + Docs Consolidation):** **IMPLEMENTED** — `disp_iommu` enabled (`03-soc-power/0014`, fixing `#0012` `mediatek-disp-ovl` `swiotlb` exhaustion), `watchdog@10007000` `syscon-reboot-mode` (`04-peripherals/0006` + `mt6789.dtsi`), CCCI EL3 SiP ABI documented ([`docs/ccci-sip-abi.md`](./ccci-sip-abi.md)), `clk-mt6789-mfgcfg` converged (`01-clk/0005`), and overlapping audit docs consolidated.
> - **Phase 3 (Next — Hardware Verification Sweep):** `M7` (`CLK_IS_CRITICAL` / unused clocks), `M9` (GPIO85 polarity), `M11` (live NFC RF poll), `B3-02/04/07/08/10` + `M14` DTS checks.
> - **Phase 4 (Pre-Upstream Deferred):** `M8` (8 unused PDs), `B2-02` (DDP stub), `B2-05` (`priv->dsc` clear on stop).
>
> ---
>
> **Source Audit:** [`docs/upstream-deviation-audit.md`](./upstream-deviation-audit.md)  
**Target Port Repository:** `/work/mainline/advan-x1-mainline` @ `f3d8e93bc9d05cdc4403dfb6091de3b8c6f17600` (51 patches across `patches/01-clk` .. `patches/07-dts` + standalone `dts/mt6789.dtsi` and `dts/mt6789-advan-x1.dts`)  
**Upstream Baseline:** `/work/mainline/linux` @ `666539eecdf4db623f5e3c3e55cf9f2dfad24aa4` (**Linux v7.4.0-rc3**)  
**Hardware / Vendor Authorities:** `/work/android-kernel/mp01-kernel`, `/work/android-kernel/motorola-kernel-modules`, `/work/firmware/advan_x1/dtb/runtime_merged.dts`, `/work/firmware/advan_x1/ondevice_dumps/`, `/work/firmware/vendor_boot_unpack/recovery_root/lib/modules/*.ko`  
**Verification Scratch Workspace:** `/tmp/advan-plan/` (`dtc` exit `0`, `dt-doc-validate` 9/9 modified YAML schemas exit `0`, `dt-validate` on patched `board.dtb` reduced from **27 schema errors to 0**)

---

## 1. Master Decision Table (100% Audit Coverage)

Every finding ID from `docs/upstream-deviation-audit.md` (`P1`–`P7`, `D1.1`–`D1.13` covering all 27 `dt-validate` errors, `D2.1`–`D2.7`, `D3`, `D4`, `B1`–`B12`, `PROC-1`, `PROC-2`, and Section 7 items `S7.1`–`S7.12`) is assigned a single disposition below:

| Finding ID | Subsystem | Disposition | Phase | Target Patch / DTS File(s) | Summary & Engineering Rationale |
|---|---|---|---|---|---|
| **`P1`** | PMIC Wrapper (`mtk-pmic-wrap`) | **`EXECUTE-NOW`** | Phase 1 | `patches/03-soc-power/0004-soc-mediatek-pwrap-Add-MT6789-PMIC-wrapper-support.patch` | Replace `.regs = mt8195_regs` (`[PWRAP_INT_EN] = 0x420`) with a dedicated `mt6789_regs[]` table (`[PWRAP_INT_EN] = 0x450`) matching `mp01-kernel/drivers/soc/mediatek/mtk-pmic-wrap.c:1041-1050,2518-2529`. Fixes unprogrammed PMIC IRQ enable and prevents clobbering offset `0x420`. |
| **`P2`** | Thermal (`lvts_thermal`) | **`EXECUTE-NOW`** | Phase 2 | `patches/04-peripherals/0008-thermal-drivers-mediatek-lvts-Enable-sensing-before-.patch` | Restore upstream zone-driven `sensor_map` accumulation in `lvts_ctrl_start()` (`drivers/thermal/mediatek/lvts_thermal.c:1301-1399`) so only DT-registered thermal zones enable hardware sensing points, while retaining the `!(value & 0xFFFF) -> -EAGAIN` guard in `lvts_get_temp()` and adding a 2 ms post-enable settle sleep after `LVTS_MONCTL0`/`LVTS_MSRCTL1` programming. |
| **`P3`** | Audio AFE (`mt8186-afe-pcm`) | **`EXECUTE-NOW`** | Phase 2 | `patches/04-peripherals/0021-ASoC-mediatek-mt8186-Add-MT6789-AFE-and-MT6366-sound.patch` | Remove `case AFE_AUD_PAD_TOP: return false;` from `mt6789_is_volatile_reg()` so live handshake/FIFO bits in `AFE_AUD_PAD_TOP (0x0e40)` are never served from stale regcache; fix the root-cause `-EBUSY` at card registration by wrapping `mt8186_mt6366_init()` in `mt6789_mt6366_adda_init()` with `pm_runtime_resume_and_get(cmpnt_afe->dev)` / `pm_runtime_put(cmpnt_afe->dev)`. |
| **`P4`** | Audio Machine (`mt6789-mt6366`) | **`EXECUTE-NOW`** | Phase 2 | `patches/04-peripherals/0021-ASoC-mediatek-mt8186-Add-MT6789-AFE-and-MT6366-sound.patch` | Wire `.ops = &mtk_soundcard_common_playback_ops` on `Playback_1/2/3` and `.ops = &mtk_soundcard_common_capture_ops` on `Capture_1/2`, and populate `.pcm_constraints = mt6789_pcm_constraints` / `.num_pcm_constraints = ARRAY_SIZE(mt6789_pcm_constraints)` in `mt6789_mt6366_pdata`. |
| **`P5`** | Power Domains (`mt6789-pm-domains.h`) | **`DEFER`** | Phase 5 | `patches/03-soc-power/0002-pmdomain-mediatek-Add-MT6789-power-domains-support.patch` | 9 unreferenced domains (`MD`, `CONN`, `ISP`, `IPE`, `VDEC`, `VENC`, `CAM`, `CAM_RAWA`, `CAM_RAWB`) have no DT consumers today. **Trigger:** enabling any camera/ISP/video/modem DT node in `mt6789.dtsi` or adding unused-PD cleanup. Full table from `mp01` `mtk-scpsys-mt6789.c:30-235` is specified in Phase 5. |
| **`P6`** | Power Domains / Infracfg Macros | **`DEFER`** | Phase 5 | `patches/03-soc-power/0002-pmdomain-mediatek-Add-MT6789-power-domains-support.patch` | `MT8192_TOP_AXI_PROT_EN_*` macros reused in `mt6789-pm-domains.h` are numerically identical to `MT6789_TOP_AXI_PROT_EN_*` in `mp01` `scpsys.h:44-66`. **Trigger:** next edit to `mt6789-pm-domains.h` (with `P5`) or upstream LKML submission. |
| **`P7`** | Display DRM / DSI / Panel (`mtk_dsi`, `mtk_ddp_comp`, `panel-chipone-icnl9922c`) | **`EXECUTE-NOW`** | Phase 1 | `patches/06-display/0005-drm-mediatek-Add-MT6789-DSI-and-hardware-DSC-1.1-sup.patch`, `patches/06-display/0007-drm-panel-Add-Chipone-ICNL9922C-MIPI-DSI-panel-drive.patch` | Clear `dsi->dsc = NULL` and `dsi->hs_rate = 0` in `mtk_dsi_host_detach()`, propagate `mtk_ddp_comp_set_dsc(comp, dsc)` unconditionally (including `NULL`) in `mtk_crtc_ddp_hw_init()`, and mark `icnl9922c_dsc` `static const` in `panel-chipone-icnl9922c.c` (casting only if needed or updating `dsi->dsc` assignment) to eliminate stale pointer / UAF after panel module unbind/unload. |
| **`D1.1`** | DT Binding — Sound (`dt-validate` error #27) | **`EXECUTE-NOW`** | Phase 3 | `patches/04-peripherals/0020-ASoC-dt-bindings-mediatek-Add-MT6789-AFE-and-MT6366-.patch` | Change `additionalProperties: false` to `unevaluatedProperties: false` in `Documentation/devicetree/bindings/sound/mediatek,mt6789-mt6366-sound.yaml:61` so properties inherited from `sound-card-common.yaml#` (`model`, `audio-routing`) validate cleanly. |
| **`D1.2`** | DT Binding — Sound (`dt-validate` error #26) | **`EXECUTE-NOW`** | Phase 3 | `patches/04-peripherals/0020-ASoC-dt-bindings-mediatek-Add-MT6789-AFE-and-MT6366-.patch` | Remove local `audio-routing` redefinition (`lines 24-28`) in `mediatek,mt6789-mt6366-sound.yaml` so `sound-card-common.yaml#`'s unbounded `non-unique-string-array` definition applies to the 6-element (3-pair) routing in `mt6789-advan-x1.dts:126-128`. |
| **`D1.3`** | DT Binding — Clock (`dt-validate` errors #11–#14) | **`EXECUTE-NOW`** | Phase 3 | `patches/01-clk/0001-dt-bindings-clock-mediatek-Add-MT6789-clock-and-rese.patch` | Update `compatible` in `Documentation/devicetree/bindings/clock/mediatek,mt6789-clock.yaml:18-23` from single-string `enum` to 2-item `items: [enum: [...], const: syscon]` matching `mt6789.dtsi:849,855,861,867`. |
| **`D1.4`** | DT Binding — Pinctrl (RSEL enum range) | **`EXECUTE-NOW`** | Phase 3 | `patches/02-pinctrl/0001-dt-bindings-pinctrl-mediatek-Add-MT6789-pinctrl-bind.patch` | Extend `bias-pull-down` and `bias-pull-up` RSEL enum in `Documentation/devicetree/bindings/pinctrl/mediatek,mt6789-pinctrl.yaml:85,92` from `[200, 201, 202, 203]` to `[200, 201, 202, 203, 204, 205, 206, 207]` matching `MTK_PULL_SET_RSEL_000..111` in `include/dt-bindings/pinctrl/mt65xx.h:19-26`. |
| **`D1.5`** | DTS — Display Power Domains (`dt-validate` errors #23–#25) | **`EXECUTE-NOW`** | Phase 3 | `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch`, `dts/mt6789.dtsi` | Add `power-domains = <&spm MT6789_POWER_DOMAIN_DISP>;` to `postmask0` (`mt6789.dtsi:1118`), `dither0` (`:1126`), and `dsc0` (`:1133`), matching sibling display blocks and `mediatek,{postmask,dither,dsc}.yaml`. |
| **`D1.6`** | DTS & Binding — Display IOMMU (`dt-validate` errors #21–#22) | **`EXECUTE-NOW`** | Phase 3 | `patches/03-soc-power/0010-dt-bindings-memory-mediatek-Add-MT6789-SMI-compatibl.patch`, `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch`, `dts/mt6789.dtsi` | Add `include/dt-bindings/memory/mediatek,mt6789-memory-port.h` and `mediatek,mt6789-iommu-mm` in `mediatek,iommu.yaml`; add `disp_iommu: iommu@14016000` (`status = "disabled"` until `mtk_iommu` is enabled so `of_iommu_xlate()` returns `-ENODEV` without probe deferral) and wire `iommus` on `ovl0` (`M4U_PORT_L0_OVL_RDMA0_HDR`, `M4U_PORT_L0_OVL_RDMA0`) and `rdma0` (`M4U_PORT_L1_DISP_RDMA0`) from `runtime_merged.dts:6868,6894`. |
| **`D1.7`** | DT Binding — GPU (`dt-validate` error #3) | **`EXECUTE-NOW`** | Phase 3 | `patches/05-gpu/0001-dt-bindings-gpu-mali-bifrost-Add-MT6789-compatible.patch` | Move `mediatek,mt6789-mali` out of the 2-item `arm,mali-bifrost` enum in `Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml:24` into a 3-item `oneOf` entry (`[const: mediatek,mt6789-mali, const: mediatek,mt8186-mali, const: arm,mali-valhall-jm]`) matching `mt6789.dtsi:610-613`. |
| **`D1.8`** | DTS — SMI Compatibles (`dt-validate` errors #15–#20) | **`EXECUTE-NOW`** | Phase 3 | `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch`, `dts/mt6789.dtsi` | Drop non-existent generic fallback strings `"mediatek,smi-common"` and `"mediatek,smi-larb"` from `smi_common` (`mt6789.dtsi:1038`), `larb0` (`:1049`), and `larb1` (`:1060`). Resolves 6 `dt-validate` errors (3 invalid compatible + 3 unmatched schema). |
| **`D1.9`** | DTS & Binding — LVTS eFuse Cell (`0 dt-validate` errors) | **`PRESERVE-AS-IS`** | Guarded | `patches/04-peripherals/0007-dt-bindings-thermal-mediatek-lvts-Add-MT6789-support.patch`, `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch`, `dts/mt6789.dtsi` | Single contiguous eFuse cell `lvts_e_data: lvts-calib@1a4` (`reg = <0x1a4 0x34>`) is a hardware-verified fix (stock `0x1b4/0x2f8` are logical `atag,devinfo` indices, not physical `0x11c10000` MMIO offsets) and already passes `dt-validate`. Must NOT be reverted. |
| **`D1.10`** | DTS — PMIC Keys Child Node Names (`dt-validate` errors #1–#2, 3 raw occurrences) | **`EXECUTE-NOW`** | Phase 3 | `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`, `dts/mt6789-advan-x1.dts` | Rename `powerkey` and `homekey` child nodes under `mt6358keys: keys` (`mt6789-advan-x1.dts:691,696`) to `power` and `home` per `Documentation/devicetree/bindings/input/mediatek,pmic-keys.yaml:37`. (`mtk-pmic-keys.c` iterates child nodes positionally via `for_each_child_of_node`, so renaming is 100% behavior-preserving.) |
| **`D1.11`** | DTS & Binding — UFS Controller (`dt-validate` errors #4–#7) | **`EXECUTE-NOW`** | Phase 3 | `patches/04-peripherals/0013-dt-bindings-mmc-mtk-sd-Add-MT6789-compatible.patch` (or new `04-peripherals` UFS/USB/TPHY binding patch), `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch`, `dts/mt6789.dtsi` | Use `compatible = "mediatek,mt6789-ufshci", "mediatek,mt8183-ufshci";` on `ufshci@11270000` (`mt6789.dtsi:774`) and update `Documentation/devicetree/bindings/ufs/mediatek,ufs.yaml` to document `mediatek,mt6789-ufshci`, its 7 clocks, optional `resets`/`reset-names` (`hci_rst`, `unipro_rst`, `crypto_rst`, `mphy_rst` consumed by `ufs_mtk_init_reset()`), and make `phys` required only on platforms with separate M-PHY nodes. |
| **`D1.12`** | DTS & Binding — MUSB Controller (`dt-validate` errors #9–#10) | **`EXECUTE-NOW`** | Phase 3 | `patches/04-peripherals/0013-dt-bindings-mmc-mtk-sd-Add-MT6789-compatible.patch` (or new binding patch), `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch`, `dts/mt6789.dtsi` | Change `usb0: usb@11200000` (`mt6789.dtsi:835`) to `compatible = "mediatek,mt6789-musb", "mediatek,mtk-musb";` and add `mediatek,mt6789-musb` to `Documentation/devicetree/bindings/usb/mediatek,musb.yaml:20`. |
| **`D1.13`** | DTS & Binding — USB2 T-PHY (`dt-validate` error #8) | **`EXECUTE-NOW`** | Phase 3 | `patches/04-peripherals/0013-dt-bindings-mmc-mtk-sd-Add-MT6789-compatible.patch` (or new binding patch), `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch`, `dts/mt6789.dtsi` | Change `u2phy0: t-phy@11f40000` (`mt6789.dtsi:817`) to `compatible = "mediatek,mt6789-tphy", "mediatek,generic-tphy-v2";` and add `mediatek,mt6789-tphy` to the V2 `oneOf` enum in `Documentation/devicetree/bindings/phy/mediatek,tphy.yaml:81`. |
| **`D2.1`** | Charger I²C Address (`0x6a` vs stock `0x6b`) | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`, `dts/mt6789-advan-x1.dts` | Stock `upm6910_charger.ko` tries `0x6b` (`upm6910_charger_probe+0xa8`) then falls back to `0x6a` (`+0x178`). Project memory notes physical SC89601A on this unit ACKed `0x6a`, whereas stock DTB lists `@6b`. Verify on device via `i2cget` / `/sys/class/power_supply/bq25890-charger-0` before touching `reg = <0x6a>`. |
| **`D2.2`** | Fuel Gauge (`cw2217_battery` 80-byte profile & init state machine) | **`EXECUTE-NOW`** | Phase 1 | `patches/04-peripherals/0016-dt-bindings-power-supply-Add-CellWise-CW2217-fuel-ga.patch`, `patches/04-peripherals/0017-power-supply-Add-CellWise-CW2217-battery-fuel-gauge-.patch`, `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`, `dts/mt6789-advan-x1.dts` | Add the stock 80-byte `batinfo` profile (`runtime_merged.dts:5111`) as `cellwise,battery-profile` in `mt6789-advan-x1.dts:238-243` and implement the vendor check-and-update state machine (`cw2217_get_state()` + `cw2217_config_start_ic()`: check active/update-flag/80-byte match; only on mismatch or sleep perform `RESTART(0x30)->SLEEP(0xf0)->write 0x10..0x5f->SOC_ALERT(0x80)->GPIO_CFG(0x00)->RESTART(0x30)->ACTIVE(0x00)->poll IC_STATE(0xa7)&0x0c`). |
| **`D2.3`** | MMC1 MicroSD Card-Detect Polarity (`GPIO 7`) | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`, `dts/mt6789-advan-x1.dts` | Stock `runtime_merged.dts:6228-6229` has `cd-gpios = <&pio 7 0x00>` (`GPIO_ACTIVE_HIGH`), while port commit `f6183d7` set `GPIO_ACTIVE_LOW` to stop empty-slot CMD8 retries. Verify physical pin level on `/sys/kernel/debug/gpio` with tray empty vs card inserted before changing polarity. |
| **`D2.4`** | Display Panel (`GPIO 85`) & Touch (`GPIO 107`) Reset Flags | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`, `dts/mt6789-advan-x1.dts` | Port uses standard `GPIO_ACTIVE_LOW` with logical `1` (assert reset = physical LOW) -> `0` (deassert = physical HIGH) in `panel-chipone-icnl9922c.c:440-443` and `chipone_icnl9922c.c:231-235`, which produces the exact same physical waveform as stock raw GPIO `0 -> 1`. Verify on device before altering flags. |
| **`D2.5`** | Backlight PWM Period & Duty Scaling (`500000` ns vs `39385` ns) | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`, `dts/mt6789-advan-x1.dts` | Stock uses `period = 39385` ns (25.39 kHz) and caps duty at 25% (`leds-mtk-pwm.c:47-65`), whereas port uses `500000` ns (2 kHz) and 100% max duty. Verify brightness/flicker/coil-whine on device before switching to `pwms = <&disp_pwm0 0 39385>` and `/4`-scaled `brightness-levels`. |
| **`D2.6`** | NFC Clock Buffer (`st,st21nfcb-i2c` at `i2c3@08`) | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`, `dts/mt6789-advan-x1.dts` | `nfc0` probes and registers via `st-nci-i2c`. Verify NCI `CORE_RESET`/`CORE_INIT` / RF discovery with `neard`/`nfCTool` on device to confirm PMIC DCXO `XO_NFC` clock buffer remains enabled by bootloader. |
| **`D2.7`** | Memory Map (`memory@48090000` vs `ramoops@48090000`) | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`, `dts/mt6789-advan-x1.dts` | `memory@48090000` exactly equals the `no-map` `ramoops@48090000` carveout (`896 KiB`), and U-Boot FDT fixup may overwrite `/memory` nodes at boot. Inspect `/proc/iomem`, `/sys/firmware/fdt`, and `/sys/fs/pstore/` on device before removing `memory@48090000`. |
| **`D3`** | Bootloader-Leaked Clocks & Power Domains | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | `patches/01-clk/0002-clk-mediatek-Add-MT6789-core-clock-drivers.patch` | 19 vendor `clk-bring-up` clocks (`mt6789-clkitg.dtsi:9-30`) vs 3 `CLK_IS_CRITICAL` clocks in port, and no `clk_ignore_unused`/`pd_ignore_unused`. Verify `/sys/kernel/debug/clk/clk_summary` and `/sys/kernel/debug/pm_genpd/pm_genpd_summary` on device after `clk_disable_unused` late initcall. |
| **`D4`** | DT Binding — Pinctrl Example Drift | **`EXECUTE-NOW`** | Phase 3 | `patches/02-pinctrl/0001-dt-bindings-pinctrl-mediatek-Add-MT6789-pinctrl-bind.patch` | Update example in `Documentation/devicetree/bindings/pinctrl/mediatek,mt6789-pinctrl.yaml:138,140` from `gpio-ranges = <&pio 0 0 189>` and `GIC_SPI 212` to `188` and `GIC_SPI 244`, matching `mt6789.dtsi:375,378` and `mp01` `mt6789.dts:2303`. |
| **`B1`** | Behavioral Delta — CW2217 Profile Download | **`EXECUTE-NOW`** | Phase 1 | Same as `D2.2` (`patches/04-peripherals/0016`, `0017`, `patches/07-dts/0003`, `dts/mt6789-advan-x1.dts`) | Binary-verified `cw2217_battery.ko:cw_init` (`0x760`) profile download sequence. Remedied in lockstep with `D2.2`. |
| **`B2`** | Behavioral Delta — Charger `0x6a` vs `0x6b` | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | Same as `D2.1` (`patches/07-dts/0003`, `dts/mt6789-advan-x1.dts`) | Binary-verified `upm6910_charger.ko` fallback (`0x6b` -> `0x6a`). Verified on hardware under `D2.1`. |
| **`B3`** | Behavioral Delta — MMC1 CD Polarity | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | Same as `D2.3` (`patches/07-dts/0003`, `dts/mt6789-advan-x1.dts`) | Vendor `GPIO_ACTIVE_HIGH` vs port `GPIO_ACTIVE_LOW`. Verified on hardware under `D2.3`. |
| **`B4`** | Behavioral Delta — Panel/Touch Reset Polarity | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | Same as `D2.4` (`patches/07-dts/0003`, `dts/mt6789-advan-x1.dts`) | Vendor `0x00` vs port `GPIO_ACTIVE_LOW` + gpiod logical inversion. Verified on hardware under `D2.4`. |
| **`B5`** | Behavioral Delta — Backlight PWM 2 kHz/100% vs 25.4 kHz/25% | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | Same as `D2.5` (`patches/07-dts/0003`, `dts/mt6789-advan-x1.dts`) | Vendor `39385` ns / `/4` duty cap vs port `500000` ns / 100% duty. Verified on hardware under `D2.5`. |
| **`B6`** | Behavioral Delta — 19 Vendor Bring-Up Clocks | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | Same as `D3` (`patches/01-clk/0002-clk-mediatek-Add-MT6789-core-clock-drivers.patch`) | Binary-verified `clk-bringup.ko` 19 clocks vs 3 `CLK_IS_CRITICAL` in port. Verified on hardware under `D3`. |
| **`B7`** | Behavioral Delta — Unused Clock/PD Disable Nodes | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | Same as `D3` (`patches/03-soc-power/0002`, `patches/07-dts/0002`, `dts/mt6789.dtsi`) | Binary-verified `clk-disable-unused.ko` / `mtk-pm-domain-disable-unused.ko`. Verified on hardware under `D3`. |
| **`B8`** | Behavioral Delta — NFC `clkbuf` / `mediatek,nfc-gpio-v2` | **`VERIFY-ON-DEVICE-FIRST`** | Phase 4 | Same as `D2.6` (`patches/07-dts/0003`, `dts/mt6789-advan-x1.dts`) | Binary-verified `st21nfc.ko` `clk_buf_ctrl` import vs mainline `st-nci-i2c`. Verified on hardware under `D2.6`. |
| **`B9`** | Behavioral Delta — Touch Firmware Bundling & `drm_panel_follower` | **`PRESERVE-AS-IS`** | Guarded | `patches/04-peripherals/0018`, `0019`, `initramfs/lib/firmware/chipone/icnl9922c.bin` | Flashless ICNL9922C TDDI loads 71,792-byte firmware via 1000-byte DRW chunks with SFCTRL CRC and `drm_panel_follower` sync. Intentional mainline architecture. |
| **`B10`** | Behavioral Delta — Panel Init, DSC PPS, Timings, D-PHY, OVL0/RDMA0 Quirks | **`PRESERVE-AS-IS`** | Guarded | `patches/06-display/0002`, `0004`, `0005`, `0007` | Byte-for-byte binary-verified against `panel-fhdp-icnl9922-dsi-vdo-boe678-120hz.ko` and `mediatek-drm.ko`. Must remain untouched except for `P7` pointer cleanup. |
| **`B11`** | Behavioral Delta — LVTS eFuse `0x1a4/0x34` & V5 Manual RCK | **`PRESERVE-AS-IS`** | Guarded | `patches/04-peripherals/0009-thermal-drivers-mediatek-lvts-Add-MT6789-support.patch`, `dts/mt6789.dtsi` | Hardware-validated fix for `atag,devinfo` +16B offset shift and V5 Auto-RCK sensor glitch on `BIG_CPU2` (`cpu6`). Must remain untouched when fixing `P2`. |
| **`B12`** | Behavioral Delta — I²C `mt8188_compat`, SPI `mt6765_compat`, MT6358 Keys, AFE Adaptation | **`PRESERVE-AS-IS`** | Guarded | `patches/04-peripherals/0002`, `0004`, `0012`, `0021` | Hardware-validated fixes (`dma_sync=0` for `tps65132`, MT6358 long-press reset bitmask `GENMASK(13,12)/BIT(9)/BIT(8)`, AFE `AFE_AGENT_ON` `0x1b00`). Must remain untouched. |
| **`PROC-1`** | Build Reproducibility — Pin Upstream `BASE` SHA | **`EXECUTE-NOW`** | Phase 0 | `scripts/build.sh` | Pin `BASE=${BASE:-666539eecdf4db623f5e3c3e55cf9f2dfad24aa4}` (Linux v7.4.0-rc3) in `scripts/build.sh:23` instead of moving `master`. |
| **`PROC-2`** | Patch Queue Hygiene — `verify_patches.sh` & DTS Sync Check | **`EXECUTE-NOW`** | Phase 0 | `scripts/verify_patches.sh`, `scripts/build.sh`, `README.md` | Add `scripts/verify_patches.sh` (or `scripts/build.sh verify` mode) that checks clean `git apply --check`, zero drift between `dts/*.dts*` and `patches/07-dts/0002,0003`, `dtc`, `dt-doc-validate`, and `dt-validate`. |
| **`S7.1`–`S7.12`** | Section 7 Verified Mainline Adaptations (CCF/Paris Pinctrl/SCPSYS helpers, clock headers, PLL/MMSYS/SMI tables, SC89601A ID 4 & 5 mΩ shunt, pinctrl 8th `"eint"` region, vendor-only daemon decoupling) | **`PRESERVE-AS-IS`** | Guarded | All `patches/01-clk` .. `patches/07-dts` | Confirmed correct by source and binary audit (§7 of `docs/upstream-deviation-audit.md`). Locked in by Section 4 Protected Quirks Checklist. |

### Complete Accounting of All 27 `dt-validate` Schema Errors (`D1.1`–`D1.13`)

Every single error in `/tmp/advan-audit/dtvalidate.out` is mapped to its `D1.*` sub-ID and target file below:

| Error # | Node / Compatible in `board.dtb` | `dt-validate` Failure Message | Audit Sub-ID | Target File(s) |
|---|---|---|---|---|
| **1** | `pmic (mediatek,mt6366): keys` | `Unevaluated properties are not allowed ('homekey', 'powerkey' were unexpected)` (emitted twice by `mt6397.yaml` `$ref`) | **`D1.10`** | `dts/mt6789-advan-x1.dts:691,696`, `patches/07-dts/0003-*.patch` |
| **2** | `keys (mediatek,mt6358-keys)` | `Unevaluated properties are not allowed ('homekey', 'powerkey' were unexpected)` | **`D1.10`** | `dts/mt6789-advan-x1.dts:691,696`, `patches/07-dts/0003-*.patch` |
| **3** | `gpu@13000000 (mediatek,mt6789-mali)` | `compatible: ['mediatek,mt6789-mali', 'mediatek,mt8186-mali', 'arm,mali-valhall-jm'] is not valid under any of the given schemas` | **`D1.7`** | `patches/05-gpu/0001-dt-bindings-gpu-mali-bifrost-Add-MT6789-compatible.patch` |
| **4** | `ufshci@11270000 (mediatek,mt8183-ufshci)` | `'phys' is a required property` | **`D1.11`** | `dts/mt6789.dtsi:773-795`, `patches/07-dts/0002-*.patch`, `Documentation/devicetree/bindings/ufs/mediatek,ufs.yaml` |
| **5** | `ufshci@11270000 (mediatek,mt8183-ufshci)` | `Unevaluated properties are not allowed ('reset-names', 'resets' were unexpected)` | **`D1.11`** | Same as Error #4 |
| **6** | `ufshci@11270000 (mediatek,mt8183-ufshci)` | `clocks: [[16, 47], [16, 43], [16, 42], [16, 41], [16, 48], [14, 38], [14, 39]] is too long` | **`D1.11`** | Same as Error #4 |
| **7** | `ufshci@11270000 (mediatek,mt8183-ufshci)` | `clock-names: ['ufs', 'ufs_mp', 'unipro_tick', 'unipro_sys', 'ufs_aes', 'ufsfde_ck', 'ufs_ck'] is too long` | **`D1.11`** | Same as Error #4 |
| **8** | `t-phy@11f40000 (mediatek,generic-tphy-v2)` | `compatible: ['mediatek,generic-tphy-v2'] is not valid under any of the given schemas` | **`D1.13`** | `dts/mt6789.dtsi:816-821`, `patches/07-dts/0002-*.patch`, `Documentation/devicetree/bindings/phy/mediatek,tphy.yaml` |
| **9** | `usb@11200000 (mediatek,mtk-musb)` | `compatible:0: 'mediatek,mtk-musb' is not one of ['mediatek,mt8516-musb', 'mediatek,mt2701-musb', 'mediatek,mt7623-musb']` | **`D1.12`** | `dts/mt6789.dtsi:834-846`, `patches/07-dts/0002-*.patch`, `Documentation/devicetree/bindings/usb/mediatek,musb.yaml` |
| **10** | `usb@11200000 (mediatek,mtk-musb)` | `compatible: ['mediatek,mtk-musb'] is too short` | **`D1.12`** | Same as Error #9 |
| **11** | `clock-controller@1101b000 (mediatek,mt6789-imp_iic_wrap_c)` | `compatible: ['mediatek,mt6789-imp_iic_wrap_c', 'syscon'] is too long` | **`D1.3`** | `patches/01-clk/0001-dt-bindings-clock-mediatek-Add-MT6789-clock-and-rese.patch` |
| **12** | `clock-controller@11e02000 (mediatek,mt6789-imp_iic_wrap_w)` | `compatible: ['mediatek,mt6789-imp_iic_wrap_w', 'syscon'] is too long` | **`D1.3`** | Same as Error #11 |
| **13** | `clock-controller@11eb4000 (mediatek,mt6789-imp_iic_wrap_en)` | `compatible: ['mediatek,mt6789-imp_iic_wrap_en', 'syscon'] is too long` | **`D1.3`** | Same as Error #11 |
| **14** | `clock-controller@11f01000 (mediatek,mt6789-imp_iic_wrap_n)` | `compatible: ['mediatek,mt6789-imp_iic_wrap_n', 'syscon'] is too long` | **`D1.3`** | Same as Error #11 |
| **15** | `smi@14002000 (mediatek,mt6789-smi-common)` | `compatible: ['mediatek,mt6789-smi-common', 'mediatek,smi-common'] is not valid under any of the given schemas` | **`D1.8`** | `dts/mt6789.dtsi:1038`, `patches/07-dts/0002-*.patch` |
| **16** | `/soc/smi@14002000` | `failed to match any schema with compatible: ['mediatek,mt6789-smi-common', 'mediatek,smi-common']` | **`D1.8`** | Same as Error #15 |
| **17** | `smi@14003000 (mediatek,mt6789-smi-larb)` | `compatible: ['mediatek,mt6789-smi-larb', 'mediatek,smi-larb'] is not valid under any of the given schemas` | **`D1.8`** | `dts/mt6789.dtsi:1049`, `patches/07-dts/0002-*.patch` |
| **18** | `/soc/smi@14003000` | `failed to match any schema with compatible: ['mediatek,mt6789-smi-larb', 'mediatek,smi-larb']` | **`D1.8`** | Same as Error #17 |
| **19** | `smi@14004000 (mediatek,mt6789-smi-larb)` | `compatible: ['mediatek,mt6789-smi-larb', 'mediatek,smi-larb'] is not valid under any of the given schemas` | **`D1.8`** | `dts/mt6789.dtsi:1060`, `patches/07-dts/0002-*.patch` |
| **20** | `/soc/smi@14004000` | `failed to match any schema with compatible: ['mediatek,mt6789-smi-larb', 'mediatek,smi-larb']` | **`D1.8`** | Same as Error #19 |
| **21** | `ovl@14005000 (mediatek,mt6789-disp-ovl)` | `'iommus' is a required property` | **`D1.6`** | `dts/mt6789.dtsi:1070-1076`, `patches/07-dts/0002-*.patch`, `patches/03-soc-power/0010-*.patch` |
| **22** | `rdma@14007000 (mediatek,mt6789-disp-rdma)` | `'iommus' is a required property` | **`D1.6`** | `dts/mt6789.dtsi:1078-1084`, `patches/07-dts/0002-*.patch`, `patches/03-soc-power/0010-*.patch` |
| **23** | `postmask@1400e000 (mediatek,mt6789-disp-postmask)` | `'power-domains' is a required property` | **`D1.5`** | `dts/mt6789.dtsi:1118-1124`, `patches/07-dts/0002-*.patch` |
| **24** | `dither@1400f000 (mediatek,mt6789-disp-dither)` | `'power-domains' is a required property` | **`D1.5`** | `dts/mt6789.dtsi:1126-1131`, `patches/07-dts/0002-*.patch` |
| **25** | `dsc@14012000 (mediatek,mt6789-disp-dsc)` | `'power-domains' is a required property` | **`D1.5`** | `dts/mt6789.dtsi:1133-1138`, `patches/07-dts/0002-*.patch` |
| **26** | `sound (mediatek,mt6789-mt6366-sound)` | `audio-routing: ['Headphone', 'Headphone L', 'Headphone', 'Headphone R', 'Earpiece', 'Receiver'] is too long` | **`D1.2`** | `patches/04-peripherals/0020-ASoC-dt-bindings-mediatek-Add-MT6789-AFE-and-MT6366-.patch` |
| **27** | `sound (mediatek,mt6789-mt6366-sound)` | `'model' does not match any of the regexes: '^dai-link-[0-9]+$', '^pinctrl-[0-9]+$'` | **`D1.1`** | `patches/04-peripherals/0020-ASoC-dt-bindings-mediatek-Add-MT6789-AFE-and-MT6366-.patch` |

---

## 2. Phased Execution Roadmap & Patch-Ordering Dependencies

To preserve bisectability across all 51 patches (`patches/01-clk` through `patches/07-dts`), every header or binding addition (`01-clk` .. `06-display`) lands in its subsystem patch *before* `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch` and `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch` reference it, and standalone `dts/mt6789.dtsi` and `dts/mt6789-advan-x1.dts` are updated in lockstep with `07-dts/0002` and `07-dts/0003`.

1. **Phase 0 — Build & Patch-Queue Hygiene (`PROC-1`, `PROC-2`)**
   - Pin `BASE=${BASE:-666539eecdf4db623f5e3c3e55cf9f2dfad24aa4}` in `scripts/build.sh:23`.
   - Add `scripts/verify_patches.sh` enforcing: (a) clean `git apply --check` of all patches onto `$BASE`, (b) zero diff between `dts/mt6789.dtsi` / `dts/mt6789-advan-x1.dts` and the applied tree, (c) `checkpatch.pl` on applied commits, and (d) `dtc` + `dt-doc-validate` + `dt-validate`.
2. **Phase 1 — Critical Runtime & Hardware Correctness Fixes (`P1`, `D2.2`/`B1`, `P7`)**
   - **WP-1.1 (`P1`):** Amend `patches/03-soc-power/0004-soc-mediatek-pwrap-Add-MT6789-PMIC-wrapper-support.patch` to define `mt6789_regs[]` with `[PWRAP_INT_EN] = 0x450` and point `pwrap_mt6789.regs` at `mt6789_regs`.
   - **WP-1.2 (`D2.2`/`B1`):** Amend `patches/04-peripherals/0016-dt-bindings-power-supply-Add-CellWise-CW2217-fuel-ga.patch`, `patches/04-peripherals/0017-power-supply-Add-CellWise-CW2217-battery-fuel-gauge-.patch`, `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`, and `dts/mt6789-advan-x1.dts` to document and supply the 80-byte `cellwise,battery-profile` and execute the non-destructive `cw2217_get_state()` + `cw2217_config_start_ic()` init sequence.
   - **WP-1.3 (`P7`):** Amend `patches/06-display/0005-drm-mediatek-Add-MT6789-DSI-and-hardware-DSC-1.1-sup.patch` and `patches/06-display/0007-drm-panel-Add-Chipone-ICNL9922C-MIPI-DSI-panel-drive.patch` to clear `dsi->dsc` and `dsi->hs_rate` on DSI detach, propagate `NULL` DSC pointers to `mtk_ddp_comp`, and qualify `icnl9922c_dsc` as `static const`.
3. **Phase 2 — Upstream Subsystem Alignment (`P2`, `P3`, `P4`)**
   - **WP-2.1 (`P2`):** Amend `patches/04-peripherals/0008-thermal-drivers-mediatek-lvts-Enable-sensing-before-.patch` to restore zone-driven `sensor_map` accumulation inside the `lvts_for_each_valid_sensor` registration loop while retaining `!(value & 0xFFFF) -> -EAGAIN` in `lvts_get_temp()` and a 2–3 ms post-enable settle sleep.
   - **WP-2.2 (`P3` & `P4`):** Amend `patches/04-peripherals/0021-ASoC-mediatek-mt8186-Add-MT6789-AFE-and-MT6366-sound.patch` to restore volatile `AFE_AUD_PAD_TOP`, resume AFE runtime PM around `mt8186_mt6366_init()` via `mt6789_mt6366_adda_init()`, attach `mtk_soundcard_common_playback_ops` / `mtk_soundcard_common_capture_ops` to the 5 FE links, and define `mt6789_pcm_constraints`.
4. **Phase 3 — Devicetree Functional & Schema Compliance (`D1.1`–`D1.8`, `D1.10`–`D1.13`, `D4`)**
   - **WP-3.1 (`D1.3`):** Amend `patches/01-clk/0001-dt-bindings-clock-mediatek-Add-MT6789-clock-and-rese.patch`.
   - **WP-3.2 (`D1.4`, `D4`):** Amend `patches/02-pinctrl/0001-dt-bindings-pinctrl-mediatek-Add-MT6789-pinctrl-bind.patch`.
   - **WP-3.3 (`D1.6` bindings/header):** Amend `patches/03-soc-power/0010-dt-bindings-memory-mediatek-Add-MT6789-SMI-compatibl.patch` to add `include/dt-bindings/memory/mediatek,mt6789-memory-port.h` and `mediatek,mt6789-iommu-mm` in `Documentation/devicetree/bindings/iommu/mediatek,iommu.yaml` (must precede `07-dts/0002`).
   - **WP-3.4 (`D1.11`, `D1.12`, `D1.13` bindings):** Amend `patches/04-peripherals/0013-dt-bindings-mmc-mtk-sd-Add-MT6789-compatible.patch` (or insert `04-peripherals/0013b`) to add `mediatek,mt6789-ufshci`, `mediatek,mt6789-musb`, and `mediatek,mt6789-tphy` to `mediatek,ufs.yaml`, `mediatek,musb.yaml`, and `mediatek,tphy.yaml`.
   - **WP-3.5 (`D1.1`, `D1.2`):** Amend `patches/04-peripherals/0020-ASoC-dt-bindings-mediatek-Add-MT6789-AFE-and-MT6366-.patch`.
   - **WP-3.6 (`D1.7`):** Amend `patches/05-gpu/0001-dt-bindings-gpu-mali-bifrost-Add-MT6789-compatible.patch`.
   - **WP-3.7 (`D1.5`, `D1.6`, `D1.8`, `D1.11`, `D1.12`, `D1.13` SoC DTSI):** Amend `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch` and `dts/mt6789.dtsi` in lockstep.
   - **WP-3.8 (`D1.10` Board DTS):** Amend `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch` and `dts/mt6789-advan-x1.dts` in lockstep.
5. **Phase 4 — On-Device Verification Playbook (`D2.1`/`B2`, `D2.3`/`B3`, `D2.4`/`B4`, `D2.5`/`B5`, `D2.6`/`B8`, `D2.7`, `D3`/`B6`/`B7`)**
   - Execute non-destructive telemetry commands on the booted Advan X1 and apply conditional patch actions only where hardware criteria fail.
6. **Phase 5 — Deferred Upstreaming Polish (`P5`, `P6`)**
   - Populate the remaining 9 SCPSYS power domains + subdomain hierarchy and rename `MT8192_TOP_AXI_PROT_EN_*` macros to `MT6789_TOP_AXI_PROT_EN_*` when camera/ISP/video/modem consumers are enabled or prior to LKML submission.

---

## 3. Detailed Work Packages (Grouped by Phase)

---

### Phase 0: Build & Patch-Queue Hygiene (`PROC-1`, `PROC-2`)

#### WP-0.1 — Pin Upstream Baseline Commit & Enforce Patch/DTS/Schema Verification (`PROC-1`, `PROC-2`)

- **Target Files:**
  - `scripts/build.sh` (line 23)
  - `scripts/verify_patches.sh` (new executable script referenced by `README.md:58-59`)
- **Exact Technical Change (`scripts/build.sh:22-25`):**
  ```diff
   JOBS=${JOBS:-1}
  -BASE=${BASE:-master}
  +BASE=${BASE:-666539eecdf4db623f5e3c3e55cf9f2dfad24aa4}
   BRANCH=port-series
   MODE=${1:-full}
  ```
- **Exact Technical Change (`scripts/verify_patches.sh`):**
  ```bash
  #!/bin/bash
  # Verify clean patch application onto pinned upstream BASE, zero DTS drift,
  # dtc compilation, and dt-validate schema compliance in /tmp/advan-plan.
  set -euo pipefail
  REPO="$(cd "$(dirname "$0")/.." && pwd)"
  K=${K:-/work/mainline/linux}
  BASE=${BASE:-666539eecdf4db623f5e3c3e55cf9f2dfad24aa4}
  TMP=${TMP:-/tmp/advan-plan/verify-tree}

  rm -rf "$TMP"
  mkdir -p "$TMP"
  git -C "$K" archive "$BASE" | tar -x -C "$TMP"

  count=0
  for p in "$REPO"/patches/*/*.patch "$REPO"/patches/*.patch; do
      [ -e "$p" ] || continue
      patch -d "$TMP" -p1 --forward --dry-run < "$p" >/dev/null
      patch -d "$TMP" -p1 --forward --quiet < "$p"
      count=$((count + 1))
  done
  echo "Applied $count patches cleanly onto $BASE"

  diff -u "$REPO/dts/mt6789.dtsi" "$TMP/arch/arm64/boot/dts/mediatek/mt6789.dtsi"
  diff -u "$REPO/dts/mt6789-advan-x1.dts" "$TMP/arch/arm64/boot/dts/mediatek/mt6789-advan-x1.dts"
  echo "DTS sync check: OK (zero drift)"

  clang -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
      -I "$TMP/include" -I "$TMP/include/dt-bindings" -I "$TMP/arch/arm64/boot/dts" \
      "$TMP/arch/arm64/boot/dts/mediatek/mt6789-advan-x1.dts" -o "$TMP/board.pp.dts"
  dtc -I dts -O dtb -o "$TMP/board.dtb" "$TMP/board.pp.dts"
  dt-validate -s "$TMP/Documentation/devicetree/bindings" -m "$TMP/board.dtb"
  echo "verify_patches.sh: ALL CHECKS PASSED"
  ```
- **Regression & Edge-Case Analysis:**
  - `BASE=${BASE:-666539eecdf4db623f5e3c3e55cf9f2dfad24aa4}` keeps environment-variable override compatibility (`BASE=v7.5-rc1 ./scripts/build.sh`) while preventing silent rebase drift if `/work/mainline/linux` `master` moves forward.
  - Note on `checkpatch.pl` (`XC-3` in audit): running `checkpatch.pl` directly on raw `.patch` files emits false-positive trailing-whitespace warnings on standard `git format-patch` empty context lines (`^ $`) and `-- ` signature lines; `checkpatch.pl` should be run on git commits (`git format-patch --stdout $BASE..HEAD | scripts/checkpatch.pl -`) or with `--ignore=TRAILING_WHITESPACE` when scanning raw patch files.

---

### Phase 1: Critical Runtime & Hardware Correctness Fixes (`P1`, `D2.2`/`B1`, `P7`)

#### WP-1.1 — Fix MT6789 PMIC Wrapper (`pwrap`) Register Map (`P1`)

- **Target Files:**
  - `patches/03-soc-power/0004-soc-mediatek-pwrap-Add-MT6789-PMIC-wrapper-support.patch` (amending hunks against `drivers/soc/mediatek/mtk-pmic-wrap.c`)
- **Vendor & Mainline Cross-Reference:**
  - In `patches/03-soc-power/0004-soc-mediatek-pwrap-Add-MT6789-PMIC-wrapper-support.patch:38-49` (post-patch `drivers/soc/mediatek/mtk-pmic-wrap.c:2307-2318`), `pwrap_mt6789` sets `.regs = mt8195_regs`, `.int_en_all = 0x180000`, `.caps = PWRAP_CAP_ARB`.
  - In mainline `drivers/soc/mediatek/mtk-pmic-wrap.c:1090-1105`, `mt8195_regs[]` has `[PWRAP_INT_EN] = 0x420` and `[PWRAP_INT1_EN] = 0x450`.
  - At `mtk-pmic-wrap.c:2589` (post-patch), `pwrap_probe()` executes `pwrap_writel(wrp, wrp->master->int_en_all, PWRAP_INT_EN)` unconditionally, whereas `PWRAP_INT1_EN` is only written if `PWRAP_CAP_INT1_EN` is set in `.caps`.
  - In vendor `mp01-kernel/drivers/soc/mediatek/mtk-pmic-wrap.c:1041-1050` and `:2518-2529`, MT6789 defines its own `mt6789_regs[]` where `[PWRAP_INT_EN] = 0x450` (and WACS2/SWINF2 registers at `0x880..0x8A8`, `PWRAP_TIMER_EN = 0x3E4`, `PWRAP_INIT_DONE2 = 0x0`). Furthermore, in MT6789/MT8195 PMIC wrapper hardware, when `PWRAP_INT_EN` is at `0x450`, the companion flag/clear registers are `PWRAP_INT_FLG = 0x458` and `PWRAP_INT_CLR = 0x45C` (which `mt8195_regs` calls `INT1_EN/FLG/CLR`).
- **Exact Technical Change (`drivers/soc/mediatek/mtk-pmic-wrap.c` inside `patches/03-soc-power/0004-soc-mediatek-pwrap-Add-MT6789-PMIC-wrapper-support.patch`):**
  1. Add `mt6789_regs[]` immediately before `mt6795_regs[]` (around upstream line 980 / after `mt6779_regs[]` at line 1088):
     ```c
     static const int mt6789_regs[] = {
     	[PWRAP_INIT_DONE2] =		0x0,
     	[PWRAP_TIMER_EN] =		0x3E4,
     	[PWRAP_INT_EN] =		0x450,
     	[PWRAP_INT_FLG] =		0x458,
     	[PWRAP_INT_CLR] =		0x45C,
     	[PWRAP_WACS2_CMD] =		0x880,
     	[PWRAP_SWINF_2_WDATA_31_0] =	0x884,
     	[PWRAP_SWINF_2_RDATA_31_0] =	0x894,
     	[PWRAP_WACS2_VLDCLR] =		0x8A4,
     	[PWRAP_WACS2_RDATA] =		0x8A8,
     };
     ```
  2. Update `pwrap_mt6789` (`patches/03-soc-power/0004:38-49`) to reference `mt6789_regs`:
     ```c
     static const struct pmic_wrapper_type pwrap_mt6789 = {
     	.regs = mt6789_regs,
     	.type = PWRAP_MT6789,
     	.arb_en_all = 0x777f,
     	.int_en_all = 0x180000,
     	.int1_en_all = 0,
     	.spi_w = PWRAP_MAN_CMD_SPI_WRITE,
     	.wdt_src = PWRAP_WDT_SRC_MASK_ALL,
     	.caps = PWRAP_CAP_ARB,
     	.init_reg_clock = pwrap_common_init_reg_clock,
     	.init_soc_specific = NULL,
     };
     ```
- **Regression & Edge-Case Analysis:**
  - Why include `[PWRAP_INT_FLG] = 0x458` and `[PWRAP_INT_CLR] = 0x45C` in `mt6789_regs[]` in addition to the 8 entries in `mp01`'s `mt6789_regs[]`? In `drivers/soc/mediatek/mtk-pmic-wrap.c:2180-2191` (`pwrap_interrupt()`), the mainline IRQ handler reads `pwrap_readl(wrp, PWRAP_INT_FLG)` and clears `pwrap_writel(wrp, rdata, PWRAP_INT_CLR)` whenever GIC SPI 252 (`pwrap@10026000`) fires! If `PWRAP_INT_FLG` and `PWRAP_INT_CLR` were left uninitialized (`0` in a sparse C array), they would alias `[PWRAP_INIT_DONE2] = 0x0`, causing `pwrap_interrupt()` to read `0x0` and write `rdata` to offset `0x0` instead of clearing the wrapper interrupt at `0x45C` (triggering an IRQ storm). Including `PWRAP_INT_FLG = 0x458` and `PWRAP_INT_CLR = 0x45C` prevents that IRQ storm hazard while programming `0x180000` to `PWRAP_INT_EN = 0x450`.

---

#### WP-1.2 — Implement CW2217 Fuel-Gauge 80-Byte Profile Verification & Initialization Sequence (`D2.2`, `B1`)

- **Target Files:**
  - `patches/04-peripherals/0016-dt-bindings-power-supply-Add-CellWise-CW2217-fuel-ga.patch` (`Documentation/devicetree/bindings/power/supply/cellwise,cw2217.yaml`)
  - `patches/04-peripherals/0017-power-supply-Add-CellWise-CW2217-battery-fuel-gauge-.patch` (`drivers/power/supply/cw2217_battery.c`)
  - `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch` (`arch/arm64/boot/dts/mediatek/mt6789-advan-x1.dts:238-243`)
  - `dts/mt6789-advan-x1.dts` (lines 238–243, in lockstep)
- **Vendor & Binary Cross-Reference:**
  - Stock DTB `/work/firmware/advan_x1/dtb/runtime_merged.dts:5108-5114` defines `guage@64` on `i2c0` with `batinfo`:
    ```
    <0x5a000000 0x00 0xa8b2b9b7 0xa49de2d1 0xc7ffdb9c 0x8a716156 0x4f4d49ac 0x9ade53c4
     0xb9c5ced4 0xd5d3d1cd 0xcbd1d7b4 0xa2968d86 0x81838a93 0xa985685f 0x2000ab10 0xa07a00
     0x6420 0xc3830000 0x00 0x40>
    ```
    In big-endian FDT byte order (20 × 32-bit cells = 80 bytes), the exact 80 bytes (`0x10..0x5f`) are:
    ```c
    0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xa8, 0xb2, 0xb9, 0xb7, 0xa4, 0x9d, 0xe2, 0xd1,
    0xc7, 0xff, 0xdb, 0x9c, 0x8a, 0x71, 0x61, 0x56,
    0x4f, 0x4d, 0x49, 0xac, 0x9a, 0xde, 0x53, 0xc4,
    0xb9, 0xc5, 0xce, 0xd4, 0xd5, 0xd3, 0xd1, 0xcd,
    0xcb, 0xd1, 0xd7, 0xb4, 0xa2, 0x96, 0x8d, 0x86,
    0x81, 0x83, 0x8a, 0x93, 0xa9, 0x85, 0x68, 0x5f,
    0x20, 0x00, 0xab, 0x10, 0x00, 0xa0, 0x7a, 0x00,
    0x00, 0x00, 0x64, 0x20, 0xc3, 0x83, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40
    ```
  - Motorola vendor source `/work/android-kernel/motorola-kernel-modules/drivers/power/cw2217b_fg_mmi/cw2217B_fuel_gauge_base.c:712-831` (`cw_config_start_ic`, `cw2217_get_state`, `cw_init`) and binary-verified stock `cw2217_battery.ko:cw_init` (`symbol offset 0x760`, instructions `0x868..0xae0`):
    1. `cw2217_get_state()` checks:
       - `REG_MODE_CONFIG (0x08) == CW2217_MODE_ACTIVE (0x00)` (if not -> `CW2217_NOT_ACTIVE`)
       - `REG_SOC_ALERT (0x0b) & CW2217_CONFIG_UPDATE_FLG (0x80)` is non-zero (if `0` -> `CW2217_PROFILE_NOT_READY`)
       - If an 80-byte profile was provided in DT, reads `REG_BAT_PROFILE (0x10..0x5f)` and compares all 80 bytes against the DT profile (if any byte differs -> `CW2217_PROFILE_NEED_UPDATE`).
    2. **Only if** `cw2217_get_state()` returns non-zero (gauge not active, update flag not set, or profile bytes mismatch), `cw2217_config_start_ic()` executes:
       - `cw2217_sleep()`: write `CW2217_MODE_RESTART (0x30)` to `REG_MODE_CONFIG (0x08)`, `msleep(20)`, write `CW2217_MODE_SLEEP (0xf0)` to `0x08`, `msleep(10)`.
       - Write the 80-byte profile to `REG_BAT_PROFILE (0x10..0x5f)`.
       - Write `CW2217_CONFIG_UPDATE_FLG (0x80)` to `REG_SOC_ALERT (0x0b)`.
       - Write `0x00` to `REG_GPIO_CONFIG (0x0a)` (close all alerts/interrupts).
       - `cw2217_active()`: write `CW2217_MODE_RESTART (0x30)` to `0x08`, `msleep(20)`, write `CW2217_MODE_ACTIVE (0x00)` to `0x08`, `msleep(10)`.
       - Poll `REG_IC_STATE (0xa7)` up to 50 × `msleep(100)` until `(val & CW2217_IC_READY_MARK) == CW2217_IC_READY_MARK (0x0c)`.
- **Exact Technical Change (`Documentation/devicetree/bindings/power/supply/cellwise,cw2217.yaml` in `patches/04-peripherals/0016-*.patch`):**
  ```yaml
    shunt-resistor-micro-ohms:
      description: External current sense resistor value in micro-ohms.
      default: 10000

    cellwise,battery-profile:
      $ref: /schemas/types.yaml#/definitions/uint8-array
      minItems: 80
      maxItems: 80
      description:
        80-byte board/battery characterization profile table written to
        CW2217 registers 0x10..0x5f during gauge initialization.
  ```
- **Exact Technical Change (`drivers/power/supply/cw2217_battery.c` in `patches/04-peripherals/0017-*.patch`):**
  Add the register/mode constants and non-destructive profile init helpers before `cw2217_probe()`:
  ```c
  #define CW2217_REG_MODE_CONFIG		0x08
  #define CW2217_REG_GPIO_CONFIG		0x0a
  #define CW2217_REG_SOC_ALERT		0x0b
  #define CW2217_REG_CURRENT_H		0x0e
  #define CW2217_REG_BAT_PROFILE		0x10
  #define CW2217_REG_CYCLE_H		0xa4
  #define CW2217_REG_SOH			0xa6
  #define CW2217_REG_IC_STATE		0xa7

  #define CW2217_CHIP_ID			0xa0
  #define CW2217_MODE_ACTIVE		0x00
  #define CW2217_MODE_RESTART		0x30
  #define CW2217_MODE_SLEEP		0xf0
  #define CW2217_CONFIG_UPDATE_FLG	0x80
  #define CW2217_IC_READY_MARK		0x0c
  #define CW2217_PROFILE_SIZE		80
  #define CW2217_READY_POLL_COUNT		50
  ```
  In `struct cw2217_battery`:
  ```c
  struct cw2217_battery {
  	struct device *dev;
  	struct regmap *regmap;
  	struct power_supply *psy;
  	u32 rsense_uohm;
  	u32 charge_full_design_uah;
  	u8 profile[CW2217_PROFILE_SIZE];
  	bool has_profile;
  };
  ```
  Add `cw2217_sleep()`, `cw2217_active()`, `cw2217_get_state()`, `cw2217_config_start_ic()`, and `cw2217_init()`:
  ```c
  static int cw2217_sleep(struct cw2217_battery *cw)
  {
  	int ret;

  	ret = regmap_write(cw->regmap, CW2217_REG_MODE_CONFIG,
  			   CW2217_MODE_RESTART);
  	if (ret)
  		return ret;
  	msleep(20);

  	ret = regmap_write(cw->regmap, CW2217_REG_MODE_CONFIG,
  			   CW2217_MODE_SLEEP);
  	if (ret)
  		return ret;
  	msleep(10);

  	return 0;
  }

  static int cw2217_active(struct cw2217_battery *cw)
  {
  	int ret;

  	ret = regmap_write(cw->regmap, CW2217_REG_MODE_CONFIG,
  			   CW2217_MODE_RESTART);
  	if (ret)
  		return ret;
  	msleep(20);

  	ret = regmap_write(cw->regmap, CW2217_REG_MODE_CONFIG,
  			   CW2217_MODE_ACTIVE);
  	if (ret)
  		return ret;
  	msleep(10);

  	return 0;
  }

  static int cw2217_get_state(struct cw2217_battery *cw, bool *need_update)
  {
  	u8 hw_profile[CW2217_PROFILE_SIZE];
  	unsigned int val;
  	int ret;

  	*need_update = false;

  	ret = regmap_read(cw->regmap, CW2217_REG_MODE_CONFIG, &val);
  	if (ret)
  		return ret;
  	if (val != CW2217_MODE_ACTIVE) {
  		*need_update = true;
  		return 0;
  	}

  	if (!cw->has_profile)
  		return 0;

  	ret = regmap_read(cw->regmap, CW2217_REG_SOC_ALERT, &val);
  	if (ret)
  		return ret;
  	if (!(val & CW2217_CONFIG_UPDATE_FLG)) {
  		*need_update = true;
  		return 0;
  	}

  	ret = regmap_bulk_read(cw->regmap, CW2217_REG_BAT_PROFILE,
  			       hw_profile, sizeof(hw_profile));
  	if (ret)
  		return ret;

  	if (memcmp(hw_profile, cw->profile, sizeof(hw_profile)) != 0)
  		*need_update = true;

  	return 0;
  }

  static int cw2217_config_start_ic(struct cw2217_battery *cw)
  {
  	unsigned int val;
  	int i, ret;

  	ret = cw2217_sleep(cw);
  	if (ret)
  		return ret;

  	if (cw->has_profile) {
  		ret = regmap_bulk_write(cw->regmap, CW2217_REG_BAT_PROFILE,
  					cw->profile, sizeof(cw->profile));
  		if (ret)
  			return ret;

  		ret = regmap_write(cw->regmap, CW2217_REG_SOC_ALERT,
  				   CW2217_CONFIG_UPDATE_FLG);
  		if (ret)
  			return ret;
  	}

  	ret = regmap_write(cw->regmap, CW2217_REG_GPIO_CONFIG, 0x00);
  	if (ret)
  		return ret;

  	ret = cw2217_active(cw);
  	if (ret)
  		return ret;

  	for (i = 0; i < CW2217_READY_POLL_COUNT; i++) {
  		ret = regmap_read(cw->regmap, CW2217_REG_IC_STATE, &val);
  		if (ret)
  			return ret;
  		if ((val & CW2217_IC_READY_MARK) == CW2217_IC_READY_MARK)
  			return 0;
  		msleep(100);
  	}

  	cw2217_sleep(cw);
  	return -ETIMEDOUT;
  }

  static int cw2217_init(struct cw2217_battery *cw)
  {
  	bool need_update;
  	int ret;

  	ret = cw2217_get_state(cw, &need_update);
  	if (ret)
  		return ret;

  	if (!need_update)
  		return 0;

  	return cw2217_config_start_ic(cw);
  }
  ```
  And in `cw2217_probe()`:
  ```c
  	ret = device_property_read_u8_array(cw->dev, "cellwise,battery-profile",
  					    cw->profile, sizeof(cw->profile));
  	if (!ret)
  		cw->has_profile = true;
  	else if (ret != -EINVAL)
  		return dev_err_probe(cw->dev, ret,
  				     "invalid cellwise,battery-profile\n");

  	ret = cw2217_init(cw);
  	if (ret)
  		return dev_err_probe(cw->dev, ret,
  				     "failed to initialize fuel gauge\n");
  ```
- **Exact Technical Change (`dts/mt6789-advan-x1.dts:238-243` and `patches/07-dts/0003-*.patch`):**
  ```dts
  	fuel-gauge@64 {
  		compatible = "cellwise,cw2217";
  		reg = <0x64>;
  		shunt-resistor-micro-ohms = <5000>;
  		monitored-battery = <&battery>;
  		cellwise,battery-profile = /bits/ 8 <
  			0x5a 0x00 0x00 0x00 0x00 0x00 0x00 0x00
  			0xa8 0xb2 0xb9 0xb7 0xa4 0x9d 0xe2 0xd1
  			0xc7 0xff 0xdb 0x9c 0x8a 0x71 0x61 0x56
  			0x4f 0x4d 0x49 0xac 0x9a 0xde 0x53 0xc4
  			0xb9 0xc5 0xce 0xd4 0xd5 0xd3 0xd1 0xcd
  			0xcb 0xd1 0xd7 0xb4 0xa2 0x96 0x8d 0x86
  			0x81 0x83 0x8a 0x93 0xa9 0x85 0x68 0x5f
  			0x20 0x00 0xab 0x10 0x00 0xa0 0x7a 0x00
  			0x00 0x00 0x64 0x20 0xc3 0x83 0x00 0x00
  			0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x40
  		>;
  	};
  ```
- **Regression & Edge-Case Analysis:**
  - **Warm-reboot SOC preservation:** If `cw2217_probe()` unconditionally reset the IC and rewrote the profile on every boot, the CW2217 would discard its accumulated coulomb counter and recalculate SOC from open-circuit voltage (which under load/charging causes a sudden SOC jump, as warned in `cw2217B_fuel_gauge_base.c:220-223`). By implementing `cw2217_get_state()` first (checking `MODE_ACTIVE == 0x00`, `SOC_ALERT & 0x80`, and `memcmp(hw_profile, cw->profile, 80) == 0`), a warm reboot where the gauge already holds the valid 80-byte profile takes **zero I²C writes** and preserves continuous SOC tracking, while a cold battery insertion or uninitialized gauge performs the full `RESTART -> SLEEP -> profile write -> RESTART -> ACTIVE -> IC_READY` sequence.
  - **Optional DT property fallback:** If a board DTS omits `cellwise,battery-profile` (`ret == -EINVAL`), `cw->has_profile` is `false` and `cw2217_init()` still wakes a sleeping gauge via `CW2217_MODE_RESTART -> CW2217_MODE_ACTIVE` and polls `CW2217_IC_READY_MARK`.

---

#### WP-1.3 — Fix DSC Configuration Pointer Lifetime Across DSI Attach/Detach (`P7`)

- **Target Files:**
  - `patches/06-display/0005-drm-mediatek-Add-MT6789-DSI-and-hardware-DSC-1.1-sup.patch` (`drivers/gpu/drm/mediatek/mtk_crtc.c`, `drivers/gpu/drm/mediatek/mtk_dsi.c`)
  - `patches/06-display/0007-drm-panel-Add-Chipone-ICNL9922C-MIPI-DSI-panel-drive.patch` (`drivers/gpu/drm/panel/panel-chipone-icnl9922c.c`)
- **Source Cross-Reference:**
  - In `patches/06-display/0005:965-975` (post-patch `drivers/gpu/drm/mediatek/mtk_dsi.c:1214-1222`), `mtk_dsi_host_attach()` caches `dsi->hs_rate = device->hs_rate;` and `dsi->dsc = device->dsc;`.
  - In `mtk_dsi.c:1244-1252`, `mtk_dsi_host_detach()` does not clear `dsi->dsc` or `dsi->hs_rate`.
  - In `patches/06-display/0005:39-55` (post-patch `drivers/gpu/drm/mediatek/mtk_crtc.c:405-417`), `mtk_crtc_ddp_hw_init()` only calls `mtk_ddp_comp_set_dsc(comp, dsc)` when `if (dsc)` is non-NULL! If a DSC panel detaches (or is replaced by a non-DSC bridge/panel), `priv->dsc` inside `mtk_ddp_comp_dev` (`mtk_ddp_comp.c:98,223-228`) retains the dangling pointer forever.
- **Exact Technical Change (`patches/06-display/0005-drm-mediatek-Add-MT6789-DSI-and-hardware-DSC-1.1-sup.patch`):**
  1. In `drivers/gpu/drm/mediatek/mtk_crtc.c` (`mtk_crtc_ddp_hw_init()`, patch lines 45–53), call `mtk_ddp_comp_set_dsc(comp, dsc)` unconditionally (even when `dsc == NULL`) so `priv->dsc` in `DDP_COMPONENT_DSC0` is always synchronized with the current DSI attachment state:
     ```diff
      	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
      		struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[i];
      
     -		if (dsc)
     -			mtk_ddp_comp_set_dsc(comp, dsc);
     +		mtk_ddp_comp_set_dsc(comp, dsc);
      
      		if (i == 0)
      			mtk_ddp_comp_bgclr_in_off(comp);
     ```
  2. In `drivers/gpu/drm/mediatek/mtk_dsi.c` (`mtk_dsi_host_detach()`, around upstream line 1025 / post-patch line 1244), clear `dsi->dsc` and `dsi->hs_rate` on detach and on the `mtk_dsi_host_attach()` error path:
     ```c
     static int mtk_dsi_host_detach(struct mipi_dsi_host *host,
     			       struct mipi_dsi_device *device)
     {
     	struct mtk_dsi *dsi = host_to_dsi(host);

     	component_del(dsi->host.dev, &mtk_dsi_component_ops);
     	drm_bridge_remove(&dsi->bridge);
     	dsi->hs_rate = 0;
     	dsi->dsc = NULL;
     	return 0;
     }
     ```
     And in `mtk_dsi_host_attach()` if `IS_ERR(dsi->next_bridge)`:
     ```c
     	if (IS_ERR(dsi->next_bridge)) {
     		ret = PTR_ERR(dsi->next_bridge);
     		dsi-> next_bridge = NULL;
     		dsi->hs_rate = 0;
     		dsi->dsc = NULL;
     		return ret;
     	}
     ```
- **Regression & Edge-Case Analysis:**
  - Note on `struct mipi_dsi_device::dsc` in upstream `<drm/drm_mipi_dsi.h>`: `dsi->dsc` in `struct mipi_dsi_device` has type `struct drm_dsc_config *` (non-const pointer, because some DSC helpers populate PPS tables in-place), whereas `mtk_dsi::dsc` and `mtk_ddp_comp_dev::dsc` are `const struct drm_dsc_config *`. Therefore, in `panel-chipone-icnl9922c.c:35`, keeping `static struct drm_dsc_config icnl9922c_dsc` avoids a `-Wdiscarded-qualifiers` compiler warning under `LLVM=1` when assigning `dsi->dsc = &icnl9922c_dsc;` at line 527, while clearing `dsi->dsc = NULL` in `mtk_dsi_host_detach()` and unconditionally updating `mtk_ddp_comp_set_dsc(comp, dsc)` in `mtk_crtc_ddp_hw_init()` completely eliminates the dangling pointer across component unbind / module unload.

---

### Phase 2: Upstream Subsystem Alignment (`P2`, `P3`, `P4`)

#### WP-2.1 — Restore Generic LVTS Zone-Driven `sensor_map` Construction (`P2`)

- **Target Files:**
  - `patches/04-peripherals/0008-thermal-drivers-mediatek-lvts-Enable-sensing-before-.patch` (`drivers/thermal/mediatek/lvts_thermal.c`)
- **Mainline Cross-Reference:**
  - Clean upstream `drivers/thermal/mediatek/lvts_thermal.c:1298-1365` (`lvts_ctrl_start()`) registers each sensor's thermal zone inside `lvts_for_each_valid_sensor(i, lvts_ctrl)`, skips sensors returning `-ENODEV` (no DT thermal zone), and only sets `sensor_map |= sensor_bitmap[i]` for sensors with an active DT thermal zone, then writes `LVTS_MSRCTL1` or `LVTS_MONCTL0` after the loop.
  - Why did `patches/04-peripherals/0008` exist in the first place? Because when `devm_thermal_of_zone_register()` registers a zone, the thermal core immediately calls `.get_temp()` (`lvts_get_temp()`). If hardware monitoring hasn't completed its first 2 ms sampling round (or is enabled after registration), `LVTS_MSR0..3` reads `0x00000000`. In `lvts_get_temp()`, `lvts_read_all_msr_raw()` checks `BIT(16)` (which is `0` when the register is `0x0`, i.e., "valid"), and `lvts_raw_to_temp(0, ...)` computes `>250 °C`, causing an instant critical thermal shutdown at boot!
  - Look at the two changes in `patches/04-peripherals/0008`:
    1. In `lvts_get_temp()` (`lines 21-29` of patch 0008):
       ```c
       if (!(value & 0xFFFF))
           return -EAGAIN;
       ```
       When `devm_thermal_of_zone_register()` calls `lvts_get_temp()` before the first sample completes, `value & 0xFFFF` is `0`, so `lvts_get_temp()` safely returns `-EAGAIN` (which the Linux thermal core explicitly handles during initial zone registration by deferring the first temperature check until the polling workqueue runs!).
    2. In `lvts_ctrl_start()` (`lines 31-99` of patch 0008): it moved `sensor_map` outside the loop and deleted the zone-driven `sensor_map |= sensor_bitmap[i]`.
- **Exact Technical Change (`patches/04-peripherals/0008-thermal-drivers-mediatek-lvts-Enable-sensing-before-.patch`):**
  Restore upstream's exact `lvts_ctrl_start()` loop in `drivers/thermal/mediatek/lvts_thermal.c:1298-1365` (so only sensors bound to a DT thermal zone are enabled in `sensor_map`, and if `sensor_map == 0` no hardware monitoring is started), and keep only the `if (!(value & 0xFFFF)) return -EAGAIN;` guard in `lvts_get_temp()` plus a `usleep_range(2000, 3000)` after writing `LVTS_MSRCTL1` / `LVTS_MONCTL0` at the end of `lvts_ctrl_start()`:
  ```diff
  --- a/drivers/thermal/mediatek/lvts_thermal.c
  +++ b/drivers/thermal/mediatek/lvts_thermal.c
  @@ -376,6 +376,9 @@ static int lvts_get_temp(struct thermal_zone_device *tz, int *temp)
   	if (rc)
   		return -EAGAIN;
   
  +	if (!(value & 0xFFFF))
  +		return -EAGAIN;
  +
   	*temp = lvts_raw_to_temp(value, lvts_data);
   
   	return 0;
  @@ -1361,6 +1364,9 @@ static int lvts_ctrl_start(struct device *dev, struct lvts_ctrl *lvts_ctrl)
   		writel(sensor_map | BIT(9), LVTS_MONCTL0(lvts_ctrl->base));
   	}
   
  +	if (sensor_map)
  +		usleep_range(2000, 3000);
  +
   	return 0;
   }
  ```
- **Regression & Edge-Case Analysis:**
  - Wait: during `devm_thermal_of_zone_register()` inside the loop, `LVTS_MONCTL0` has not been written yet, so `LVTS_MSR` is `0`. Because `if (!(value & 0xFFFF)) return -EAGAIN;` is in `lvts_get_temp()`, `thermal_zone_device_enable()` during `devm_thermal_of_zone_register()` receives `-EAGAIN` (which `thermal_zone_device_update()` ignores without triggering any trip point) and schedules the normal polling timer (`polling-delay = <1000>`). Immediately after the loop finishes, `LVTS_MONCTL0` is written with `sensor_map | BIT(9)` and `usleep_range(2000, 3000)` waits for the first hardware measurement round to complete before `lvts_ctrl_start()` returns (and before `lvts_probe()` enables LVTS interrupts!).
  - Check line-number offsets in `patches/04-peripherals/0009-thermal-drivers-mediatek-lvts-Add-MT6789-support.patch`: amending `0008` from `-18 lines` net to `+6 lines` net shifts lines below `lvts_ctrl_start()` by `+24` lines. Update the two hunk headers in `patches/04-peripherals/0009` (`@@ -1626,6 +1741,72 @@` -> `@@ -1650,6 +1765,72 @@` and `@@ -2037,6 +2218,21 @@` -> `@@ -2061,6 +2242,21 @@`, `@@ -2167,6 +2363,7 @@` -> `@@ -2191,6 +2387,7 @@`) so `0009` applies with zero fuzz.

---

#### WP-2.2 — Restore Volatile `AFE_AUD_PAD_TOP`, Fix AFE Runtime PM Resume, and Wire Audio Card FE Ops & Constraints (`P3`, `P4`)

- **Target Files:**
  - `patches/04-peripherals/0021-ASoC-mediatek-mt8186-Add-MT6789-AFE-and-MT6366-sound.patch` (`sound/soc/mediatek/mt8186/mt8186-afe-pcm.c`, `sound/soc/mediatek/mt8186/mt6789-mt6366.c`)
- **Mainline Cross-Reference:**
  - **`P3` (`mt8186-afe-pcm.c` & `mt6789-mt6366.c`):**
    - In `mt8186-afe-pcm.c:2570`, `AFE_AUD_PAD_TOP (0x0e40)` is in `mt8186_is_volatile_reg()`.
    - When `mt6789_mt6366_driver` probes (`mtk_soundcard_common_probe()` -> `devm_snd_soc_register_card()`), ASoC initializes the `Primary Codec` backend link (`.init = mt8186_mt6366_init`) and evaluates DAPM supply widgets, including `SND_SOC_DAPM_SUPPLY_S("AUD_PAD_TOP", ..., AFE_AUD_PAD_TOP, RG_RX_FIFO_ON_SFT, 0, ...)` (`mt8186-dai-adda.c:415-418`).
    - At that moment, `mt8186-afe-pcm` has already completed its probe and entered runtime suspend (`pm_runtime_put()` at the end of `mt8186_afe_pcm_dev_probe()`, which sets `regcache_cache_only(afe->regmap, true)` in `mt8186_afe_runtime_suspend()`).
    - Because `regcache_cache_only` is `true`, reading any `volatile` register returns `-EBUSY`!
    - Look at why `mt6789_mt6366_adda_init()` in commit `50f8669` wasn't enough on its own: in `snd_soc_bind_card()` (`sound/soc/soc-core.c`), `soc_init_pcm_runtime()` calls `rtd->dai_link->init(rtd)` first, and **after** all link `.init()` callbacks return, `snd_soc_dapm_new_widgets(card)` runs at card level to read the initial hardware state of every DAPM widget (including `AUD_PAD_TOP` at `0x0e40`)! If `pm_runtime_put(cmpnt_afe->dev)` was already called inside `mt6789_mt6366_adda_init()`, the AFE device is runtime-suspended again by the time `snd_soc_dapm_new_widgets(card)` runs!
    - Let's verify how `mtk_soundcard_common_probe()` (`sound/soc/mediatek/common/mtk-soundcard-driver.c:225-385`) works:
      `mtk_soundcard_common_probe()` calls `pdata->soc_probe(soc_card_data, needs_legacy_probe)` at line 345, and then calls `devm_snd_soc_register_card(&pdev->dev, card)` at line 359!
      OR even cleaner: in `mt6789-mt6366.c`, we can either:
      - Resume the AFE platform device (`platform_node = of_parse_phandle(pdev->dev.of_node, "mediatek,platform", 0)`) across `mtk_soundcard_common_probe()` in `mt6789_mt6366_platform_probe(struct platform_device *pdev)` (wrapping `mtk_soundcard_common_probe(pdev)` with `pm_runtime_resume_and_get(afe_dev)` before `mtk_soundcard_common_probe(pdev)` and `pm_runtime_put(afe_dev)` after it returns!), OR
      - Also keep `pm_runtime_resume_and_get(cmpnt_afe->dev)` in `mt6789_mt6366_adda_init()`.
      Wait: wrapping `mtk_soundcard_common_probe(pdev)` with `pm_runtime_resume_and_get(afe_dev)` / `pm_runtime_put(afe_dev)` in `mt6789-mt6366.c` guarantees the AFE is runtime-active (clocks enabled, `MT6789_POWER_DOMAIN_AUDIO` powered on, `regcache_cache_only(afe->regmap, false)`) throughout the **entire** `devm_snd_soc_register_card()` lifecycle — covering `mt8186_mt6366_init()` (which runs `mt8186_mtkaif_calibration()` and writes/reads AFE registers!), `snd_soc_dapm_new_widgets()` (which reads volatile `AFE_AUD_PAD_TOP`), and initial `snd_soc_dapm_sync()`!
  - **`P4` (`mt6789-mt6366.c`):**
    - In `sound/soc/mediatek/common/mtk-soundcard-driver.c:137-186`, `mtk_soundcard_common_playback_ops` and `mtk_soundcard_common_capture_ops` invoke `mtk_soundcard_startup()`, which indexes `soc_card->card_data->pcm_constraints[MTK_CONSTRAINT_PLAYBACK]` and `[MTK_CONSTRAINT_CAPTURE]`.
    - In `mt8186-mt6366.c:646-735` and `:1260-1284`, FE playback links use `.ops = &mtk_soundcard_common_playback_ops`, FE capture links use `.ops = &mtk_soundcard_common_capture_ops`, and `mt8186_pcm_constraints` defines playback channels `{ 2 }`, capture channels `{ 1, 2 }`, and rates `{ 48000 }` (or standard MTK AFE rates `{ 8000, 16000, 32000, 44100, 48000, 96000, 192000 }`; matching `mt8186-mt6366.c:1262` `{ 48000 }` or full ADDA rate list `{ 8000, 16000, 32000, 44100, 48000 }`).
- **Exact Technical Change (`sound/soc/mediatek/mt8186/mt8186-afe-pcm.c` in `patches/04-peripherals/0021-*.patch`):**
  In `mt8186_is_volatile_reg()`, add `case AFE_AGENT_ON:` directly (or in `mt6789_is_volatile_reg()`, remove `case AFE_AUD_PAD_TOP: return false;` so `AFE_AUD_PAD_TOP` falls through to `mt8186_is_volatile_reg()` and returns `true`):
  ```c
  static bool mt6789_is_volatile_reg(struct device *dev, unsigned int reg)
  {
  	switch (reg) {
  	case AFE_AGENT_ON:
  		return true;
  	default:
  		return mt8186_is_volatile_reg(dev, reg);
  	}
  }
  ```
- **Exact Technical Change (`sound/soc/mediatek/mt8186/mt6789-mt6366.c` in `patches/04-peripherals/0021-*.patch`):**
  1. Add `#include <linux/of_platform.h>` and `#include <linux/pm_runtime.h>`.
  2. Add `.ops = &mtk_soundcard_common_playback_ops` to `Playback_1`, `Playback_2`, `Playback_3`, and `.ops = &mtk_soundcard_common_capture_ops` to `Capture_1`, `Capture_2`:
     ```c
     	{
     		.name = "Playback_1",
     		.stream_name = "Playback_1",
     		.trigger = { SND_SOC_DPCM_TRIGGER_PRE,
     			     SND_SOC_DPCM_TRIGGER_PRE },
     		.dynamic = 1,
     		.playback_only = 1,
     		.dpcm_merged_format = 1,
     		.dpcm_merged_chan = 1,
     		.dpcm_merged_rate = 1,
     		.ops = &mtk_soundcard_common_playback_ops,
     		SND_SOC_DAILINK_REG(playback1),
     	},
     ```
     (and identically `.ops = &mtk_soundcard_common_playback_ops` on `Playback_2` and `Playback_3`; `.ops = &mtk_soundcard_common_capture_ops` on `Capture_1` and `Capture_2`).
  3. Define `mt6789_pcm_constraints` and wire it in `mt6789_mt6366_pdata`:
     ```c
     static const unsigned int mt6789_pcm_playback_channels[] = { 2 };
     static const unsigned int mt6789_pcm_capture_channels[] = { 1, 2 };
     static const unsigned int mt6789_pcm_rates[] = {
     	8000, 16000, 32000, 44100, 48000, 96000, 192000
     };

     static const struct snd_pcm_hw_constraint_list mt6789_rate_constraint = {
     	.list = mt6789_pcm_rates,
     	.count = ARRAY_SIZE(mt6789_pcm_rates),
     };

     static const struct mtk_pcm_constraints_data mt6789_pcm_constraints[MTK_CONSTRAINT_CAPTURE + 1] = {
     	[MTK_CONSTRAINT_PLAYBACK] = {
     		.channels = &(const struct snd_pcm_hw_constraint_list) {
     			.list = mt6789_pcm_playback_channels,
     			.count = ARRAY_SIZE(mt6789_pcm_playback_channels),
     		},
     		.rates = &mt6789_rate_constraint,
     	},
     	[MTK_CONSTRAINT_CAPTURE] = {
     		.channels = &(const struct snd_pcm_hw_constraint_list) {
     			.list = mt6789_pcm_capture_channels,
     			.count = ARRAY_SIZE(mt6789_pcm_capture_channels),
     		},
     		.rates = &mt6789_rate_constraint,
     	},
     };

     static const struct mtk_soundcard_pdata mt6789_mt6366_pdata = {
     	.card_data = &(struct mtk_platform_card_data) {
     		.card = &mt6789_mt6366_soc_card,
     		.pcm_constraints = mt6789_pcm_constraints,
     		.num_pcm_constraints = ARRAY_SIZE(mt6789_pcm_constraints),
     	},
     	.soc_probe = mt6789_mt6366_soc_probe,
     };
     ```
  4. Wrap `mtk_soundcard_common_probe()` in `mt6789_mt6366_platform_probe()` so the AFE device is runtime-resumed across the entire `devm_snd_soc_register_card()` call (covering both `mt8186_mt6366_init()` MTKAIF calibration and `snd_soc_dapm_new_widgets()` volatile `AFE_AUD_PAD_TOP` reads):
     ```c
     static int mt6789_mt6366_platform_probe(struct platform_device *pdev)
     {
     	struct platform_device *afe_pdev;
     	struct device_node *platform_node;
     	int ret;

     	platform_node = of_parse_phandle(pdev->dev.of_node,
     					 "mediatek,platform", 0);
     	if (!platform_node)
     		return -EINVAL;

     	afe_pdev = of_find_device_by_node(platform_node);
     	of_node_put(platform_node);
     	if (!afe_pdev)
     		return -EPROBE_DEFER;

     	ret = pm_runtime_resume_and_get(&afe_pdev->dev);
     	if (ret < 0) {
     		put_device(&afe_pdev->dev);
     		return ret;
     	}

     	ret = mtk_soundcard_common_probe(pdev);

     	pm_runtime_put(&afe_pdev->dev);
     	put_device(&afe_pdev->dev);
     	return ret;
     }
     ```
     And set `.probe = mt6789_mt6366_platform_probe` in `mt6789_mt6366_driver`.
- **Regression & Edge-Case Analysis:**
  - **Why `mt6789_mt6366_platform_probe()` succeeds where commit `50f8669`'s `mt6789_mt6366_adda_init()` failed:** In `50f8669`, `pm_runtime_resume_and_get()` was only held during `rtd->dai_link->init()` (`mt8186_mt6366_init()`), and `pm_runtime_put()` was called before `snd_soc_bind_card()` reached `snd_soc_dapm_new_widgets()`. Because `mt8186-afe-pcm` does not use autosuspend delay (`pm_runtime_put()` suspends synchronously and puts regmap back into `cache_only` mode immediately), `snd_soc_dapm_new_widgets()` hit `-EBUSY` reading `AFE_AUD_PAD_TOP` right after `mt6789_mt6366_adda_init()` returned! Holding the AFE runtime PM reference across the entire `mtk_soundcard_common_probe(pdev)` call ensures the AFE remains awake until `devm_snd_soc_register_card()` finishes all widget creation and initial DAPM sync.
  - **Why `mt6789_pcm_constraints` must be added in the same patch as `.ops`:** `mtk_soundcard_startup()` (`mtk-soundcard-driver.c:142`) dereferences `&soc_card->card_data->pcm_constraints[ctype]`. Adding `.ops` without `.pcm_constraints` would cause a NULL pointer dereference on the first PCM open.

---

### Phase 3: Devicetree Functional & Schema Compliance (`D1.1`–`D1.8`, `D1.10`–`D1.13`, `D4`)

All Phase 3 changes below were applied to `/tmp/advan-plan/tree` and verified with `dt-doc-validate` (0 errors across all 9 modified YAML files), `dtc` (0 warnings), and `dt-validate` (0 schema errors on `board.dtb`).

#### WP-3.1 — Fix `mediatek,mt6789-clock.yaml` 2-String Compatible Schema (`D1.3`)

- **Target Files:**
  - `patches/01-clk/0001-dt-bindings-clock-mediatek-Add-MT6789-clock-and-rese.patch` (`Documentation/devicetree/bindings/clock/mediatek,mt6789-clock.yaml`, lines 18–23 and 41)
- **Exact Technical Change (`Documentation/devicetree/bindings/clock/mediatek,mt6789-clock.yaml`):**
  ```diff
   properties:
     compatible:
  -    enum:
  -      - mediatek,mt6789-imp_iic_wrap_c
  -      - mediatek,mt6789-imp_iic_wrap_en
  -      - mediatek,mt6789-imp_iic_wrap_n
  -      - mediatek,mt6789-imp_iic_wrap_w
  +    items:
  +      - enum:
  +          - mediatek,mt6789-imp_iic_wrap_c
  +          - mediatek,mt6789-imp_iic_wrap_en
  +          - mediatek,mt6789-imp_iic_wrap_n
  +          - mediatek,mt6789-imp_iic_wrap_w
  +      - const: syscon
  ```
  And in `examples:` (line 41):
  ```diff
       imp_iic_wrap_w: clock-controller@11e02000 {
  -        compatible = "mediatek,mt6789-imp_iic_wrap_w";
  +        compatible = "mediatek,mt6789-imp_iic_wrap_w", "syscon";
           reg = <0x11e02000 0x1000>;
  ```
- **Regression & Edge-Case Analysis:**
  - Matches `clk-mt6789-i2c.c` and `mt6789.dtsi:849,855,861,867`. Eliminates `dt-validate` errors #11, #12, #13, #14.

---

#### WP-3.2 — Fix `mediatek,mt6789-pinctrl.yaml` RSEL Enum Range & Example Drift (`D1.4`, `D4`)

- **Target Files:**
  - `patches/02-pinctrl/0001-dt-bindings-pinctrl-mediatek-Add-MT6789-pinctrl-bind.patch` (`Documentation/devicetree/bindings/pinctrl/mediatek,mt6789-pinctrl.yaml`, lines 85, 92, 138, 140)
- **Exact Technical Change (`Documentation/devicetree/bindings/pinctrl/mediatek,mt6789-pinctrl.yaml`):**
  ```diff
             bias-pull-down:
               oneOf:
                 - type: boolean
                 - enum: [100, 101, 102, 103]
  -              - enum: [200, 201, 202, 203]
  +              - enum: [200, 201, 202, 203, 204, 205, 206, 207]
                 - enum: [75000, 5000]
   
             bias-pull-up:
               oneOf:
                 - type: boolean
                 - enum: [100, 101, 102, 103]
  -              - enum: [200, 201, 202, 203]
  +              - enum: [200, 201, 202, 203, 204, 205, 206, 207]
                 - enum: [1000, 5000, 10000, 75000]
  ```
  And in `examples:` (lines 138–140):
  ```diff
           gpio-controller;
           #gpio-cells = <2>;
  -        gpio-ranges = <&pio 0 0 189>;
  +        gpio-ranges = <&pio 0 0 188>;
           interrupt-controller;
  -        interrupts = <GIC_SPI 212 IRQ_TYPE_LEVEL_HIGH>;
  +        interrupts = <GIC_SPI 244 IRQ_TYPE_LEVEL_HIGH>;
           #interrupt-cells = <2>;
  ```
- **Regression & Edge-Case Analysis:**
  - Aligns schema with `include/dt-bindings/pinctrl/mt65xx.h:19-26` (`MTK_PULL_SET_RSEL_000` = 200 through `MTK_PULL_SET_RSEL_111` = 207) and aligns the example with `mt6789.dtsi:375,378`.

---

#### WP-3.3 — Add MT6789 IOMMU Port Binding Header & `mediatek,mt6789-iommu-mm` Schema (`D1.6` Part 1)

- **Target Files:**
  - `patches/03-soc-power/0010-dt-bindings-memory-mediatek-Add-MT6789-SMI-compatibl.patch` (amend to also create `include/dt-bindings/memory/mediatek,mt6789-memory-port.h` and add `mediatek,mt6789-iommu-mm` to `Documentation/devicetree/bindings/iommu/mediatek,iommu.yaml`)
- **Vendor Cross-Reference:**
  - `mp01-kernel/include/dt-bindings/memory/mt6789-larb-port.h:33-42` and `runtime_merged.dts:6868,6881,6894,6964,7064` define the exact LARB0 and LARB1 display ports:
    - LARB0 (`0x94`): port 0 = `DISP_POSTMASK0` (`0x00`), port 1 = `OVL_RDMA0_HDR` (`0x01`), port 2 = `OVL_RDMA0` (`0x02`), port 3 = `DISP_FAKE0` (`0x03`).
    - LARB1 (`0x95`): port 0 = `OVL_2L_RDMA0_HDR` (`0x20`), port 1 = `OVL_2L_RDMA0` (`0x21`), port 2 = `DISP_RDMA0` (`0x22`), port 3 = `DISP_WDMA0` (`0x23`), port 4 = `DISP_FAKE1` (`0x24`).
- **Exact Technical Change (`include/dt-bindings/memory/mediatek,mt6789-memory-port.h` in `patches/03-soc-power/0010-*.patch`):**
  ```c
  /* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
  /*
   * Copyright (c) 2022 MediaTek Inc.
   * Author: Jianjiao Zeng <jianjiao.zeng@mediatek.com>
   */
  #ifndef _DT_BINDINGS_MEMORY_MT6789_MEMORY_PORT_H_
  #define _DT_BINDINGS_MEMORY_MT6789_MEMORY_PORT_H_

  #include <dt-bindings/memory/mtk-memory-port.h>

  /* larb0 */
  #define M4U_PORT_L0_DISP_POSTMASK0		MTK_M4U_ID(0, 0)
  #define M4U_PORT_L0_OVL_RDMA0_HDR		MTK_M4U_ID(0, 1)
  #define M4U_PORT_L0_OVL_RDMA0			MTK_M4U_ID(0, 2)
  #define M4U_PORT_L0_DISP_FAKE0			MTK_M4U_ID(0, 3)

  /* larb1 */
  #define M4U_PORT_L1_OVL_2L_RDMA0_HDR		MTK_M4U_ID(1, 0)
  #define M4U_PORT_L1_OVL_2L_RDMA0		MTK_M4U_ID(1, 1)
  #define M4U_PORT_L1_DISP_RDMA0			MTK_M4U_ID(1, 2)
  #define M4U_PORT_L1_DISP_WDMA0			MTK_M4U_ID(1, 3)
  #define M4U_PORT_L1_DISP_FAKE1			MTK_M4U_ID(1, 4)

  #endif /* _DT_BINDINGS_MEMORY_MT6789_MEMORY_PORT_H_ */
  ```
- **Exact Technical Change (`Documentation/devicetree/bindings/iommu/mediatek,iommu.yaml` in `patches/03-soc-power/0010-*.patch`):**
  ```diff
             - mediatek,mt2712-m4u  # generation two
             - mediatek,mt6779-m4u  # generation two
  +          - mediatek,mt6789-iommu-mm         # generation two
             - mediatek,mt6795-m4u  # generation two
  @@ -182,6 +183,7 @@ allOf:
                 - mediatek,mt2701-m4u
                 - mediatek,mt2712-m4u
                 - mediatek,mt6795-m4u
  +              - mediatek,mt6789-iommu-mm
                 - mediatek,mt6893-iommu-mm
                 - mediatek,mt8173-m4u
  @@ -199,6 +201,7 @@ allOf:
         properties:
           compatible:
             enum:
  +            - mediatek,mt6789-iommu-mm
               - mediatek,mt6893-iommu-mm
               - mediatek,mt8186-iommu-mm
  ```
- **Regression & Edge-Case Analysis:**
  - Placing `mediatek,mt6789-memory-port.h` and the `mediatek,iommu.yaml` addition in `patches/03-soc-power/0010` ensures the header exists before `patches/07-dts/0002` includes `<dt-bindings/memory/mediatek,mt6789-memory-port.h>`, preserving bisectability.

---

#### WP-3.4 — Update UFS, MUSB, and T-PHY Bindings for MT6789 (`D1.11`, `D1.12`, `D1.13` Part 1)

- **Target Files:**
  - `patches/04-peripherals/0013-dt-bindings-mmc-mtk-sd-Add-MT6789-compatible.patch` (or a new patch `patches/04-peripherals/0013b-dt-bindings-ufs-usb-phy-Add-MT6789-compatibles.patch` immediately after `0013`)
- **Mainline Cross-Reference:**
  - `Documentation/devicetree/bindings/ufs/mediatek,ufs.yaml`:
    - `ufs_mtk_init_reset()` (`drivers/ufs/host/ufs-mediatek.c:237-249`) explicitly requests `reset-names = "hci_rst", "unipro_rst", "crypto_rst", "mphy_rst"`, and `ufs_mtk_bind_mphy()` (`:302-338`) explicitly permits platforms without a separate M-PHY DT node (`if (err == -ENODEV) err = 0;`).
    - On MT6789 (`runtime_merged.dts:6162-6175`, `mp01` `mt6789.dts:3907-3934`), `ufshci@11270000` has 7 clocks (`"ufs"`, `"ufs_mp"`, `"unipro_tick"`, `"unipro_sys"`, `"ufs_aes"`, `"ufsfde_ck"`, `"ufs_ck"`), 3 resets (`"hci_rst"`, `"unipro_rst"`, `"crypto_rst"`), and no separate `ufsphy` node.
  - `Documentation/devicetree/bindings/usb/mediatek,musb.yaml:17-24`: requires `items: [enum: [...], const: mediatek,mtk-musb]`.
  - `Documentation/devicetree/bindings/phy/mediatek,tphy.yaml:78-90`: requires `items: [enum: [...], const: mediatek,generic-tphy-v2]`.
- **Exact Technical Change (`Documentation/devicetree/bindings/ufs/mediatek,ufs.yaml`):**
  ```diff
   properties:
     compatible:
  -    enum:
  -      - mediatek,mt8183-ufshci
  -      - mediatek,mt8192-ufshci
  -      - mediatek,mt8195-ufshci
  +    oneOf:
  +      - enum:
  +          - mediatek,mt8183-ufshci
  +          - mediatek,mt8192-ufshci
  +          - mediatek,mt8195-ufshci
  +      - items:
  +          - const: mediatek,mt6789-ufshci
  +          - const: mediatek,mt8183-ufshci
   
     clocks:
       minItems: 1
       maxItems: 8
   
     clock-names:
       minItems: 1
       maxItems: 8
   
     phys:
       maxItems: 1
   
     reg:
       maxItems: 1
   
  +  resets:
  +    minItems: 1
  +    maxItems: 4
  +
  +  reset-names:
  +    minItems: 1
  +    items:
  +      - const: hci_rst
  +      - const: unipro_rst
  +      - const: crypto_rst
  +      - const: mphy_rst
  +
     vcc-supply: true
   
     mediatek,ufs-disable-mcq:
       $ref: /schemas/types.yaml#/definitions/flag
       description: The mask to disable MCQ (Multi-Circular Queue) for UFS host.
   
   required:
     - compatible
     - clocks
     - clock-names
  -  - phys
     - reg
     - vcc-supply
   
   unevaluatedProperties: false
   
   allOf:
     - $ref: ufs-common.yaml
   
     - if:
         properties:
           compatible:
             contains:
  -            enum:
  -              - mediatek,mt8195-ufshci
  +            const: mediatek,mt8195-ufshci
       then:
         properties:
           clocks:
             minItems: 8
           clock-names:
             items:
               - const: ufs
               - const: ufs_aes
               - const: ufs_tick
               - const: unipro_sysclk
               - const: unipro_tick
               - const: unipro_mp_bclk
               - const: ufs_tx_symbol
               - const: ufs_mem_sub
  +      required:
  +        - phys
       else:
  -      properties:
  -        clocks:
  -          maxItems: 1
  -        clock-names:
  -          items:
  -            - const: ufs
  +      if:
  +        properties:
  +          compatible:
  +            contains:
  +              const: mediatek,mt6789-ufshci
  +      then:
  +        properties:
  +          clocks:
  +            minItems: 7
  +            maxItems: 7
  +          clock-names:
  +            items:
  +              - const: ufs
  +              - const: ufs_mp
  +              - const: unipro_tick
  +              - const: unipro_sys
  +              - const: ufs_aes
  +              - const: ufsfde_ck
  +              - const: ufs_ck
  +      else:
  +        properties:
  +          clocks:
  +            maxItems: 1
  +          clock-names:
  +            items:
  +              - const: ufs
  +        required:
  +          - phys
  ```
- **Exact Technical Change (`Documentation/devicetree/bindings/usb/mediatek,musb.yaml`):**
  ```diff
     compatible:
       items:
         - enum:
             - mediatek,mt8516-musb
             - mediatek,mt2701-musb
  +          - mediatek,mt6789-musb
             - mediatek,mt7623-musb
         - const: mediatek,mtk-musb
  ```
- **Exact Technical Change (`Documentation/devicetree/bindings/phy/mediatek,tphy.yaml`):**
  ```diff
         - items:
             - enum:
                 - mediatek,mt2712-tphy
  +              - mediatek,mt6789-tphy
                 - mediatek,mt6893-tphy
                 - mediatek,mt7629-tphy
  ```
- **Regression & Edge-Case Analysis:**
  - Notice that in `mediatek,ufs.yaml`, `reset-names:` uses `minItems: 1` + `items:` (4 items) **without** `maxItems: 4`, satisfying `http://devicetree.org/meta-schemas/items.yaml` (`dt-doc-validate` verified 0 errors).
  - Keeping `"mediatek,mt8183-ufshci"`, `"mediatek,mtk-musb"`, and `"mediatek,generic-tphy-v2"` as the second compatible string on each node ensures `ufs-mediatek.c`, `musb_mtk.c`, and `phy-mtk-tphy.c` bind without requiring any C driver match-table changes.

---

#### WP-3.5 — Fix `mediatek,mt6789-mt6366-sound.yaml` Schema Composition (`D1.1`, `D1.2`)

- **Target Files:**
  - `patches/04-peripherals/0020-ASoC-dt-bindings-mediatek-Add-MT6789-AFE-and-MT6366-.patch` (`Documentation/devicetree/bindings/sound/mediatek,mt6789-mt6366-sound.yaml`, lines 24–28 and 61)
- **Exact Technical Change (`Documentation/devicetree/bindings/sound/mediatek,mt6789-mt6366-sound.yaml`):**
  ```diff
   properties:
     compatible:
       const: mediatek,mt6789-mt6366-sound
   
  -  audio-routing:
  -    $ref: /schemas/types.yaml#/definitions/non-unique-string-array
  -    description:
  -      List of connections between audio components (sink, source pairs).
  -    minItems: 2
  -
     mediatek,platform:
       $ref: /schemas/types.yaml#/definitions/phandle
       description: The phandle of the MT6789 ASoC AFE platform node.
  @@ -58,7 +52,7 @@ required:
     - model
     - mediatek,platform
   
  -additionalProperties: false
  +unevaluatedProperties: false
  ```
- **Regression & Edge-Case Analysis:**
  - When composing with `allOf: [{$ref: sound-card-common.yaml#}]`, `unevaluatedProperties: false` allows `model` and `audio-routing` defined in `sound-card-common.yaml#`, and dropping the local `minItems: 2` override on `audio-routing` prevents `dtschema` from inferring `maxItems: 2`. Eliminates `dt-validate` errors #26 and #27.

---

#### WP-3.6 — Fix `arm,mali-bifrost.yaml` 3-String Valhall Compatible Branch (`D1.7`)

- **Target Files:**
  - `patches/05-gpu/0001-dt-bindings-gpu-mali-bifrost-Add-MT6789-compatible.patch` (`Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml`, lines 21–45)
- **Exact Technical Change (`Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml`):**
  Replace the hunk at lines 21–25 (which erroneously added `mediatek,mt6789-mali` to the 2-string `arm,mali-bifrost` branch) with a 3-string `arm,mali-valhall-jm` branch at line 40, while keeping the `allOf` 2-power-domain (`core0`, `core1`) hunk at line 233:
  ```diff
  --- a/Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml
  +++ b/Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml
  @@ -38,6 +38,10 @@ properties:
             - rockchip,rk3576-mali
             - samsung,exynos850-mali
         - const: arm,mali-bifrost # Mali Bifrost GPU model/revision is fully discoverable
  +    - items:
  +        - const: mediatek,mt6789-mali
  +        - const: mediatek,mt8186-mali
  +        - const: arm,mali-valhall-jm
       - items:
           - enum:
               - mediatek,mt8195-mali
  @@ -231,6 +235,7 @@ allOf:
         properties:
           compatible:
             contains:
               enum:
  +              - mediatek,mt6789-mali
                 - mediatek,mt8186-mali
                 - mediatek,mt8370-mali
       then:
  ```
- **Regression & Edge-Case Analysis:**
  - Matches `gpu: gpu@13000000` in `mt6789.dtsi:610-613` (`compatible = "mediatek,mt6789-mali", "mediatek,mt8186-mali", "arm,mali-valhall-jm";`) and `patches/05-gpu/0002-drm-panfrost-Add-MT6789-Mali-G57-MC2-compatible.patch`. Eliminates `dt-validate` error #3.

---

#### WP-3.7 — Update `mt6789.dtsi` for Display Power Domains, Display IOMMU, SMI Compatibles, UFS, MUSB, and T-PHY (`D1.5`, `D1.6`, `D1.8`, `D1.11`, `D1.12`, `D1.13`)

- **Target Files (Update in Lockstep):**
  - `dts/mt6789.dtsi`
  - `patches/07-dts/0002-arm64-dts-mediatek-Add-MT6789-SoC-base-device-tree.patch`
- **Exact Technical Change (`dts/mt6789.dtsi` and `patches/07-dts/0002-*.patch`):**
  ```diff
  --- a/arch/arm64/boot/dts/mediatek/mt6789.dtsi
  +++ b/arch/arm64/boot/dts/mediatek/mt6789.dtsi
  @@ -14,6 +14,7 @@
   #include <dt-bindings/interrupt-controller/arm-gic.h>
   #include <dt-bindings/interrupt-controller/irq.h>
   #include <dt-bindings/clock/mt6789-clk.h>
  +#include <dt-bindings/memory/mediatek,mt6789-memory-port.h>
   #include <dt-bindings/reset/mt6789-resets.h>
   #include <dt-bindings/phy/phy.h>
   #include <dt-bindings/pinctrl/mt65xx.h>
  @@ -771,7 +772,7 @@
   		};
   
   		ufshci: ufshci@11270000 {
  -			compatible = "mediatek,mt8183-ufshci";
  +			compatible = "mediatek,mt6789-ufshci", "mediatek,mt8183-ufshci";
   			reg = <0x00 0x11270000 0x00 0x2300>;
   			interrupts = <GIC_SPI 137 IRQ_TYPE_LEVEL_HIGH>;
   			clocks = <&infracfg_ao CLK_IFRAO_UFS>,
  @@ -814,7 +815,7 @@
   		};
   
   		u2phy0: t-phy@11f40000 {
  -			compatible = "mediatek,generic-tphy-v2";
  +			compatible = "mediatek,mt6789-tphy", "mediatek,generic-tphy-v2";
   			#address-cells = <1>;
   			#size-cells = <1>;
   			ranges = <0x0 0x0 0x11f40000 0x1000>;
  @@ -832,7 +833,7 @@
   		};
   
   		usb0: usb@11200000 {
  -			compatible = "mediatek,mtk-musb";
  +			compatible = "mediatek,mt6789-musb", "mediatek,mtk-musb";
   			reg = <0x00 0x11200000 0x00 0x10000>;
   			interrupts = <GIC_SPI 128 IRQ_TYPE_LEVEL_HIGH>;
   			interrupt-names = "mc";
  @@ -1035,7 +1036,7 @@
   		};
   
   		smi_common: smi@14002000 {
  -			compatible = "mediatek,mt6789-smi-common", "mediatek,smi-common";
  +			compatible = "mediatek,mt6789-smi-common";
   			reg = <0x00 0x14002000 0x00 0x1000>;
   			clocks = <&mmsys CLK_MM_SMI_COMMON>,
   				 <&mmsys CLK_MM_SMI_COMMON>,
  @@ -1046,7 +1047,7 @@
   		};
   
   		larb0: smi@14003000 {
  -			compatible = "mediatek,mt6789-smi-larb", "mediatek,smi-larb";
  +			compatible = "mediatek,mt6789-smi-larb";
   			reg = <0x00 0x14003000 0x00 0x1000>;
   			clocks = <&mmsys CLK_MM_SMI_COMMON>,
   				 <&mmsys CLK_MM_SMI_COMMON>;
  @@ -1057,7 +1058,7 @@
   		};
   
   		larb1: smi@14004000 {
  -			compatible = "mediatek,mt6789-smi-larb", "mediatek,smi-larb";
  +			compatible = "mediatek,mt6789-smi-larb";
   			reg = <0x00 0x14004000 0x00 0x1000>;
   			clocks = <&mmsys CLK_MM_SMI_COMMON>,
   				 <&mmsys CLK_MM_SMI_COMMON>;
  @@ -1072,6 +1073,8 @@
   			reg = <0x00 0x14005000 0x00 0x1000>;
   			clocks = <&mmsys CLK_MM_DISP_OVL0>;
   			interrupts = <GIC_SPI 287 IRQ_TYPE_LEVEL_HIGH>;
  +			iommus = <&disp_iommu M4U_PORT_L0_OVL_RDMA0_HDR>,
  +				 <&disp_iommu M4U_PORT_L0_OVL_RDMA0>;
   			power-domains = <&spm MT6789_POWER_DOMAIN_DISP>;
   		};
   
  @@ -1080,6 +1083,7 @@
   			reg = <0x00 0x14007000 0x00 0x1000>;
   			clocks = <&mmsys CLK_MM_DISP_RDMA0>;
   			interrupts = <GIC_SPI 289 IRQ_TYPE_LEVEL_HIGH>;
  +			iommus = <&disp_iommu M4U_PORT_L1_DISP_RDMA0>;
   			power-domains = <&spm MT6789_POWER_DOMAIN_DISP>;
   		};
   
  @@ -1121,6 +1125,7 @@
   			reg = <0x00 0x1400e000 0x00 0x1000>;
   			clocks = <&mmsys CLK_MM_DISP_POSTMASK0>;
   			interrupts = <GIC_SPI 296 IRQ_TYPE_LEVEL_HIGH>;
  +			power-domains = <&spm MT6789_POWER_DOMAIN_DISP>;
   		};
   
   		dither0: dither@1400f000 {
  @@ -1128,6 +1133,7 @@
   			reg = <0x00 0x1400f000 0x00 0x1000>;
   			clocks = <&mmsys CLK_MM_DISP_DITHER0>;
   			interrupts = <GIC_SPI 297 IRQ_TYPE_LEVEL_HIGH>;
  +			power-domains = <&spm MT6789_POWER_DOMAIN_DISP>;
   		};
   
   		dsc0: dsc@14012000 {
  @@ -1135,6 +1141,19 @@
   			reg = <0x00 0x14012000 0x00 0x1000>;
   			clocks = <&mmsys CLK_MM_DISP_DSC_WRAP0>;
   			interrupts = <GIC_SPI 300 IRQ_TYPE_LEVEL_HIGH>;
  +			power-domains = <&spm MT6789_POWER_DOMAIN_DISP>;
  +		};
  +
  +		disp_iommu: iommu@14016000 {
  +			compatible = "mediatek,mt6789-iommu-mm";
  +			reg = <0x00 0x14016000 0x00 0x1000>;
  +			interrupts = <GIC_SPI 303 IRQ_TYPE_LEVEL_HIGH>;
  +			clocks = <&mmsys CLK_MM_SMI_IOMMU>;
  +			clock-names = "bclk";
  +			power-domains = <&spm MT6789_POWER_DOMAIN_DISP>;
  +			mediatek,larbs = <&larb0 &larb1>;
  +			#iommu-cells = <1>;
  +			status = "disabled";
   		};
   
   		dsi0: dsi@14013000 {
  ```
- **Regression & Edge-Case Analysis:**
  - **Critical `disp_iommu` runtime subtlety:** Why MUST `disp_iommu: iommu@14016000` have `status = "disabled";` right now?
    In `drivers/iommu/of_iommu.c:28-29`, `of_iommu_xlate()` checks:
    ```c
    if (!of_device_is_available(iommu_spec->np))
        return -ENODEV;
    ```
    When `disp_iommu` has `status = "disabled";`, `of_iommu_xlate()` immediately returns `-ENODEV` (which `of_dma_configure_id()` treats as "no IOMMU attached, use direct/CMA DMA"), so `ovl0` and `rdma0` probe immediately without waiting for an `mtk_iommu` driver, while `dt-validate` validates both `disp_iommu` and the `iommus` properties on `ovl0` and `rdma0` with 0 errors! If `status = "disabled";` were omitted before `mtk_iommu.c` has a `"mediatek,mt6789-iommu-mm"` driver entry, `iommu_fwspec_init()` (`drivers/iommu/iommu.c:3078-3079`) would return `-EPROBE_DEFER` and delay display probe until `driver_deferred_probe_timeout`.
  - Adding `power-domains = <&spm MT6789_POWER_DOMAIN_DISP>;` to `postmask0`, `dither0`, and `dsc0` attaches those three DDP components to the `disp` genpd alongside `ovl0`, `rdma0`, `color0`, `ccorr0`, `aal0`, `gamma0`, and `dsi0`, ensuring the `disp` power domain reference count reflects all active display blocks.

---

#### WP-3.8 — Rename PMIC Keys Child Nodes to `power` and `home` (`D1.10`)

- **Target Files (Update in Lockstep):**
  - `dts/mt6789-advan-x1.dts` (lines 691–700)
  - `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch`
- **Exact Technical Change (`dts/mt6789-advan-x1.dts:686-700` and `patches/07-dts/0003-*.patch`):**
  ```diff
   		mt6358keys: keys {
   			compatible = "mediatek,mt6358-keys";
   			mediatek,long-press-mode = <1>;
   			power-off-time-sec = <0>;
   
  -			powerkey {
  +			power {
   				linux,keycodes = <KEY_POWER>;
   				wakeup-source;
   			};
   
  -			homekey {
  +			home {
   				linux,keycodes = <KEY_VOLUMEUP>;
   				wakeup-source;
   			};
   		};
  ```
- **Regression & Edge-Case Analysis:**
  - In `drivers/input/keyboard/mtk-pmic-keys.c:267-315` (`mtk_pmic_keys_probe()`), child nodes of `keys` are parsed in order via `for_each_child_of_node(node, child)` using `index++` into `mtk_pmic_keys_regs[]` (index 0 = `MTK_PMIC_PWRKEY_INDEX`, index 1 = `MTK_PMIC_HOMEKEY_INDEX`), not by matching child node names. Renaming `powerkey` -> `power` and `homekey` -> `home` has zero runtime behavioral impact while eliminating `dt-validate` errors #1 and #2.

---

### Phase 4: On-Device Verification Playbook for `VERIFY-ON-DEVICE-FIRST` Items

Every hardware-dependent item (`D2.1`/`B2`, `D2.3`/`B3`, `D2.4`/`B4`, `D2.5`/`B5`, `D2.6`/`B8`, `D2.7`, `D3`/`B6`/`B7`) must be verified on the booted Advan X1 over ADB/serial before modifying any patch.

---

#### VP-4.1 — Charger I²C Address `0x6a` vs `0x6b` (`D2.1`, `B2`)

- **On-Device Shell Commands:**
  ```sh
  # 1. Check whether bq25890_charger bound to 5-006a and read its chip ID
  dmesg | grep -iE "bq25890|sc89601|charger|5-006"
  ls -la /sys/bus/i2c/devices/5-006a/ /sys/class/power_supply/
  cat /sys/class/power_supply/bq25890-charger-0/uevent 2>/dev/null || true

  # 2. Probe i2c-5 addresses 0x6a and 0x6b directly (reg 0x14 = PN/revision)
  i2cget -f -y 5 0x6a 0x14 2>/dev/null || echo "0x6a NACK"
  i2cget -f -y 5 0x6b 0x14 2>/dev/null || echo "0x6b NACK"
  ```
- **Pass Criteria vs. Fail Criteria:**
  - **PASS (Current `0x6a` is correct):** `/sys/class/power_supply/bq25890-charger-0/uevent` exists, `i2cget -f -y 5 0x6a 0x14` returns `0x64` (`(0x64 >> 3) & 0x7 == 4`, Southchip SC89601A), and `0x6b` returns `NACK`.
  - **FAIL (Hardware is at `0x6b`):** `dmesg` shows `bq25890-charger 5-006a: Cannot read chip ID` (`-ENXIO`/`-EREMOTEIO`), `0x6a` returns `NACK` (or a non-`0x64` value), and `i2cget -f -y 5 0x6b 0x14` returns `0x64`.
- **Conditional Patch Action:**
  - **If PASS:** Reclassify `D2.1` / `B2` as **`PRESERVE-AS-IS`** (`charger@6a` with `reg = <0x6a>` in `dts/mt6789-advan-x1.dts:302-304` is confirmed for this board revision).
  - **If FAIL:** Amend `dts/mt6789-advan-x1.dts:302-304` and `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch` from `charger@6a { ... reg = <0x6a>; ... }` to `charger@6b { ... reg = <0x6b>; ... }`.

---

#### VP-4.2 — MicroSD (`mmc1`) Card-Detect Polarity on GPIO 7 (`D2.3`, `B3`)

- **On-Device Shell Commands:**
  ```sh
  # 1. Check GPIO 7 physical level and logical state with SD slot EMPTY vs INSERTED
  grep -E "gpio-7 |mmc1" /sys/kernel/debug/gpio
  dmesg | grep -iE "mmc1|11240000"
  ls -la /sys/block/mmcblk* 2>/dev/null || echo "no mmcblk device"
  ```
- **Pass Criteria vs. Fail Criteria:**
  - **PASS (`GPIO_ACTIVE_LOW` is correct):**
    - With SD slot **empty**: `/sys/kernel/debug/gpio` shows `gpio-7 ( ... |cd ) in hi IRQ ACTIVE LOW` (physical HIGH pulled up by `bias-pull-up`, logical inactive = card absent) and `dmesg` does **not** show repeated failed `mmc1` init / CMD8 retries.
    - With SD card **inserted**: `/sys/kernel/debug/gpio` shows `gpio-7 ( ... |cd ) in lo IRQ ACTIVE LOW` (switch grounds pin to GND, logical active = card present) and `/sys/block/mmcblk1` (or `mmcblk0`) enumerates cleanly.
  - **FAIL (`GPIO_ACTIVE_LOW` is inverted):**
    - With SD slot **empty**: `/sys/kernel/debug/gpio` shows `gpio-7 ... in lo` (so `GPIO_ACTIVE_LOW` reports card present when empty), OR with SD card **inserted** `/sys/kernel/debug/gpio` shows `gpio-7 ... in hi` and the card is only detected after removal.
- **Conditional Patch Action:**
  - **If PASS:** Reclassify `D2.3` / `B3` as **`PRESERVE-AS-IS`** (optionally add `cd-debounce-delay-ms = <0>;` or `<200>;` if insertion bounce is observed).
  - **If FAIL:** Amend `dts/mt6789-advan-x1.dts:190` and `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch` to:
    ```dts
    	cd-debounce-delay-ms = <0>;
    	cd-gpios = <&pio 7 GPIO_ACTIVE_HIGH>;
    ```

---

#### VP-4.3 — Display Panel (`GPIO 85`) & Touch (`GPIO 107`) Reset GPIO Flags (`D2.4`, `B4`)

- **On-Device Shell Commands:**
  ```sh
  # Verify physical pin levels of GPIO 85 (panel reset) and GPIO 107 (touch reset) while display & touch are active
  grep -E "gpio-85 |gpio-107 " /sys/kernel/debug/gpio
  cat /sys/class/drm/card0-DSI-1/status /sys/class/drm/card0-DSI-1/enabled
  cat /sys/bus/spi/devices/spi0.0/fw_ver 2>/dev/null || dmesg | grep -iE "icnl9922c|drm"
  ```
- **Pass Criteria vs. Fail Criteria:**
  - **PASS:** When the panel is active (`card0-DSI-1/enabled` = `enabled`) and `chipone-icnl9922c spi0.0` is running (`fw_ver` readable or `HWID 0x99c220` + SRAM boot succeeded), `/sys/kernel/debug/gpio` reports:
    - `gpio-85 ( ... |reset ) out hi ACTIVE LOW` (logical `0` = deasserted = physical HIGH `1`)
    - `gpio-107 ( ... |reset ) out hi ACTIVE LOW` (logical `0` = deasserted = physical HIGH `1`)
  - **FAIL:** Either pin rests at physical `lo` (`out lo`) after `prepare()` / `probe()` while the corresponding peripheral fails to respond.
- **Conditional Patch Action:**
  - **If PASS:** Reclassify `D2.4` / `B4` as **`PRESERVE-AS-IS`**. (In Linux `gpiod`, an active-low reset pin *should* be declared `GPIO_ACTIVE_LOW` in DT so `gpiod_set_value_cansleep(reset_gpio, 1)` drives physical LOW to assert reset and `gpiod_set_value_cansleep(reset_gpio, 0)` drives physical HIGH to release reset; stock Android's `0x00` flag was paired with legacy `of_get_named_gpio()` raw `gpio_set_value(pin, 0/1)` calls.)
  - **If FAIL:** Invert `reset-gpios` in `dts/mt6789-advan-x1.dts:222,950` and `patches/07-dts/0003-*.patch` only if hardware uses an active-high reset transistor stage.

---

#### VP-4.4 — Backlight PWM Period (`39385` ns vs `500000` ns) & Duty Cap (`D2.5`, `B5`)

- **On-Device Shell Commands:**
  ```sh
  # 1. Inspect current PWM state and test brightness steps 1..7
  cat /sys/kernel/debug/pwm
  cat /sys/class/backlight/backlight/max_brightness /sys/class/backlight/backlight/actual_brightness
  for lvl in 1 2 3 4 5 6 7; do
      echo $lvl > /sys/class/backlight/backlight/brightness
      sleep 1
      cat /sys/kernel/debug/pwm
  done
  ```
- **Pass Criteria vs. Fail Criteria:**
  - **PASS (if 2 kHz / 100% is acceptable, or FAIL if stock 25.4 kHz / 25% cap is required):**
    - Note that in stock Android (`runtime_merged.dts:2134-2142` + `mp01` `drivers/leds/leds-mtk-pwm.c:47-65`), the hardware backlight boost converter is driven at **25.39 kHz** (`period = 39385` ns) and its duty cycle is capped at **25%** (`duty = period * brightness / 4 / max_hw_brightness`, i.e., `<= 9846` ns) so LED current never exceeds 25% full-scale boost rating! Driving `100%` duty (`level = 7`, `65535/65535`) on a boost LED driver designed for a 25% duty limit risks over-driving the LED string and causing inductor audible whine at 2 kHz.
  - **Fail Criteria:** Audible coil whine at 2 kHz, visible camera-shutter PWM banding, or brightness saturating/overheating above level 5 (`32768/65535` = 50% duty, already 2× stock max duty!).
- **Conditional Patch Action:**
  - **Recommended Action (Align to Stock Electrical Envelope):** Amend `dts/mt6789-advan-x1.dts:36-42` and `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch` so `pwms` uses the stock `39385` ns (25.39 kHz) period and `brightness-levels` scales `0..255` across a `0..1020` scale (capping maximum duty at `255 / 1020 = 25.0%`, matching `leds-mtk-pwm.c`'s `/ 4` formula using standard upstream `pwm-backlight` `num-interpolated-steps` or explicit levels):
    ```dts
    	backlight_lcd: backlight {
    		compatible = "pwm-backlight";
    		pwms = <&disp_pwm0 0 39385>;
    		brightness-levels = <0 4 8 16 32 64 128 255 1020>;
    		default-brightness-level = <6>;
    		status = "okay";
    	};
    ```
    Wait: in upstream `drivers/video/backlight/pwm_bl.c:254`, `scale = pb->scale` is ALWAYS the maximum value in `brightness-levels[]` (`pb->levels[pb->max_brightness]`), and `max_brightness` exposed to sysfs is the last index! So if `brightness-levels` has `1020` as its last entry, userspace writing `max_brightness` (`8`) would still request `1020 / 1020 = 100%` duty!
    To cap maximum duty in upstream `pwm-backlight` at 25% (`9846` ns out of `39385` ns), or if `pwm-backlight` always maps its final `brightness-levels` entry to `100%` of `period`:
    - Let's check `drivers/video/backlight/pwm_bl.c`! In `pwm_bl.c`, `compute_duty_cycle()` computes `duty_cycle = brightness * period / pb->scale` where `pb->scale = pb->levels[max_brightness]`. Thus in standard `pwm-backlight`, the highest entry in `brightness-levels` ALWAYS produces `duty_cycle == period` (100% duty)!
    - Therefore, if on-device testing shows `100%` duty overdrives the backlight boost converter, the clean upstream ways to cap duty at 25% are:
      1. Set `pwms = <&disp_pwm0 0 39385>;` in `mt6789-advan-x1.dts:38` (fixing the 25.39 kHz carrier frequency immediately), AND
      2. If the boost converter requires the `/4` hardware duty cap, add optional `max-brightness = <64>;` (supported by `pwm_bl.c` via `max-brightness` or in `pwm-mtk-disp.c` / `pwm-backlight`).

---

#### VP-4.5 — NFC Clock Buffer & NCI Initialization (`D2.6`, `B8`)

- **On-Device Shell Commands:**
  ```sh
  # Check NFC registration and trigger NCI core reset/init via raw NFC socket or i2c3 irq counter
  dmesg | grep -iE "nfc|st-nci|i2c 3-0008"
  cat /sys/class/nfc/nfc0/dev 2>/dev/null
  grep -E "nfc|gpio-1 " /proc/interrupts /sys/kernel/debug/gpio
  # Bring up nfc0 via python raw AF_NFC NCI poll or nfctool if installed:
  hciconfig 2>/dev/null || ip link 2>/dev/null
  ```
- **Pass Criteria vs. Fail Criteria:**
  - **PASS:** `nfc0` is present in `/sys/class/nfc/nfc0`, asserting `reset-gpios` (`GPIO 106`) causes ST21NFCD to fire an NCI frame on `GPIO 1` (IRQ counter in `/proc/interrupts` increments during probe/open), proving the 26 MHz XO clock buffer (`XO_NFC` on MT6358/MT6366 DCXO) is already enabled by the bootloader or strapped in hardware.
  - **FAIL:** `st-nci` times out waiting for `NCI_OP_CORE_RESET_RSP` / `NCI_OP_CORE_INIT_RSP` (`-ETIMEDOUT` in `dmesg`) and `GPIO 1` IRQ count stays at `0`.
- **Conditional Patch Action:**
  - **If PASS:** Reclassify `D2.6` / `B8` as **`PRESERVE-AS-IS`**.
  - **If FAIL:** Enable `XO_NFC` (`PMIC_DCXO_CW00` @ `0x0788`) via the MT6358 PMIC regmap during platform init or a minimal `mediatek,mt6358-clkbuf` clock provider wired to `clocks = <&pmic_clkbuf ...>` on `nfc@8`.

---

#### VP-4.6 — Memory Map & Ramoops Overlap (`memory@48090000` vs `ramoops@48090000`) (`D2.7`)

- **On-Device Shell Commands:**
  ```sh
  # Check what U-Boot actually passes in /memory nodes at runtime and whether ramoops probes cleanly
  grep -iE "48090000|ramoops|pstore|System RAM" /proc/iomem
  dmesg | grep -iE "ramoops|pstore|OF: reserved mem"
  ls -la /sys/fs/pstore/
  for f in /sys/firmware/devicetree/base/memory*/reg; do
      echo "=== $f ==="
      hexdump -C "$f"
  done
  ```
- **Pass Criteria vs. Fail Criteria:**
  - **PASS (Consolidate `memory` nodes):** `/sys/fs/pstore/console-ramoops-0` is present and populated, and `/sys/firmware/devicetree/base/memory*/reg` shows how U-Boot fixes up `/memory`.
  - Note: Defining `memory@48090000` with `reg = <0x00 0x48090000 0x00 0x000e0000>` (identical to `ramoops@48090000` with `no-map`) in static `mt6789-advan-x1.dts:83-86` is an anti-pattern because if U-Boot does *not* overwrite `/memory@48090000`, `memblock_add(0x48090000, 0xe0000)` + `memblock_mark_nomap(0x48090000, 0xe0000)` leaves an isolated 896 KiB nomap island in `memblock.memory`, whereas `ramoops` with `no-map` does not require a dedicated `memory@48090000` node (`ramoops` maps `no-map` regions via `memremap()`).
- **Conditional Patch Action:**
  - Once confirmed on device that removing `memory@48090000` preserves `/sys/fs/pstore/console-ramoops-0` across a warm reboot, amend `dts/mt6789-advan-x1.dts:83-86` and `patches/07-dts/0003-arm64-dts-mediatek-Add-Advan-X1-board-device-tree.patch` to delete the redundant `memory@48090000` node:
    ```diff
    -	memory@48090000 {
    -		device_type = "memory";
    -		reg = <0x00 0x48090000 0x00 0x000e0000>;
    -	};
    -
     	memory@61000000 {
    ```

---

#### VP-4.7 — Bootloader-Leaked Clocks & Power Domains Audit (`D3`, `B6`, `B7`)

- **On-Device Shell Commands:**
  ```sh
  # 1. Inspect post-boot clock and power-domain state (after clk_disable_unused and genpd_power_off_unused)
  cat /sys/kernel/debug/pm_genpd/pm_genpd_summary
  grep -E "enable_cnt|pwrap_ulposc|atb|dpmaif|btif|ccif|debugsys|cldma" /sys/kernel/debug/clk/clk_summary

  # 2. Check SPM PWR_STATUS (0x1000616C) directly to see which hardware MTCMOS domains remain powered on
  devmem 0x1000616c 32
  ```
- **Pass Criteria vs. Fail Criteria:**
  - **PASS for `B6` (19 vendor bring-up clocks):** System boots past `clk_disable_unused` (late initcall) with 0 hangs, PMIC wrapper (`pwrap@10026000`) and LVTS (`thermal-sensor@1100b000`) continue functioning because they explicitly claim `CLK_IFRAO_PMIC_AP`, `CLK_IFRAO_PMIC_TMR`, and `CLK_IFRAO_THERM` in `mt6789.dtsi:557-559,590`, and the unclaimed modem/debug clocks (`CCIF*`, `CLDMA_BCLK`, `DPMAIF_MAIN`, `BTIF`, `DEBUGSYS`, `ATB`) are safely gated (saving power because modem/connsys are not active).
  - **FAIL for `B6`:** If PMIC wrapper transactions fail after `clk_disable_unused` gates `CLK_TOP_PWRAP_ULPOSC_SEL` (the ULPOSC clock feeding the PMIC wrapper's internal SPI state machine!), `pwrap` reads/writes will time out after late init!
  - **Check for `B7` (leaked power domains via `devmem 0x1000616c 32`):**
    Check bits in `PWR_STATUS` (`0x1000616c`):
    - `BIT(0)` = `MD`, `BIT(1)` = `CONN`, `BIT(2..5)` = `MFG0..3`, `BIT(13)` = `ISP`, `BIT(15)` = `IPE`, `BIT(16)` = `VDEC`, `BIT(18)` = `VENC`, `BIT(21)` = `DISP`, `BIT(22)` = `AUDIO`, `BIT(23..25)` = `CAM/RAWA/RAWB`.
    - If `BIT(13)`, `BIT(15)`, `BIT(16)`, `BIT(18)`, or `BIT(23..25)` are `1` in `0x1000616c`, the bootloader left camera/video MTCMOS domains powered on, burning idle power.
- **Conditional Patch Action:**
  - **If `CLK_TOP_PWRAP_ULPOSC_SEL` is required by `pwrap@10026000`:** Wire `<&topckgen CLK_TOP_PWRAP_ULPOSC>` (or mark `CLK_TOP_PWRAP_ULPOSC_SEL` `CLK_IS_CRITICAL` in `clk-mt6789-topckgen.c` inside `patches/01-clk/0002-clk-mediatek-Add-MT6789-core-clock-drivers.patch`), and leave unused modem/CCIF clocks gated.
  - **If `0x1000616c` shows leaked camera/ISP/video power domains:** Execute Phase 5 (`WP-5.1`) to populate all 15 power domains in `mt6789-pm-domains.h` and `mt6789.dtsi` so Linux `pm_genpd_poweroff_unused()` cleanly powers them off at `late_initcall`.

---

### Phase 5: Deferred Upstreaming Polish (`P5`, `P6`)

**Trigger Condition to Execute Phase 5:**
1. Any DT consumer of `MD`, `CONN`, `ISP`, `IPE`, `VDEC`, `VENC`, `CAM`, `CAM_RAWA`, or `CAM_RAWB` is enabled in `mt6789.dtsi`, **OR**
2. `VP-4.7` (`devmem 0x1000616c 32`) shows the bootloader leaves any of those domains powered on at boot and `pm_genpd_poweroff_unused` is needed to shut them down, **OR**
3. The patch series is being prepared for upstream LKML submission.

#### WP-5.1 — Populate Complete MT6789 SCPSYS Power Domain Table & Rename `MT6789_TOP_AXI_PROT_EN_*` Macros (`P5`, `P6`)

- **Target Files:**
  - `patches/03-soc-power/0002-pmdomain-mediatek-Add-MT6789-power-domains-support.patch` (`drivers/pmdomain/mediatek/mt6789-pm-domains.h` and `include/linux/soc/mediatek/infracfg.h`)
- **Vendor Cross-Reference:**
  - `mp01-kernel/drivers/soc/mediatek/mtk-scpsys-mt6789.c:30-240` and `mp01-kernel/drivers/soc/mediatek/scpsys.h:44-66`.
- **Exact Technical Specification (`drivers/pmdomain/mediatek/mt6789-pm-domains.h`):**
  Define all `MT6789_TOP_AXI_PROT_EN_*` register offsets and bitmasks explicitly (eliminating the `MT8192_TOP_AXI_PROT_EN_*` macro dependency for `P6`) and populate all 15 domains (`MT6789_POWER_DOMAIN_MD` = 0 through `MT6789_POWER_DOMAIN_CAM_RAWB` = 14) for `P5`:
  ```c
  /* INFRACFG bus protection register offsets for MT6789 */
  #define MT6789_TOP_AXI_PROT_EN_SET			0x2a0
  #define MT6789_TOP_AXI_PROT_EN_CLR			0x2a4
  #define MT6789_TOP_AXI_PROT_EN_STA1			0x228
  #define MT6789_TOP_AXI_PROT_EN_1_SET			0x2a8
  #define MT6789_TOP_AXI_PROT_EN_1_CLR			0x2ac
  #define MT6789_TOP_AXI_PROT_EN_1_STA1			0x258
  #define MT6789_TOP_AXI_PROT_EN_MM_SET			0x2d4
  #define MT6789_TOP_AXI_PROT_EN_MM_CLR			0x2d8
  #define MT6789_TOP_AXI_PROT_EN_MM_STA1			0x2ec
  #define MT6789_TOP_AXI_PROT_EN_2_SET			0x714
  #define MT6789_TOP_AXI_PROT_EN_2_CLR			0x718
  #define MT6789_TOP_AXI_PROT_EN_2_STA1			0x724
  #define MT6789_TOP_AXI_PROT_EN_VDNR_SET			0xb84
  #define MT6789_TOP_AXI_PROT_EN_VDNR_CLR			0xb88
  #define MT6789_TOP_AXI_PROT_EN_VDNR_STA1		0xb90
  #define MT6789_TOP_AXI_PROT_EN_MM_2_SET			0xdcc
  #define MT6789_TOP_AXI_PROT_EN_MM_2_CLR			0xdd0
  #define MT6789_TOP_AXI_PROT_EN_MM_2_STA1		0xdd8

  /* INFRACFG bus protection bitmasks for MT6789 (mp01 scpsys.h:44-66) */
  #define MT6789_TOP_AXI_PROT_EN_MD			BIT(7)
  #define MT6789_TOP_AXI_PROT_EN_INFRA_VDNR_MD		(BIT(0) | BIT(10) | BIT(15))
  #define MT6789_TOP_AXI_PROT_EN_CONN			(BIT(13) | BIT(18))
  #define MT6789_TOP_AXI_PROT_EN_CONN_2ND			BIT(14)
  #define MT6789_TOP_AXI_PROT_EN_1_CONN			BIT(10)
  #define MT6789_TOP_AXI_PROT_EN_1_MFG1			BIT(21)
  #define MT6789_TOP_AXI_PROT_EN_2_MFG1			(BIT(5) | BIT(6))
  #define MT6789_TOP_AXI_PROT_EN_MFG1			(BIT(21) | BIT(22))
  #define MT6789_TOP_AXI_PROT_EN_2_MFG1_2ND		BIT(7)
  #define MT6789_TOP_AXI_PROT_EN_MM_2_ISP			BIT(8)
  #define MT6789_TOP_AXI_PROT_EN_MM_2_ISP_2ND		BIT(9)
  #define MT6789_TOP_AXI_PROT_EN_MM_IPE			BIT(16)
  #define MT6789_TOP_AXI_PROT_EN_MM_IPE_2ND		BIT(17)
  #define MT6789_TOP_AXI_PROT_EN_MM_VDEC			BIT(24)
  #define MT6789_TOP_AXI_PROT_EN_MM_VDEC_2ND		BIT(25)
  #define MT6789_TOP_AXI_PROT_EN_MM_VENC			BIT(26)
  #define MT6789_TOP_AXI_PROT_EN_MM_VENC_2ND		BIT(27)
  #define MT6789_TOP_AXI_PROT_EN_MM_DISP			(BIT(10) | BIT(12))
  #define MT6789_TOP_AXI_PROT_EN_DISP			(BIT(6) | BIT(23))
  #define MT6789_TOP_AXI_PROT_EN_2_AUDIO			BIT(4)
  #define MT6789_TOP_AXI_PROT_EN_MM_CAM			(BIT(0) | BIT(2))
  #define MT6789_TOP_AXI_PROT_EN_MM_CAM_2ND		(BIT(1) | BIT(3))
  ```
  And the remaining domain entries from `mp01` `mtk-scpsys-mt6789.c:30-222`:
  - `[MT6789_POWER_DOMAIN_MD]`: `.name = "md"`, `.sta_mask = BIT(0)`, `.ctl_offs = 0x300`, `.pwr_sta_offs = 0x16c`, `.pwr_sta2nd_offs = 0x170`, `.ext_buck_iso_offs = 0x398`, `.ext_buck_iso_mask = 0x3`, `.caps = MTK_SCPD_MODEM_PWRSEQ | MTK_SCPD_KEEP_DEFAULT_OFF`, bus prot `EN_MD` + `EN_INFRA_VDNR_MD`.
  - `[MT6789_POWER_DOMAIN_CONN]`: `.name = "conn"`, `.sta_mask = BIT(1)`, `.ctl_offs = 0x304`, `.pwr_sta_offs = 0x16c`, `.pwr_sta2nd_offs = 0x170`, `.caps = MTK_SCPD_KEEP_DEFAULT_OFF`, bus prot `EN_CONN` + `EN_CONN_2ND` + `EN_1_CONN`.
  - `[MT6789_POWER_DOMAIN_ISP]`: `.name = "isp"`, `.sta_mask = BIT(13)`, `.ctl_offs = 0x334`, `.pwr_sta_offs = 0x16c`, `.pwr_sta2nd_offs = 0x170`, `.sram_pdn_bits = BIT(8)`, `.sram_pdn_ack_bits = BIT(12)`, bus prot `EN_MM_2_ISP` + `EN_MM_2_ISP_2ND` (parent in DT: `DISP`).
  - `[MT6789_POWER_DOMAIN_IPE]`: `.name = "ipe"`, `.sta_mask = BIT(15)`, `.ctl_offs = 0x33c`, `.pwr_sta_offs = 0x16c`, `.pwr_sta2nd_offs = 0x170`, `.sram_pdn_bits = BIT(8)`, `.sram_pdn_ack_bits = BIT(12)`, bus prot `EN_MM_IPE` + `EN_MM_IPE_2ND` (parent in DT: `DISP`).
  - `[MT6789_POWER_DOMAIN_VDEC]`: `.name = "vdec"`, `.sta_mask = BIT(16)`, `.ctl_offs = 0x340`, `.pwr_sta_offs = 0x16c`, `.pwr_sta2nd_offs = 0x170`, `.sram_pdn_bits = BIT(8)`, `.sram_pdn_ack_bits = BIT(12)`, bus prot `EN_MM_VDEC` + `EN_MM_VDEC_2ND` (parent in DT: `DISP`).
  - `[MT6789_POWER_DOMAIN_VENC]`: `.name = "venc"`, `.sta_mask = BIT(18)`, `.ctl_offs = 0x348`, `.pwr_sta_offs = 0x16c`, `.pwr_sta2nd_offs = 0x170`, `.sram_pdn_bits = BIT(8)`, `.sram_pdn_ack_bits = BIT(12)`, bus prot `EN_MM_VENC` + `EN_MM_VENC_2ND` (parent in DT: `DISP`).
  - `[MT6789_POWER_DOMAIN_CAM]`: `.name = "cam"`, `.sta_mask = BIT(23)`, `.ctl_offs = 0x35c`, `.pwr_sta_offs = 0x16c`, `.pwr_sta2nd_offs = 0x170`, `.sram_pdn_bits = BIT(8)`, `.sram_pdn_ack_bits = BIT(12)`, bus prot `EN_MM_CAM` + `EN_MM_CAM_2ND` (parent in DT: `DISP`).
  - `[MT6789_POWER_DOMAIN_CAM_RAWA]`: `.name = "cam_rawa"`, `.sta_mask = BIT(24)`, `.ctl_offs = 0x360`, `.pwr_sta_offs = 0x16c`, `.pwr_sta2nd_offs = 0x170`, `.sram_pdn_bits = BIT(8)`, `.sram_pdn_ack_bits = BIT(12)` (parent in DT: `CAM`).
  - `[MT6789_POWER_DOMAIN_CAM_RAWB]`: `.name = "cam_rawb"`, `.sta_mask = BIT(25)`, `.ctl_offs = 0x364`, `.pwr_sta_offs = 0x16c`, `.pwr_sta2nd_offs = 0x170`, `.sram_pdn_bits = BIT(8)`, `.sram_pdn_ack_bits = BIT(12)` (parent in DT: `CAM`).
- **Regression & Edge-Case Analysis:**
  - In mainline `mtk-pm-domains.c`, subdomain hierarchy (`DISP -> ISP/IPE/VDEC/VENC/CAM` and `CAM -> CAM_RAWA/CAM_RAWB`, per `mp01` `scp_subdomain_mt6789[]` at `mtk-scpsys-mt6789.c:224-235`) is represented by nesting `power-domain@N` DT nodes inside their parent `power-domain@M` node in `mt6789.dtsi` (just as `mfg0 -> mfg1 -> mfg2/mfg3` is nested at `mt6789.dtsi:288-313`). Do NOT add child `power-domain@N` DT nodes for `ISP/IPE/VDEC/VENC/CAM` in `mt6789.dtsi` until their subsystem clock drivers (`clk-mt6789-img`, `clk-mt6789-ipe`, `clk-mt6789-vdec`, `clk-mt6789-venc`, `clk-mt6789-cam`) are registered, because `scpsys_add_one_domain()` requires each domain's `subsys-*` clocks to resolve at probe time.

---

### Build-Time Validation & Post-Flash Runtime Smoke Test Commands (Phases 1–3)

After applying Phases 0–3, run the following commands to validate before flashing:

```bash
# 1. Verify clean patch series application, zero DTS drift, dtc, and 0 dt-validate errors
work ./scripts/verify_patches.sh

# 2. Run full kernel + DTB + U-Boot FIT build against pinned BASE (666539eecdf4db623f5e3c3e55cf9f2dfad24aa4)
work ./scripts/build.sh
```

After flashing `build/uboot/advan-x1.itb` and booting the Advan X1:

```sh
# Phase 1 smoke test: PMIC wrapper IRQs (P1), CW2217 profile & battery telemetry (D2.2/B1), DRM/DSC (P7)
grep -E "pwrap|pmic" /proc/interrupts
cat /sys/class/power_supply/cw2217-battery/uevent
cat /sys/class/drm/card0-DSI-1/status /sys/class/drm/card0-DSI-1/modes

# Phase 2 smoke test: LVTS 13 thermal zones (P2) & ALSA MT6789-MT6366 audio card (P3, P4)
for z in /sys/class/thermal/thermal_zone*; do
    printf "%-20s : %s mC\n" "$(cat $z/type)" "$(cat $z/temp)"
done
cat /proc/asound/cards /proc/asound/pcm
dmesg | grep -iE "afe|mt6789-mt6366|lvts|cw2217|pwrap" | grep -iE "err|fail|warn|timeout|busy" || echo "0 errors in dmesg"
```

---

## 4. Protected Quirks Checklist (Do NOT Revert During Cleanup)

Every item below is a hardware-verified fix or intentional mainline adaptation (`PRESERVE-AS-IS`). Any patch amendment in Phases 0–5 MUST leave these invariants intact:

- [ ] **1. LVTS eFuse Physical MMIO Offset (`reg = <0x1a4 0x34>`) (`B11`, `D1.9`, `S7.4`):**
  `lvts_e_data: lvts-calib@1a4` in `dts/mt6789.dtsi:811-813` MUST stay at `<0x1a4 0x34>` (single cell `"lvts-calib-data-1"`). Stock `0x1b4/0x2f8` are logical `atag,devinfo` array indices, not physical `0x11c10000` eFuse MMIO offsets; changing `0x1a4` back to `0x1b4` shifts calibration by +16 bytes and causes instant `>250 °C` critical thermal shutdowns.
- [ ] **2. LVTS V5 Manual-RCK Calibration (`lvts_ctrl_read_count_rc_n`) (`B11`, `S7.4`):**
  MT6789 V5 LVTS silicon has an Auto-RCK (`0x0E = 0x01`) glitch on `MT6789_BIG_CPU2` (`cpu6`, controller 1 sensor 2) that returns raw `LVTS_EDATA02` (`-261.4 °C`). `lvts_ctrl_read_count_rc_n()` (`LVTS_CMD_MANUAL_RCK_V5 = 0xC1030E00`, `LVTS_DEF_COUNT_RC = 2750`, `LVTS_COUNT_RC_DELTA_MAX = 1061`) in `patches/04-peripherals/0009-thermal-drivers-mediatek-lvts-Add-MT6789-support.patch` MUST be preserved untouched when editing `patches/04-peripherals/0008`.
- [ ] **3. Display Bootloader Handover, OVL0 `bgclr_in_off`, Shadow Bypass, and RDMA0 48992-Byte FIFO (`B10`, `S7.10`):**
  In `patches/06-display/0002`, `0004`, `0005`:
  - `mtk_ddp_comp_bgclr_in_off(comp)` on `i == 0` in `mtk_crtc_ddp_hw_init()` and clearing `OVL0_2L_GO_BG` in `mtk-mmsys` (prevents OVL0 stalling when `clk_disable_unused` gates bootloader-chained `mm_disp_ovl0_2l`).
  - `DISP_OVL_BYPASS_SHADOW = BIT(22)` on OVL0 and `RDMA_BYPASS_SHADOW | RDMA_READ_WORK_REG` (`0x6`) + `fifo_size = 48992` + `zero_fifo_threshold = true` + bootloader active-engine soft-reset on RDMA0.
  - `mtk_dsi_stop_bootloader_vdo()`, `DSI_SHADOW_DEBUG = BYPASS_SHADOW | READ_WORKING`, `need_sleepout_ulps = true`, `mt6789_dphy_timing = true` (`timcon0 = 0x090f0808`, `timcon1 = 0x0f2d0d24` at 852 MHz), `poll_for_idle = true`, `DSC_OBUF_SW_VAL = 0x800002d9`, and MIPI-TX PLL handover (`phy-mtk-mipi-dsi-mt8183.c`).
- [ ] **4. Panel Init Stream, DSC 1.1 PPS Table, and TPS65132 Bias Regulators (`B10`, `S7.7`):**
  In `patches/06-display/0007-drm-panel-Add-Chipone-ICNL9922C-MIPI-DSI-panel-drive.patch` and `dts/mt6789-advan-x1.dts:326-345,944-961`:
  - Byte-exact vendor init sequence, DSC 1.1 PPS parameters, 60/90/120 Hz timings, `hs_rate = 852000000`, `ldo18-gpios` (`GPIO 150`), and `vsp-supply`/`vsn-supply` (`tps65132` on `i2c6@3e` with `GPIO 108`/`109` `GPIO_ACTIVE_HIGH`).
- [ ] **5. Chipone ICNL9922C SPI TDDI Firmware & `drm_panel_follower` Sync (`B9`, `S7.8`):**
  In `patches/04-peripherals/0019-Input-add-Chipone-ICNL9922C-SPI-TDDI-touchscreen-dri.patch` and `initramfs/lib/firmware/chipone/icnl9922c.bin`:
  - 71,792-byte firmware upload over 1000-byte DRW SPI chunks (within the 1024-byte MTK SPI DMA FIFO limit), SFCTRL hardware CRC32 verification, dedicated touch reset `GPIO 107`, and `drm_panel_follower` synchronization after panel reset `GPIO 85`.
- [ ] **6. Pinctrl 8th `"eint"` Register Region & `DRV_GRP4` Drive-Strength Table (`S7.6`):**
  In `dts/mt6789.dtsi:355-379` and `patches/02-pinctrl/0002-pinctrl-mediatek-Add-MT6789-pinctrl-driver.patch`:
  - 8th `reg` entry `<0x00 0x1000b000 0x00 0x1000>` (`reg-names = ..., "eint"`) required by mainline `pinctrl-mtk-common-v2.c:388`, and `.drv_n = DRV_GRP4` (2..16 mA) on `MTK_PIN_NODRV`.
- [ ] **7. Bus, PMIC Key, Charger ID, Fuel-Gauge Shunt, and AFE Adaptations (`B12`, `S7.5`, `S7.11`):**
  - I²C `mt8188_compat` (`dma_sync = 0`, fixing `tps65132 -ETIMEDOUT`) in `patches/04-peripherals/0004`.
  - SPI `mt6765_compat` in `patches/04-peripherals/0002`.
  - MT6358 long-press reset register bitmask (`GENMASK(13, 12)`, `BIT(9)`, `BIT(8)`) in `patches/04-peripherals/0012`.
  - Southchip SC89601A chip ID `4` (`REG_FIELD(0x14, 3, 5)`) in `patches/04-peripherals/0015` and CW2217 `shunt-resistor-micro-ohms = <5000>` in `dts/mt6789-advan-x1.dts:241`.
  - AFE `AFE_AGENT_ON (0x1b00)` memory-interface enable register, `MT6789_POWER_DOMAIN_AUDIO`, `CLK_TOP_TCK_26M_MX9`, and optional `audiosys` reset in `patches/04-peripherals/0021` and `dts/mt6789.dtsi:698-757`.
