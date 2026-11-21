# Advan X1 mainline Linux port

Mainline Linux for the Advan X1 (MediaTek MT6789 / Helio G99, Android phone).
Boots at **Non-Secure EL2** via upstream U-Boot
([`advan-x1-uboot`](https://github.com/sysretq0/advan-x1-uboot), chainloaded
from stock LK via Loki Stage-1) using a SHA-256-verified **U-Boot FIT image**
(`advan-x1.itb`) and **`boot.scr`** (`bootflow scan -lb`):

- `dts/` — clean board device tree, rebuilt from stock Advan X1 DT
  (authority: `main_dtb_0.dts` / `dtbo_unpacked.dts`), mainline-shaped.
- `config/` — kernel config fragments.
- `patches/` — email-ready patch series on pristine mainline
  (clk / PHY / power-domain / display bringup for MT6789). Applied to an
  upstream clone at build time; no kernel fork.
- `boot/` — U-Boot `boot.cmd.in`, plain-text `uEnv.txt.in` runtime override
  template, and optional loose-file `extlinux.conf.example`.
- `tools/` — `pack_uboot_boot.py` (FIT + `boot.scr` + `<=32 MiB` single-space
  or automatic 2-space split packager; legacy `pack_linux_bundle.py` kept only
  for historical reference).
- `scripts/` — build + CI fetch helpers.
- `initramfs/` — standalone BusyBox initramfs with USB ACM (`ttyGS0`) gadget,
  standard `root=PARTLABEL=...` / `root=UUID=...` / `root=/dev/...` support,
  and legacy `sdc72` nested-GPT fallback.
- `docs/` — hardware facts, roadmap, per-boot verdicts.
- `legacy/` — pre-redo hack artifacts, kept as reference. **Nothing here ships.**

## U-Boot Boot Architecture (<=32 MiB Single-Space or 2-Space Split)

`tools/pack_uboot_boot.py` targets a **32 MiB** Space 1 budget by default:

1. **Single-space mode (`<= 32 MiB`, default)**:
   - `gzip` FIT (`advan-x1.itb`): ~22.7 MiB (`Image.gz` + DTB + `initramfs.cpio.gz`)
   - `lzma` FIT (`--compression lzma`, 1 MiB dict): ~17.5 MiB (fits even inside
     a 32 MiB `esp` partition sharing space with existing `/EFI/` bootloaders)
   - `space1/` contains `/boot.scr`, `/uEnv.txt`, and `/advan-x1.itb` — drop
     directly onto a FAT32/ext4 boot partition (`linux_boot` 48 MiB or `esp`
     32 MiB) or flash `boot-space1.img`.
2. **Automatic 2-space split mode (`> 32 MiB`, or `--mode split`)**:
   - **Space 1 (`space1/`, ~4 KiB)**: `/boot.scr` + `/uEnv.txt` on the small
     boot/ESP partition (`linux_boot` or `esp`).
   - **Space 2 (`space2/`, overflow when `> 32 MiB`)**: `/advan-x1.itb` or
     `/boot/advan-x1.itb` on a second flat FAT32/ext4 partition (default
     `linux_boot` 48 MiB, fallback `esp` / `boot`). `boot.scr` discovers Space 2
     automatically by GPT partition label.
   - **Rootfs**: defaults to the existing nested-GPT Bookworm rootfs on
     `xnu_data` (`sdc72`); `rootfs` (`sdc64`) is left untouched unless explicitly
     targeted via `extra_bootargs=root=...` in `/uEnv.txt`.

Runtime kernel cmdline overrides (e.g. `extra_bootargs=noswitch` or
`extra_bootargs=root=PARTLABEL=rootfs rootfstype=ext4`) can be edited in plain
text in `/uEnv.txt` on Space 1 without rebuilding `boot.scr` or the FIT image.

## Policy

- Every peripheral address, IRQ, clock ID and display parameter must trace to
  Advan X1 stock artifacts — no configs borrowed from alien boards.
- Device memory claims need evidence; unverified stays recorded unverified.
- All commits in `advan-x1-mainline` must apply cleanly onto pristine upstream
  Linux (`verify_patches.sh` + `checkpatch.pl`) and be GPG-signed.
