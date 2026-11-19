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
import argparse
import fcntl
import os
import pathlib
import stat
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BOOT_SOURCE = ROOT / "boot_partition.asm"
KERNEL_BINARY = ROOT / "build" / "kernel.bin"
KERNEL_ELF = ROOT / "build" / "kernel.elf"
SECTOR_SIZE = 512
MAX_KERNEL_SECTORS = 65535


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


def open_image(image_path):
    path = pathlib.Path(image_path).expanduser()
    if path.is_symlink() or not path.is_file():
        raise ValueError("Only regular image files are accepted; block devices and symlinks are not allowed.")
    return path.resolve(strict=True)


def open_disk(device_path):
    path = pathlib.Path(device_path).expanduser()
    if path.is_symlink():
        raise ValueError("The disk path must not be a symbolic link.")
    try:
        mode = path.stat().st_mode
    except OSError as error:
        raise ValueError(f"Cannot access disk: {error}") from error
    if not stat.S_ISBLK(mode):
        raise ValueError("This command requires a block device, for example /dev/sda.")
    if os.geteuid() != 0:
        raise ValueError("Installing to a physical disk requires root privileges.")
    return path


def block_device_size(path):
    with path.open("rb", buffering=0) as device:
        size = os.fstat(device.fileno()).st_size
        if size:
            return size
        size = struct.unpack("Q", fcntl.ioctl(device.fileno(), 0x80081272, bytes(8))[0:8])[0]
    return size


def read_image_layout(path):
    block_device = stat.S_ISBLK(path.stat().st_mode)
    with path.open("rb", buffering=0) as image:
        image_size = block_device_size(path) if block_device else os.fstat(image.fileno()).st_size
        if image_size % SECTOR_SIZE:
            raise ValueError("Image size is not aligned to 512-byte sectors.")
        mbr = read_exact(image, 0, SECTOR_SIZE)
        partitions = read_partitions(mbr, image_size)
    return image_size, mbr, partitions


def read_exact(image, offset, size):
    image.seek(offset)
    data = image.read(size)
    if len(data) != size:
        raise ValueError("Unexpected end of image.")
    return data


def print_layout(path):
    image_size, _, partitions = read_image_layout(path)
    print(f"Image: {path}")
    print(f"Size: {image_size} bytes ({image_size // SECTOR_SIZE} sectors)")
    print("Partitions:")
    for partition in partitions:
        size_bytes = partition["sectors"] * SECTOR_SIZE
        print(
            f"  {partition['number']}: type=0x{partition['type']:02x}, "
            f"LBA={partition['start']}, sectors={partition['sectors']}, "
            f"size={size_bytes // (1024 * 1024)} MiB"
        )


