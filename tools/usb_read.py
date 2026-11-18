#!/usr/bin/env python3
#
# MSPC (MOLOTOK SYSTEM Peripheral System)
# Copyright (C) 2026 Molotok
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later later version.

import pathlib
import struct
import sys

SECTOR = 512


def read_exact(image, offset, size):
    image.seek(offset)
    data = image.read(size)
    if len(data) != size:
        raise ValueError("read past end of USB or image")
    return data


def partition_table(image):
    mbr = read_exact(image, 0, SECTOR)
    if mbr[510:512] != b"\x55\xaa":
        return [{"number": 1, "start": 0, "sectors": 0}]
    partitions = []
    for index in range(4):
        entry = mbr[446 + index * 16:462 + index * 16]
        kind = entry[4]
        start, sectors = struct.unpack_from("<II", entry, 8)
        if kind and start and sectors:
            partitions.append({"number": index + 1, "start": start, "sectors": sectors, "type": kind})
    if not partitions:
        return [{"number": 1, "start": 0, "sectors": 0}]
    return partitions


class FatReader:
    def __init__(self, image, partition_start):
        self.image = image
        self.base = partition_start * SECTOR
        boot = read_exact(image, self.base, SECTOR)
        fat16_signature = boot[54:62]
        fat32_signature = boot[82:90]
        if boot[510:512] != b"\x55\xaa" or (fat16_signature not in (b"FAT12   ", b"FAT16   ") and fat32_signature != b"FAT32   "):
            raise ValueError("no FAT12/FAT16/FAT32 boot sector at partition start")
        self.bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
        if self.bytes_per_sector not in (512, 1024, 2048, 4096):
            raise ValueError("unsupported FAT bytes-per-sector value")
        self.sectors_per_cluster = boot[13]
        self.reserved = struct.unpack_from("<H", boot, 14)[0]
        self.fats = boot[16]
        root_entries = struct.unpack_from("<H", boot, 17)[0]
        total16 = struct.unpack_from("<H", boot, 19)[0]
        sectors_per_fat16 = struct.unpack_from("<H", boot, 22)[0]
        total32 = struct.unpack_from("<I", boot, 32)[0]
        sectors_per_fat32 = struct.unpack_from("<I", boot, 36)[0]
        self.total = total16 or total32
        sectors_per_fat = sectors_per_fat16 or sectors_per_fat32
        root_directory_sectors = ((root_entries * 32) + self.bytes_per_sector - 1) // self.bytes_per_sector
        first_fat = self.reserved
        first_root = first_fat + self.fats * sectors_per_fat
        self.first_data = first_root + root_directory_sectors
        self.sectors_per_fat = sectors_per_fat
        self.fat = read_exact(image, self.base + first_fat * self.bytes_per_sector,
                              sectors_per_fat * self.bytes_per_sector)

        if fat32_signature == b"FAT32   ":
            self.type = 32
            self.root_directory_sector = 0
            self.root_clusters = struct.unpack_from("<I", boot, 44)[0] & 0x0fffffff
            self.root_size = 0
        else:
            self.type = 16

            self.root_directory_sector = first_root
            self.root_clusters = 0
            self.root_size = root_directory_sectors * self.bytes_per_sector
        

    def cluster_offset(self, cluster):
        sector = self.first_data + (cluster - 2) * self.sectors_per_cluster
        return self.base + sector * self.bytes_per_sector

    def next_cluster(self, cluster):
        if self.type == 16:
            offset = cluster * 2
            if offset + 2 > len(self.fat):
                return 0xffffffff
            value = struct.unpack_from("<H", self.fat, offset)[0]
            return value if value < 0xfff8 else 0xffffffff
        offset = cluster * 4
        if offset + 4 > len(self.fat):
            return 0xffffffff
        value = struct.unpack_from("<I", self.fat, offset)[0] & 0x0fffffff
        if value >= 0x0ffffff8:
            return 0xffffffff
        return value

    def cluster_chain_data(self, start_cluster, size):
        if not start_cluster:
            return b""
        output = bytearray()
        cluster = start_cluster
        while cluster >= 2 and cluster < 0x0ffffff8:
            offset = self.cluster_offset(cluster)
            for _ in range(self.sectors_per_cluster):
                block = read_exact(self.image, offset, self.bytes_per_sector)
                output.extend(block)
                if len(output) >= size:
                    return bytes(output[:size])
                offset += self.bytes_per_sector
            cluster = self.next_cluster(cluster)
        return bytes(output[:size])

    def directory_entries(self, start_cluster=None):
        if self.type == 16 and start_cluster is None:
            data = read_exact(self.image, self.base + self.root_directory_sector * self.bytes_per_sector,
                              self.root_size)
        else:
            cluster = start_cluster if start_cluster is not None else self.root_clusters
            data = self.cluster_chain_data(cluster, 16 * 1024 * 1024)
        entries = []
        long_name = bytearray()
        for offset in range(0, len(data), 32):
            entry = data[offset:offset + 32]
            if not entry or entry[0] == 0:
                break
            if entry[0] == 0xe5:
                continue
            attr = entry[11]
            if attr == 0x0f:
                name_part = entry[1:11] + entry[14:26] + entry[28:32]
                chars = []
                for index in range(0, len(name_part), 2):
                    value = name_part[index] | (name_part[index + 1] << 8)
                    if value == 0 or value == 0xffff:
                        break
                    if value < 128:
                        chars.append(chr(value))
                long_name = bytearray(chars) + long_name
                continue
            base = entry[8:11].decode("ascii", "ignore").strip()
            name = entry[0:8].decode("ascii", "ignore").strip()
            if base:
                name = name.rstrip() + "." + base.rstrip()
            if long_name:
                name = long_name.decode("utf-16le", "replace")
                long_name = bytearray()
            cluster = struct.unpack_from("<H", entry, 26)[0]
            if self.type == 32:
                cluster |= struct.unpack_from("<H", entry, 20)[0] << 16
            size = struct.unpack_from("<I", entry, 28)[0]
            entries.append({"name": name, "cluster": cluster, "size": size, "directory": bool(attr & 0x10)})
        return entries


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: usb-read.py DEVICE_OR_IMAGE PARTITION [ls|copy NAME OUTPUT] ")
    path = pathlib.Path(sys.argv[1]).expanduser()
    if not path.exists():
        raise SystemExit("USB/image path does not exist")
    partition_number = int(sys.argv[2])
    command = sys.argv[3] if len(sys.argv) > 3 else "ls"

    with path.open("rb") as image:
        partitions = partition_table(image)
        selected = next((p for p in partitions if p["number"] == partition_number), None)
        if not selected:
            raise SystemExit("partition number not found")
        fat = FatReader(image, selected["start"])
        entries = fat.directory_entries()
        if command == "ls":
            print(f"FAT{fat.type} partition {partition_number}")
            for entry in entries:
                suffix = "/" if entry["directory"] else f" {entry['size']} bytes"
                print(entry["name"] + suffix)
            return
        if command != "copy" or len(sys.argv) != 6:
            raise SystemExit("usage: usb-read.py DEVICE_OR_IMAGE PARTITION copy NAME OUTPUT")
        name = sys.argv[4].lower()
        output = pathlib.Path(sys.argv[5])
        entry = next((item for item in entries if item["name"].lower() == name), None)
        if not entry:
            raise SystemExit("file not found in root directory")
        if entry["directory"]:
            raise SystemExit("directory copy is not supported yet")
        data = fat.cluster_chain_data(entry["cluster"], entry["size"])
        output.write_bytes(data)
        print(f"Copied {len(data)} bytes to {output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error) as error:
        raise SystemExit(f"USB read error: {error}")
