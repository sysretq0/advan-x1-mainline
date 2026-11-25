# Phase 5: Clean Mainline CCCI Modem Bring-up Plan — ADVAN X1 (MT6789 / Helio G99)

**Status:** Phase 5.0–5.5 COMPLETE & HARDWARE-VERIFIED (MD HS2 READY, `/dev/wwan0at0` AT, DPMAIF v1 + SMEM UL `wwan0` 4G LTE IP data, Native ModemManager 1.24.2 + NetworkManager auto-connect)  
**Companion Docs:** [`docs/ccci-sip-abi.md`](ccci-sip-abi.md) (EL3 SiP SMC & Loki/LK handover contract) · [`docs/hardware-status.md`](hardware-status.md) (overall device status)  
**Target Kernel:** Pristine upstream Linux (`drivers/net/wwan/wwan_core.c` + `t7xx`-style DPMAIF NAPI rings; modular patch series in `advan-x1-mainline`)  
**Reference Cross-Checks:**
- Advan X1 stock firmware & dumps: `/work/firmware/advan_x1/` (`dtb/runtime_merged.dts`, `ondevice_dumps/pl_lk.txt`, `tee.img` BL31, `scp.img` RV55, `lk.img`, `md1img.img`)
- Advan X1 live Android 14 oracle (`INOI_A750_ID_U_V7_20260822_user`, kernel `5.10.233`): `/work/mainline/ref/advan-x1-oracle/` (`modules/ccci_*.ko`, `bin/ccci_fsd`, `bin/ccci_mdinit`, `bin/libmtk-ril.so`, `bl31.bin`, `scp_rv55.bin`, `ccci_dump.log`, `dmesg_ccci.log`, `radio.log`)
- MT6789 Android 5.10 reference source: `/work/android-kernel/mp01-kernel/drivers/misc/mediatek/eccci/`
- MT6789 Linux 7.3.3 reference repo: [`zinwa-q25-linux`](https://github.com/wolf109909/zinwa-q25-linux) (`/work/mainline/ref/zinwa-q25-linux`)
- Upstream Linux WWAN/DPMAIF reference: `/work/mainline/linux/drivers/net/wwan/wwan_core.c` and `/work/mainline/linux/drivers/net/wwan/t7xx/`

---

## 0. Clean Mainline Architecture & Hard Safety Invariants

1. **Reuse ONLY** the stock `md1img` firmware blob (already pre-loaded into DRAM by stock LK) and **read-only** stock ext4 NVRAM files.
2. **Do NOT** wrap Android userspace daemons (`ccci_mdinit`, `ccci_fsd`, `ccci_rpcd`, `libprop_preload`) and **do NOT** port the monolithic out-of-tree MediaTek `eccci` + `ccmni` stack (unlike `zinwa-q25-linux`, which ports `drivers/mediatek/eccci` + `ccmni` + runs Android `ccci_mdinit`/`ccci_rpcd` from stock `super`).
3. **Upstream Subsystem Targets:**
   - Control/AT/FS/System plane: clean platform driver `mtk-ccci-ccif` (CCIF0 + CCISM ringbufs) exposing `WWAN_PORT_AT` (and internal FS/control channels) through upstream `drivers/net/wwan/wwan_core.c`.
   - Data plane: clean `mtk-dpmaif` driver modeled on upstream `drivers/net/wwan/t7xx/` NAPI/BAT/PIT patterns, adapted for MT6789 DPMAIF v1 (12-byte PIT, 1 RXQ, single GIC SPI IRQ, and MT6789's `SMEM_USER_MD_DATA` SW uplink ring), exposing standard `wwan0` (and secondary PDN `wwanX`) via `wwan_register_ops()`.
   - SCP dependency eliminated: lightweight in-kernel (or userspace) **Emulated SCP CCISM Ring** on `SMEM_USER_CCISM_SCP` (`0x8e810000`, 32 KiB) + `AP_CCIF2` (`0x1023c000`) — no `mtk-scp` remoteproc required.
4. **NVRAM SAFETY RULE (NON-NEGOTIABLE):**
   - **Never** mount or expose live NVRAM block devices (`nvdata` `/dev/block/sdc7`, `nvcfg` `/dev/block/sdc6`, `protect1` `/dev/block/sdc16`, `protect2` `/dev/block/sdc17`, `nvram` `/dev/block/sdc34`) read-write.
   - Always mount stock ext4 partitions strictly `ro,nosuid,nodev,noexec,noload` (or `ro,norecovery`) as lowerdir and layer a **volatile `tmpfs` upperdir (`overlayfs`)** so modem writes (`FS_CCCI_OP_WRITE`, `FS_CCCI_OP_CREATEDIR`, `FS_CCCI_OP_DELETE`, `FS_CCCI_OP_RENAME`) succeed in RAM without ever mutating flash.
5. **Proven 4-Step MT6789 LTE Registration Sequence:**
   1. Serve NVRAM FS RPC reads directly from the read-only ext4 + volatile tmpfs overlay (`nvdata/md`, `protect_f/md`, `protect_s/md`).
   2. Complete Emulated SCP CCISM Ring handshake (`SCP_CLK_SET_DONE` SMC → `CCISM_SHM_INIT` → `CCISM_SHM_INIT_ACK` → init Q0 ring at `0x8e810000` → `CCISM_SHM_INIT_DONE`) so MD stays alive >120s with zero asserts.
   3. Run stock RIL cold-boot + initial EPS bearer APN AT sequence (`+CFUN=4` → `+EAPNLOCK`/`+EAPNSET`/`+ECNCFG`/`+EIAAPN`/`+EIAREG` → `+CFUN=1`).
   4. **CRITICAL:** AP must enable DPMAIF (`PIT`/`BAT` DL + `MD_DATA` SW UL) **before/during LTE attach** — on LTE, attach simultaneously establishes the default EPS data bearer; without AP DPMAIF active, the network accepts attach and immediately drops it.

---

## 1. Ground-Truth Discovery Matrix (All 10 `[DISCOVER]` Items)

### 1.1 Power & Clocks (`WP-5.1`)

| Parameter | Advan X1 Verified Value | Local Ground-Truth Source | Matches `zinwa-q25-linux`? |
| :--- | :--- | :--- | :--- |
| `MT6789_POWER_DOMAIN_MD` ID | `0` (`sta_mask = BIT(0)`, `ctl_offs = 0x300`, `pwr_sta_offs = 0x16c`, `pwr_sta2nd_offs = 0x170`) | `runtime_merged.dts:4690` (`power-domains = <0x2d 0x00>`), `mp01-kernel/drivers/soc/mediatek/mtk-scpsys-mt6789.c:31-44`, `patches/03-soc-power/0002-pmdomain-mediatek-Add-MT6789-power-domains-support.patch:88-110` | **Yes** (`patches/mt6789-soc/0024*:2205-2214`) |
| `ext_buck_iso` | `ext_buck_iso_offs = 0x398`, `ext_buck_iso_mask = GENMASK(1, 0)` (`0x3`) | `mp01-kernel/drivers/soc/mediatek/mtk-scpsys-mt6789.c:35-36`, `patches/03-soc-power/0002*:94-95` | **Yes** |
| `MT6789_TOP_AXI_PROT_EN_MD` | `BIT(7)` (`SET=0x02a0`, `CLR=0x02a4`, `STA1=0x0228`, `BUS_PROT_WR_IGN`) | `mp01-kernel/drivers/soc/mediatek/mtk-scpsys-mt6789.c:38-39`, `patches/03-soc-power/0002*:57,97-101` | **Yes** |
| `MT6789_TOP_AXI_PROT_EN_INFRA_VDNR_MD` | `BIT(0) \| BIT(10) \| BIT(15)` (`SET=0x0b84`, `CLR=0x0b88`, `STA1=0x0b90`, `BUS_PROT_WR_IGN`) | `mp01-kernel/drivers/soc/mediatek/mtk-scpsys-mt6789.c:40-41`, `patches/03-soc-power/0002*:58,102-106` | **Yes** |
| SCPSYS MD `.caps` | `MTK_SCPD_MODEM_PWRSEQ \| MTK_SCPD_EXT_BUCK_ISO \| MTK_SCPD_KEEP_DEFAULT_OFF` (`mp01`: `MTK_SCPD_MD_OPS \| MTK_SCPD_BYPASS_INIT_ON`) | `patches/03-soc-power/0002*:108-109` (note: `zinwa` also adds `MTK_SCPD_SKIP_RESET_B`, which is a no-op under `MTK_SCPD_MODEM_PWRSEQ` in upstream `mtk-pm-domains.c:634,716,813`) | **Yes** (functional match; see §1.2 for `KEEP_DEFAULT_OFF` caveat) |
| MD Regulators (`mddriver`) | `md_vmodem-supply = <&mt6358_vmodem_reg>` (`buck_vmodem`, phandle `0x5d`, fixed `825000 uV` / `0xc96a8`); `md_vsram-supply = <&mt6358_vsram_core_reg>` (`ldo_vsram_core`, phandle `0x5e`, fixed `825000 uV` / `0xc96a8`) | `runtime_merged.dts:4694-4697` + phandle resolution `L3975`, `L4110` | **Yes** (`mt8781-zinwa-q25.dts:1666-1669`; Q25 also adds `dvfsrc-vcore-supply`) |
| SPM `srclken_o1` & `power_flow_config` | `mediatek,srclken_o1 = <0x200000>` (`BIT(21)` in SPM `0x10006000`); `mediatek,power_flow_config = <0x12>` (`BIT(1)=SRCLKEN_O1_BIT \| BIT(4)=SKIP_TOPCLK_BIT`) | `runtime_merged.dts:4686-4687`, `mp01-kernel/drivers/misc/mediatek/eccci/platform/md_sys1_platform.h:84-90`, `ccci_dump.log` (`bypass md_cd_topclkgen_on`, `bypass md_cd_srcclkena_setting`, `set srclken_o1_on done`) | **Yes** (`mt8781-zinwa-q25.dts:1656-1657`) |
| DPMAIF Clocks (`dpmaif@10014000`) | `<&infracfg_ao 55>` (`0x37`, `CLK_IFRAO_DPMAIF_MAIN`, gate `IFRAO3` bit 26, parent `top_dpmaif_main` = `CLK_TOP_DPMAIF_MAIN` `160` / mux `CLK_TOP_DPMAIF_MAIN_SEL` `42`) | `runtime_merged.dts:4652-4653` (`clocks = <0x2b 0x37>`, `"infra-dpmaif-clk"`), `patches/01-clk/0001*:192,310,378`, `patches/01-clk/0002*:375,1088` | **Yes** (`mt8781-zinwa-q25.dts:1618-1619`) |
| CCIF0 Clocks (`ccifdriver@10209000`) | `<&infracfg_ao 30>` (`0x1e`, `CLK_IFRAO_CCIF_AP` bit 23), `<&infracfg_ao 33>` (`0x21`, `CLK_IFRAO_CCIF_MD` bit 26), `<&infracfg_ao 25>` (`0x19`, `CLK_IFRAO_CCIF1_AP` bit 12), `<&infracfg_ao 26>` (`0x1a`, `CLK_IFRAO_CCIF1_MD` bit 13), `<&infracfg_ao 57>` (`0x39`, `CLK_IFRAO_CCIF4_MD` bit 29), `<&infracfg_ao 51>` (`0x33`, `CLK_IFRAO_CCIF5_MD` bit 10) | `runtime_merged.dts:4664-4665`, `patches/01-clk/0001*:348-380`, `patches/01-clk/0002*:343-377` | **Yes** (`mt8781-zinwa-q25.dts:1631-1642`) |
| CCIF2 / SCP Clocks (`ccci_scp`) | `<&infracfg_ao 52>` (`0x34`, `CLK_IFRAO_CCIF2_AP` `IFRAO3` bit 16), `<&infracfg_ao 53>` (`0x35`, `CLK_IFRAO_CCIF2_MD` `IFRAO3` bit 17) | `runtime_merged.dts:4704-4705`, `patches/01-clk/0001*:375-376`, `patches/01-clk/0002*:372-373` | **Yes** (`mt8781-zinwa-q25.dts:1677-1679`) |

*Note:* All 10 required clock gates (`ifrao_dpmaif_main`, `ifrao_ccif*`, `dpmaif_main_sel`) are **already implemented** in our `patches/01-clk/` and confirmed present in `c7286ac` `clk_summary`.

---

### 1.2 Bootloader MD State & `MTK_SCPD_KEEP_DEFAULT_OFF` AXI Bus-Protection Analysis

**1. Why `PWR_STATUS(0x1000616c) = 0xF83A1BC1` has `MD` (`BIT(0)`) ON at Linux boot:**
- Verified directly in Advan X1's own bootloader log (`/work/firmware/advan_x1/ondevice_dumps/pl_lk.txt:1852-1886`) and live `/sys/kernel/ccci/lk_md` (`LK Load MD:[Enabled](0x00000001) / LK load MD success!`):
  1. Stock LK reads `md1img_a` (`/dev/block/sdc19`), loads it into DRAM at `0xdc000000`, configures BANK0/BANK1/BANK4 HW remaps, applies EMI MPU regions `10..16` and `22..25`, frees unused holes back to `mblock`, and **permanently locks** MD HW remap via `MTK_SIP_LK_CCCI_CONTROL(MD_HW_REMAP_LOCKED=8)` (`pl_lk.txt:1876-1877`).
  2. LK then calls `ccci_md_power_on` (`pl_lk.txt:1881-1884`: sets SPM O1 `0x80215860` and powers ON MD MTCMOS).
  3. LK then calls `ccci_let_md_go` (`pl_lk.txt:1885-1886`: `"Trigger MD run"`), releasing MD from reset **before** jumping to Linux!
  4. When Android boots later, `ccci_md_all.ko` first checks `MD_CHECK_DONE` (BL31 `0x1001a824..0x1001a830 == {1,1,1,1}`), logs `[POWER ON] dummy: MD MTCMOS ON`, triggers `MD_KERNEL_BOOT_UP` (`0x20000024 = 1`), and synchronizes via CCIF0 SRAM handshake (`HS1`/`HS2`).

**2. Does `MTK_SCPD_KEEP_DEFAULT_OFF` hang AXI bus protection without an EL3 MD-stop SMC?**
- **At Linux boot (`late_initcall_sync`):** **NO hang** — verified in upstream `/work/mainline/linux/drivers/pmdomain/mediatek/mtk-pm-domains.c:993-1021` and proven on hardware (`0c39203`, `c7286ac`):
  - `scpsys_add_one_domain()` sees `MTK_SCPD_KEEP_DEFAULT_OFF`, logs `"A default off power domain has been ON"`, does **not** touch hardware, and calls `pm_genpd_init(&pd->genpd, NULL, true)` (marking `is_off = true` in genpd software state). Because genpd believes `MD` is already OFF, `genpd_power_off_unused()` never invokes `scpsys_power_off()` at boot.
- **On runtime power-off (`pm_runtime_put_sync()` / module unload / `scpsys_remove_one_domain()`):** **YES — CRITICAL HAZARD!**
  - Disassembly of Advan X1 BL31 (`bl31.bin`, `0x48205668`) proves there is **NO EL3 MD-stop SMC** (only `MD_KERNEL_BOOT_UP=0`, `MD_CHECK_FLAG=2`, `MD_CHECK_DONE=3`, `MD_BOOT_STATUS=4`, `MD_LK_BOOT_PLAT=8`, and `MD_FLIGHT_MODE_SET=7` which only updates an ATF/SPM flag).
  - If a driver attaches to `MT6789_POWER_DOMAIN_MD` and calls `pm_runtime_put_sync()` while MD is still executing (without first cleanly halting MD via `AT+EPOF` + MD RGU reset / flight mode), `scpsys_power_off()` enables `MT6789_TOP_AXI_PROT_EN_MD` (`BIT(7)`) and `MT6789_TOP_AXI_PROT_EN_INFRA_VDNR_MD` (`BIT(0)|BIT(10)|BIT(15)`) and polls `STA1` — **which will time out / hang the AXI interconnect** if MD still has inflight bus transactions!
  - **Zinwa Q25 cross-check:** `zinwa-q25-linux/patches/mt6789-soc/0024*:2452-2470` explicitly patches `scpsys_remove_one_domain()` to skip `scpsys_power_off_internal()` when `MTK_SCPD_KEEP_DEFAULT_OFF` is set for this exact reason.
  - **Mainline Advan X1 rule:** Keep `MTK_SCPD_KEEP_DEFAULT_OFF` on `MT6789_POWER_DOMAIN_MD`; on initial bring-up, adopt LK's already-ON MD state cleanly (`pm_runtime_get_sync()` once, or skip redundant power-off until a full `AT+EPOF` + `MD_RGU` halt sequence is verified).

---

### 1.3 Reserved Memory & EMI MPU (Advan X1 Exact Map vs Zinwa Q25)

**CRITICAL DIVERGENCE vs `zinwa-q25-linux`:** Advan X1's `md_mem_usage` DRAM regions are **32 MiB lower (`0xdc000000..0xe4000000`)** than Zinwa Q25 (`0xde000000..0xe6000000`) and differ in region 3 size (`0x2c40000` vs `0x2c50000`). **Never copy Q25's `md_mem` nodes.**

#### A. Top-Level Reserved Memory (`runtime_merged.dts:489-541` & `pl_lk.txt:2067-2075`)

| DT Node / Name | Advan X1 Phys Base | Advan X1 Size | MD View Base (BANK4) | EMI MPU Region ID(s) | Matches `zinwa-q25-linux`? |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `md_mem_usage` #1 (`mblock-23`) | `0xdc000000` | `0x13f0000` (19.94 MiB) | `0x00000000` (BANK0) | **10** (`0xdc000000..0xdd3f0000`) | **NO** (Q25: `0xde000000`, `0x13f0000`) |
| `md_mem_usage` #2 (`mblock-24`) | `0xddc00000` | `0x0430000` (4.19 MiB) | BANK0+`0x1c00000` | **16** (`0xddc00000..0xde030000`) | **NO** (Q25: `0xdfc00000`, `0x430000`) |
| `md_mem_usage` #3 (`mblock-25`) | `0xde800000` | `0x2c40000` (44.25 MiB) | BANK0+`0x2800000` | **15** (`0xde800000+0x1e40000`), **14** (`0xe0640000+0xe00000`), **13** (`0xe1400000+0x40000`) | **NO** (Q25: `0xe0800000`, `0x2c50000`) |
| `md_mem_usage` #4 (`mblock-26`) | `0xe3200000` | `0x0e00000` (14.00 MiB) | BANK0+`0x7200000` | **11** (`0xe3200000+0x6b0000`), **12** (`0xe38b0000+0x750000`), **22** (`0xe3200000+0xe00000`) | **NO** (Q25: `0xe5200000`, `0xe00000`) |
| `ap_md_nc_smem` (`mblock-27`) | `0x8e000000` | `0x0e10000` (14.06 MiB) | `0x40000000` (BANK4 slot 0) | **25** (`0x8e000000..0x8ee10000`) | **Yes** (`0x8e000000`, `0xe10000`) |
| `ap_md_c_smem` (`mblock-28`) | `0x8c000000` | `0x16a0000` (22.63 MiB) | `0x48000000` (BANK4 slot 4) | **24** (`0x8c000000..0x8d6a0000`), **23** (`0x8d600000+0x40000` WIFI proxy) | **Yes** (`0x8c000000`, `0x16a0000`) |
| `ccci_tag_mem` (`mblock-22`) | `0xbf7f0000` | `0x0010000` (64 KiB) | AP-only (LK→kernel tags) | N/A | **Yes** (`0xbf7f0000`, `0x10000`) |
| `reserve-memory-scp_share` (`mblock-21`) | `0x8f000000` | `0x07a0000` (7.63 MiB) | SCP DRAM share (unused w/o SCP) | N/A | **Yes** (`0x8f000000`, `0x7a0000`) |
| `SCP-reserved` (`mblock-20`) | `0xbf800000` | `0x0600000` (6.00 MiB) | SCP FW DRAM | N/A | **Yes** (`0xbf800000`, `0x600000`) |

*LK Note:* LK initially reserves 128 MiB (`0xdc000000..0xe4000000`, `ccci_dump.log:20`), applies MPU regions `10..16, 22..25` (`pl_lk.txt:1853-1863`), and returns unused holes (`0xdd3f0000+0x810000`, `0xde030000+0x7d0000`, `0xe1440000+0x1dc0000`, `0xe4000000+0x18000000`) to AP DRAM. Because LK already programs all EMI MPU regions and locks MD HW remap before Linux starts, **mainline Linux does not need to reprogram EMI MPU** — it only needs to reserve (`no-map`) those exact regions in `mt6789-advan-x1.dts` (or parse `ccci_tag_mem` / DT reservations passed by LK/U-Boot).

#### B. Key Sub-Regions Inside `ap_md_nc_smem` (`0x8e000000` / MD `0x40000000`) & `ap_md_c_smem` (`0x8c000000` / MD `0x48000000`)

Verified from live Advan X1 `/proc/ccci_dump` (`ccci_dump.log:120-149`) and `pl_lk.txt:1768-1852`:

| `SMEM_USER_ID` | Enum Name | AP Phys Base | MD Phys Base | Size | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `10` | `SMEM_USER_RAW_DFD` | `0x8e000000` | `0x40000000` | `0x800000` (8 MiB) | MD DFD dump |
| `12` | `SMEM_USER_RAW_MDCCCI_DBG` | `0x8e800000` | `0x40800000` | `0x000800` (2 KiB) | MD CCCI exception/boot debug |
| `13` | `SMEM_USER_RAW_MDSS_DBG` | `0x8e800800` | `0x40800800` | `0x003800` (14 KiB) | MD subsys debug |
| `14` | `SMEM_USER_RAW_RUNTIME_DATA` | `0x8e80e800` | `0x4080e800` | `0x001000` (4 KiB) | AP→MD HS2 runtime data |
| `15` | `SMEM_USER_RAW_FORCE_ASSERT` | `0x8e80f800` | `0x4080f800` | `0x000400` (1 KiB) | Force-assert trigger |
| `0` | `SMEM_USER_RAW_DBM` | `0x8e80fe00` | `0x4080fe00` | `0x000200` (512 B) | Modem sleep/wakelock DBM |
| **`16`** | **`SMEM_USER_CCISM_SCP`** | **`0x8e810000`** | **`0x40810000`** | **`0x008000` (32 KiB)** | **Emulated SCP CCISM Ring (see §1.7)** |
| **`19`** | **`SMEM_USER_CCISM_MCU`** | **`0x8e829000`** | **`0x40829000`** | **`0x0b4400` (721 KiB)** | **CCIF0 AP↔MD normal channel rings** |
| `20` | `SMEM_USER_CCISM_MCU_EXP` | `0x8e8dd400` | `0x408dd400` | `0x01e400` (121 KiB) | CCIF0 exception channel rings |
| `41` | `SMEM_USER_MD_DRDI` | `0x8e900000` | `0x40900000` | `0x010000` (64 KiB) | DRDI RF calibration table |
| **`42`** | **`SMEM_USER_MD_DATA`** | **`0x8e910000`** | **`0x40910000`** | **`0x500000` (5 MiB)** | **DPMAIF SW Uplink TX ring & buffers (see §1.10)** |
| `32` | `SMEM_USER_MD_NVRAM_CACHE` | `0x8c000000` | `0x48000000` | `0x1200000` (18 MiB) | Cacheable NVRAM/CCB shared pool |
| `22` | `SMEM_USER_RAW_MD_CONSYS` | `0x8d600000` | `0x49600000` | `0x040000` (256 KiB) | MD↔CONSYS proxy |
| `24` | `SMEM_USER_RAW_USIP` | `0x8d640000` | `0x49640000` | `0x060000` (384 KiB) | Audio uSIP shared mem |

*Note on `ap_md_nc_smem` offset alignment:* In `pl_lk.txt:1770-1785` vs `ccci_dump.log:125-140`, `SMEM_USER_CCISM_SCP` (`0x8e810000`, `0x8000`) and `SMEM_USER_MD_DATA` (`0x8e910000`, `0x500000`) are fixed-aligned identically, while `SMEM_USER_CCISM_MCU` (`ID 19`) starts at `0x8e829000` in kernel `ccci_dump.log` (vs `0x8e828000` in LK log due to kernel `reg[13]<7>` 0x2000 vs LK 0x1000). **Always parse/derive exact NC-SMEM offsets from `ccci_tag_mem` (`0xbf7f0000`) or match the kernel runtime table passed to MD in HS2 (`SMEM_USER_RAW_RUNTIME_DATA`).**

---

### 1.4 EL3 / BL31 SiP SMCs (Verified Directly from Advan X1 `tee.img` BL31 + `ccci_*.ko`)

Disassembled directly from Advan X1 BL31 (`/work/firmware/advan_x1/tee.img`, MKIMG `atf`, `off=0x200`, `size=0x30558`, load base `0x48200000`) and cross-verified against Advan X1 `ccci_md_all.ko`, `ccci_ccif.ko`, `ccci_dpmaif.ko`, `ccci_fsm_scp.ko`, `mp01-kernel`, and `zinwa-q25-linux`:

| SMC Name | SMC32 / SMC64 ID | BL31 Table Off & Handler VA | Role | Matches `zinwa` / `mp01`? |
| :--- | :--- | :--- | :--- | :--- |
| **`MTK_SIP_KERNEL_CCCI_CONTROL`** | `0x82000505` / **`0xC2000505`** | `@0x2b088` → **`0x48205668`** | Kernel CCCI control (cases `0, 5, 6, 7, 9, 13`; all others return `-15`) | **Yes** (`mtk_sip_svc.h:105`) |
| `MTK_SIP_LK_CCCI_CONTROL` | `0x8200040b` / `0xC200040B` | `@0x2b0a8` → `0x4820555c` | LK-only HW remap (`1..4`, `6`, `8=LOCK`, `12=SCP_INFO`); locked permanently by LK (`[0x48243740]=1`) | **Yes** |
| `MTK_SIP_EMIMPU_CONTROL` / `EMIDBG` | `0x8200050b` / `0xC200050B` | `@0x2b768` → `0x482179e8` | EMI MPU debug/control (LK configures EMI MPU via `MTK_SIP_BL33_MPU_PERM_SET`; `0x260/0x261/0x262` are NOT present in Advan X1 BL31 table) | **Diverges** (BL31 uses `0x50B` `EMIDBG` + LK `BL33_MPU_PERM_SET`, not legacy `0x260..0x262`) |
| `MTK_SIP_KERNEL_GET_RND` | `0x8200026a` / `0xC200026A` | `@0x2b228` → `0x482188e4` | Hardware RNG for `port_rpc` (`IPC_RPC_GET_TDD_EINT_NUM` / random) | **Yes** (`mtk_sip_svc.h:90`) |

#### Exact `MTK_SIP_KERNEL_CCCI_CONTROL` (`x0 = 0xC2000505`) Sub-Commands & Argument Layouts (BL31 `0x48205668` + `ccci_md_all.ko`)

| `x1` (`CCCI_SECURE_REQ_ID`) | `x2` (Sub-Op) | `x3` (Arg) | BL31 Action (Disassembled from `bl31.bin`) | Callers in Advan X1 `.ko` |
| :--- | :--- | :--- | :--- | :--- |
| `0` (`MD_DBGSYS_REG_DUMP`) | `reg_id` | `0` | Calls `0x48205078` (debug register dump) | `internal_md_dump_debug_register` |
| `5` (`MD_CLOCK_REQUEST`) | `0` (`MD_REG_AP_MDSRC_REQ`) | `locked` (`0`/`1`) | Requests/releases AP MDSRC 26MHz clock (`0x48204f84`) | `md_cd_lock_modem_clock_src` (`0x38c38`) |
| `5` (`MD_CLOCK_REQUEST`) | `1` (`MD_REG_AP_MDSRC_ACK`) | `0` | Polls MDSRC ACK (`0x48204fb8`) | `md_cd_lock_modem_clock_src` (`0x38de4`) |
| `5` (`MD_CLOCK_REQUEST`) | `2` (`MD_REG_AP_MDSRC_SETTLE`) | `0` | Reads settle delay ms (`0x48204fe4`) | `md_cd_lock_modem_clock_src` (`0x38c74`) |
| `5` (`MD_CLOCK_REQUEST`) | `3` (`MD_WAKEUP_AP_SRC`) | `1` (CCIF) / `4` (DPMAIF) | Reads/acknowledges MD wakeup source (`0x48205014`) | `ccif_resume_noirq` (`w3=1`), `ccci_dpmaif_resume_noirq_v1` (`w3=4`) |
| `5` (`MD_CLOCK_REQUEST`) | `4` (`MD_GET_SLEEP_STATE`) | `0` | Checks if MD1 is asleep (`0x48205038`) | `spm_is_md1_sleep_ccci` (`0xc78c`) |
| **`6` (`MD_POWER_CONFIG`)** | **`0` (`MD_KERNEL_BOOT_UP`)** | `0` | **`0x4820aab0`: Writes `1` to MD boot slave `0x20000024`**, returns `boot_status_0` (`[0x20000070]`), `boot_status_1` (`[0x20000074]`), `boot_slave` (`[0x20000024]`) in `a1..a3` | **`md_cd_let_md_go` (`0x3e618`)** |
| `6` (`MD_POWER_CONFIG`) | `2` (`MD_CHECK_FLAG`) | `0` | `0x4820ab34`: Reads 4 words at `0x1001a824..0x1001a830` into `a0..a3` | `md_start_platform` (`0x3cf50`) |
| `6` (`MD_POWER_CONFIG`) | `3` (`MD_CHECK_DONE`) | `0` | `0x4820aae0`: Returns `0` iff all 4 words at `0x1001a824..0x1001a830 == 1`, else `1` | `md_start_platform` (`0x3cebc`) |
| `6` (`MD_POWER_CONFIG`) | `4` (`MD_BOOT_STATUS`) | `0` | `0x48207d24`: Selects `0x1020e700 = 0/1`, reads MD boot status at `0x1020e300` into `a1..a2` | `md_cd_get_md_bootup_status` (`0x38f74`) |
| `6` (`MD_POWER_CONFIG`) | `8` (`MD_LK_BOOT_PLAT`) | `stage` | `0x4820b748`: Returns `0` (no-op stub on MT6789) | `md_cd_power_on` (`0x3e028`) |
| `7` (`MD_FLIGHT_MODE_SET`) | `flight_mode` (`0`/`1`) | `0` | `0x4820b750`: Updates ATF/SPM flight-mode flag (does NOT halt MD CPU!) | `flight_mode_set_by_atf` (`0x3f0ec`) |
| `9` (`MD_DEBUG_DUMP`) | `...` | `...` | `0x48205080`: MD debug dump | `md_dump_reg` |
| **`13` (`SCP_CLK_SET_DONE`)** | `0` | `0` | **`0x48215804`: Writes `0x534d454d` (`"MEMS"`), `0x5343505f` (`"_PCS"`), `0x8e810000`, `0x8000` into `AP_CCIF2_CHDATA` (`0x1023c100..0x1023c10c`)** | **`ccci_fsm_scp.ko:ccci_scp_md_state_sync_work` (`0x11b4`)** |

---

### 1.5 MMIO Bases & GIC SPI IRQs (Verified from `runtime_merged.dts` + Live `/proc/interrupts`)

| Block | Advan X1 Phys Base(s) & Size | GIC SPI IRQ (Linux `/proc/interrupts` GICv3 ID) | Local Ground-Truth Source | Matches `zinwa-q25-linux`? |
| :--- | :--- | :--- | :--- | :--- |
| **AP_CCIF0 / MD_CCIF0** (`ccifdriver@10209000`) | `0x10209000` (`0x1000`, `ap_ccif0`), `0x1020a000` (`0x1000`, `md_ccif0`), SRAM = `512 B` (`+0x100`) | **SPI 226** (`0xe2`, Level High → GICv3 `258` `CCIF_AP_DATA0`), **SPI 227** (`0xe3`, Level High → GICv3 `259` `CCIF_AP_DATA1`) | `runtime_merged.dts:4659-4666, 4752-4760`; live `/proc/interrupts` lines 55-56 | **Yes** (`mt8781-zinwa-q25.dts:1624-1630`) |
| **AP_CCIF2 / MD_CCIF2** (`ccci_scp`) | `0x1023c000` (`0x1000`, `ap_ccif2`), `0x1023d000` (`0x1000`, `md_ccif2`) | Polled / SCP-side IRQ (no AP IRQ in DT) | `runtime_merged.dts:4701-4707, 4772-4780` | **Yes** (`mt8781-zinwa-q25.dts:1673-1680`) |
| **MD WDT / RGU** (`mddriver`) | `0x0d0cf000` (`0x1000`, `md_rgu` / MD TOPRGU) | **SPI 110** (`0x6e`, Edge Rising → GICv3 `142` `MD_WDT`) | `runtime_merged.dts:4688-4689`; live `/proc/interrupts` line 57 | **Yes** (`mt8781-zinwa-q25.dts:1658-1661`) |
| **DPMAIF** (`dpmaif@10014000`, 7 reg regions) | `[0]` `0x10014000` (`ao_ul`), `[1]` `0x10014400` (`ao_dl`), `[2]` `0x1022d000` (`pd_ul`), `[3]` `0x1022d100` (`pd_dl`), `[4]` `0x1022d400` (`pd_misc`), `[5]` `0x1022c000` (`pd_md_misc`), `[6]` `0x1022e000` (`pd_sram`) — each `0x1000` | **SPI 241** (`0xf1`, Level High → GICv3 `273` `DPMAIF_AP`) — **single shared UL+DL IRQ** | `runtime_merged.dts:4647-4656`; live `/proc/interrupts` line 54; `ccci_hif_dpmaif_v1.c:5232-5238` | **Yes** (`mt8781-zinwa-q25.dts:1607-1616`) |
| **MD Infra / Boot / SPM** | `topckgen` `0x10000000`, `infracfg_ao` `0x10001000`, `scpsys`/`md_power_o1` `0x10006000`, `emimpu` `0x10226000` (SPI 221 / `0xdd`), `modem_temp_share` `0x10018000`, MD boot slave `0x20000024` (EL3-only) | `emimpu`: **SPI 221** (`0xdd`, Level High) | `runtime_merged.dts:1272, 1279, 1334, 3598-3604, 4669, 4815`; `bl31.bin:0x4820aab0` | **Yes** |

**CCIF0/CCIF2 Register Offsets** (`mp01-kernel/drivers/misc/mediatek/eccci/hif/ccif_hif_reg.h:30-38`):
- `APCCIF_CON` = `0x00`, `APCCIF_BUSY` = `0x04`, `APCCIF_START` = `0x08`, `APCCIF_TCHNUM` = `0x0C` (write ch# `0..15` to doorbell MD), `APCCIF_RCHNUM` = `0x10` (bitmask of pending MD→AP ch#), `APCCIF_ACK` = `0x14` (W1C ack), `APCCIF_IRQ0_MASK` = `0x20`, `APCCIF_IRQ1_MASK` = `0x24`, `APCCIF_CHDATA` = `0x100` (512-byte SRAM; HS1/HS2 SRAM doorbell uses `RINGQ_SRAM = 15`).

---

### 1.6 CCCI Header Struct, Channel IDs & CCIF0 Queue Mapping

#### A. CCCI Packet Header (16 Bytes, Little-Endian)
Verified from `mp01-kernel/drivers/misc/mediatek/include/mt-plat/mtk_ccci_common.h:77-84` (identical in `zinwa-q25-linux` and Advan X1 `ccci_fsd` `CCCI_H(0x%X)(0x%X)(0x%X)(0x%X)`):
```c
struct ccci_header {
	u32 data[2];        /* data[0]: magic/arg0 (0xFFFFFFFF for ctrl/sys); data[1]: payload len incl. 16B hdr (or msg_id) */
	u16 channel:16;     /* enum CCCI_CH logical channel ID */
	u16 seq_num:15;     /* per-channel sequence number */
	u16 assert_bit:1;   /* 1 = enable sequence check */
	u32 reserved;       /* arg1 / extra param */
} __packed;             /* 16 bytes */
```

#### B. Essential Channel IDs & CCIF0 Queue Assignments (`inc/ccci_core.h:310-355`, `port/port_cfg.c:12-28, 365-554`)

| Logical Channel | RX Ch ID (MD→AP) | TX Ch ID (AP→MD) | CCIF0 Queue (`txq`/`rxq`) | Gen95 Ring Sizes (`rx`/`tx` in `SMEM_USER_CCISM_MCU`) | Android Device / Mainline Mapping | Matches `zinwa`? |
| :--- | :---: | :---: | :---: | :--- | :--- | :--- |
| **CONTROL** (HS1/HS2/Boot/EE) | **`0`** (`CCCI_CONTROL_RX`) | **`1`** (`CCCI_CONTROL_TX`) | Q0 (normal) / SRAM Q15 (HS1) | `80 KiB` RX / `128 KiB` TX | Internal FSM (`ccci_ctrl`) | **Yes** |
| **SYSTEM** (CCISM/SIM/Status) | **`2`** (`CCCI_SYSTEM_RX`) | **`3`** (`CCCI_SYSTEM_TX`) | Q0 | Shared on Q0 (`80K`/`128K`) | Internal FSM (`ccci_sys`) | **Yes** |
| **STATUS** (Heartbeat Poll) | **`67`** (`CCCI_STATUS_RX`) | **`68`** (`CCCI_STATUS_TX`) | Q0 | Shared on Q0 | Internal keepalive (`ccci_poll`) | **Yes** |
| **FS** (NVRAM File RPC) | **`14`** (`CCCI_FS_RX`) | **`15`** (`CCCI_FS_TX`) | **Q4** (`DATA_FSD_Q = 4`) | `20 KiB` RX / `20 KiB` TX | `/dev/ccci_fs` → Clean RO-ext4+tmpfs FS daemon | **Yes** |
| **AT / RIL** (UART2) | **`10`** (`CCCI_UART2_RX`) | **`12`** (`CCCI_UART2_TX`) | **Q5** (`DATA_AT_CMD_Q = 5`) | `20 KiB` RX / `20 KiB` TX | `/dev/ttyC0` → Upstream `WWAN_PORT_AT` (`/dev/wwan0at0`) | **Yes** |
| **RPC** (Kernel/User RPC) | **`32`** (`CCCI_RPC_RX`) | **`33`** (`CCCI_RPC_TX`) | Q1 | `80 KiB` RX / `40 KiB` TX | `/dev/ccci_rpc` + kernel `ccci_rpc_k` (e.g. `0x400D` TXQ remap) | **Yes** |
| **CCMNI1 / Data** | `20` (`CCCI_CCMNI1_RX`) | `22` (`CCCI_CCMNI1_TX`) | DPMAIF (not CCIF0) | DPMAIF PIT/BAT + `MD_DATA` | `ccmni0..N` → Upstream `wwan0` netdev | **Yes** |
| **CCISM_SCP** | CCIF2 Q0 (dedicated) | CCIF2 Q0 (dedicated) | **CCIF2** (`0x1023c000`) | `16 KiB` RX / `1 KiB` TX in `SMEM_USER_CCISM_SCP` | Emulated SCP CCISM Ring (see §1.7) | **Yes** |

*CCIF0 Ring Framing (`ccci_ringbuf.c:16-23, 133-265`):* Every CCIF0/CCIF2 queue in `SMEM_USER_CCISM_MCU` (`0x8e829000`) and `SMEM_USER_CCISM_SCP` (`0x8e810000`) uses identical `ccci_ringbuf` framing: 8B ring header `{0xEE0000EE, 0xEE0000EE}`, 24B `struct ccci_ringbuf { rx_control{rd,wr,len}; tx_control{rd,wr,len}; }`, `rx_buffer[rx_size]`, `tx_buffer[tx_size]`, 8B ring footer `{0xFF0000FF, 0xFF0000FF}` (`CCCI_RINGBUF_CTL_LEN = 40 B`). Each packet inside the ring is wrapped as: `u32 0xAABBAABB` + `u32 data_len` + `data[data_len]` (8-byte aligned) + `u32 0xCCDDEEFF` + `u32 0xCCDDEEFF`.

---

### 1.7 Emulated SCP CCISM Ring (Binary-Verified from Advan X1 `scp.img` RV55 + `bl31.bin` + `ccci_fsm_scp.ko`)

Disassembled directly from Advan X1's own SCP RISC-V firmware (`/work/firmware/advan_x1/scp.img`, `tinysys-scp-RV55_A` at `0x0b19a..0x0b608`, `0x0ba92..0x0bb10`, tables at `0x4e03c` and `0x4e06c`) and BL31 (`0x48215804`):

#### A. Shared-Memory Layout (`SMEM_USER_CCISM_SCP`: AP `0x8e810000`, MD `0x40810000`, Size `0x8000` = 32 KiB)
From `scp_rv55.bin` `0x4e06c` (`rx_queue_buffer_size[md0] = {0x4000, 0x0}`) and `0x4e03c` (`tx_queue_buffer_size[md0] = {0x0400, 0x0}`): **MD1 uses exactly ONE `ccci_ringbuf` (Queue 0)** starting at offset `0x0000` (Queue 1 has size 0 and is skipped at `0x0b2be`/`0x0b2d0`):

| Offset in `0x8e810000` | Size | Field / Contents | Notes |
| :--- | :--- | :--- | :--- |
| `0x0000..0x0007` | `8 B` | `{ 0xEE0000EE, 0xEE0000EE }` | `CCCI_RBF_HEADER` (`ccci_create_ringbuf` `0x0baf0`) |
| `0x0008..0x0013` | `12 B` | `rx_control { read = 0, write = 0, length = 0x4000 }` | MD → SCP ring control (16,384 B) |
| `0x0014..0x001F` | `12 B` | `tx_control { read = 0, write = 0, length = 0x0400 }` | SCP → MD ring control (1,024 B) |
| `0x0020..0x401F` | `0x4000` (16 KiB) | `rx_buffer[16384]` | MD writes `0xAABBAABB + len + payload + 0xCCDDEEFF×2` |
| `0x4020..0x441F` | `0x0400` (1 KiB) | `tx_buffer[1024]` | Emulated SCP writes responses/loopback |
| `0x4420..0x4427` | `8 B` | `{ 0xFF0000FF, 0xFF0000FF }` | `CCCI_RBF_FOOTER` |
| `0x4428..0x7FFF` | `0x3BD8` | Zero-filled unused tail | Total active footprint = `0x4428` (17,448 B) |

#### B. Minimal Emulated SCP Init & Keepalive Handshake (No `mtk-scp` Remoteproc Required)
1. **Wait for MD `READY`** (HS1 + HS2 complete on CCIF0).
2. **Enable CCIF2 clocks:** `CLK_IFRAO_CCIF2_AP` (`52`) and `CLK_IFRAO_CCIF2_MD` (`53`).
3. **Notify BL31:** Call `arm_smccc_smc(0xC2000505, 13 /* SCP_CLK_SET_DONE */, 0, ...)`. BL31 writes `{0x534d454d, 0x5343505f, 0x8e810000, 0x00008000}` into `AP_CCIF2_CHDATA` (`0x1023c100..0x1023c10c`).
4. **Send `CCISM_SHM_INIT`:** Send CCCI system message `0x119` on `CCCI_SYSTEM_TX` (ch `3`, CCIF0 Q0).
5. **Receive `CCISM_SHM_INIT_ACK`:** Wait for MD system message `0x11A` on `CCCI_SYSTEM_RX` (ch `2`, CCIF0 Q0).
6. **Initialize `0x8e810000`:** `memset_io(0x8e810000, 0, 0x8000)` and populate the Q0 `ccci_ringbuf` header/control/footer table above (`mb()` after writes).
7. **Send `CCISM_SHM_INIT_DONE`:** Send CCCI system message `0x11B` on `CCCI_SYSTEM_TX` (ch `3`, CCIF0 Q0).
8. **Minimal Runtime Service:** Periodically (or via CCIF2 poll) read `AP_CCIF2 + 0x10` (`APCCIF_RCHNUM`); if non-zero, W1C to `AP_CCIF2 + 0x14` (`APCCIF_ACK`), drain any packet from Q0 `rx_buffer`, and if MD sends a loopback/ping packet (`scp_rv55.bin:0x0b5b8-0x0b600`, ch `0` or `5`), echo the payload back to Q0 `tx_buffer` and ring `AP_CCIF2 + 0x0C` (`APCCIF_TCHNUM = 0`).

*Cross-check vs `zinwa-q25-linux`:* `zinwa-q25-linux` uses the vendor `ccci_fsm_scp.c` (identical `CCISM_SHM_INIT`/`ACK`/`DONE` `0x119`/`0x11A`/`0x11B` and `SCP_CLK_SET_DONE=13`), which waits for real SCP IPI; our emulated ring replaces only the SCP side so mainline requires zero `mtk-scp` remoteproc!

---

### 1.8 NVRAM Partition Layout & FS RPC Protocol (Binary-Verified from Advan X1 `/vendor/bin/ccci_fsd`)

#### A. Advan X1 On-Device NVRAM Partition Map (Live Android Oracle)

| Partition Name | Block Device | Filesystem | Stock Android Mount | Mainline Mount Policy (**STRICT RO + TMPFS**) | Key MD Directories Present on Advan X1 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `nvdata` | `/dev/block/sdc7` | `ext4` | `/mnt/vendor/nvdata` | `ro,noload` lower + `tmpfs` upper (`overlay`) | `md/NVRAM/{CALIBRAT,NVD_IMEI,NVD_DATA,NVD_CORE,BACKUP,INFO_FILE}`, `md_cmn`, `APCFG/APRDCL/MD_SBP` |
| `nvcfg` | `/dev/block/sdc6` | `ext4` | `/mnt/vendor/nvcfg` | `ro,noload` lower + `tmpfs` upper (`overlay`) | `fg/`, `sensor/` (also listed in `ccci_fsd` table) |
| `protect1` | `/dev/block/sdc16` | `ext4` | `/mnt/vendor/protect_f` | `ro,noload` lower + `tmpfs` upper (`overlay`) | `md/{ST33A004,ST34A001,ST36_001,ST37_000,ST38_001,LD40_001,LD81..85,MT00A001,MT00B001,MTFW_000,NA77_005,AA01_012,SL00_000,SL01_000}` |
| `protect2` | `/dev/block/sdc17` | `ext4` | `/mnt/vendor/protect_s` | `ro,noload` lower + `tmpfs` upper (`overlay`) | `md/{ST33B004,ST34B001,...}` (mirror of `protect_f/md`) |
| `nvram` | `/dev/block/sdc34` | raw bin | raw backup (`libnvram`) | **Do NOT open RW** (not needed for normal LTE boot) | Legacy raw backup |

#### B. Exact FS RPC Wire Format & 37 Opcodes (`0x1001..0x1025`) Disassembled from Advan X1 `/vendor/bin/ccci_fsd` (`0xbc4c..0xbc88`)

- **Transport:** `CCCI_FS_RX = 14` / `CCCI_FS_TX = 15` on CCIF0 Q4 (`20 KiB` RX / `20 KiB` TX). Each message begins with the 16-byte `struct ccci_header` (`PORT_F_USER_HEADER`), followed by `u32 op_id` (`[r4+0]` at `ccci_fsd:0xbc4c`), `u32 arg_num`, and TLV argument pairs `{ u32 len; u8 data[align4(len)]; }`.
- **Switch Table Disassembly (`ccci_fsd` VA `0xbc50..0xbc88`, `sub r0, r3, #0x1001; cmp r0, #36`):**

| Opcode | Handler VA (`ccci_fsd`) | Verified Operation | Key Syscalls / Evidence in `ccci_fsd` | Needed for LTE Bring-up? |
| :--- | :--- | :--- | :--- | :--- |
| **`0x1001`** | `0x0e964` | **`FS_CCCI_OP_OPEN`** | `FS_CCCI_Open`, `access`, `open` | **Required** |
| **`0x1002`** | `0x1004c` | **`FS_CCCI_OP_SEEK`** | `lseek`, `"Seek:"` | **Required** |
| **`0x1003`** | `0x103c8` | **`FS_CCCI_OP_READ`** | `__read_chk`, `"Read:"` | **Required** |
| **`0x1004`** | `0x10740` | **`FS_CCCI_OP_WRITE`** | `__write_chk`, `"Write: [overtime]"` (lands in `tmpfs` overlay!) | **Required** (into volatile tmpfs) |
| **`0x1005`** | `0x10c18` | **`FS_CCCI_OP_CLOSE`** | `fsync`, `close`, `"Close:"` | **Required** |
| **`0x1006`** | `0x1b0c0` | **`FS_CCCI_OP_CLOSEALL`** | `close` loop, `"CloseAll:"` | **Required** |
| **`0x1007`** | `0x11300` | **`FS_CCCI_OP_CREATEDIR`** | `stat`, `mkdir`, `FS_CCCI_CreateDir` | **Required** (into volatile tmpfs) |
| `0x1008` | `0x11c04` | `FS_CCCI_OP_REMOVEDIR` | `rmdir`, `"RemoveDir:"` | Easy stub/overlay |
| **`0x1009`** | `0x0fc70` | **`FS_CCCI_OP_GETFILESIZE`** | `fstat`, `lseek`, `"GetFileSize:"` | **Required** |
| `0x100A` | `0x1234c` | `FS_CCCI_OP_GETFOLDERSIZE` | `FS_CCCI_GetFolderSize`, `stat` | Optional |
| `0x100B` | `0x12d48` | `FS_CCCI_OP_RENAME` | `rename`, `"Rename:"` | Overlay |
| `0x100C` | `0x13974` | `FS_CCCI_OP_MOVE` | `"Move:"` | Overlay |
| `0x100D` | `0x15154` | `FS_CCCI_OP_COUNT` | `stat`, `"Count:"` | Optional |
| `0x100E` | `0x15950` | `FS_CCCI_OP_GETDISKINFO` | `statfs` | Optional |
| `0x100F` | `0x15ba8` | `FS_CCCI_OP_DELETE` | `unlink`, `"Delete:"` | Overlay |
| `0x1010` | `0x162f0` | `FS_CCCI_OP_GETATTRIBUTES` | `"GetAttributes:"` | Recommended |
| **`0x1011`** | `0x0e964` | **`FS_CCCI_OP_OPENHINT`** | Aliases `0x0e964` (`FS_CCCI_Open`) | **Required** |
| `0x1012` | `0x0c0e8` | `FS_CCCI_OP_FINDFIRST` | `"FindFirst:"` | Recommended |
| `0x1013` | `0x18254` | `FS_CCCI_OP_FINDNEXT` | `readdir`, `"FindNext:"` | Recommended |
| `0x1014` | `0x18fac` | `FS_CCCI_OP_FINDCLOSE` | `"FindClose:"` | Recommended |
| `0x1015..0x1017` | `0x0c1bc..0x0c2c0` | `LOCKFAT` / `UNLOCKALL` / `SHUTDOWN` | Inline status returns (`0` OK) | Return `0` |
| `0x1018` | `0x19414` | `FS_CCCI_OP_XDELETE` | `"XDelete:"` | Overlay |
| `0x1019..0x101A` | `0x0c31c`, `0x19d18` | `CLEARDISKFLAG` / `GETDRIVE` | `"GetDrive:"` (returns drive char for `FS_DRIVE_I_SYSTEM`) | Recommended |
| `0x101B` | `0x19f88` | `FS_CCCI_OP_GETCLUSTERSIZE` | `statfs`, `"GetClusterSize:"` | Return `4096` |
| `0x101C` | `0x0c3cc` | `FS_CCCI_OP_SETDISKFLAG` | Inline | Return `0` |
| `0x101D..0x1020` | `0x1d18c..0x1c538` | `OTP_WRITE` / `OTP_READ` / `OTP_QUERYLEN` / `OTP_LOCK` | `/dev/otp` (`FS_OTP*`) | Return `-EOPNOTSUPP` |
| `0x1021` | `0x0e468` | `FS_CCCI_OP_RESTORE` | `NVM_RestoreFromBinRegion_OneFile` | Return `-1` (RO safety) |
| **`0x1022..0x1023`** | `0x0c51c`, `0x0c5d8` | **`FS_CCCI_OP_CMPT_READ` / `CMPT_WRITE`** | Compound Open+Seek+Read/Write+Close (`"CMPT File name:"` / `"CMPTW"`) | **Required** (heavily used by Gen95 MD!) |
| `0x1024` | `0x0c614` | `FS_CCCI_OP_BIN_REGION_ACCESS` | `"Main: FS_CCCI_OP_BIN_REGION_ACCESS"` | Return `-1` (RO safety) |
| `0x1025` | `0x1a48c` | `FS_CCCI_OP_GETFILEDETAIL` | `stat`, `"GetFileDetail:"` (`mtime`/`atime`/`ctime`) | Recommended |

*Exact Path Mapping & UTF-16LE Drive-Letter Switch (Binary-Verified in `ccci_fsd` `0xdb00` + `0x0e9c4..0x0eb18`, `.data` table at VA `0x20b74`, stride `0x24`):*
`FS_ConvName` (`0xdb00`) converts MD's UTF-16LE path to ASCII, replaces `\` (`0x5c`) with `/` (`0x2f`), and rejects `..` traversal. `FS_CCCI_Open` (`0x0e9c4..0x0eaf4`) then matches the first two ASCII bytes (`<DRIVE>:`) into index `r7` (`0..9`) indexing `0x20b74 + r7 * 0x24`:

| Drive Tag | LE `u16` (`0x0e9c8..0x0ea58`) | Index `r7` | Exact Root Path (`.data` VA `0x20b74 + r7*0x24`) | Mainline Overlay Strategy |
| :--- | :--- | :--- | :--- | :--- |
| **`Z:`** | `0x3a5a` | `0` | `/mnt/vendor/nvdata/md` | `ro,noload` ext4 (`sdc7`) + volatile `tmpfs` upperdir |
| **`X:`** | `0x3a58` | `1` | `/mnt/vendor/protect_f/md` | `ro,noload` ext4 (`sdc16`) + volatile `tmpfs` upperdir |
| **`Y:`** | `0x3a59` | `2` | `/mnt/vendor/protect_s/md` | `ro,noload` ext4 (`sdc17`) + volatile `tmpfs` upperdir |
| **`W:`** | `0x3a57` | `3` | `/vendor/firmware` *(special RO branch at `0x0eb7c`)* | `ro` firmware dir |
| **`V:`** | `0x3a56` | `4` | `/mnt/vendor/nvdata/md_cmn` | `ro,noload` ext4 (`sdc7`) + volatile `tmpfs` upperdir |
| **`U:`** | `0x3a55` | `5` | `/data/vendor/mdlpm` | volatile `tmpfs` |
| **`T:`** | `0x3a54` | `6` | `/vendor/etc/mdota` | `ro` / empty `tmpfs` |
| **`S:`** | `0x3a53` | `7` | `/mnt/vendor/nvcfg` | `ro,noload` ext4 (`sdc6`) + volatile `tmpfs` upperdir |
| **`R:`** | `0x3a52` | `8` | `/vendor/etc/md` | `ro` / empty `tmpfs` |
| **`Q:`** | `0x3a51` | `9` | `/data/vendor_de/md` | volatile `tmpfs` |

*Exact `CMPT_READ` (`0x1022`, `0x0dc24`) & `CMPT_WRITE` (`0x1023`, `0x0e0fc`) TLV Layout (Binary-Verified):*
- `opid_map` (`u32` at `+0x00`): `BIT(0)` = `OPEN` (`0x0e964`), `BIT(1)` = `GETFILESIZE` (`0x0fc70`, Read only), `BIT(2)` = `SEEK` (`0x1004c`), `BIT(3)` = `READ` (`0x103c8`, Read only), `BIT(4)` = `CLOSE` (`0x10c18`), `BIT(5)` = `WRITE` (`0x10740`, Write only).
- `CMPT_READ` (`40 B` param struct, TLV0 = UTF-16LE filename, TLV1 = param struct): `+0x00 opid_map`, `+0x04 ret`, `+0x08 err`, `+0x0c open_flag`, `+0x10 out_file_size`, `+0x14 seek_offset`, `+0x18 seek_whence`, `+0x1c buf_ptr` (AP-local, points to response payload), `+0x20 read_len`, `+0x24 out_act_read`.
- `CMPT_WRITE` (`44 B` param struct, TLV0 = UTF-16LE filename, TLV1 = param struct, TLV2 = write payload `[r8,#0x14]`): `+0x00 opid_map`, `+0x04 ret`, `+0x08 err`, `+0x0c open_flag`, `+0x10 seek_offset`, `+0x14 seek_whence`, `+0x18 rsv`, `+0x1c write_len`, `+0x20 file_handle` (existing fd if `BIT(0)==0`), `+0x24 out_act_written`.

*Cross-check vs `zinwa-q25-linux`:* `zinwa-q25-linux` does NOT reimplement FS RPC; it runs stock Android `/vendor/bin/ccci_mdinit` (which embeds `ccci_fsd`) in a preload harness (`rootfs/usr/lib/systemd/system/q25-ccci@.service`). Our clean mainline design replaces `ccci_fsd` with a ~400-line open-source handler speaking `0x1001..0x1025` over RO ext4 + volatile `tmpfs`.

---

### 1.9 Stock RIL AT Init Sequence & `zinwa-q25-linux` ModemManager Plugin Notes

#### A. Advan X1 Complete Cold-Boot Chronological AT Sequence (`docs/reference/radio.log`, `L1..L7400`, `07:04:02.284..07:05:31.471`)
Verified end-to-end from Advan X1 cold-boot `docs/reference/radio.log` (single USIM in Slot 0, XL Axiata `51011` LTE, Slot 1 empty):

1. **Stage 0 — MD Boot & `/dev/ttyC0` Readiness (`L2..L43`, `07:04:02.284..07:04:04.626`):**
   - MD1 transitions `init` (`07:04:02.284`) → `reset` (`07:04:02.288`) → `bootup` (`07:04:02.806`) → **`ready` (`07:04:03.777`, 0.97s)** (`L2..L13`).
   - `gsm0710muxd` opens `/dev/ttyC0` (`L15, L19`) and waits for modem unsolicited readiness URCs: **`+EIND: 64`** (`07:04:03.978`, `L27`) followed by **`+EIND: 128`** (`07:04:04.604`, `L39-40` — AT subsystem ready; re-emitted on MUX channel open at `L72`/`L89`).
2. **Stage 1 — Cold AT Init, SIM Power-Up & RF Enable (`L110..L972`, `07:04:04.795..07:04:06.244`):**
   - Channel & URC setup: `AT+EMDVER?` (`L110`), `AT+ES3G?` (`L213` → `+ES3G: 1`), `ATE0Q0V1` (`L413`), **`AT+EIAREG=1`** (`L449` — enables initial-EPS-attach URC), `AT+CMEE=1` (`L479`), `AT+ESIMS=1` (`L551`), `AT+EREG=3` (`L579`), `AT+EGRAT=6` (`L583`), `AT+EGREG=3,1` / `AT+EGREG=3` (`L600, L617`), `AT+PSBEARER=1` (`L632`), `AT+ECSQ=1` (`L653`), `AT+CSCS="UCS2"` (`L659`), `AT+EOPS=3,2` (`L706`).
   - **CRITICAL ADVAN X1 COLD-BOOT SIM/RF TRIGGER (`L713..L916`):**
     ```
     AT+EBOOT=0        # L713 (07:04:05.010) normal boot mode
     AT+EDSBP=2        # L728 (07:04:05.015) dynamic SBP mode
     AT+EPOC           # L733 (07:04:05.018) Power-On Complete (allows SIM init!)
     AT+EFUN=0         # L743 (07:04:05.023) RF OFF, SIM ON (DSDS bitmask: 0=both off)
     # ... ~1.0s later:
     < +ESIMS: 1,29    # L815 (07:04:06.011) SIM0 inserted & ready (slot1 gets 0,1 = absent)
     < +EUSIM: 1, 2, 2 # L820 (07:04:06.014) USIM present
     AT+EFUN=1         # L868 (07:04:06.024) RF ON for SIM0 (bitmask: 1=SIM0, 2=SIM1, 3=both)
     AT+ERAT=6,4       # L916 (07:04:06.085) RAT mode 6 (LTE+UMTS+GSM), preferred 4 (LTE)
     ```
   - **Result (+3.6s after `AT+EFUN=1`, `L1107..L1163`, `07:04:09.634`):**
     - `AT< +EPDN:=***` (`L1107`)
     - **`AT< +EIAREG: ME ATTACH "internet.MNC011.MCC510.GPRS",IPV4V6,0`** (`L1109`)
     - **`AT< +EOPS: 0,2,"51011",4096`** (`L1111` — registered on XL Axiata `51011`, ACT `4096` = E-UTRAN/LTE!)
     - `AT< +EIND: 32` (`L1125`), `+EIND: 2` (`L1160`), `+EIND: 1` (`L1163`); `AT+CPIN?` → `+CPIN: READY` (`L1306-1310`).
   - *Note on `+EFUN` vs `+CFUN`:* Stock dual-SIM `mtkfusionrild` uses `AT+EBOOT=0` + `AT+EPOC` + `AT+EFUN=0` → `AT+EFUN=1` (DSDS bitmask) rather than `AT+CFUN=4` → `AT+CFUN=1`. On bare single-channel Linux/ModemManager (`zinwa-q25-linux`), MOLY accepts `AT+CFUN=4` → `AT+CFUN=1`, **or** `AT+EPOC` + `AT+EFUN=0` → `AT+EFUN=1` (keep `AT+EPOC` + `AT+EFUN=1` as primary fallback if `AT+CFUN=4` stalls on Advan X1!).
3. **Stage 2 — APN Table & Initial EPS Attach Provisioning (`L2820..L2881`, `07:04:23.622..07:04:23.670`):**
   - Verified live in `radio.log` (`L2820..L2881`, `L3308..L3319`, `L6016..L6031`) and `zinwa` `mm-plugin-mtk-ccci.c:70-98`:
     ```
     AT+EAPNLOCK=1
     AT+EAPNSET="",1,"",""
     AT+EAPNSET="<apn>",2,"type=default,supl;protocol=IPV4V6;roaming_protocol=IPV4V6;authtype=0;carrier_enabled=1;max_conns=0;max_conns_time=0;wait_time=0;bearer_bitmask=0;inactive_timer=0"
     AT+EAPNLOCK=0
     AT+EIAAPN="<apn>",0,"IPV4V6","IPV4V6",0,"",""
     AT+EDALLOW=1        # L3319 (slot 1 gets AT+EDALLOW=0 at L3308; later AT+ECNCFG=1,0,,0 & AT+EDALLOW=2,1 at L6016-6031)
     ```
   - **CRITICAL:** AP DPMAIF MUST already be enabled before `AT+EFUN=1` / `AT+CFUN=1` so the simultaneous initial EPS data bearer (`+EIAREG: ME ATTACH`) is not dropped by the network.
4. **Stage 3 — Data PDN Activation & IP/DNS Retrieval (`L4962..L5268`, `L5648..L5787`, `07:04:30.463..07:04:34.329`):**
   - Default internet bearer (`L4962..L5065`):
     ```
     AT+EAPNACT=1,"internet","default",0,,"dnn=internet",,0   # L4962
     < +CGEV: ME PDN ACT 0                                    # L4971
     AT+EPDN=<aid>,"nwact"                                    # L4973, L5039, L5047
     < +EAPNINFO: EPDN, "at", 0, 4, "IPv4v6", ...             # L4980
     < +EAPNINFO: EPDN, "ipchg", 0, 4, "IPv4v6", ...          # L4988
     AT+CGCONTRDP=0                                           # L4985, L5043
     AT+CGEQOSRDP=0                                           # L4999 -> +CGEQOSRDP: 0,6,,,,,240000,240000
     AT+CGACT?                                                # L5005 -> +CGACT: 0,1
     ```
   - **LIVE PROOF OF DYNAMIC `if_id` (`L5055, L5065, L5268, L5787`):**
     - IMS took `cid=100` (`100 % 100 = 0` → **`ccmni0`**, IPv6 `/64`, `L5682, L5787`), while default internet took **`transIntfId = 201` (`201 % 100 = 1` → `ccmni1`)** with `10.191.43.43/32`, DNS `112.215.198.248`/`112.215.198.254`, MTU `1500` (`L5055, L5065, L5268`)!
     - On stock Android, `NetAgentService` (`L388-452, L2969-3039, L4975-5001`) handles `AT+EIF` interface-up acks on a separate internal channel (hence no `AT+EIF` in tag `AT`); on bare Linux/ModemManager without `NetAgentService`, the modem emits `+EIF: <if_id>,...` on the primary AT port and requires `AT+EIF=<if_id>,"ifst","up",3` plus mapping DPMAIF `channel_id = if_id % 100` (never hardcode index `0`!).

#### B. `zinwa-q25-linux` ModemManager Plugin Notes (`packages/modemmanager-q25/`)
- Target: ModemManager 1.24.2 `mtk-ccci` plugin (`mm-plugin-mtk-ccci.c`, `mm-modem-helpers-mtk-ccci.c`, `77-mm-mtk-ccci.rules`).
- Key lessons reusable for our mainline `wwan_core` + `ModemManager` integration:
  1. `77-mm-mtk-ccci.rules` must match ONLY the primary AT port (`WWAN_PORT_AT` / `ttyC0`) and netdev (`wwan0`), **never** probe FS/RPC/log control ports.
  2. Cold-boot `initialization_started()` must wait for `+EIND: 128`, check `AT+CFUN?` and promote `0 → 4` (or issue `AT+EPOC` + `AT+EFUN=0` on DSDS firmware) so SIM becomes ready without enabling RF prematurely.
  3. Standard MM `load_initial_eps_bearer` is disabled (`NULL`) because MOLY uses `+EIAAPN`/`+EAPNSET` (and `AT+EIAAPN?` is unsupported); `set_initial_eps_bearer_settings()` runs the `EAPNLOCK`/`EAPNSET`/`ECNCFG`/`EIAAPN`/`EIAREG` sequence and always unlocks `+EAPNLOCK=0` on error/timeout.
  4. Disconnects arrive via `+EIF: ...,ifdown` URCs (`AT+EAPNACT?` is unsupported on MOLY), and data netdev channel selection MUST use `if_id % 100` (proven live on Advan X1 where default internet bound to `cid=201` → channel `1`).
  5. NetworkManager profile requires `gsm.initial-eps-bearer-configure yes gsm.initial-eps-bearer-apn <apn>` + static IPv4 `/32` MTU `1400`–`1500`.

---

### 1.10 DPMAIF Hardware Version, Register Map, Descriptors & Exact Diff vs Upstream `drivers/net/wwan/t7xx/`

Verified directly from Advan X1 `runtime_merged.dts:4647-4656`, Advan X1 `ccci_dpmaif.ko` disassembly (`dpmaif_late_init.1003`, `dpmaif_bat_init.1045`, `.rodata:0x72c0`), `mp01-kernel` (`ccci_hif_dpmaif_v1.[ch]`, `dpmaif_reg_v1.h`), `zinwa-q25-linux`, and upstream Linux `/work/mainline/linux/drivers/net/wwan/t7xx/`.

#### A. MT6789 / Advan X1 DPMAIF Hardware Parameters (Binary-Verified)

| Parameter | Advan X1 Verified Value | Source | Matches `zinwa-q25-linux`? |
| :--- | :--- | :--- | :--- |
| **HW Version** | **`dpmaif_ver = 1`** (Gen95 / MT6295M, `chip_info = 6789` / `0x1a85`) | `runtime_merged.dts:4654-4655`, `ccci_dpmaif.ko` | **Yes** (`mt8781-zinwa-q25.dts:1620-1621`) |
| **Capability Bitmask** | **`dpmaif_capability = 6`** (`BIT(1)=DPMAIF_CAP_2RXQ \| BIT(2)=DPMAIF_CAP_PIT_DEG`, LRO `BIT(0)=0`) | `runtime_merged.dts:4651`, `ccci_hif_dpmaif_comm.h:22-24` | **Yes** (`mt8781-zinwa-q25.dts:1617`) |
| **DL PIT Ring** | **`2048` entries × `12 bytes` = `24,576 B` (`0x6000`)** | `ccci_dpmaif.ko:dpmaif_late_init.1003` (`0x2c5a8`: `w9=0x800`, `w1=0x6000`), `ccci_hif_dpmaif_v1.h:101,107` | **Yes** |
| **DL BAT Ring** | **`1024` entries × `8 bytes` = `8,192 B` (`0x2000`)**, `pkt_buf_sz = 1664 B` (`0x680`) / `1920 B` (`0x780`) | `ccci_dpmaif.ko:dpmaif_bat_init.1045` (`0x2d364..0x2d398`: `w8=0x400`, `w1=0x2000`, `w10=0x680`, `w9=0x780`) | **Yes** (NOTE: `zinwa` fixed a BAT reallocation ordering bug in `ccci_hif_dpmaif_v1.c:1338-1360` — attach frag to `skb_shinfo` BEFORE reallocating BAT slot!) |
| **HW UL DRB Ring** | `2048` entries × `8 bytes` = `16,384 B` (`0x4000`), 4 HW TXQs | `ccci_dpmaif.ko:dpmaif_late_init.1003` (`0x2c7dc..0x2c7f8`: `w8=0x800`, `w1=0x4000`) | **Yes** |
| **SW UL TX Solution (`SMEM_USER_MD_DATA`)** | **ENABLED (`tx_sw_solution_enable = 1`)**: 3 UL queues (`DPMA_UL_QUEUE_NUM = 3`), `g_smem_drb_qsize = {1024, 1024, 1024}`, `DPMA_SKB_DATA_LEN = 1600 B`, inside **`SMEM_USER_MD_DATA` (`0x8e910000` / MD `0x40910000`, `5 MiB`)**, doorbell via **CCIF0 `ID_CCIF_USER_DATA = 0`** | `ccci_dpmaif.ko` `.rodata:0x72c0` (`0x400, 0x400, 0x400`), `ccci_hif_dpmaif_v1.c:5022-5191`, `ccci_hif_dpmaif_v1.h:27-39`, `ccif_hif_reg.h:88` | **Yes** |

#### B. Descriptor Structs (Advan X1 `dpmaif_ver = 1`)

1. **DL Normal PIT (`12 bytes`, `ccci_hif_dpmaif_v1.h:130-139`):**
```c
struct dpmaifq_normal_pit {
	u32 packet_type:1;   /* 0 = payload, 1 = msg */
	u32 c_bit:1;         /* 1 = more frags, 0 = last */
	u32 buffer_type:1;   /* 0 = pkt BAT, 1 = frag BAT */
	u32 buffer_id:13;    /* BAT index (0..1023) */
	u32 data_len:16;
	u32 p_data_addr;     /* DMA addr [31:0] */
	u32 data_addr_ext:8; /* DMA addr [39:32] */
	u32 reserved:24;
}; /* 12 bytes */
```
2. **DL Message PIT (`12 bytes` / `16 bytes` union, `ccci_hif_dpmaif_v1.h:149-163`):**
   - Word 0: `packet_type:1 (1)`, `c_bit:1`, `check_sum:2`, `error_bit:1`, `rsv:11`, `channel_id:8` (maps to `ccmniN` / `wwanN`!), `network_type:3` (`0`=IPv4? check `lhif_header`), `rsv2:5`.
   - Word 1: `count_l:16`, `flow:4`, `cmd:3`, `rsv3:9`.
   - Word 2: `reserved4:32`.
3. **DL BAT Entry (`8 bytes`, `ccci_hif_dpmaif_v1.h:168-172`):**
   - `{ u32 p_buffer_addr; u32 buffer_addr_ext:8; u32 reserved:24; }`.
4. **UL DRB Payload & Message Pair (`8 bytes` each, `ccci_hif_dpmaif_v1.h:273-299`):**
   - `dpmaif_drb_msg`: Word 0 `{ dtyp:2 (1=MSG), c_bit:1 (1), rsv:13, packet_len:16 }`, Word 1 `{ count_l:16, channel_id:8, network_type:3, r:1, ipv4:1, l4:1, rsv:2 }`.
   - `dpmaif_drb_pd`: Word 0 `{ dtyp:2 (0=PD), c_bit:1 (0=last), rsv:5, data_addr_ext:8, data_len:16 }`, Word 1 `{ p_data_addr }` (for SW UL TX, points to MD-view `0x40910000 + offset` inside `SMEM_USER_MD_DATA`!).
5. **`SMEM_USER_MD_DATA` (`0x8e910000`, `0x500000` B) SW UL Layout (`ccci_hif_dpmaif_v1.c:5022-5085`):**
   - Offset `0x000000`: `struct drb_queue_info qinfo[3]` (`{ u32 rd, wr, rel, sz; }` × 3 = `48 B`, `sz = 1024`).
   - Offset `0x000030`: `struct dpmaif_drb_pd drb_ring[3][2048]` (`3 × 1024 × 2 × 8 B = 49,152 B`; each packet uses 2 DRB slots: 1 `drb_msg` + 1 `drb_pd`).
   - Offset `0x00C030`: `u8 skb_data[3][1024][1600]` (`4,915,200 B`, MD-view `0x4091C030+`; total used = `4,964,400 B` of `5,242,880 B`).

#### C. Key DPMAIF v1 Register Map (`dpmaif_reg_v1.h`) vs Upstream `drivers/net/wwan/t7xx/` (`t7xx_reg.h`, `t7xx_dpmaif.h`)

| Aspect | MT6789 DPMAIF v1 (Advan X1) | Upstream `drivers/net/wwan/t7xx/` (T700 / PCIe Gen97+) | Impact on Mainline Driver Design |
| :--- | :--- | :--- | :--- |
| **Bus / Transport** | On-SoC platform MMIO (`0x10014000`, `0x1022c000..e000`), direct DRAM EMI | PCIe EP BAR (`DPMAIF_PORT_MODE_PCIE = BIT(30)`, `0x55` reserve reg) | Platform driver (`of_iomap` 7 bases), no PCIe/CLDMA/ATUs |
| **GIC IRQs** | **1 shared IRQ** (`GIC_SPI 241`, UL+DL+ERR) | **2+ MSI-X IRQs** (`DPMAIF_INT`, `DPMAIF2_INT`) | Single ISR dispatching NAPI poll + UL completion |
| **DL Queues / HPC** | **1 DL RXQ** (`DPMAIF_RXQ_NUM = 1`), **no HPC/DLQ** (`0x1022d600/900` absent) | 2 DLQs (`DPMAIF_RXQ_NUM = 2`) with MMW HPC hash aggregation | Much simpler single PIT ring (`PD_DL` `+0x20..0x34`) |
| **DL PIT Entry Size** | **12 bytes** (`dpmaifq_normal_pit`), 2048 entries | **16 bytes** (extra `pit_seq` / `h_bid` word), `DPMAIF_DL_PIT_SEQ_VALUE = 251` | Define `struct mtk_dpmaif_v1_pit` (3 × `__le32`) |
| **DL BAT Ring Size** | **1024 entries** × 8 B (`pkt_buf_sz = 1664 B`) | Dynamic / larger BAT + separate FRG BAT | Allocate 1024-entry BAT + 1664 B RX SKBs (or page pool) |
| **UL Data Path** | **SW TX via `SMEM_USER_MD_DATA` (`0x8e910000`, 3 queues × 1024 × 1600 B) + CCIF0 `ID_CCIF_USER_DATA=0` doorbell** (HW UL DRB regs at `0x1022d000` still initialized) | Pure HW UL DRB DMA (5 TXQs, `DPMAIF_UL_ADD_DESC_CH0..4`, `DPMAIF_AO_UL_CHNL0_CON0` in `PD_SRAM_UL`) | **Must implement `MD_DATA` SW UL ring** for TX to work on MT6789! |
| **Register Base Offsets** | `AO_UL=0x10014000`, `AO_DL=0x10014400` (`PKTINFO_CON0=+0x00`, `BAT_STA0=+0x10`, `PIT_STA0=+0x20`); `PD_UL=0x1022d000` (`CHNL0_CON0=+0x20`, `ADD_DESC=+0x00/+0xB0`); `PD_DL=0x1022d100` (`BAT_INIT=+0x00`, `BAT_ADD=+0x04`, `PIT_INIT=+0x20`, `PIT_ADD=+0x24`); `PD_MISC=0x1022d400` (`UL_L2TISAR0=+0x00`, `DL_L2TISAR0=+0x50`, `IP_BUSY=+0x60`); `PD_MD_MISC=0x1022c000` (`MD_L1TISR0=+0x1C` = `0x03`) | Same `0x10014000`/`0x1022d000` silicon heritage! `PD_DL` (`+0x00..0x24`) & `PD_MISC` (`+0x00, +0x50, +0x60`) match, **but** `t7xx` moved `AO_DL_PKTINFO/BAT/PIT` & `AO_UL_CHNL0` into `PD_SRAM_DL` (`+0xc00`) / `PD_SRAM_UL` (`+0xd00`) | Reuse `t7xx` NAPI/BAT/PIT state-machine architecture, with a clean MT6789 v1 register map header |
| **Zinwa Q25 Bug Warning** | `ccci_hif_dpmaif_v1.c:1338` in stock/MP01 reallocates FRG BAT slot *before* attaching frag to SKB (use-after-overwrite under load) | Upstream `t7xx` properly detaches/unmaps before refilling BAT | Model refill strictly after upstream `t7xx_hif_dpmaif_rx.c` |

---

## 2. Clean Mainline Phased Implementation Roadmap (Phases 5.1–5.5)

- **Phase 5.1 (DTS & Memory Map) — DONE (`0df2309`):** Added Advan X1's 9 exact `no-map` reserved-memory nodes (`0xdc000000`, `0xddc00000`, `0xde800000`, `0xe3200000`, `0x8e000000`, `0x8c000000`, `0xbf7f0000`, `0x8f000000`, `0xbf800000`), `MT6789_POWER_DOMAIN_MD` (`srclken-o1` / `power-flow-config`), and `modem` DT node in `mt6789.dtsi` + `mt6789-advan-x1.dts`. Verified on hardware (`deferred=none`, all 9 carveouts active).
- **Phase 5.2 (CCIF0 Core + Boot Handshake + Emulated SCP CCISM) — DONE (`e78b929`, `patches/04-peripherals/0026..0031`):** Complete CCIF0 ringbuffer transport, RPC responder (`0x4005..0x4014`), Gen95 CCB control header init, `/dev/ccci_fs` + `/dev/ccci_rpc` userspace bridges (`tools/ccci_fsd`, `tools/ccci_rpcd`), Emulated SCP CCISM Ring (`0x8e810000`), and HS1/HS2 handshake reaching **MD READY** with `+EIND: 128` and zero MD asserts/WDTs.
- **Phase 5.3 (RO NVRAM FS RPC + `WWAN_PORT_AT`) — DONE (`39bbf06`, `patches/04-peripherals/0032`):** Exposed `CCCI_UART2` (`ch 10/12`, Q5) as upstream `wwan_core` `WWAN_PORT_AT` (`/dev/wwan0at0`) backed by read-only ext4 + volatile tmpfs overlay `ccci_fsd`. Verified live bidirectional AT commands (`1.2 ms` RTT) and physical USIM read (`+ICCID`, `+ESIMAPP`, XL Axiata `510/11`).
- **Phase 5.4 (DPMAIF v1 + `MD_DATA` SW UL + `wwan0` Netdev) — DONE (`e2fcb28`, `patches/04-peripherals/0033`):** Clean upstream-styled `mtk_ccci_dpmaif` driver (12B PIT DL + 1024-entry BAT NAPI RX, `SMEM_USER_MD_DATA` 3-queue SW UL TX + CCIF0 `ID_CCIF_USER_DATA=0`, `CLK_IFRAO_CLDMA_BCLK` `"bclk"` + `CLK_IFRAO_DPMAIF_MAIN`, `wwan_register_ops()` `wwan0`). Hardware-verified live 4G LTE data (`DPMAIF v1 started` in `1.58 ms`, `ping 1.1.1.1` 4/4 18.9 ms RTT, exact 1:1 DMA parity, 0 errors/WDTs).
- **Phase 5.5 (ModemManager Integration) — DONE (`e959721`, `tools/modemmanager-mtk-ccci/`, `tools/mtk-wwan-connect/`):** Built and deployed `libmm-plugin-mtk-ccci.so` for Debian ModemManager 1.24.2 + udev rules targeting `/dev/wwan0at0` + `wwan0` + NetworkManager `Mobile LTE` auto-connect profile + standalone `mtk-wwan-connect` fallback. Hardware-verified full cold-boot auto-connect at `19.26 s`, live `systemd-timesyncd` NTP sync over 4G LTE, and clean shutdown teardown.

---

## 3. Verification Status

- **Phase 5.0–5.5 Complete:** End-to-end cellular control, NVRAM FS/RPC, USIM, DPMAIF v1 + SMEM UL 4G LTE IP data, and native ModemManager + NetworkManager integration are **100% hardware-verified** on physical Advan X1 (`e2fcb28` + `e959721`).
