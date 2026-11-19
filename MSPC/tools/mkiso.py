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

SECTOR = 2048
FLOPPY_SECTORS = 2880
FLOPPY_SIZE = FLOPPY_SECTORS * 512
KERNEL_LBA = 4
BOOT_CATALOG_LBA = 19
L_PATH_TABLE_LBA = 20
M_PATH_TABLE_LBA = 21
ROOT_LBA = 22
FLOPPY_LBA = 23


def both16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def both32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def directory_record(extent, size, flags, identifier):
    record_length = 33 + len(identifier) + (1 if len(identifier) % 2 == 0 else 0)
    record = bytearray(record_length)
    record[0] = record_length
    record[1] = 0
    record[2:10] = both32(extent)
    record[10:18] = both32(size)
    record[18:25] = bytes((126, 11, 13, 0, 0, 0, 0))
    record[25] = flags
    record[26] = 0
    record[27] = 0
    record[28:32] = both16(1)
    record[32] = len(identifier)
    record[33:33 + len(identifier)] = identifier
    return bytes(record)


def write_sector(image, lba, data):
    if len(data) > SECTOR:
        raise ValueError("sector data is too large")
    start = lba * SECTOR
    image[start:start + len(data)] = data


def make_catalog(boot_lba):
    catalog = bytearray(SECTOR)
    catalog[0:4] = bytes((1, 0, 0, 0))
    catalog[30:32] = b"\x55\xaa"
    checksum = (-sum(struct.unpack("<16H", catalog[:32]))) & 0xffff
    catalog[28:30] = struct.pack("<H", checksum)
    catalog[32:44] = struct.pack("<BBHBBHI", 0x88, 2, 0x07c0, 0, 0, 1, boot_lba)
    return catalog


def make_pvd(volume_sectors, root_record):
    pvd = bytearray(SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = b"MSPC".ljust(32, b" ")
    pvd[40:72] = b"MSPC_OS".ljust(32, b" ")
    pvd[80:88] = both32(volume_sectors)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(10)
    pvd[140:144] = struct.pack("<I", L_PATH_TABLE_LBA)
    pvd[148:152] = struct.pack(">I", M_PATH_TABLE_LBA)
    pvd[156:190] = root_record
    pvd[813:830] = b"2026111300000000\x00"
    pvd[830:847] = b"2026111300000000\x00"
    pvd[847:864] = b"0000000000000000\x00"
    pvd[864:881] = b"2026111300000000\x00"
    pvd[881] = 1
    return pvd


def make_iso(boot_path, kernel_bin_path, kernel_elf_path, output_path):
    boot = pathlib.Path(boot_path).read_bytes()
    kernel_bin = pathlib.Path(kernel_bin_path).read_bytes()
    kernel_elf = pathlib.Path(kernel_elf_path).read_bytes()
    if len(boot) != 512 or boot[510:512] != b"\x55\xaa":
        raise ValueError("BIOS boot sector must be exactly 512 bytes and end in 0x55AA")
    kernel_sectors = (len(kernel_bin) + 511) // 512
    if kernel_sectors + KERNEL_LBA > 36 * 80:
        raise ValueError("kernel does not fit on the emulated floppy")

    floppy = bytearray(FLOPPY_SIZE)
    floppy[:512] = boot
    kernel_start = KERNEL_LBA * 512
    floppy[kernel_start:kernel_start + len(kernel_bin)] = kernel_bin

    floppy_file_sectors = FLOPPY_SECTORS
    kernel_file_lba = FLOPPY_LBA + floppy_file_sectors
    kernel_file_sectors = (len(kernel_elf) + SECTOR - 1) // SECTOR
    volume_sectors = kernel_file_lba + kernel_file_sectors
    iso = bytearray(volume_sectors * SECTOR)

    write_sector(iso, 17, b"\x00CD001\x01" + b"EL TORITO SPECIFICATION".ljust(32, b" ") + bytes(32) + struct.pack("<I", BOOT_CATALOG_LBA))
    write_sector(iso, 18, b"\xffCD001\x01")
    catalog = make_catalog(FLOPPY_LBA)
    write_sector(iso, BOOT_CATALOG_LBA, catalog)

    path_entry = bytes((1, 0)) + struct.pack("<I", ROOT_LBA) + struct.pack("<H", 1) + b"\x00\x00"
    write_sector(iso, L_PATH_TABLE_LBA, path_entry)
    write_sector(iso, M_PATH_TABLE_LBA, bytes((1, 0)) + struct.pack(">I", ROOT_LBA) + struct.pack(">H", 1) + b"\x00\x00")

    root_record = directory_record(ROOT_LBA, SECTOR, 2, b"\x00")
    root_data = bytearray()
    root_data += directory_record(ROOT_LBA, SECTOR, 2, b"\x00")
    root_data += directory_record(ROOT_LBA, SECTOR, 2, b"\x01")
    root_data += directory_record(FLOPPY_LBA, FLOPPY_SIZE, 0, b"MSPC.IMG;1")
    root_data += directory_record(kernel_file_lba, len(kernel_elf), 0, b"KERNEL.ELF;1")
    root_data = root_data.ljust(SECTOR, b"\x00")
    write_sector(iso, ROOT_LBA, root_data)
    write_sector(iso, 16, make_pvd(volume_sectors, root_record))

    floppy_start = FLOPPY_LBA * SECTOR
    iso[floppy_start:floppy_start + FLOPPY_SIZE] = floppy
    kernel_start = kernel_file_lba * SECTOR
    iso[kernel_start:kernel_start + len(kernel_elf)] = kernel_elf

    if iso[16 * SECTOR + 40:16 * SECTOR + 47] != b"MSPC_OS":
        raise ValueError("ISO volume label validation failed")
    if sum(struct.unpack("<16H", catalog[:32])) & 0xffff:
        raise ValueError("El Torito validation-entry checksum failed")
    if catalog[32] != 0x88 or catalog[33] != 2 or struct.unpack_from("<I", catalog, 40)[0] != FLOPPY_LBA:
        raise ValueError("El Torito boot-entry validation failed")
    if iso[floppy_start + 510:floppy_start + 512] != b"\x55\xaa":
        raise ValueError("embedded BIOS boot signature missing")
    floppy_kernel_start = KERNEL_LBA * 512
    if iso[floppy_start + floppy_kernel_start:floppy_start + floppy_kernel_start + len(kernel_bin)] != kernel_bin:
        raise ValueError("embedded kernel validation failed")
    pathlib.Path(output_path).write_bytes(iso)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("usage: mkiso.py BOOT.BIN KERNEL.BIN KERNEL.ELF OUTPUT.ISO")
    make_iso(*sys.argv[1:])