#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
pack_linux_bundle.py — Package an ARM64 Image, initramfs, and DTB into a
loki-loader Linux Bundle ('LNXB').

Uses a compact 4 KiB-aligned binary header so a chainloader can read only
the exact blocks occupied by the payload instead of a fixed raw window.
"""

import argparse
import struct
import sys
from pathlib import Path

LINUX_BUNDLE_MAGIC = 0x42584E4C  # 'LNXB'
LINUX_BUNDLE_VERSION = 1
HEADER_SIZE = 4096  # 1 UFS sector / 4KB aligned


def align_up(val: int, align: int) -> int:
    return (val + align - 1) & ~(align - 1)


def main():
    parser = argparse.ArgumentParser(description="Package ARM64 Linux payload into a loki-loader bundle")
    parser.add_argument("--kernel", required=True, type=Path, help="ARM64 Linux kernel Image (boot/Image)")
    parser.add_argument("--initramfs", type=Path, default=None, help="Optional initramfs / rootfs image")
    parser.add_argument("--dtb", type=Path, default=None, help="Optional device tree blob (.dtb) with a /chosen node")
    parser.add_argument("--cmdline", type=str, default="",
                        help="Bootargs published to /chosen/bootargs by the "
                             "loader. Empty = publish nothing. The DTB must "
                             "carry a bootargs stub whose value length "
                             "reserves the capacity (a valsz-0 stub only "
                             "accepts an empty cmdline).")
    parser.add_argument("--out", required=True, type=Path, help="Output bundle binary path")
    args = parser.parse_args()

    if not args.kernel.is_file():
        sys.exit(f"Error: kernel file not found: {args.kernel}")

    kernel_data = args.kernel.read_bytes()
    if len(kernel_data) < 64 or struct.unpack_from("<I", kernel_data, 56)[0] != 0x644D5241:
        print("Warning: input kernel is missing the arm64 Image magic "
              "'ARM\\x64' at offset 56; the loader will only accept this as "
              "raw fallback", file=sys.stderr)

    initrd_data = args.initramfs.read_bytes() if args.initramfs and args.initramfs.is_file() else b""
    dtb_data = args.dtb.read_bytes() if args.dtb and args.dtb.is_file() else b""

    if args.dtb and args.dtb.is_file() and len(dtb_data) >= 4:
        magic = struct.unpack_from(">I", dtb_data, 0)[0]
        if magic != 0xD00DFEED:
            print("Warning: DTB does not begin with the flat-device-tree magic "
                  "0xD00DFEED the loader looks for", file=sys.stderr)

    # Layout offsets (every component 4KB aligned inside the bundle).
    # The ARM64 boot protocol requires the Image to sit at a 2 MiB-aligned
    # staging base; the loader compensates for the bundle header (see
    # do_boot_linux), so kernel_offset only has to be 4KB aligned here.
    # The initramfs, however, must NOT overlap the kernel's header-declared
    # reservation (image_size, which covers the kernel's own boot-time
    # scratch — BSS, early page tables, relocation buffer). Reserve
    # image_size + a 2 MiB guard, then start the initramfs on a 2 MiB
    # boundary so the kernel's reserved footprint stays clean.
    kernel_offset = HEADER_SIZE
    kernel_size = len(kernel_data)

    image_size = kernel_size
    if len(kernel_data) >= 64:
        hdr_mag = struct.unpack_from("<I", kernel_data, 0x38)[0]
        if hdr_mag == 0x644D5241:  # 'ARPd' LE-spelling of ARM\x64
            text_offset = struct.unpack_from("<Q", kernel_data, 0x08)[0]
            image_size = max(kernel_size,
                             struct.unpack_from("<Q", kernel_data, 0x10)[0])
            if text_offset:
                print(f"Note: kernel Image text_offset=0x{text_offset:x}")

    if initrd_data:
        initramfs_offset = align_up(kernel_offset + image_size + 2 * 1024 * 1024,
                                    2 * 1024 * 1024)
        initramfs_size = len(initrd_data)
    else:
        initramfs_offset = 0
        initramfs_size = 0

    if dtb_data:
        base = initramfs_offset + initramfs_size if initrd_data else kernel_offset + kernel_size
        dtb_offset = align_up(base, 4096)
        dtb_size = len(dtb_data)
    else:
        dtb_offset = 0
        dtb_size = 0

    # Bundle cmdline: UTF-8, no embedded NULs, capped by the header field.
    cmdline_bytes = args.cmdline.encode("utf-8") if args.cmdline else b""
    if b"\x00" in cmdline_bytes:
        sys.exit("Error: --cmdline must not contain NUL bytes (fields are "
                 "NUL-terminated in the header)")
    if len(cmdline_bytes) > 255:
        sys.exit(f"Error: --cmdline is {len(cmdline_bytes)} bytes; the header "
                 "field caps at 255 (+ NUL terminator)")

    # Build Header (320 bytes struct padded to HEADER_SIZE)
    # struct linux_bundle_header:
    #   u32 magic
    #   u32 version
    #   u32 kernel_offset
    #   u32 kernel_size
    #   u32 initramfs_offset
    #   u32 initramfs_size
    #   u32 dtb_offset
    #   u32 dtb_size
    #   u32 reserved[6]
    #   char cmdline[256]
    header = struct.pack(
        "<8I6I256s",
        LINUX_BUNDLE_MAGIC,
        LINUX_BUNDLE_VERSION,
        kernel_offset,
        kernel_size,
        initramfs_offset,
        initramfs_size,
        dtb_offset,
        dtb_size,
        0, 0, 0, 0, 0, 0,
        cmdline_bytes.ljust(256, b"\x00"),
    ).ljust(HEADER_SIZE, b"\x00")

    total_size = align_up(
        max(kernel_offset + kernel_size,
            initramfs_offset + initramfs_size,
            dtb_offset + dtb_size),
        4096,
    )

    bundle = bytearray(total_size)
    bundle[:HEADER_SIZE] = header
    bundle[kernel_offset:kernel_offset + kernel_size] = kernel_data
    if initrd_data:
        bundle[initramfs_offset:initramfs_offset + initramfs_size] = initrd_data
    if dtb_data:
        bundle[dtb_offset:dtb_offset + dtb_size] = dtb_data

    args.out.write_bytes(bundle)
    print(f"[+] Wrote Linux Bundle: {args.out} ({len(bundle)} bytes, {len(bundle) // 4096} UFS blocks)")
    print(f"    Kernel:     offset 0x{kernel_offset:x}, size {kernel_size} bytes")
    if initramfs_size:
        print(f"    Initramfs:  offset 0x{initramfs_offset:x}, size {initramfs_size} bytes")
    if dtb_size:
        print(f"    DTB:        offset 0x{dtb_offset:x}, size {dtb_size} bytes")
    if cmdline_bytes:
        print(f"    Cmdline:    \"{args.cmdline}\" ({len(cmdline_bytes)} bytes, "
              "published via /chosen/bootargs)")
    print("    NOTE: the loader exports initramfs coordinates via /chosen "
          "only when the DTB already carries linux,initrd-start/end.")
    if cmdline_bytes:
        print("    NOTE: bootargs publication requires a DTB bootargs stub "
          "with enough reserved capacity (a 0-length stub rejects it and "
          "the loader boots without the cmdline).")


if __name__ == "__main__":
    main()
