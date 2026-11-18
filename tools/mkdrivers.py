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

MAGIC = b"MSPCDRV1"


def build_pack(directory, output):
    files = sorted(pathlib.Path(directory).glob("*.drv"))
    if not files:
        raise ValueError("drivers directory contains no .drv manifests")
    pack = bytearray(MAGIC)
    pack += struct.pack("<H", len(files))
    for path in files:
        name = path.name.encode("ascii")
        data = path.read_bytes()
        if len(name) > 63 or len(data) > 1024:
            raise ValueError(f"driver manifest is too large: {path.name}")
        pack += struct.pack("<BH", len(name), len(data))
        pack += name
        pack += data
    pathlib.Path(output).write_bytes(pack)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: mkdrivers.py DRIVER_DIRECTORY OUTPUT_PACK")
    try:
        build_pack(sys.argv[1], sys.argv[2])
    except (OSError, ValueError) as error:
        raise SystemExit(f"driver pack error: {error}")