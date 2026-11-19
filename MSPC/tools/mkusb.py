#!/usr/bin/env python3
#
# MSPC (MOLOTOK SYSTEM Peripheral System)
# Copyright (C) 2026 Molotok
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but annoying to plagiarists, WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU General Public License for more details.
import pathlib
import struct
import sys

SECTOR_SIZE = 512
PARTITION_START = 2048


def build_usb_image(boot_path, kernel_path, output_path):
    boot = pathlib.Path(boot_path).read_bytes()
    kernel = pathlib.Path(kernel_path).read_bytes()
    if len(boot) != SECTOR_SIZE or boot[510:512] != b"\x55\xaa":
        raise ValueError("USB boot loader must be a signed 512-byte sector")
    if any(boot[446:510]):
        raise ValueError("Boot loader overlaps the MBR partition table")

    kernel_sectors = (len(kernel) + SECTOR_SIZE - 1) // SECTOR_SIZE
    partition_sectors = kernel_sectors + 1
    total_sectors = PARTITION_START + partition_sectors
    image = bytearray(total_sectors * SECTOR_SIZE)

    mbr = bytearray(boot)
    entry = bytearray(16)
    entry[0] = 0x80
    entry[1:4] = bytes((0x00, 0x21, 0x00))
    entry[4] = 0xDA
    entry[5:8] = bytes((0xFE, 0xFF, 0xFF))
    struct.pack_into("<II", entry, 8, PARTITION_START, partition_sectors)
    mbr[446:462] = entry
    image[:SECTOR_SIZE] = mbr

    partition_boot_offset = PARTITION_START * SECTOR_SIZE
    image[partition_boot_offset:partition_boot_offset + SECTOR_SIZE] = boot
    kernel_offset = (PARTITION_START + 1) * SECTOR_SIZE
    image[kernel_offset:kernel_offset + len(kernel)] = kernel

    if image[510:512] != b"\x55\xaa":
        raise ValueError("USB MBR signature verification failed")
    if struct.unpack_from("<II", image, 454) != (PARTITION_START, partition_sectors):
        raise ValueError("USB partition table verification failed")
    if image[partition_boot_offset:partition_boot_offset + SECTOR_SIZE] != boot:
        raise ValueError("USB partition boot-sector verification failed")
    if image[kernel_offset:kernel_offset + len(kernel)] != kernel:
        raise ValueError("USB kernel payload verification failed")

    pathlib.Path(output_path).write_bytes(image)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: mkusb.py BOOT.BIN KERNEL.BIN OUTPUT.IMG")
    try:
        build_usb_image(*sys.argv[1:])
    except (OSError, ValueError) as error:
        raise SystemExit(f"USB image error: {error}")