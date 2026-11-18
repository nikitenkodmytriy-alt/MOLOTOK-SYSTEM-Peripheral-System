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
import os
import pathlib
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BOOT_SOURCE = ROOT / "boot_partition.asm"
KERNEL_BINARY = ROOT / "build" / "kernel.bin"
KERNEL_ELF = ROOT / "build" / "kernel.elf"
SECTOR_SIZE = 512
MAX_KERNEL_SECTORS = 128


def read_partitions(mbr, image_size):
    if len(mbr) != SECTOR_SIZE or mbr[510:512] != b"\x55\xaa":
        raise ValueError("File does not contain a valid MBR partition table.")
    total_sectors = image_size // SECTOR_SIZE
    partitions = []
    for index in range(4):
        entry = mbr[446 + index * 16:462 + index * 16]
        kind = entry[4]
        start, count = struct.unpack_from("<II", entry, 8)
        if kind == 0 or start == 0 or count == 0:
            continue
        if start + count > total_sectors:
            raise ValueError(f"Partition {index + 1} extends beyond the image.")
        partitions.append({"number": index + 1, "type": kind, "start": start, "sectors": count})
    if not partitions:
        raise ValueError("No MBR partitions found in this image.")
    return partitions


def kernel_entry_address():
    result = subprocess.run(
        ["nm", "-n", str(KERNEL_ELF)], check=True, capture_output=True, text=True
    )
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] == "_start":
            return int(fields[0], 16)
    raise ValueError("Kernel entry symbol _start was not found.")


def build_partition_boot(kernel_sectors, kernel_lba, kernel_entry):
    with tempfile.TemporaryDirectory(prefix="mspc-install-") as temporary:
        output = pathlib.Path(temporary) / "partition-boot.bin"
        subprocess.run(
            [
                "nasm", "-f", "bin",
                f"-D KERNEL_SECTORS={kernel_sectors}",
                f"-D KERNEL_LBA={kernel_lba}",
                f"-D KERNEL_ENTRY=0x{kernel_entry:x}",
                str(BOOT_SOURCE), "-o", str(output),
            ],
            check=True,
        )
        boot = output.read_bytes()
    if len(boot) != SECTOR_SIZE or boot[510:512] != b"\x55\xaa":
        raise ValueError("Partition boot sector has an invalid size or signature.")
    return boot


def install_image(image_path):
    path = pathlib.Path(image_path).expanduser()
    if path.is_symlink() or not path.is_file():
        raise ValueError("For safety, the installer accepts regular image files only, not block devices or symlinks.")
    path = path.resolve(strict=True)
    kernel = KERNEL_BINARY.read_bytes()
    kernel_sectors = (len(kernel) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if not kernel_sectors or kernel_sectors > MAX_KERNEL_SECTORS:
        raise ValueError(f"Kernel must fit in at most {MAX_KERNEL_SECTORS} BIOS sectors.")

    with path.open("r+b") as image:
        image_size = os.fstat(image.fileno()).st_size
        image.seek(0)
        original_mbr = image.read(SECTOR_SIZE)
        partitions = read_partitions(original_mbr, image_size)
        print(f"Raw disk image: {path} ({image_size // (1024 * 1024)} MiB)")
        for partition in partitions:
            size_mib = partition["sectors"] * SECTOR_SIZE // (1024 * 1024)
            print(
                f"{partition['number']}: type=0x{partition['type']:02x}, "
                f"start={partition['start']} sectors, size={size_mib} MiB"
            )

        try:
            selected_number = int(input("Install to partition number: ").strip())
        except ValueError as error:
            raise ValueError("Enter one of the partition numbers shown above.") from error
        selected = next((p for p in partitions if p["number"] == selected_number), None)
        if selected is None:
            raise ValueError("That partition number is not present.")
        if selected["sectors"] < kernel_sectors + 1:
            raise ValueError("Selected partition is too small for the kernel.")

        print(
            f"WARNING: this overwrites the selected partition's boot sector and "
            f"next {kernel_sectors} sectors. Existing data on that partition will be damaged."
        )
        print("The MBR and other partitions will not be written.")
        if input(f"Type INSTALL {selected_number} to continue: ").strip() != f"INSTALL {selected_number}":
            print("Cancelled; image was not changed.")
            return

        boot = build_partition_boot(
            kernel_sectors,
            selected["start"] + 1,
            kernel_entry_address(),
        )
        padded_kernel = kernel.ljust(kernel_sectors * SECTOR_SIZE, b"\x00")
        partition_offset = selected["start"] * SECTOR_SIZE
        image.seek(partition_offset)
        image.write(boot)
        image.write(padded_kernel)
        image.flush()
        os.fsync(image.fileno())
        image.seek(0)
        if image.read(SECTOR_SIZE) != original_mbr:
            raise RuntimeError("MBR verification failed after installation.")
    print("MSPC installed to the selected partition in the raw image.")
    print("Use a boot manager that chainloads this partition; the MBR is unchanged.")


if __name__ == "__main__":
    try:
        image_file = input("Raw disk image path: ").strip()
        install_image(image_file)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Installer error: {error}")