# MediaTek MT6789 CCCI EL3 SiP ABI & Bootloader Handover Contract

This document records the EL3 Trusted Firmware-A (BL31) SiP SMC ABI and
Loki / Little Kernel (LK) bootloader handover contract relied upon by
`drivers/net/wwan/mtk_ccci/` on MediaTek MT6789 (Helio G99 / ADVAN X1).

---

## 1. Subsystem Placement & Upstream Boundary (`M2b`)

- **Current location:** `drivers/net/wwan/mtk_ccci/` (`CONFIG_MTK_CCCI_MT6789`).
- **Rationale:** Phase 5.3+ bring-up (`docs/ccci-bringup-plan.md`) builds the
  WWAN control port (AT/MBIM/QMI over CCIF0) and DPMAIF IP data plane directly
  on top of this core. Keeping the driver under `drivers/net/wwan/mtk_ccci/`
  avoids churning file paths mid-bring-up.
- **Upstream submission boundary:** If upstreamed to LKML prior to WWAN netdev
  and DPMAIF data-plane completion, split the SoC modem power/boot + CCIF0
  ringbuffer transport under `drivers/soc/mediatek/` (or `drivers/remoteproc/`)
  and keep `drivers/net/wwan/` strictly for the WWAN device / DPMAIF netdev
  layer.

---

## 2. EL3 SiP SMC Interface (`MTK_SIP_KERNEL_CCCI_CONTROL`)

All modem boot and power-status SMCs use the standard upstream MediaTek SiP
service ID from `<linux/soc/mediatek/mtk_sip_svc.h>`:

- **Function ID (SMC64 / Fast):** `MTK_SIP_KERNEL_CCCI_CONTROL` (`0xC2000505`)
- **Calling convention:** `arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL, req_id, sub_op, arg, 0, 0, 0, 0, &res)`

### 2.1 Request IDs (`x1 = req_id`)

| `req_id` | Macro | `sub_op` (`x2`) | `arg` (`x3`) | Purpose |
|---|---|---|---|---|
| `5` | `MTK_CCCI_SMC_CLOCK_REQ` | clock op | — | Reserved EL3 modem clock request (stock ABI). |
| `6` | `MTK_CCCI_SMC_POWER_CONFIG` | `0` (`MTK_CCCI_PWR_KERNEL_BOOT_UP`) | `0` | Release MD1 MCU from reset and latch `MD_BOOT_VECTOR`. Returns `res.a0` status, `res.a1`/`a2` boot status words, `res.a3` slave flag. |
| `6` | `MTK_CCCI_SMC_POWER_CONFIG` | `2` (`MTK_CCCI_PWR_CHECK_FLAG`) | `0` | Query BL31 MD verification gate flag (`md1_verified`). |
| `6` | `MTK_CCCI_SMC_POWER_CONFIG` | `3` (`MTK_CCCI_PWR_CHECK_DONE`) | `0` | Notify BL31 that LK/kernel image check completed. |
| `6` | `MTK_CCCI_SMC_POWER_CONFIG` | `4` (`MTK_CCCI_PWR_BOOT_STATUS`) | `0` | Read MD1 BROM/firmware boot progress (`res.a1` = `status0`, `res.a2` = `status1`; e.g., `0x5443000c / 0x53320000`). |
| `7` | `MTK_CCCI_SMC_FLIGHT_MODE_SET` | `0` (normal) / `1` (flight) | `0` | Configure EL3 flight-mode state before MD boot. |
| `13` | `MTK_CCCI_SMC_SCP_CLK_SET_DONE` | `0` | `0` | Notify EL3 that SCP/clock setup completed (stock ABI). |

### 2.2 Return Value Contract (`res.a0`)

`mtk_ccci_smc()` inspects `(long)res->a0` and never silently treats error codes
as valid register values:

| `res.a0` (signed) | Meaning | Driver Behavior |
|---|---|---|
| `0` | Success (`SMC_OK`) | Proceed; consume `res.a1`–`res.a3`. |
| `-1` (`SMCCC_RET_NOT_SUPPORTED`) | Unsupported SMC / sub-op | Return `-EOPNOTSUPP` and log error. |
| `-15` (`0xfffffffffffffff1`) | BL31 MD security/verification gate failure | Return `-EIO`, abort boot, never interpret `a1`/`a2` as `boot_status`. |
| Any other `< 0` | Vendor BL31 error | Return `-EIO` and log signed code. |

---

## 3. Bootloader (LK / Loki) Handover Contract

On MT6789 retail devices booting mainline via Loki chainload after Stock LK:

1. **LK `ccci_tag_mem` (`0xbf7f0000`):** Stock LK populates the 27-entry tag
   block including `md1_chk` (`magic = "CHECK_HEADER"`, `size = 512`) and
   `md1_verified = 1`.
2. **EMI MPU & Bank Remap (Loki EL1):** Before handing off to Linux, Loki
   preserves Stock LK's EMI MPU permissions so AP can read `md1rom`
   (`0xdc000000`) and `md1dsp` (`0xe3200000`). On boot, `mtk_ccci_core` reads
   the first 4 words of `md1rom` and emits a `dev_warn()` if the read returns
   all-`0` or all-`0xffffffff` (indicating an EMI MPU lock).
3. **`md-mem0` Sizing:** `ccci->md_img_size` is derived from the `md-mem0` DT
   `memory-region` carveout (`0x013f0000` on ADVAN X1) rather than exceeding the
   carveout with a hardcoded default.
4. **MD MTCMOS Power-Cycle Guard:** Before cycling MD MTCMOS OFF → ON via
   `genpd`, `mtk_ccci_core` checks `APCCIF_RCHNUM` and skips the forced OFF
   transition if CCIF0 already shows active modem traffic, preventing an
   in-flight `TOP_AXI_PROT_EN_MD` bus hang.
5. **Regulators:** `vmodem` (`825 mV`) and `vsram` (`vsram-core`, `825 mV`)
   voltage programming checks `regulator_set_voltage()` return codes prior to
   enabling rails.
6. **SCPSYS `srclken-o1` & `power-flow-config` (`B3-08`):** Verified against
   Advan stock `runtime_merged.dts:4686-4687` (`mediatek,power_flow_config =
   <0x12>`, `mediatek,srclken_o1 = <0x200000>`) and `mp01-kernel`
   `md_sys1_platform.c:854-922, 1698` (`BIT(1) = SRCLKEN_O1_BIT`, `BIT(4) =
   MD_PLL_SETTING`, `BIT(21)` in `SPM_POWER_ON_VAL0`). Mainline DT exposes
   these with standard hyphenated names (`mediatek,srclken-o1`,
   `mediatek,power-flow-config`).
7. **CCIF0 SRAM Tail Footer (`B3-10` — `0x7274626e` / `"rtbn"`):** Before
   releasing MD1 from reset, `mtk_ccif_hw_reset()` clears AP+MD CCIF0 SRAM
   and installs the 12-byte Gen95 bootup trace / `MDSS_DBG` footer at
   `APCCIF_CHDATA + MTK_CCIF_SRAM_SIZE - 12`: magic `0x7274626e` (`"rtbn"`),
   MD-view `MDSS_DBG` base (`MTK_MD_NC_SMEM_MD_BASE + NC_OFF_MDSS_DBG`), and
   `NC_SZ_MDSS_DBG` (matching stock `ccci_md_hif_start(1) ->
   ccci_reset_ccif_hw`, binary-derived from `ccci_md_all.ko` and verified on
   hardware through MD BootROM pass (`boot_status=0x5443000c / 0x53320000`;
   HS1 handshake WIP)).
