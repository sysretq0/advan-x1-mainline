#!/usr/bin/env python3
"""
Pack mainline Linux artifacts into a U-Boot FIT + boot.scr deployment tree
for Advan X1 (MediaTek MT6789 / Helio G99).

Architecture:
  - Produces a SHA-256-verified U-Boot FIT image (`advan-x1.itb`) containing:
      * Compressed ARM64 Linux kernel Image (load/entry = 0x61000000)
      * Device Tree Blob (`mt6789-advan-x1.dtb`)
      * Optional initramfs (`initramfs.cpio.gz`, compression = "none" for Linux)
  - Compiles `boot.scr` (U-Boot bootmeth_script) + generates plain-text `uEnv.txt`.
  - Space budget enforcement (`--max-size`, default 32 MiB):
      * Single-space mode (`space1/` <= 32 MiB):
          `boot.scr`, `uEnv.txt`, and `advan-x1.itb` all reside on one partition
          (e.g. 32 MiB FAT32 `esp` or a dedicated `/boot` partition).
      * Automatic 2-space split mode (when payload exceeds `--max-size`, or `--mode split`):
          - `space1/` (~4 KiB): `boot.scr` + `uEnv.txt` (lives on 32 MiB `esp`)
          - `space2/` (unbounded): `advan-x1.itb` (lives on `mainline` ext4/FAT32)
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import lzma
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

KERNEL_LOAD_ADDR = 0x61000000
KERNEL_ENTRY_ADDR = 0x61000000
DEFAULT_BOOTARGS = (
    "earlycon=uart8250,mmio32,0x11002000 "
    "console=ttyGS0,115200 console=tty0 nokaslr loglevel=7 "
    "loop.max_part=16 rdinit=/init panic=5"
)


def parse_size(spec: str) -> int:
    s = spec.strip().upper()
    mult = 1
    if s.endswith("K") or s.endswith("KB") or s.endswith("KIB"):
        mult = 1024
        s = s.rstrip("KIB")
    elif s.endswith("M") or s.endswith("MB") or s.endswith("MIB"):
        mult = 1024 * 1024
        s = s.rstrip("MIB")
    elif s.endswith("G") or s.endswith("GB") or s.endswith("GIB"):
        mult = 1024 * 1024 * 1024
        s = s.rstrip("GIB")
    return int(float(s) * mult)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def find_mkimage(explicit: str | None) -> str:
    if explicit:
        if os.path.isfile(explicit) and os.access(explicit, os.X_OK):
            return explicit
        raise FileNotFoundError(f"mkimage not executable: {explicit}")
    candidates = [
        shutil.which("mkimage"),
        "/work/bootloader/u-boot-build/tools/mkimage",
        "/work/bootloader/advan-x1-uboot/build/tools/mkimage",
        "/work/bootloader/u-boot-worktree/tools/mkimage",
    ]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    raise FileNotFoundError(
        "mkimage not found. Install u-boot-tools or pass --mkimage /path/to/mkimage."
    )


def compress_kernel(src: Path, dst: Path, compression: str, lzma_dict: int) -> None:
    if compression == "none":
        shutil.copy2(src, dst)
        return
    if compression == "gzip":
        with src.open("rb") as fin, gzip.open(dst, "wb", compresslevel=9) as fout:
            shutil.copyfileobj(fin, fout, length=1024 * 1024)
        return
    if compression == "lzma":
        # Raw LZMA_ALONE format (required by U-Boot lzmaBuffToBuffDecompress)
        # with bounded dictionary so decompression fits in U-Boot's malloc pool.
        filters = [{"id": lzma.FILTER_LZMA1, "preset": 6, "dict_size": lzma_dict}]
        with src.open("rb") as fin, lzma.open(
            dst, "wb", format=lzma.FORMAT_ALONE, filters=filters
        ) as fout:
            shutil.copyfileobj(fin, fout, length=1024 * 1024)
        return
    if compression == "lz4":
        lz4_bin = shutil.which("lz4")
        if not lz4_bin:
            raise FileNotFoundError("lz4 binary not found in PATH")
        subprocess.run([lz4_bin, "-9", "-f", str(src), str(dst)], check=True)
        return
    raise ValueError(f"Unsupported compression: {compression}")


def render_its(
    kernel_path: Path,
    dtb_path: Path,
    initramfs_path: Path | None,
    compression: str,
    fit_conf: str,
    description: str,
) -> str:
    ramdisk_node = ""
    conf_ramdisk = ""
    if initramfs_path is not None:
        ramdisk_node = f"""
        ramdisk-1 {{
            description = "Advan X1 Mainline Initramfs";
            data = /incbin/("{initramfs_path}");
            type = "ramdisk";
            arch = "arm64";
            os = "linux";
            compression = "none";
            hash-1 {{
                algo = "sha256";
            }};
        }};"""
        conf_ramdisk = '\n            ramdisk = "ramdisk-1";'

    return f"""/dts-v1/;

