# ADVAN X1 MAINLINE — 100% READ-ONLY GROUND-TRUTH & CROSS-REFERENCE DEVIATION AUDIT

**Document ID:** AX1ML-AUDIT-2026-11-25
**Date:** 2026-11-25
**Repository:** `/work/mainline/advan-x1-mainline` (branch `main`, HEAD `0497522`, 133 commits)
**Kernel baseline:** upstream Linux `v7.4-rc3/rc4`, pinned `666539eecdf4db623f5e3c3e55cf9f2dfad24aa4`
**Patch series:** 75 patches in `patches/01-clk..07-dts` + 2 optional in `patches-extra/`

---

## 0. Attestation, scope and authority hierarchy

### 0.1 Read-only attestation

No file, patch, DTS, daemon, commit, branch, ref, or config was created, edited, amended,
rebased, staged, or pushed during the audit. Only read operations were executed
(`git log/show/status`, `grep`, `read`, `strings`, `nm`, `objdump`, `modinfo`, ADB
`getprop`/`cat`/`dmesg`/`strings`). The document you are reading is the **only** new file
created, and it is untracked by explicit user request. Final attestation at audit time:

```
$ git -C /work/mainline/advan-x1-mainline status --porcelain=v1
(empty)
$ git -C /work/mainline/advan-x1-mainline log -1 --oneline
0497522 display: keep DSI video mode active across panel_unprepare for CRTC vblank drain
$ git -C /work/mainline/advan-x1-mainline rev-list --count HEAD
133
```

### 0.2 Authority hierarchy (strictly enforced)

1. **Tier A — Advan X1 authoritative ground truth (wins all board-level conflicts).**
   - Live stock Android 5.10.233 oracle at `127.0.0.1:5555` (`adb root`):
     `/proc/pl_lk`, `/proc/ccci_dump`, `/sys`, live `dmesg`, `expdb`, stock sysfs.
   - `/work/firmware/advan_x1/dtb/runtime_merged.dts` (decompiled stock DTB),
     `lk_main.dts`, `dtbo_00.dts`.
   - `/work/mainline/ref/advan-x1-oracle/modules/*.ko` (CCCI/DPMAIF/SPM/SCP kmods),
     `audio_modules/*.ko` (FS1599, MT6366, ACCDET, spk-amp), `fs1599.fsm`,
     stock userspace `bin/ccci_fsd`, `bin/ccci_rpcd`, `libmtk-ril.so`.
   - Stock logs: `ccci_dump.log`, `dmesg_ccci.log`, `radio.log`.
2. **Tier B — Motorola `mp01` reference (SoC architecture & wire protocols ONLY).**
   `/work/android-kernel/mp01-kernel` source, `mt6789.dts`, `k6789v1_64.dts`,
   `cust_*.dtsi`, Motorola kmods. A board constant that matches mp01 while
   contradicting Tier A is a **critical ODM contamination bug**.
3. **Tier C — our mainline tree & `git log`.** All 133 commits from scaffold `5207c88`
   through HEAD `0497522` (the "v7.4-rc4 through current HEAD" scope), 75 patches,
   both DTS files, `tools/ccci_*`, audio/UCM2, OTG/SD/NFC, S3 suspend.

### 0.3 Evidence sources and exact paths

| Tier | Source | Path / endpoint |
|---|---|---|
| A-live | Stock Android (root) | `adb -s 127.0.0.1:5555 shell …` |
| A-live | LK log | `/proc/pl_lk` |
| A-live | CCCI dump | `/proc/ccci_dump` |
| A-live | Crash/boot history | `/dev/block/by-name/expdb` (`strings`) |
| A-static | Merged stock DTB | `/work/firmware/advan_x1/dtb/runtime_merged.dts` |
| A-static | DTBO overlay | `/work/firmware/advan_x1/dtb/dtbo_00.dts` |
| A-static | Prebuilt kmods | `/work/mainline/ref/advan-x1-oracle/modules/*.ko` |
| A-static | Audio kmods | `/work/mainline/ref/advan-x1-oracle/audio_modules/*.ko`, `fs1599.fsm` |
| A-static | Stock daemons | `/work/mainline/ref/advan-x1-oracle/bin/ccci_fsd`, `ccci_rpcd` |
| B | mp01 kernel | `/work/android-kernel/mp01-kernel/` |
| C | Mainline DTS | `dts/mt6789.dtsi`, `dts/mt6789-advan-x1.dts` |
| C | Patches | `patches/01-clk` … `patches/07-dts` |
| C | Daemons/tools | `tools/ccci_fsd/`, `tools/ccci_rpcd/`, `tools/ucm2/` |

---

## 1. Executive summary

### 1.1 Verdict counts

115 recorded findings (some cross-dimension duplicates — e.g. thermal trips appear in
D-18 / D2-08 / D3-01; microSD CD in D-12 / D2-20).

| Verdict | Count | Definition |
|---|---:|---|
| **[CONTAMINATION]** | **3** | Matches Zinwa/Motorola instead of Advan authority (all low-risk provenance) |
| **[STOCK-DELTA-RISK]** | **14** | Differs from Advan stock/prebuilt `.ko` without full justification |
| **[UNVERIFIED]** | **27** | Borrowed from mp01/sibling SoC or assumed; not pinned to Tier A |
| **[INTENTIONAL-OK]** | **71** | Verified deliberate mainline improvement with no lost board erratum |

**No high-severity ODM contamination in any DTS register/IRQ/clock/regulator value.**
All 60+ reg bases, GIC SPI numbers/triggers, 31 mblock carveouts, the 80-byte CW2217
profile, charger electricals, PSCI idle states, panel/touch GPIOs and clock-gate indices
match Advan ground truth. Contamination findings are provenance-only.

### 1.2 Highest-risk triage list

| Priority | ID | Item | Proof / why it matters |
|---|---|---|---|
| **P0** | **D3-03** | LVTS PROTTC / STAGE-3 hardware reboot trip not programmed | `0009…patch:1366-1404` only writes `LVTS_MONINT=0`; `lvts_thermal.c:1071-1073` comments out `PROTTA/B/C`; stock `soc_temp_lvts.c:815-835,880-966` programs PROTTC threshold from 113500, max-of-4 `PROTCTL[17:16]=01`, and `STAGE3_INT_EN`. No HW reset if kernel hangs. |
| **P0** | **D3-18** | CCCI HS1/HS2 handshake 30 s → 5 s, FS-in-flight extension dropped | `0026:1385,1395` `msecs_to_jiffies(5000)`; stock `ccci_fsm_internal.h:155-156` `BOOT_TIMEOUT (30*1000)` and `ccci_fsm.c:432-435` `if (atomic_read(&ctl->fs_ongoing)) count = 0;`. |
| **P1** | **D2-09** | NTC / PMIC / consys thermal zones absent | `dtsi:1379-1735` 13 LVTS zones only; `runtime_merged.dts:710-760,772-962` has `ap_ntc`, `ltepa_ntc`, `mt6366_temp[0..3]`, `consys`; live `dmesg`/sysfs confirms all present. |
| **P1** | **D2-15** | FS1599 Re25/F0 impedance calibration missing | `0035:123-313` static init only; `snd-soc-fs1599.ko` has `fsm_cal_spkr_zmimp`, `fsm_set_re25_data`, `fsm_check_spkcoef`, `fsm_stub_pre/post_f0`; `runtime_merged.dts:6100` `scp_spk_process_enable=<0 0x04 0x0f 0x13>`. |
| **P1** | **D2-17** | ACCDET CDD thresholds + efuse/moisture fallback missing | `dts/...:584-597`, `0036:608-661` have no `nvmem`/CDD; `runtime_merged.dts:3806` CDD `<0 0x79 0xc0 0x258>`, `:3811-3812` efuse; `mt6358-accdet.ko` has `moisture_efuse`, `moisture_vdd`. |
| **P1** | **D2-22** | NFC `XO_NFC` clock buffer missing — NFC non-functional | `dts/...:453-460,1049-1057` no clock-buffer consumer; `runtime_merged.dts:4325-4332` `XO_NFC` index 2 enabled; live mainline `VP-4.5 NFC: dev= irq=none`. |
| **P2** | **D2-20** | microSD CD polarity contradicts Tier A and Tier B | `dts/...:352` `GPIO_ACTIVE_LOW`; `runtime_merged.dts:6229` `<pio 7 0>` = ACTIVE_HIGH; `cust_mt6789_msdc.dtsi:52` ACTIVE_HIGH. |
| **P2** | **F01/F06/F07** | Zinwa MM plugin; Motorola-cited TDDI/tps65132 alignment | `e959721`, `ce06c2e`, `7a2c78a` commit bodies; provenance/upstreaming debt only. |

