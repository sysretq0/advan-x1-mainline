# Advan X1 mainline Linux port

Mainline Linux for the Advan X1 (MediaTek MT6789 / Helio G99, Android phone).
Boots through a demonstrated bootloader chain (stock LK → `loki-loader` → LNXB
kernel bundle); this repo carries everything **after** the handoff:

- `dts/` — clean board device tree, rebuilt from stock Advan X1 DT
  (authority: `main_dtb_0.dts` / `dtbo_unpacked.dts`), mainline-shaped using the
  Volla M101TBR200 vendor tree as SoC reference only.
- `config/` — kernel config fragments.
- `patches/` — email-ready patch series on pristine mainline
  (clk / PHY / power-domain bringup for MT6789). Applied to an upstream clone
  at build time; no kernel fork.
- `scripts/` — build + LNXB packaging + gated flash helpers.
- `docs/` — hardware facts, roadmap, per-boot verdicts.
- `legacy/` — the pre-redo hack artifacts (fixed-clock stubs, `/dev/mem` gate
  pokes, stub musb clocks), kept as reference. **Nothing here ships.**

## Policy

- Every peripheral address, IRQ, clock ID and display parameter must trace to
  Advan X1 stock artifacts — no configs borrowed from alien boards.
- The SoC reference is the Volla `M101TBR200` (mt6789/mt8781 sibling tablet)
  vendor kernel tree @ `HelloVolla/android_kernel_volla_mt8781@887784f`:
  register/ID tables only, rewritten mainline-style.
- Device memory claims need evidence; unverified stays recorded unverified.

## Status

See `docs/roadmap.md`. Legacy state before redo: boots 7–24, panel console +
initramfs evidence channel working, UFS/USB brought up bootloader-dependently
(gates left live by the loader). The redo replaces all of that with real
clock/power/PHY bringup.