/ {{
    description = "{description}";
    #address-cells = <1>;

    images {{
        kernel-1 {{
            description = "Advan X1 (MT6789) Mainline Linux Kernel";
            data = /incbin/("{kernel_path}");
            type = "kernel";
            arch = "arm64";
            os = "linux";
            compression = "{compression}";
            load = <0x{KERNEL_LOAD_ADDR:08x}>;
            entry = <0x{KERNEL_ENTRY_ADDR:08x}>;
            hash-1 {{
                algo = "sha256";
            }};
        }};

        fdt-1 {{
            description = "Advan X1 (MT6789) Device Tree";
            data = /incbin/("{dtb_path}");
            type = "flat_dt";
            arch = "arm64";
            compression = "none";
            hash-1 {{
                algo = "sha256";
            }};
        }};{ramdisk_node}
    }};

    configurations {{
        default = "{fit_conf}";

        {fit_conf} {{
            description = "Advan X1 (MT6789) Mainline Linux";
            kernel = "kernel-1";
            fdt = "fdt-1";{conf_ramdisk}
        }};
    }};
}};
"""


def render_template(src: Path, subs: dict[str, str]) -> str:
    text = src.read_text(encoding="utf-8")
    for k, v in subs.items():
        text = text.replace(f"@{k}@", v)
    return text


def build_fat_image(space1_dir: Path, out_img: Path, size_bytes: int, label: str) -> bool:
    mkfs = shutil.which("mkfs.vfat") or shutil.which("mkfs.fat")
    mcopy = shutil.which("mcopy")
    if not mkfs or not mcopy:
        print(
            "[i] Skipping boot-space1.img (mkfs.vfat/mcopy not installed on host).",
            file=sys.stderr,
        )
        return False

    if out_img.exists():
        out_img.unlink()
    with out_img.open("wb") as f:
        f.truncate(size_bytes)

    subprocess.run([mkfs, "-F", "32", "-n", label[:11], str(out_img)], check=True)
    for item in sorted(space1_dir.iterdir()):
        subprocess.run(
            [mcopy, "-i", str(out_img), "-s", str(item), "::/"],
            check=True,
        )
    return True


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(
        description="Pack Advan X1 mainline kernel into U-Boot FIT + boot.scr (<=32 MiB or 2-space split)"
    )
    ap.add_argument("--kernel", required=True, type=Path, help="Uncompressed arm64 Image")
    ap.add_argument("--dtb", required=True, type=Path, help="Compiled mt6789-advan-x1.dtb")
    ap.add_argument("--initramfs", type=Path, default=None, help="Optional initramfs.cpio.gz")
    ap.add_argument("--out-dir", required=True, type=Path, help="Output directory")
    ap.add_argument(
        "--compression",
        choices=["gzip", "lzma", "lz4", "none"],
        default="gzip",
        help="Kernel compression inside FIT (default: gzip)",
    )
    ap.add_argument(
        "--lzma-dict",
        default="1M",
        help="LZMA dictionary size when --compression=lzma (default: 1M, safe for 4 MiB U-Boot heap)",
    )
    ap.add_argument(
        "--mode",
        choices=["auto", "single", "split"],
        default="auto",
        help="Layout mode: auto (<=max-size -> single, else 2-space split), single, or split",
    )
    ap.add_argument(
        "--max-size",
        default="32M",
        help="Space 1 partition budget (default: 32M)",
    )
    ap.add_argument(
        "--fs-overhead",
        default="1M",
        help="Reserved FAT32/filesystem overhead inside --max-size (default: 1M)",
    )
    ap.add_argument("--fit-name", default="advan-x1.itb", help="FIT filename (default: advan-x1.itb)")
    ap.add_argument("--fit-conf", default="conf-advan-x1", help="FIT configuration node name")
    ap.add_argument(
        "--payload-label",
        default="linux_boot",
        help="Space 2 GPT partition label used in split mode (default: linux_boot)",
    )
    ap.add_argument("--bootargs", default=DEFAULT_BOOTARGS, help="Default Linux kernel cmdline")
    ap.add_argument("--mkimage", default=None, help="Path to U-Boot mkimage tool")
    ap.add_argument(
        "--make-fat-image",
        action="store_true",
        help="Also generate a flashable FAT32 image of space1/ if mkfs.vfat + mcopy are available",
    )
    ap.add_argument(
        "--fat-label",
        default="MAINLINE",
        help="FAT32 volume label for --make-fat-image (default: MAINLINE)",
    )
    args = ap.parse_args()

    kernel_src = args.kernel.resolve()
    dtb_src = args.dtb.resolve()
    initramfs_src = args.initramfs.resolve() if args.initramfs else None
    out_dir = args.out_dir.resolve()
    max_size = parse_size(args.max_size)
    fs_overhead = parse_size(args.fs_overhead)
    usable_budget = max_size - fs_overhead
    lzma_dict = parse_size(args.lzma_dict)

    for p in (kernel_src, dtb_src):
        if not p.is_file():
            print(f"ERROR: Missing input file: {p}", file=sys.stderr)
            return 1
    if initramfs_src and not initramfs_src.is_file():
        print(f"ERROR: Missing initramfs: {initramfs_src}", file=sys.stderr)
        return 1

    mkimage = find_mkimage(args.mkimage)
    out_dir.mkdir(parents=True, exist_ok=True)
    fit_out = out_dir / args.fit_name

    with tempfile.TemporaryDirectory(prefix="advan-x1-fit-") as tmp:
        tmp_dir = Path(tmp)
        k_comp = tmp_dir / f"Image.{args.compression}"
        print(f"[*] Compressing kernel ({args.compression})...")
        compress_kernel(kernel_src, k_comp, args.compression, lzma_dict)
        print(
            f"    raw={kernel_src.stat().st_size:,} B -> "
            f"compressed={k_comp.stat().st_size:,} B ({k_comp.stat().st_size / 1048576:.2f} MiB)"
        )

        its_path = tmp_dir / "advan-x1.its"
        its_text = render_its(
            kernel_path=k_comp,
            dtb_path=dtb_src,
            initramfs_path=initramfs_src,
            compression=args.compression,
            fit_conf=args.fit_conf,
            description=f"Advan X1 (MT6789) Mainline Linux FIT ({args.compression})",
        )
        its_path.write_text(its_text, encoding="utf-8")

        print(f"[*] Building FIT image with {mkimage}...")
        subprocess.run(
            [mkimage, "-f", str(its_path), str(fit_out)],
            check=True,
            stdout=subprocess.DEVNULL,
        )

        # Render & compile boot.scr
        boot_cmd_tpl = repo_root / "boot" / "boot.cmd.in"
        uenv_tpl = repo_root / "boot" / "uEnv.txt.in"
        extlinux_ex = repo_root / "boot" / "extlinux.conf.example"

        boot_cmd_rendered = tmp_dir / "boot.cmd"
        boot_cmd_rendered.write_text(
            render_template(
                boot_cmd_tpl,
                {
                    "FIT_FILE": args.fit_name,
                    "FIT_CONF": args.fit_conf,
                    "PAYLOAD_PART_LABEL": args.payload_label,
                    "DEFAULT_BOOTARGS": args.bootargs,
                },
            ),
            encoding="utf-8",
        )
        boot_scr = out_dir / "boot.scr"
        subprocess.run(
            [
                mkimage,
                "-A",
                "arm64",
                "-O",
                "linux",
                "-T",
                "script",
                "-C",
                "none",
                "-n",
                "Advan X1 Mainline Boot Script",
                "-d",
                str(boot_cmd_rendered),
                str(boot_scr),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )

    fit_size = fit_out.stat().st_size
    scr_size = boot_scr.stat().st_size
    single_total = fit_size + scr_size + 4096  # + uEnv.txt margin

    if args.mode == "single":
        if single_total > usable_budget:
            print(
                f"ERROR: Single-space payload ({single_total / 1048576:.2f} MiB) exceeds "
                f"usable Space 1 budget ({usable_budget / 1048576:.2f} MiB of {max_size / 1048576:.1f} MiB).\n"
                f"       Use --compression lzma or --mode auto / --mode split.",
                file=sys.stderr,
            )
            return 2
        layout = "single"
    elif args.mode == "split":
        layout = "split"
    else:
        layout = "single" if single_total <= usable_budget else "split"

    # Assemble clean space1/ (and space2/ when split)
    space1 = out_dir / "space1"
    space2 = out_dir / "space2"
    if space1.exists():
        shutil.rmtree(space1)
    if space2.exists():
        shutil.rmtree(space2)
    space1.mkdir(parents=True)

    uenv_text = render_template(
        uenv_tpl,
        {
            "LAYOUT_MODE": layout,
            "FIT_FILE": args.fit_name,
            "FIT_CONF": args.fit_conf,
            "PAYLOAD_PART_LABEL": args.payload_label,
            "FORCE_SPLIT": "1" if layout == "split" else "0",
        },
    )
    (out_dir / "uEnv.txt").write_text(uenv_text, encoding="utf-8")

    shutil.copy2(boot_scr, space1 / "boot.scr")
    shutil.copy2(out_dir / "uEnv.txt", space1 / "uEnv.txt")
    if extlinux_ex.is_file():
        (space1 / "extlinux").mkdir(exist_ok=True)
        shutil.copy2(extlinux_ex, space1 / "extlinux" / "extlinux.conf.example")

    if layout == "single":
        shutil.copy2(fit_out, space1 / args.fit_name)
    else:
        space2.mkdir(parents=True)
        shutil.copy2(fit_out, space2 / args.fit_name)

    space1_bytes = sum(p.stat().st_size for p in space1.rglob("*") if p.is_file())
    space2_bytes = (
        sum(p.stat().st_size for p in space2.rglob("*") if p.is_file())
        if space2.exists()
        else 0
    )

    fat_created = False
    fat_img_path = out_dir / "boot-space1.img"
    if args.make_fat_image:
        fat_size = max_size if layout == "single" else min(max_size, 4 * 1024 * 1024)
        fat_created = build_fat_image(space1, fat_img_path, fat_size, args.fat_label)

    # Write manifest + sha256sums
    manifest = {
        "board": "advan,x1",
        "soc": "mediatek,mt6789",
        "layout": layout,
        "compression": args.compression,
        "max_size_bytes": max_size,
        "usable_budget_bytes": usable_budget,
        "fit": {
            "filename": args.fit_name,
            "config": args.fit_conf,
            "size_bytes": fit_size,
            "size_mib": round(fit_size / 1048576, 2),
            "sha256": sha256_file(fit_out),
            "kernel_load_addr": f"0x{KERNEL_LOAD_ADDR:08x}",
            "has_initramfs": initramfs_src is not None,
        },
        "space1": {
            "target": "esp (or dedicated <=32 MiB boot partition)",
            "size_bytes": space1_bytes,
            "size_mib": round(space1_bytes / 1048576, 2),
            "files": sorted(str(p.relative_to(space1)) for p in space1.rglob("*") if p.is_file()),
        },
        "space2": {
            "target": f"GPT partition labeled '{args.payload_label}' (ext4/FAT32)",
            "size_bytes": space2_bytes,
            "size_mib": round(space2_bytes / 1048576, 2),
            "files": (
                sorted(str(p.relative_to(space2)) for p in space2.rglob("*") if p.is_file())
                if space2.exists()
                else []
            ),
        },
        "fat_image": fat_img_path.name if fat_created else None,
    }
    (out_dir / "boot-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    sum_files = [fit_out, boot_scr, out_dir / "uEnv.txt"]
    if fat_created:
        sum_files.append(fat_img_path)
    sums_lines = [f"{sha256_file(f)}  {f.name}" for f in sum_files]
    (out_dir / "uboot-sha256sums.txt").write_text("\n".join(sums_lines) + "\n", encoding="utf-8")

    print(f"=== U-Boot Packaging Summary ({layout.upper()} MODE) ===")
    print(f"  FIT image : {fit_out.name} ({fit_size:,} B / {fit_size / 1048576:.2f} MiB, {args.compression})")
    print(f"  Space 1   : {space1_bytes:,} B ({space1_bytes / 1048576:.2f} MiB) <= {usable_budget / 1048576:.1f} MiB usable budget")
    if layout == "split":
        print(f"  Space 2   : {space2_bytes:,} B ({space2_bytes / 1048576:.2f} MiB) -> GPT label '{args.payload_label}'")
    if fat_created:
        print(f"  FAT32 img : {fat_img_path.name} ({fat_img_path.stat().st_size / 1048576:.1f} MiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