### 1.3 Issues resolved by live Tier-A oracle (downgraded)

- **"125 °C thermal trips"** — not in HEAD. `f0902b6` raised them; `b553e64` restored
  exactly stock `113500 mC`. Live proof: `/sys/class/thermal/thermal_zone0/type=soc_max`,
  `trip_point_0_temp=113500`, `gpu2=85000`.
- **"OTG boost 5.15 V"** — not a delta. Live stock
  `/sys/class/power_supply/charger/voltage_max=5000000`; mainline `ti,boost-voltage=<5000000>`
  matches. The `sc,sc89601a,boost-voltage=<0x141e>`=5150 mV in stock DTB is the
  *alternate strapping* set; active `upm,upm6910,*` set is 5000 mV.
- **Charger address 0x6a vs 0x6b** — hardware-verified. Live stock binds
  `upm6910-charger 5-006b`; mainline initramfs probe logs `reg14@0x6a=0x64` (PN=4),
  `reg14@0x6b=NACK` on the same i2c5 bus; stock also has a separate `sc854x-charger@6A`
  on i2c7 (`11f00000.i2c`) which live logs show **NACK** — confirming two distinct ICs.
- **LVTS efuse offsets** — mainline `lvts-calib@1a4 + 0x34` is correct; stock/mp01
  `@1b4`/`@2f8` are logical `atag,devinfo` word indices, not physical MMIO. Live
  golden temp = 60, 13/13 zones accurate, 0 false critical trips.

---

## 2. Live Tier-A oracle evidence (raw proof)

### 2.1 Identity and boot state

```
$ adb connect 127.0.0.1:5555
connected to 127.0.0.1:5555
$ adb -s 127.0.0.1:5555 shell 'id; getprop ro.boot.bootreason; getprop sys.boot.reason;
                               getprop ro.build.version.release; cat /proc/version'
uid=0(root) gid=0(root) groups=0(root),1004(input),1007(log),1011(adb),…
2sec_reboot
reboot,userrequested
14
Linux version 5.10.233-android12-9-00062-g49c66df526b8-ab13101360
(clang r416183b) #1 SMP PREEMPT Thu Feb 20 17:25:36 UTC 2025
```

### 2.2 `/proc/pl_lk` (LK ground truth)

```
show buff sig 0xcdab3412, size 0x40000, pl size 0x7566, lk size 0x13173, last_boot step 0x4!
PL_LOG_STORE:set ram_header->sig = 0xABCD1234
[CPU_LL_FREQ] AD_ARMPLL_LL_CK = 1032 Mhz
[CCIPLL_FREQ] AD_CCIPLL_BL0_CK = 675 Mhz
INFRACFG_AO_INFRA_BUS_DCM_CTRL: 0x787FE0, … MODULE_SW_CG_5: 0x0
MMSYS_CONFIG_MMSYS_CG_0: 0xFE848040, MMSYS_CG_2: 0xFFFFFBFE
PMIF ULPOSC1 K done: 261M
[RGU] rst from: lk
```

### 2.3 Charger sysfs + dmesg (electrical proof)

```
/sys/class/power_supply/charger/manufacturer = SouthChip
/sys/class/power_supply/charger/current_max  = 500000
/sys/class/power_supply/charger/input_voltage_limit = 4300000
/sys/class/power_supply/charger/voltage_max  = 5000000

dmesg:
[   32.278719] upm6910-charger 5-006b: upm6910_do_event event = 1
[   32.287343] [upm6910]:upm6910_set_ivl: vindpm volt = 4300000
[   32.281726] upm6910_charger
[   32.281759] cw2217_battery
dmesg (i2c7 sc854x):
[  966.762170] i2c-mt65xx 11f00000.i2c: addr: 6a, transfer ACK error  (repeated)
```

### 2.4 Panel ground truth

```
dmesg:
[   32.281766] panel_fhdp_icnl9922_dsi_vdo_boe678_120hz
[   32.055941] [DISP]mtk_dsi_start_vdo_mode, vid_mode:3   (repeats at 2 s cadence → 120 Hz)
```

### 2.5 Thermal + idle states

```
/sys/class/thermal/thermal_zone0/type = soc_max
/sys/class/thermal/thermal_zone0/trip_point_0_temp = 113500
/sys/class/thermal/thermal_zone9/type = gpu2
/sys/class/thermal/thermal_zone9/trip_point_0_temp = 85000
/sys/power/mem_sleep = [s2idle]
/sys/devices/system/cpu/cpu0/cpuidle/state*/name =
    WFI cpuoff_l clusteroff_l mcusysoff system_mem system_pll system_bus s2idle
```

### 2.6 `expdb` modem mblock layout (strings, read-only)

```
ccci_lk_trace: mblock_alloc:sz=0xe10000, ali=0x2000000, low=0x50000000, lim=0x90000000
ccci_lk_trace: mblock_alloc result=>name=ap_md_nc_smem,0x8e000000,0x0
ccci_lk_trace: smem_info.base_addr: 8e000000
ccci_lk_trace: smem_info.total_smem_size: e10000
ccci_lk_trace: mblock_alloc:sz=0x16a0000, ali=0x2000000, …
ccci_lk_trace: mblock_alloc result=>name=ap_md_c_smem,0x8c000000,0x0
ccci_lk_trace: cache_buffer_addr: 8c000000
ccci_lk_trace: cache_buffer_size: 16a0000
ccci_lk_trace: cache_buffer_itm_cnt: 6
ccci_lk_trace: [CCCI] set_md_mpu_noncached_start_addr 0x40000000
ccci_lk_trace: error: 'mediatek,dfd_cache' not found!
```

Mainline `dts/mt6789-advan-x1.dts:150-156`:
```
md_c_smem: memory@8c000000 { reg = <0x00 0x8c000000 0x00 0x016a0000>; no-map; };
md_nc_smem: memory@8e000000 { reg = <0x00 0x8e000000 0x00 0x00e10000>; no-map; };
```
→ **exact parity** with live expdb; all 31 carveouts match `runtime_merged.dts:470-652`.

### 2.7 `/proc/ccci_dump`

```
[    8.235328] md_eint:src_pin, sizeof: 4, sim_info: 1, 1
[   10.295474] md_eint:debounce, sizeof: 4, sim_info: 50, 50
[   30.008983] CCIF start=0x2
```

---

## 3. Dimension 1 — DTS parity master table

### 3.1 Deviations