def load_payload():
    if not KERNEL_BINARY.is_file() or not KERNEL_ELF.is_file():
        raise ValueError("Kernel artifacts are missing. Run 'make' before installing.")
    kernel = KERNEL_BINARY.read_bytes()
    kernel_sectors = (len(kernel) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if not kernel_sectors or kernel_sectors > MAX_KERNEL_SECTORS:
        raise ValueError(f"Kernel must fit in at most {MAX_KERNEL_SECTORS} BIOS sectors.")
    return kernel, kernel_sectors


def create_backup(path, offset, size, backup_path):
    with path.open("rb") as source:
        original = read_exact(source, offset, size)
    target = pathlib.Path(backup_path).expanduser()
    if target.is_symlink():
        raise ValueError("The backup path must not be a symbolic link.")
    if target.exists():
        raise ValueError("Backup file already exists; choose another path.")
    target.write_bytes(original)
    print(f"Backup written: {target}")
    return original


def restore_backup(path, offset, original):
    with path.open("r+b") as image:
        image.seek(offset)
        image.write(original)
        image.flush()
        os.fsync(image.fileno())


def mounted_parts(path):
    try:
        result = subprocess.run(
            ["lsblk", "-nrpo", "NAME,MOUNTPOINTS", str(path)],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return []
    mounted = []
    for line in result.stdout.splitlines():
        fields = line.rsplit(maxsplit=1)
        if len(fields) == 2 and fields[1].strip():
            mounted.append(f"{fields[0]} {fields[1]}")
    return mounted


def disk_layout(path):
    image_size, mbr, partitions = read_image_layout(path)
    print(f"Disk: {path}")
    print(f"Size: {image_size} bytes ({image_size // SECTOR_SIZE} sectors)")
    mounted = mounted_parts(path)
    if mounted:
        print("Mounted filesystems:")
        for item in mounted:
            print(f"  {item}")
    print("Partitions:")
    for partition in partitions:
        size_bytes = partition["sectors"] * SECTOR_SIZE
        print(
            f"  {partition['number']}: type=0x{partition['type']:02x}, "
            f"LBA={partition['start']}, sectors={partition['sectors']}, "
            f"size={size_bytes // (1024 * 1024)} MiB"
        )


def install_image(image_path, partition_number=None, yes=False, backup_path=None):
    path = open_image(image_path)
    image_size, original_mbr, partitions = read_image_layout(path)
    kernel, kernel_sectors = load_payload()
    padded_kernel = kernel.ljust(kernel_sectors * SECTOR_SIZE, b"\x00")

    if partition_number is None:
        print_layout(path)
        try:
            partition_number = int(input("Install to partition number: ").strip())
        except (ValueError, EOFError) as error:
            raise ValueError("A partition number is required.") from error

    selected = next((p for p in partitions if p["number"] == partition_number), None)
    if selected is None:
        raise ValueError(f"Partition {partition_number} was not found.")
    if selected["sectors"] < kernel_sectors + 1:
        raise ValueError("The selected partition is too small for the MSPC boot sector and kernel.")

    print(f"Selected partition: {partition_number}")
    print(f"Partition range: LBA {selected['start']} + {selected['sectors']} sectors")
    print(f"Kernel range: LBA {selected['start'] + 1} + {kernel_sectors} sectors")
    print("WARNING: the partition boot sector and kernel payload will be overwritten.")
    print("The MBR and all other partitions will not be changed.")

    if not yes:
        try:
            answer = input(f"Type INSTALL {partition_number} to continue: ").strip()
        except EOFError:
            answer = ""
        if answer != f"INSTALL {partition_number}":
            print("Cancelled; the image was not changed.")
            return False

    boot = build_partition_boot(
        kernel_sectors,
        selected["start"] + 1,
        kernel_entry_address(),
    )
    partition_offset = selected["start"] * SECTOR_SIZE
    overwritten_size = SECTOR_SIZE + len(padded_kernel)
    if backup_path:
        original_payload = create_backup(path, partition_offset, overwritten_size, backup_path)
    else:
        original_payload = None

    try:
        with path.open("r+b") as image:
            image.seek(partition_offset)
            image.write(boot)
            image.write(padded_kernel)
            image.flush()
            os.fsync(image.fileno())

            image.seek(0)
            if image.read(SECTOR_SIZE) != original_mbr:
                raise RuntimeError("MBR verification failed after installation.")
            actual_payload = read_exact(image, partition_offset, overwritten_size)
            expected_payload = boot + padded_kernel
            if actual_payload != expected_payload:
                raise RuntimeError("Written boot sector or kernel verification failed.")
    except Exception:
        if original_payload is not None:
            restore_backup(path, partition_offset, original_payload)
            print("Installation failed; the overwritten range was restored from the backup.", file=sys.stderr)
        raise

    print("MSPC installed successfully.")
    print("Use an existing BIOS boot manager to chainload the selected partition.")
    return True


def install_disk(device_path, partition_number=None, yes=False, backup_path=None):
    path = open_disk(device_path)
    mounted = mounted_parts(path)
    if mounted:
        raise ValueError("The disk or one of its partitions is mounted. Unmount it before installation.")

    image_size, original_mbr, partitions = read_image_layout(path)
    kernel, kernel_sectors = load_payload()
    padded_kernel = kernel.ljust(kernel_sectors * SECTOR_SIZE, b"\x00")

    if partition_number is None:
        disk_layout(path)
        try:
            partition_number = int(input("Install to partition number: ").strip())
        except (ValueError, EOFError) as error:
            raise ValueError("A partition number is required.") from error

    selected = next((p for p in partitions if p["number"] == partition_number), None)
    if selected is None:
        raise ValueError(f"Partition {partition_number} was not found.")
    if selected["sectors"] < kernel_sectors + 1:
        raise ValueError("The selected partition is too small for the MSPC boot sector and kernel.")

    print(f"Selected disk: {path}")
    print(f"Selected partition: {partition_number}")
    print(f"Partition range: LBA {selected['start']} + {selected['sectors']} sectors")
    print(f"Kernel range: LBA {selected['start'] + 1} + {kernel_sectors} sectors")
    print("DANGER: this destroys the selected partition boot sector and kernel area.")

    if yes:
        try:
            answer = input(f"Type the exact disk path {path} to continue: ").strip()
        except EOFError:
            answer = ""
        if answer != str(path):
            print("Cancelled; the disk was not changed.")
            return False
    else:
        try:
            answer = input(f"Type INSTALL DISK {partition_number} to continue: ").strip()
        except EOFError:
            answer = ""
        if answer != f"INSTALL DISK {partition_number}":
            print("Cancelled; the disk was not changed.")
            return False

    boot = build_partition_boot(
        kernel_sectors,
        selected["start"] + 1,
        kernel_entry_address(),
    )
    partition_offset = selected["start"] * SECTOR_SIZE
    overwritten_size = SECTOR_SIZE + len(padded_kernel)
    if backup_path:
        original_payload = create_backup(path, partition_offset, overwritten_size, backup_path)
    else:
        original_payload = None

    try:
        with path.open("r+b", buffering=0) as device:
            fcntl.flock(device.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            device.seek(partition_offset)
            device.write(boot)
            device.write(padded_kernel)
            os.fsync(device.fileno())

            device.seek(0)
            if read_exact(device, 0, SECTOR_SIZE) != original_mbr:
                raise RuntimeError("MBR verification failed after installation.")
            actual_payload = read_exact(device, partition_offset, overwritten_size)
            if actual_payload != boot + padded_kernel:
                raise RuntimeError("Written disk boot sector or kernel verification failed.")
    except Exception:
        if original_payload is not None:
            restore_backup(path, partition_offset, original_payload)
            print("Installation failed; the overwritten range was restored from the backup.", file=sys.stderr)
        raise

    print("MSPC installed successfully on the physical disk.")
    print("The MBR and all other partitions were left unchanged.")
    return True


def build_argument_parser():
    parser = argparse.ArgumentParser(
        description="Install MSPC into a partition of a regular raw disk image.",
    )
    subcommands = parser.add_subparsers(dest="command")

    list_command = subcommands.add_parser("list", help="List MBR partitions in an image.")
    list_command.add_argument("image", help="Path to a raw disk image.")
    check_command = subcommands.add_parser("check", help="Validate a raw image and the built kernel.")
    check_command.add_argument("image", help="Path to a raw disk image.")

    install = subcommands.add_parser("install", help="Install MSPC into one partition.")
    install.add_argument("image", help="Path to a raw disk image.")
    install.add_argument("partition", nargs="?", type=int, help="Partition number to overwrite.")
    install.add_argument("--yes", action="store_true", help="Skip the typed confirmation.")
    install.add_argument("--backup", metavar="PATH", help="Save the overwritten range to a backup file.")

    disk_list = subcommands.add_parser("disk-list", help="List partitions on a physical disk.")
    disk_list.add_argument("device", help="Block device, for example /dev/sda.")

    disk_check = subcommands.add_parser("disk-check", help="Validate a physical disk and the built kernel.")
    disk_check.add_argument("device", help="Block device, for example /dev/sda.")

    disk_install = subcommands.add_parser("disk-install", help="Install MSPC on a physical disk.")
    disk_install.add_argument("device", help="Block device, for example /dev/sda.")
    disk_install.add_argument("partition", nargs="?", type=int, help="Partition number to overwrite.")
    disk_install.add_argument("--yes", action="store_true", help="Require exact device-path confirmation.")
    disk_install.add_argument("--backup", metavar="PATH", help="Save the overwritten range to a backup file.")
    return parser


def main(argv=None):
    parser = build_argument_parser()
    args = parser.parse_args(argv)

    if args.command is None:
        parser.print_help()
        return 2
    if args.command == "list":
        image = open_image(args.image)
        print_layout(image)
        return 0
    if args.command == "check":
        image = open_image(args.image)
        read_image_layout(image)
        kernel, kernel_sectors = load_payload()
        print_layout(image)
        print(f"Kernel: {len(kernel)} bytes ({kernel_sectors} sectors), entry 0x{kernel_entry_address():08x}")
        return 0
    if args.command == "disk-list":
        disk_layout(open_disk(args.device))
        return 0
    if args.command == "disk-check":
        disk = open_disk(args.device)
        disk_layout(disk)
        kernel, kernel_sectors = load_payload()
        print(f"Kernel: {len(kernel)} bytes ({kernel_sectors} sectors), entry 0x{kernel_entry_address():08x}")
        return 0

    try:
        if args.command == "install":
            install_image(args.image, args.partition, args.yes, args.backup)
        elif args.command == "disk-install":
            install_disk(args.device, args.partition, args.yes, args.backup)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Installer error: {error}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())