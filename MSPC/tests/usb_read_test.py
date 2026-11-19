#!/usr/bin/env python3
#
# MSPC (MOLOTOK SYSTEM Peripheral System)
# Copyright (C) 2026 Molotok
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

import pathlib
import struct
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix="mspc-usb-test-") as temporary:
    temporary = pathlib.Path(temporary)
    image = temporary / "flash.img"
    output = temporary / "HELLO.TXT"
    sectors = 10240
    partition_start = 2048

    data = bytearray(sectors * 512)
    entry = bytearray(16)
    entry[4] = 0x06
    struct.pack_into("<II", entry, 8, partition_start, sectors - partition_start)
    data[446:462] = entry
    data[510:512] = b"\x55\xaa"

    base = partition_start * 512
    boot = bytearray(512)
    boot[54:62] = b"FAT16   "
    struct.pack_into("<H", boot, 11, 512)
    boot[13] = 1
    struct.pack_into("<H", boot, 14, 1)
    boot[16] = 2
    struct.pack_into("<H", boot, 17, 512)
    struct.pack_into("<H", boot, 19, sectors - partition_start)
    struct.pack_into("<H", boot, 22, 16)
    boot[510:512] = b"\x55\xaa"
    data[base:base + 512] = boot

    fat = bytearray(16 * 512)
    struct.pack_into("<H", fat, 0, 0xfff8)
    struct.pack_into("<H", fat, 2, 0xffff)
    fat_offset = base + 512
    data[fat_offset:fat_offset + len(fat)] = fat
    data[fat_offset + len(fat):fat_offset + 2 * len(fat)] = fat

    root_offset = base + (1 + 32) * 512
    directory_entry = bytearray(512)
    directory_entry[0:11] = b"HELLO   TXT"
    directory_entry[11] = 0x20
    struct.pack_into("<H", directory_entry, 26, 2)
    struct.pack_into("<I", directory_entry, 28, 12)
    data[root_offset:root_offset + 512] = directory_entry

    data_start = base + (1 + 32 + 32) * 512
    data[data_start:data_start + 12] = b"MSPC USB OK"
    image.write_bytes(data)

    listing = subprocess.run(
        [sys.executable, str(root / "tools/usb_read.py"), str(image), "1", "ls"],
        check=True,
        text=True,
        capture_output=True,
    )
    assert "HELLO.TXT" in listing.stdout

    subprocess.run(
        [sys.executable, str(root / "tools/usb_read.py"), str(image), "1", "copy", "HELLO.TXT", str(output)],
        check=True,
        text=True,
        capture_output=True,
    )
    assert output.read_bytes() == b"MSPC USB OK\x00"

print("PASS: FAT16 USB image root listing and file copy")