| ID | Verdict | Subsystem | Mainline File:Line (Commit) | Mainline value | Advan stock value/line | mp01 value | Impact / action |
|---|---|---|---|---|---|---|---|
| D-01 | [STOCK-DELTA-RISK] | vgpu rail | `dts/mt6789-advan-x1.dts:629-640` | 500000–950000 µV, boot-on, coupled | `runtime_merged.dts:3957-3964` `buck_vgpu` 500000–**1293750** (0x7a120–0x13bdb6) | `mt6366.dtsi:182-188` 500000–1293750 | Safe only because OPP ceiling 900 mV; document or restore. |
| D-02 | [STOCK-DELTA-RISK] | vsram_gpu | `dts/...:870-879` | 750000–1050000 µV, boot-on, spread 100000 | `runtime_merged.dts:4119-4125` 500000–1293750, no boot-on | `mt6366.dtsi:325-330` | Verify coupler range; no other consumer needs 1.29 V. |
| D-03 | [UNVERIFIED] | VMDDR | `dts/...:812-818` | 700000–750000, always-on | absent from stock regulators (`:3913-4323`) | absent | Confirm against PMIC dump/sysfs. |
| D-04 | [INTENTIONAL-OK] | charger address | `dts/...:498-500` (f6183d7) | `charger@6a` i2c5, REG14 PN=4 | `runtime_merged.dts:5290` `upm6910_chg@6b`; `:5703` `sc854x@6A` on i2c7 | none | Live-verified (§2.3). Keep; comment dual IC. |
| D-05 | [INTENTIONAL-OK] | charger electricals | `dts/...:505-511` | CV 4450000, ICHG 2000000, pre 480000, term 250000, boost 5000000/1200000 | `:8279` `battery_cv=0x43e6d0`=4450000; `:8306` `0x1e8480`=2000000; `:5299-5302` exact | n/a | Exact match to active set. |
| D-06 | [INTENTIONAL-OK] | LVTS efuse | `dtsi:789,1013-1015` (b553e64) | `lvts-calib@1a4` size 0x34 | `:6133-6141` `@1b4+0x2c`,`@2f8+0x08` (atag indices) | `mt6789.dts:3880` | Physical-MMIO fix; do not revert. |
| D-07 | [INTENTIONAL-OK] | touch | `dts/...:385-404,1037-1047` | SPI0@0, 3 MHz (`0x2dc6c0`), IRQ GPIO9 falling, reset GPIO107 active-low (`0x6b`), 1080×2460 (`0x438`×`0x99c`) | `dtbo_00.dts:1763-1789` exact | mp01 Goodix reset 152, y=2280 | Advan overlay match only. |
| D-08 | [INTENTIONAL-OK] | panel GPIO | `dts/...:1020-1027,1190-1193,532-553` | reset 85 (`0x55`), LDO18 150 (`0x96`), bias 108/109 (`0x6c/0x6d`), 5.8 V boot-on | `dtbo_00.dts:1030-1035` exact | `k6789v1_64.dts:94-95` pm-enable 12 | Advan-specific LDO18 preserved. |
| D-09 | [INTENTIONAL-OK] | NFC DT | `dts/...:453-461,1049-1059` | `nfc@8` i2c3, IRQ GPIO1 level-high, reset 106 (`0x6a`) | `:5241-5246`, `dtbo_00.dts:1340-1355` exact | n/a | DT parity OK; functional gap D2-22. |
| D-10 | [INTENTIONAL-OK] | TCPC | `dts/...:470-496` | `rt1711h@4e` i2c5, IRQ GPIO13 (`0x0d`) level-low, wakeup | `:5582-5593` exact | n/a | Keep. |
| D-11 | [INTENTIONAL-OK] | speaker amp | `dts/...:526-530` | `@34` i2c6, `foursemi,fs1599` | `:5669-5675` `speaker_amp@34` | mp01 `@5c rt5512` | Advan match. |
| D-12 | [INTENTIONAL-OK] | msdc pins | `dts/...:338-355,976-1018` | pins 71-76, drive 4, bias 101/102, 200 MHz (`0xbebc200`) | `:6207-6235` exact | same pins | CD polarity deviation → D2-20. |
| D-13 | [STOCK-DELTA-RISK] | SPI reg window | `dtsi:773,834,848,862,876,890` | size `0x1000` ×6 | `:4963,4983,5008,5018,5028,5038` size `0x100` | same style | Harmless; strict OF validation would flag. |
| D-14 | [UNVERIFIED] | arch timer PPI | `dtsi:274-277` | PPI 13/14/11/10 LEVEL_LOW, 13 MHz (`0xc65d40`) | `:1017-1021` type `0x04` LEVEL_HIGH | `mt6789.dts:1159-1166` LEVEL_HIGH | Upstream convention; leave unless spurious PPIs. |
| D-15 | [UNVERIFIED] | battery constants | `dts/...:28-35` | 5000000 µAh, min 3400000 µV | no capacity; `:4406` PMIC_MIN_VOL `0x82dc`=3350 mV | n/a | Verify vs gauge HAL. |
| D-16 | [UNVERIFIED] | backlight PWM | `dts/...:37-44` | period 39385 ns, 8 levels + 16 interp | `:6804-6812` base `1100e000`, IRQ SPI 203 (`0xcb`); period LK-set | n/a | Confirm vs live pwmchip duty. |
| D-17 | [UNVERIFIED] | USB2 PHY | `dtsi:1025-1032` | eye-vrt 4, eye-term 4, discth 7; no `rev4`/`rx_sqth`/`intr_cal` | `:7871-7885` same 3 **plus** `rev4=1`, `rx_sqth=5`, `intr_cal` efuse mask `0x1f` | n/a | Port missing cal if USB issues recur (#0009). |
| D-18 | [STOCK-DELTA-RISK] | thermal trips | `dtsi:1385-1734` | passive 85000, crit 113500, hyst 2000 on all zones + maps | `:782-787` only `soc_max`; `:846-861` only `gpu2` | same values | Expanded coverage changes behavior; document. |
| D-19 | [INTENTIONAL-OK] | GPU OPP | `dtsi:305-347` | 8 OPPs: 390@675k (`0xa4cb8`), 1100@900k (`0xdbba0`) | `:6278-6506` 45 OPPs, same endpoints | `mt6789.dts:4053-4235` | Exact stock-member subset. |
| D-20 | [STOCK-DELTA-RISK] | CMA | `dts/...:64-70` | 64 MiB (`0x04000000`) | `:654-659` 160 MiB (`0x0a000000`) | n/a | Policy; monitor allocation failures. |
| D-21 | [UNVERIFIED] | vsim1 | `dts/...:856-861` | min 1700000, max `0x2f4d60`=3100000 | `:4003-4009` min `0x00` | `mt6366.dtsi:222-227` same | Low risk; driver clamps. |
| D-22 | [UNVERIFIED] | vaud28 | `dts/...:707-712` | 2.8 V, no always-on | `:4267-4275` always-on | `mt6366.dtsi:453-459` always-on | Intentional saving; note if audio defers. |
| D-23 | [INTENTIONAL-OK] | vcn33 | `dts/...:827-832` | one rail 3.3–3.5 V | `:4227-4241` split bt/wifi, same range | same split | Cosmetic. |

### 3.2 Verified perfect parity (no deviation)

- **CPU topology/OPP control:** MPIDRs `0x000..0x700`, capacity 383/1024, perf-domain regs
  `0x11bc10`/`0x11bd30` size `0x120` (`dtsi:355-360` vs `runtime_merged.dts:2086-2091`).
- **SPM:** all 9 PSCI params `0x10001, 0x10002, 0x1010101, 0x1010102, 0x1010701,
  0x1010f01, 0x1010f02, 0x1010f03, 0x1011f01` (HEAD uses `0x01011f01` for s2idle),
  latencies/residencies byte-identical (`dtsi:183-262` vs `:220-308`). Live §2.5.
- **All SoC reg bases + GIC SPIs:** 60+ nodes — SPI 106, 110, 128, 135, 137, 141,
  144-153, 191-196, 201, 203, 226-227, 241, 244, 252, 285, 287, 289-297, 300, 301,
  303, 396-398 — all numbers and trigger flags exact.
- **Clock gates:** mmc1 `21/21/28`, LVTS `4`, DPMAIF `55`, UFS `47/43/42/41/48/38/39`,
  I²C `imp_wrap`/`imp_wrap_c` + infracfg `61`.
- **Reserved memory:** all 31 mblocks exact (`0x48080000`…`0xfe007000`), 8 GiB node.
- **CW2217:** 80-byte profile byte-for-byte vs `runtime_merged.dts:5111` `batinfo`.
- **Display:** DSI/DSC/mmsys/larb/ovl/rdma/color/aal/gamma/postmask/dither/IOMMU regs + SPIs
  285,287,289,291,292,294-297,300,301,303 exact.
- **UFS/msdc1:** `0x11270000/0x2300`, `0x11240000/0x1000`+`0x11ef0000/0x1000`,
  IRQs 137/135, pins 71-76, drive 4, bias 101/102, 200 MHz, vmch/vmc rails exact.
- **GPU:** `0x13000000/0x4000`, IRQs job=398/mmu=397/gpu=396.
- **LVTS:** `0x1100b000/0x1000`, SPI 201, clock 4.
- **PMIC:** IRQ GPIO 137 level-high, ACCDET 15-cell PWM array + 3/4-key arrays exact.
- **Modem:** CCIF/DPMAIF regs, IRQs 110/226/227/241, `srclken-o1=0x200000`,
  `power-flow-config=0x12`, SMEM mapping exact.
- **Patch sync:** `patches/07-dts/0002`/`0003` `+` lines byte-identical to both DTS files.

---

## 4. Dimension 2a — Modem / CCCI master table

| ID | Verdict | Item | Mainline File:Line | Mainline value | Advan X1 stock proof | mp01 ref | Impact / action |
|---|---|---|---|---|---|---|---|
| D1 | [INTENTIONAL-OK] | CCIF0 offsets | `0026:128-137` | CON 0x0, BUSY 0x4, START 0x8, TCHNUM 0xc, RCHNUM 0x10, ACK 0x14, IRQ0/1 0x20/0x24, CHDATA 0x100 | `runtime_merged.dts:4752-4760` | `ccci_hif_reg.h:30-38` | Keep. |
| D2 | [STOCK-DELTA-RISK] | CCIF SRAM size | `0026:139`; `dtsi:731-747` | hardcoded 512 B, footer CHDATA+512-12/-8/-4 | `:4662` `mediatek,sram_size=<0x200>` | DT-driven | Expose DT prop w/ 512 fallback. |
| D3 | [INTENTIONAL-OK] | HS1/HS2 Q15 | `0026:141,168-169,854-1101` | `ICCC`=0x49434343, `CCCI`=0x43434349 | `ccci_ccif.ko` HS1 strings; live `CCIF start=0x2` (§2.7) | same | Keep. |
| D4 | [INTENTIONAL-OK] | CCIF IRQs | `dtsi:736-737` | SPI 226/227 LEVEL_HIGH | `:4663` `<0xe2 4><0xe3 4>` | same | Exact. |
| D5 | [INTENTIONAL-OK] | MD WDT IRQ | `dtsi:377-378` | SPI 110 EDGE_RISING | `:4689` `<0x6e 1>` | same | Exact. |
| D6 | [STOCK-DELTA-RISK] | CCIF reset ver | `0026:120-123,545-571` | hardcoded 0: RST3_SET 0x150 BIT8 | `ccci_ccif.ko` reads `mediatek,ccif_hw_reset_ver` (absent → 0) | mp01 fn | Behavior equal; document. |
| D7 | [INTENTIONAL-OK] | DPMAIF regs | `dtsi:702-719`; `0033:264-344` | 7×0x1000: `10014000`,`10014400`,`1022d000`,`1022d100`,`1022d400`,`1022c000`,`1022e000` | `:4648` identical | `dpmaif_reg_v1.h` | Exact. |
| D8 | [INTENTIONAL-OK] | PIT/BAT/FRG | `0033:356-368,1469-1498` | BAT 1024×8 B, PIT 2048×12 B, PKT 1664, FRG 1920, UL 3×1024 | `.ko` `bat_init` 2048/1024/1664/1920 | mp01 3584 **not used** | Advan values used, not mp01. |
| D9 | [INTENTIONAL-OK] | Coalescing | `0033:1290-1342` | BAT_REMAIN 64, CHK_PIT 6, CHK_BAT 3, CHK_FRG 3 | stock-derived | `ccci_hif_dpmaif.h:31,56-58` | Keep. |
| D10 | [INTENTIONAL-OK] | DPMAIF IRQ | `dtsi:713` | SPI 241 (`0xf1`) LEVEL_HIGH, shared UL+DL | `:4650` `<0xf1 4>` | same | Live LTE verified. |
| D11 | [INTENTIONAL-OK] | NC_OFF map | `0026:223-262`; `0030:103`; `0031:352-358` | MDSS 0x00800800, CCB 0x00818000, CCISM 0x00810000/0x00829000, DRDI 0x00900000, DATA 0x00910000 (5 MiB) | bringup plan + `.ko` symbols | mp01 region table | Exact. |
| D12 | [INTENTIONAL-OK] | RPC `0x4002` | `0027:478,921-934` | GPIO-num lookup | `.ko` `mov w9,#0x4002` @`0x1c968`, branch @`0x1d13c` | mp01 enum | Disassembly-confirmed. |
| D13 | [INTENTIONAL-OK] | RPC `0x4005` | `0027:481,893-919` | EINT attr lookup | `.ko` `mov w5,#0x4005` @`0x1d848`, `get_eint_attr_DTSVal` | same | Confirmed (live §2.7 debounce=50). |
| D14 | [INTENTIONAL-OK] | RPC `0x400f` | `0031:125` | AP sys-property pass-through | stock `ccci_rpcd` string; kernel `.ko` absent | — | userspace-only op. |
| D15 | [INTENTIONAL-OK] | RPC `0x4010` | `0031:126` | SAR table idx | stock `ccci_rpcd` + `ccci_util_lib.ko` 2× LE 0x4010 | — | Confirmed. |
| D16 | [INTENTIONAL-OK] | RPC `0x4014` | `0031:129,150` | DRDI copy | stock `ccci_rpcd` DRDI handlers | — | Confirmed. |
| D17 | [INTENTIONAL-OK] | RPC `0x4015` | `0031:130` | save MD capid | stock `ccci_rpcd` `md_capid 0x%x` | — | Confirmed. |
| D18 | [INTENTIONAL-OK] | NVRAM overlay | `tools/ccci_fsd/ccci-fsd-launch.sh:34,46,78,92,99,105,110-132` | `blockdev --setro`+`getro==1`, `ro,noload` lower + tmpfs upper, drives Z..S, 7 partitions | stock RW `/mnt/vendor/nvdata/md` etc. | — | Safety improvement; keep fail-closed. |
| D19 | [UNVERIFIED] | CCB guard | `0030:7-13,102-126` | 20×64 B @`0x818000`, guard `0xeeff0011` | `.ko` has CCB symbols; guard constant not found | mp01 has guard fields, no constant | HW-verified end-to-end; pin source. |
| D20 | [INTENTIONAL-OK] | `/dev/ccci_rpc` mmap | `0031:346-372` | 64 KiB DRDI, WC, `vm_pgoff==0` | `.ko` `port_rpc_dev_mmap`, `remap drdi smem addr:0x%llx` | same | Keep. |
| D21 | [INTENTIONAL-OK] | AT port | `0032:6-10,115,180,314-324` | CCIF0 Q5, ch10 RX/ch12 TX | `ccci_dump.log:8868-8932` ch10/12 | — | Live AT/USIM verified. |
| D22 | [INTENTIONAL-OK] | SMEM UL doorbell | `0033:984-1035,1116-1119` | `PD_MD_L1TISR0+0x1c = 3`, CCIF IRQ1 bit 23 | live data traffic | `dpmaif_reg.h:193` | Code correct; doc error D26. |
| D23 | [INTENTIONAL-OK] | Wake IRQs | `0039:45-86,108-115` | wdt, ccif0/1, DPMAIF `enable_irq_wake` | `.ko` identical | mp01 v1 | No erratum lost. |
| D24 | [INTENTIONAL-OK] | MD MTCMOS | `0026:1254-1380` | LK ON, RCHNUM guard, OFF→rtbn→ON→BOOT_UP | LK `ccci_md_power_on` | mp01 | Guard avoids TOP_AXI hang. |
| D25 | [INTENTIONAL-OK] | EL3 SMC | `0026:103-108,477-489`; `ccci-sip-abi.md:20-70` | `MTK_SIP_KERNEL_CCCI_CONTROL=0xC2000505`, req 5/6/7/13 | Advan `tee.img` disassembly | mp01 IDs | Verified. |
| D26 | [UNVERIFIED] | Doc doorbell wording | `docs/ccci-bringup-plan.md:383,423` | claims `ID_CCIF_USER_DATA=0` doorbell | real doorbell DPMAIF `PD_MD_L1TISR0` | `ccci_hif_reg.h:88`=callback index | Doc-only; fix sentence. |
| D27 | [INTENTIONAL-OK] | DPMAIF `bclk` | `dtsi:714-716` (e2fcb28) | +`CLK_IFRAO_CLDMA_BCLK` | stock DT only `infra-dpmaif-clk` | Zinwa 1 clock | Fixed 1.68 ms init. |

---

## 5. Dimension 2b — Power / battery / thermal / display / touch / audio / storage

| ID | Verdict | Subsystem | Mainline File:Line (Commit) | Mainline value | Advan stock proof | mp01 ref | Impact / action |
|---|---|---|---|---|---|---|---|
| D2-01 | [INTENTIONAL-OK] | Charger ID | `dts/...:499`; `0015:37,97` | `0x6a`, PN=4 (REG14 `0x64`) | live mainline pstore; live stock `5-006b` | none | HW-verified (§2.3). |
| D2-02 | [INTENTIONAL-OK] | BC1.2/HVDCP | `0015:76-90,108-111` | skip REG_RST; clear HVDCP/DPDM bits | `.ko` `upm6910_chg_bc12_work_func`, hidden mode | — | Caveat: HVDCP bricks stay 5 V. Verify if 9 V expected. |
| D2-03 | [INTENTIONAL-OK] | Charger limits | `dts/...:500-511` | 4450000/2000000/250000/480000/5000000/1200000 | `:5297-5302`, `:8279`, `:8306` exact | — | Live VBUS 5.0 V confirms. |
| D2-04 | [INTENTIONAL-OK] | CW2217 | `dts/...:28-35,413-441` | 5000000 µAh, shunt 5000 µΩ, 80-byte profile | `:5111` byte-identical; `:5714` `0x05` mΩ | — | Keep. |
| D2-05 | [INTENTIONAL-OK] | LVTS efuse | `dtsi:1013-1015` | `@1a4+0x34` | `:6133` atag cells | `mt6789.dts:3880` | Live golden temp 60. |
| D2-06 | [INTENTIONAL-OK] | LVTS RCK | `0009:45,70-85,199` | count_rc 2750, delta 1061 | stock LK/V5 HW | `mtk_lvts_tc_mt6893.c:324,855` | Wire/algorithm only. |
| D2-07 | [INTENTIONAL-OK] | LVTS filter | `0008:15-25`,`0009:263` | skip zero, 4×EAGAIN, settle 2 ms | 13 sensors filtered | `mt6789.dts:872+` 14 | Keep; see D2-09. |
| D2-08 | [INTENTIONAL-OK] | Trip curves | `dtsi:1385-1407,1623-1732` | crit 113500, hyst 2000, passive 85000 | `:783` `0x1bb5c`=113500; `:846` `0x14c08`=85000 | `mt6789.dts:876,930` | 125 °C reverted by b553e64. |
| **D2-09** | **[STOCK-DELTA-RISK]** | Thermal zones | `dtsi:1379-1735` | 13 LVTS only | `:710-760` NTC tables −40…+125 °C; `:772-962` `ap_ntc`, `ltepa_ntc`, `mt6366_temp`, `consys`; live §2.5 | `mt6789.dts:647,821,964-994` | Add ADC-thermal NTC + PMIC + consys before LTE TX/fast charge. |
| D2-10 | [INTENTIONAL-OK] | S3 SPM | `dtsi:67-262`; `0039` | 9 states, s2idle `0x01011f01` | `:222-306` identical | same | Live PWR_STATUS `0xF83A1BC1`. |
| D2-11 | [UNVERIFIED] | DSC PPS | `0007:91-145`,`0005:73-205` | 1080×2460, 2×540, slice_h 20, delay 512, chunk 540 | `.ko` has `ext_params_60/90/120`; DSC blob not string-recoverable | — | Check 120 Hz flicker/banding. |
| D2-12 | [INTENTIONAL-OK] | Panel init | `0007:194-410` | `f0 99 22 0c 70 c1 12 00 …` | `.ko .rodata@0x30805` byte-identical | — | Keep. |
| D2-13 | [INTENTIONAL-OK] | Touch FW | `0019:93-121,536-635,914-934` | HWID `0x99c220`, CRC32, `fw_ver 0x0104` | `chipone-tddi.ko`, live `0x0104` | — | Keep. |
| D2-14 | [INTENTIONAL-OK] | FS1599 init | `0035:93,123-160,334` | DEVID mask `0x00ff`/`0x0021`, 8-bit reg/16-bit BE, 24 entries | `.ko` v4.0.6 + `fs1599.fsm` same | — | Keep. |
| **D2-15** | **[STOCK-DELTA-RISK]** | FS1599 cal | `0035:123-313` | no `re25`/`zmimp`/`F0`/`spkcoef` | `.ko`: `fsm_cal_spkr_zmimp`, `fsm_set_re25_data`, `fsm_stub_pre/post_f0`; `:6100` SCP spk process | — | Overdrive/voice-coil heating risk. Port calibration. |
| D2-16 | [INTENTIONAL-OK] | ACCDET | `dts/...:584-597`; `0036:30-34,611-661` | mic-vol 6, 15-cell PWM, 3-key `0,80,220,400`, 4-key `0,58,121,192,400` | `:3797-3812` exact | — | Keep. |
| **D2-17** | **[STOCK-DELTA-RISK]** | ACCDET CDD | same | no CDD/nvmem/moisture | `:3806` `<0 0x79 0xc0 0x258>`; `:3811-3812` efuse; `.ko` moisture | — | Add CDD + efuse fallback. |
| D2-18 | [UNVERIFIED] | UCM2 | `HiFi.conf:1-58`; `0037:60-115` | AIF/Mic muxes, aux-devs | no UCM2 in Tier A; stock FSM/SCP | — | Verify speaker/headset switch on HW. |
| D2-19 | [INTENTIONAL-OK] | UFS | `dtsi:980-997`; `dts/...:358-360` | MCQ off, vemc 2.9–3.3 V, 7 clks/3 resets | `:6162-6175` exact | mp01 rail name | Keep (#0004 fixed). |
| **D2-20** | **[STOCK-DELTA-RISK]** | microSD CD | `dts/...:352` (f6183d7) | `GPIO_ACTIVE_LOW` | `:6229` `<pio 7 0>` ACTIVE_HIGH | `cust_mt6789_msdc.dtsi:52` ACTIVE_HIGH | Test with card; revert if undetected. |
| D2-21 | [UNVERIFIED] | RT1711H PD | `dts/...:471-496` | sink PDO `0x06432000` (5 V/2 A), source `0x06419000` | `:5582-5605` `0x19032`/`0x190c8` | mp01 same | Verify 3 A advert/swap/VCONN. |
| **D2-22** | **[STOCK-DELTA-RISK]** | NFC clock | `dts/...:453-460,1049-1057` | no `XO_NFC` buffer | `:4325-4332` XO_NFC idx 2 enabled; `st21nfc.ko` `clk_buf_ctrl` | — | Live `dev= irq=none`; NFC dead. Port clock-buffer consumer. |
| D2-23 | [INTENTIONAL-OK] | disp_iommu | `dtsi:1232-1259,1282-1300,1370-1378` | larb0/1 + iommu `14016000` | `:6570-6687` | — | 0 M4U faults live. |
| D2-24 | [INTENTIONAL-OK] | GPU OPP | `dtsi:305-347` | 8 OPPs to 1100@900 mV | `:6278+` 44 OPPs, same endpoints | — | No cap dropped. |
| D2-25 | [INTENTIONAL-OK] | GPU supplies | `dts/...:629-640,870-879,936-947`; `0005` | vgpu/vsram_gpu coupler, mux park | `:6252-6275` gpufreq rails | mp01 mux parking | Live 675 mV@390 MHz. |
| D2-26 | [UNVERIFIED] | GPU efuse | `dtsi:807-822`; `0002` | no nvmem cell | `:6256-6258` `efuse_segment@78` | — | Dump `0x11c10078`. |
| D2-27 | [UNVERIFIED] | Patch hygiene | `0009:15,306` | adds 2238-line `lvts_thermal.c.orig` | — | — | Not built; delete before submission. |

---

## 6. Dimension 3 — Android 5.10 GKI vs mainline 7.4 intentional deltas

| ID | Verdict | Delta | Mainline File:Line (Commit) | Mainline | Stock 5.10 | Erratum/safety | Action |
|---|---|---|---|---|---|---|---|
| D3-01 | [INTENTIONAL-OK] | Critical trips | `dtsi:1392-1730` (b553e64) | 113500/2000 ×13 | `:783` 113500 | preserved | Live §2.5; CI assert no 125000. |
| D3-02 | [INTENTIONAL-OK] | Passive/maps | `dtsi:1386-1404,1626-1731` | 85000 + CPU/GPU maps | `:846` 85000, contribution `0x400`=1024 | strengthened | Keep. |
| **D3-03** | **[STOCK-DELTA-RISK]** | LVTS HW reboot | `0009:1366-1404`; `lvts_thermal.c:1047-1083,1282` | `MONINT=0`; PROTTA/B/C commented; no STAGE3 | `soc_temp_lvts.c:815-835,880-966` PROTTC from 113500, max-of-4, STAGE3_INT_EN | **MISSING** | Backport `set_all_tc_hw_reboot()`. **P0.** |
| D3-04 | [INTENTIONAL-OK] | efuse shape | `dtsi:789,1013-1015` | 1 physical cell | 2 logical atag cells | preserved | Add comment. |
| D3-05 | [UNVERIFIED]→resolved | OTG boost | `dts/...:510-511` | 5000000/1200000 | sc 5150 / upm 5000 | live-verified | No action (§1.3, §2.3). |
| D3-06 | [INTENTIONAL-OK] | GPU DVFS | `dtsi:305-346`; `0002` | 8-step + mux park | 45-step, max 1100@900 mV | ceiling preserved | Never exceed 900 mV. |
| D3-07 | [INTENTIONAL-OK] | disp_iommu | `dtsi:1277-1287,1367-1374` | mtk-iommu + larb | 60+ M4U ports | preserved | 0 swiotlb faults. |
| D3-08 | [INTENTIONAL-OK] | pm_runtime | `325f990`, `3b1f69f` | no critical flags | CLK_IS_CRITICAL/always-on | preserved | Keep leak check. |
| D3-09 | [INTENTIONAL-OK] | panel follower | `0019:783-813`; `dts/...:386-389` | follower sync | vendor TDDI daemon | no erratum | Keep. |
| D3-10 | [INTENTIONAL-OK] | RO NVRAM | `ccci-fsd-launch.sh:34-50,61,78-118` | RO + tmpfs | RW ext4 | improved | Verify `--setro` post-switch_root. |
| D3-11 | [INTENTIONAL-OK] | wwan_core | `0026/0028/0031/0032/0033` | wwan0/at0 + bridges | ttyC0/ccmni/ccci_fs | EE SHM kept | Keep bridges. |
| D3-12 | [INTENTIONAL-OK] | No bootargs crutches | `configfrag:3`; `boot/*` | none | needed crutches | preserved | HEAD clean. |
| D3-13 | [INTENTIONAL-OK] | U-Boot FIT | `tools/pack_uboot_boot.py` | Loki→U-Boot | LK direct | LK MD state kept | Keep no-power-off rule. |
| D3-14 | [INTENTIONAL-OK] | mtk-drm | `0004-0008` | full DDP + DSC 1.1 | vendor HAL | preserved | 0.8 ms DSI bind. |
| D3-15 | [UNVERIFIED] | DSC init | `0007:91-122,194-410`; `dts/...:1179-1201` | hardcoded PPS | panel `.ko` blob | no evidence of loss | Diff vs `lk.img` if cold blank. |
| D3-16 | [UNVERIFIED] | SD VCORE | `dts/...:338-356,984-1016` | no `req-vcore` | `:6233-6234` `req-vcore=650000 µV` | **possibly missing** | Add 650 mV vote or SDR104 test. |
| D3-17 | [INTENTIONAL-OK] | UFS | `dtsi:980-997`; `dts/...:358-360` | MCQ off, vemc | same rail | preserved | Keep. |
| **D3-18** | **[STOCK-DELTA-RISK]** | CCCI handshake | `0026:1385,1395` | 5000 ms ×2 | `ccci_fsm_internal.h:155-156` 30000 ms; `ccci_fsm.c:432-435` FS no-timeout | **dropped** | Restore 30 s + FS extension or add retry. **P0.** |
| D3-19 | [INTENTIONAL-OK] | NVRAM RO | `ccci-bringup-plan.md:23-30,433` | destructive ops rejected | RW writes | improved | Keep. |
| D3-20 | [INTENTIONAL-OK] | S3 s2idle | `dtsi:180-265` | PSCI byte-identical | 9 states | preserved | Keep WIP until operator S3 test. |
| D3-21 | [UNVERIFIED] | FS1599 driver | `0035:124-148,250-260,346-352` | hardcoded init/shutdown | `.fsm` calibrated blob | no explicit limit | Unverified, not safety delta. |

---

## 7. Dimension 4 — `git log` hygiene & provenance

### 7.1 Findings table

| ID | Verdict | Commit / File:Line | Exact quote | Finding | Action |
|---|---|---|---|---|---|
| **F01** | **[CONTAMINATION]** | `e959721` + `tools/modemmanager-mtk-ccci/mm-plugin-mtk-ccci.c:6` | "Adapted for the mainline Linux WWAN subsystem from Zinwa Q25's mm-plugin-mtk-ccci.c (GPL-2.0-or-later)." | Zinwa sibling source despite `README.md:109-112` policy. Userspace, HW-verified. | Keep attribution; note re-verification vs `radio.log`/`libmtk-ril.so`; list dropped Q25 constants. |
| F02 | [UNVERIFIED] | `93e4b56` | "align MT6789 DSI ULPS exit, MIPI-TX handoff, and DISP_26M with mp01-kernel" | mp01-sourced; `7e8db92` later changed values from live stock → drift proven. | Re-audit survivors vs Tier A; cite stock. |
| F03 | [UNVERIFIED] | `a15a79d` | "Fix reg_shadow_dbg_off from 0xc00 to 0x190 matching mp01-kernel DSI_SHADOW_DEBUG" | Whole MIPI-TX fix justified by mp01; later corrected live. | Mark surviving values stock-verified. |
| F04 | [UNVERIFIED] | `e204148` | "34-bit IOVA / 35-bit PA with 32-bit TTBR matching mp01 and MT6893/MT8192" | IOMMU geometry from siblings; HW-tested (0 faults). | Add stock-DTB/live citation. |
| F05 | [UNVERIFIED] | `9eadefc` | "PWRAP_INT_EN (0x450) … matching downstream mp01" | PWRAP offsets mp01-only; later doc calls for "fixed MT6789 pwrap register map". | Cross-check offsets vs Advan `.ko`/DTS. |
| **F06** | **[CONTAMINATION]** | `ce06c2e` | "to match Motorola chipone_tddi_mmi_v2 (cts_platform.c:643)" / "Align icnl9922c_verify_tcs_rx() with Motorola cts_tcs.c:311-323" | Motorola as alignment target; stock `.ko` also cited. | Make Advan `.ko` authority; drop Motorola-only quirks. |
| **F07** | **[CONTAMINATION]** | `7a2c78a` | "matching upstream ti,tps65132 boards (qcom-msm8926-motorola-peregrine, falcon)" | LCD-bias DT props justified by Motorola boards. | Cite Advan `runtime_merged.dts` GPIO/enable. |
| F08 | [INTENTIONAL-OK] | `1042eda` | "matching stock chipone-tddi.ko … and Motorola cts_core.c" | Stock named first; actual authority. | Optional note. |
| F09 | [INTENTIONAL-OK] | `bfb654a`,`4c1800f`,`9113a5b`,`4673679` | "mp01-kernel … and stock-diagnose.log revealed three hardware mismatches" | mp01 paired with stock oracle. | Preserve citation. |
| F10 | [INTENTIONAL-OK] | `3b1f69f` | "remove Q25/Zinwa DTS comment, replace all 10 author YAML maintainers" | Removes contamination. | None. |
| F11 | [UNVERIFIED] | `612ecef` + `patches/04-peripherals/0004-i2c-mt65xx-Add-MT6789-I2C-support.patch:6,22` | "Map mediatek,mt6789-i2c to mt8188_compat … dma_sync = 0" | MT8188 borrowed; `1ac07e6` used mp01 `dma_sync=1`; flipped on analogy. | Verify vs stock `i2c-mt65xx.ko`. |
| F12 | [INTENTIONAL-OK] | `325f990` | "strip early bring-up hacks … drop arbitrary -20000..115000 clamp … drop hardcoded bootargs and fake spidev5" | Verified at HEAD. | None. |
| F13 | [INTENTIONAL-OK] | `869e76b`→`79100e0` | "add clk_ignore_unused" → "revert(dts): remove clk_ignore_unused" | Crutch reverted; HEAD clean. | None. |
| F14 | [UNVERIFIED] | `5207c88` | "Volla mt8781 vendor clk files … no configs borrowed from alien boards" | Root vendored alien files; removed by `8d192e9`. | Note in provenance narrative. |
| F15 | [UNVERIFIED] | `configfrag:158-160`; `initramfs/init:282-284,550-553`; `e83bc05`,`9465fc0` | `CONFIG_STRICT_DEVMEM=n`, `CONFIG_IO_STRICT_DEVMEM=n`, `peek()/poke() { devmem … }` | `/dev/mem` crutch still at HEAD. | Gate behind debug config or remove. |
| F16 | [UNVERIFIED] | `patches-extra/0001-input-touchscreen-Add-Chipone-TDDI-touchscreen-driver.patch:15977,23141` | "TODO: If system will issure enable/disable command …" / "copied from fs/namei.c" | Dead scaffolding, excluded upstream. | Delete TODO; verify GPL attribution. |
| F17 | [UNVERIFIED] | `patches/04-peripherals/0023-regulator-tps65132-Ensure-I2C-wakeup-and-sync-enable.patch:17` | "instead of always assuming 0" | Documents former unverified default. | No code action. |

### 7.2 Stale comments / dead scaffolding / history-vs-HEAD drift

1. **`34cd9ee`** — "revert MFGCFG/mux-notifier/assigned-clock-parents"; HEAD still has
   `patches/01-clk/0005…patch` (re-added `313926a`) and a different
   `mt6789_mfg_mux_notifier_cb` (`0002:1197,1280`, re-added `3b1f69f`).
2. **`dts/mt6789.dtsi:8-10`** — "P1 scope: … UFS/USB/mphy/scpsys nodes land in P2-P4";
   HEAD already carries all of them. (Line 6 stock-provenance claim is accurate.)
3. **`README.md:13,46,52`** — "60 upstream-ready patches"; actual 75 (+2 optional).
4. **`docs/upstream-status.md:14,20,23,39,42`** — "71 Upstream Patches", `04-peripherals/37`,
   `06-display/8`, "across all 60 patches"; internally inconsistent with 75.
5. **`README.md:29-31,38-39`; `docs/hardware-status.md:3,38,48,164,172-179`** — 8 GiB RAM,
   Gen95 modem, speaker amp/ACCDET, S3 suspend marked WIP/not-started though all implemented
   and HW-verified through HEAD; "Last hardware-verified commit: e959721" predates 5 later.
6. **`docs/ccci-sip-abi.md:95`** — "HS1 handshake WIP" contradicts
   `docs/ccci-bringup-plan.md:3` (Phase 5 COMPLETE) and `e2fcb28`.
7. **`scripts/build-legacy-reference.sh:2-5,40`** — dead paths `/re/workspaces/...`,
   `/opt/llvm-r30/bin`, wrong baseline `7.4.0-rc2`.
8. **`legacy/advan_x1-linux.dts:24-28,193,294`; `legacy/initramfs/boot24-init.sh:2`** —
   `bootargs … clk_ignore_unused pd_ignore_unused`, "programming assumed in EL1 handoff",
   "no-devmem experiment". Archival only; "nothing here ships" (`README.md:72`).
9. **`initramfs/init:550-553`** — `peek()/poke()` + `STRICT_DEVMEM=n` remain (F15).

### 7.3 Signing

133/133 commits carry GPG signatures. 131 verify with current key
`516AFC6F76455F1D385CA713D9FE17BA643CA835`. 2 history-root commits
(`5207c884e7a106a3dd95cba9c908d913e6a649e0`, `31136210faab44c383cd06440bf62a06d87299e8`)
are signed with an old, locally unavailable EDDSA key
`EE125A8C9B304AE14850F32D1029AF2348D4BAE4`. No unsigned commits.

---

## 8. Dimension 5 — Classification ledger

| Verdict | IDs | Count |
|---|---|---:|
| [CONTAMINATION] | F01, F06, F07 | 3 |
| [STOCK-DELTA-RISK] | D-01, D-02, D-13, D-18, D-20, D2, D6, D2-09, D2-15, D2-17, D2-20, D2-22, D3-03, D3-18 | 14 |
| [UNVERIFIED] | D-03, D-14, D-15, D-16, D-17, D-21, D-22, D19, D26, D2-11, D2-18, D2-21, D2-26, D2-27, D3-05, D3-15, D3-16, D3-21, F02-F05, F11, F14-F17 | 27 |
| [INTENTIONAL-OK] | all remaining (D-04..D-12, D-19, D-23, D1, D3-D5, D7-D18, D20-D25, D27, D2-01..D2-08, D2-10, D2-12..D2-14, D2-16, D2-19, D2-23..D2-25, D3-01, D3-02, D3-04, D3-06..D3-14, D3-17, D3-19, D3-20, F08-F10, F12, F13) | 71 |

---

## 9. Appendix A — decoded hex constants (proof of parity)

| Stock DTS / log value | Decimal | Mainline property / line | Match |
|---|---:|---|---|
| `0x1162` (upm usb-vreg) | 4450 mV | `ti,battery-regulation-voltage=<4450000>` `dts/...:505` | ✓ |
| `0x7d0` (upm usb-ichg) | 2000 mA | `ti,charge-current=<2000000>` `:506` | ✓ |
| `0x1e0` (upm precharge) | 480 mA | `ti,precharge-current=<480000>` `:508` | ✓ |
| `0xfa` (upm termination) | 250 mA | `ti,termination-current=<250000>` `:507` | ✓ |
| `0x1388` (upm boost-voltage) | 5000 mV | `ti,boost-voltage=<5000000>` `:510` | ✓ |
| `0x4b0` (boost-current) | 1200 mA | `ti,boost-max-current=<1200000>` `:511` | ✓ |
| `0x11f8` (sc usb-vlim) | 4600 mV | (alternate strapping set — unused) | n/a |
| `0x1130` (sc usb-vreg) | 4400 mV | (alternate) | n/a |
| `0x1f4` (sc usb-ichg/ilim) | 500 mA | live `current_max=500000` | ✓ live |
| `0xb4` / `0x12c` (sc pre/term) | 180 / 300 mA | (alternate) | n/a |
| `0x141e` (sc boost) | 5150 mV | (alternate; live VBUS 5000 mV) | n/a |
| `0x1bb5c` (soc_max_crit) | 113500 m°C | `temperature=<113500>` `dtsi:1392-1730` | ✓ |
| `0x14c08` (gpu throttle) | 85000 m°C | `temperature=<85000>` `dtsi:1386-1404` | ✓ |
| `0x7d0` (hysteresis) | 2000 m°C | `hysteresis=<2000>` | ✓ |
| `0x400` (contribution) | 1024 | `THERMAL_NO_LIMIT` maps | ✓ |
| `0xbebc200` | 200 MHz | `max-frequency=<200000000>` `dts/...:342` | ✓ |
| `0x2dc6c0` | 3 MHz | `spi-max-frequency=<3000000>` `:389` | ✓ |
| `0x438` / `0x99c` | 1080 / 2460 | `touchscreen-size-x/y` `:397-398` | ✓ |
| `0x55` / `0x96` / `0x6c` / `0x6d` | 85 / 150 / 108 / 109 | panel/bias GPIOs `:1020-1027,532-553` | ✓ |
| `0x6b` / `0x07` / `0x0d` / `0x09` | 107 / 7 / 13 / 9 | touch reset / SD CD / TCPC IRQ / touch IRQ | ✓ (CD polarity Δ D2-20) |
| `0xa4cb8` / `0xdbba0` | 675000 / 900000 µV | GPU OPP endpoints `dtsi:305-347` | ✓ |
| `0x2f4d60` | 3100000 µV | vsim1 max `:861` | ✓ |
| `0xc65d40` | 13 MHz | arch timer clock `dtsi:274-277` | ✓ |
| `0xe2` / `0xe3` / `0xf1` / `0x6e` | SPI 226 / 227 / 241 / 110 | CCIF0/DPMAIF/MD-WDT IRQ | ✓ |
| `0x01011f01` | s2idle PSCI param | `arm,psci-suspend-param` `dtsi:255-261` | ✓ |
| `0x8e000000 + 0xe10000` | AP-MD NC SMEM | `md_nc_smem` `dts/...:154-156` | ✓ (live expdb) |
| `0x8c000000 + 0x16a0000` | AP-MD C SMEM | `md_c_smem` `dts/...:150-152` | ✓ (live expdb) |
| `0x06419000` / `0x06432000` | source 5 V/1 A / sink 5 V/2 A PDOs | `dts/...:490-491` | ✓ (wire values) |

---

## 10. Appendix B — exact commands used (all read-only)

```bash
adb connect 127.0.0.1:5555
adb -s 127.0.0.1:5555 shell 'getprop ro.boot.bootreason; getprop sys.boot.reason; cat /proc/version'
adb -s 127.0.0.1:5555 shell 'cat /proc/pl_lk'
adb -s 127.0.0.1:5555 shell 'for p in /sys/class/power_supply/*; do …; done'
adb -s 127.0.0.1:5555 shell 'dmesg | grep -aiE "sc89601|upm6910|bq25890|cw2217|icnl|panel"'
adb -s 127.0.0.1:5555 shell 'for z in /sys/class/thermal/thermal_zone*; do …trip_point_*_temp…; done'
adb -s 127.0.0.1:5555 shell 'cat /sys/power/mem_sleep; cat /sys/power/state; …cpuidle/state*/name…'
adb -s 127.0.0.1:5555 shell 'strings /dev/block/by-name/expdb | grep -aiE "ccci|mblock|reboot"'
adb -s 127.0.0.1:5555 shell 'grep -aiE "CCIF|md_eint" /proc/ccci_dump'

git -C /work/mainline/advan-x1-mainline status --porcelain=v1
git -C /work/mainline/advan-x1-mainline log --format='%h %s%n%b'
git -C /work/mainline/advan-x1-mainline log -G'TODO|FIXME|mp01|Motorola|Zinwa|guess|temp|unverified' --oneline --all
git -C /work/mainline/advan-x1-mainline log --format='%G? %h' | sort | uniq -c
strings -a /work/mainline/ref/advan-x1-oracle/modules/*.ko
nm -n /work/mainline/ref/advan-x1-oracle/modules/ccci_md_all.ko | grep -iE 'rpc|dpmaif|nc_off'
objdump -d /work/mainline/ref/advan-x1-oracle/modules/ccci_md_all.ko
grep -nE '<node>@|reg =|interrupts =|temperature =' \
  /work/firmware/advan_x1/dtb/runtime_merged.dts \
  /work/mainline/advan-x1-mainline/dts/mt6789.dtsi \
  /work/mainline/advan-x1-mainline/dts/mt6789-advan-x1.dts
```

---

## 11. Triage decision register (awaiting operator)

Items requiring a decision before any code change:

- [ ] **D3-03** — backport LVTS `set_all_tc_hw_reboot()` / PROTTC at 113500 m°C, **or** accept SW-only thermal protection.
- [ ] **D3-18** — restore 30 s CCCI HS1/HS2 timeout + FS-in-flight extension, **or** keep 5 s + retry.
- [ ] **D2-09** — add NTC/PMIC/consys thermal zones from `runtime_merged.dts:710-962`.
- [ ] **D2-15** — port FS1599 Re25/F0 calibration from `snd-soc-fs1599.ko` / `fs1599.fsm`.
- [ ] **D2-17** — add ACCDET CDD array + efuse/moisture fallback.
- [ ] **D2-22** — add MT6366 `XO_NFC` clock-buffer consumer; re-test NFC.
- [ ] **D2-20** — test microSD CD with card inserted; revert to `GPIO_ACTIVE_HIGH` if needed.
- [ ] **D-01/D-02** — narrow GPU rails documented vs restore 1293750 µV.
- [ ] **D-13/D-17/D-20** — SPI reg window, USB PHY cal, CMA size.
- [ ] **D19/D26, F01/F02-F07/F11,F15/F16** — provenance and doc corrections; remove `/dev/mem` crutch.
- [ ] **Stale docs** — patch counts (60/71 → 75), WIP claims, `ccci-sip-abi.md:95`.

**END OF AUDIT DOCUMENT — NO TRACKED FILES OR COMMITS MODIFIED.**
