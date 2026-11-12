# MT6789 clean bringup roadmap

Goal: remove every bootloader-dependency hack and bring the SoC up with real
mainline drivers, subsystem by subsystem. Each phase ends with a gated flash
(`/flash-gate`) and a device verdict recorded in project memory.

Reference sources:
- Advan X1 stock DT (`extracted/v5/dtb/main_dtb_0.dts`) — board authority.
- Volla vendor kernel (`HelloVolla/android_kernel_volla_mt8781@887784f`) —
  SoC register/ID tables, rewritten mainline-style.
- Minimal Phone MP01 vendor kernel (`sysretq0/mp01-kernel`, fork of
  `minimalcompany/mp01-kernel`, Android 5.10.233) — same-SoC phone sibling;
  second reference for UFS/USB PHY and vendor CCF, used to triangulate
  SoC-vs-board differences.
- Upstream mainline `drivers/clk/mediatek/` — architecture patterns
  (mt8186 / mt8195 generation).

## Phases

### P0 — repo + build pipeline (done when: bundle builds from pristine tree + patches/)
- [x] Scaffold repo, import legacy artifacts, index references.
- [ ] `scripts/build.sh`: pristine mainline clone + `patches/` series +
      config frags + LNXB pack. No manual tree surgery.
- [ ] Baseline boot with legacy DT shape rebuilt from stock artifacts
      (no stub clocks yet — parity with boot 24 verdict surface).

### P1 — infra_ao + topckgen + apmixedsys clock drivers
- [ ] `clk-mt6789-infra_ao.c` — gate map from vendor table, mainline
      simple-probe style. UART0/UFS/USB gates land here.
- [ ] `clk-mt6789-topckgen.c` — muxes/dividers referenced by active nodes.
- [ ] `clk-mt6789-apmixedsys.c` — PLLs needed by USB/UFS PHYs.
- [ ] DT: replace `clk26m` fixed stubs with real clock-parent references.
- [ ] Gate: drop `clk_ignore_unused` for touched subsystems; boot verdict.

### P2 — UFS real bringup
- [ ] mphy / ufs-mediatek mainline-shape PHY support for MT6789.
- [ ] UFSHCI clocks referenced from real clock tree (no leftover-gate
      dependence).
- [ ] Gate: UFS probe with gate-closed start (loader stops leaving gates
      on, or toggled off before probe). Ext4 root verdict.

### P3 — USB real bringup
- [ ] mt65xx/mt6789 USB PHY (tphy) driver entry for MT6789.
- [ ] musb/ssusb clock references real (univpll/main/mcu), remove IFRAO_SSUSB
      `/dev/mem` poke.
- [ ] Gate: gadget + ssh-over-USB verdict, then host mode.

### P4 — power domains (scpsys) + SMP
- [ ] `clk-mt6789-power.c`/mtk-pm-domains for MT6789 (29 clock refs in stock
      scpsys node are the table of dependencies).
- [ ] Drop `pd_ignore_unused`; idle workloads can probe their own power.
- [ ] A76 cluster online (open issue #0001) — likely needs MCPM/PSCI wakeup
      path; scpsys first.
- [ ] Gate: `nproc` = 8, suspend path sane.

### P5 — display beyond SimpleFB
- [ ] mmsys gates (`0x1c000000` family) + drm simpledrm/mtk-drm path.
- [ ] Gate: real panel driver `hdplus,icnl9922c,120hz` on mainline drm.

## Verdict record

Each device gate = one boot attempt, hash-verified staged bundle, panel/initramfs
evidence captured, verdict + evidence hash recorded in project memory before the
next change lands.
