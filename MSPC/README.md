# MSPC OS 1.1

License: GNU GPLv2 or later. Read and sign [LICENSE.md](LICENSE.md).

MSPC OS 1.1 is an experimental 32-bit x86 operating system written in C and NASM. It boots to a conventional VGA command line on legacy BIOS.

## Build

Requirements: GCC with 32-bit code generation, NASM, GNU make, binutils, and Python 3.

```sh
make
make iso
make usb-image
```

`make` builds `build/kernel.elf`; `make iso` creates `build/mspc-os.iso`; `make usb-image` creates the raw BIOS USB image `build/mspc-usb.img`.

## Accounts and Files

On first start, create an account. That first account is root. Use `useradd <name>` to add users, `root grant <name>` to grant root to more than one account, `root revoke <name>` to revoke it, and `su <name>` to switch users. `chmod <octal-mode> <path>` changes RAMFS permissions.

Root owns `/system`; `/system/boot.conf` is writable only by root by default. `write <path> <text>` and `etxt <path>` can modify files when permissions allow. The RAM filesystem and account table are volatile. Passwords are stored as unsalted 32-bit hashes; this is a learning project and the account system is not suitable for protecting real data.

Other commands include `ls`, `cd`, `pwd`, `mkdir`, `touch`, `cat`, `cp`, `mv`, `rm`, `tree`, `df`, `calc`, `hex`, `dec`, `history`, `mem`, `cpu`, `drivers`, `run`, `download`, `gltest`, `gfx`, and `3dtest`. Run `help` for syntax.

## Text Editor

`etxt <file>` opens a full-screen editor. Use arrows to move, Home/End for the line ends, Insert to toggle overwrite mode, and Backspace/Delete to edit. `Ctrl+S` saves. `Ctrl+Q` asks whether to save, discard, or continue editing. The editor supports files up to 1 KiB in RAM.

## Write and Run Programs

Create `hello.mspc` with `etxt`, save using Ctrl+S, then run `run hello.mspc`. The interpreter supports `print <text>` with `$variable` interpolation, `let <name> <integer>`, `show <name>`, `add|sub|mul|div <name> <integer>`, `input <name>`, `if <name> ==|!=|<|> <integer> goto <line>`, `goto <line>`, `cat <path>`, `write <path> <text>`, `append <path> <text>`, `ping <IPv4-or-host>`, `dns <host>`, `clear`, and `end`. Scripts can contain up to 64 lines, each under 128 characters, and stop after 1,000 executed instructions. Example:

```text
let count 1
print Counting: $count
show count
add count 1
if count < 5 goto 3
end
```

Programs run inside a small interpreter; MSPC does not include a C compiler or executable binary loader yet. Programs and files are stored in RAM and disappear after reboot.

## Install to an Image

Run `make install IMAGE=... PARTITION=1` to install MSPC into a partition of a regular raw disk image. Use `make install-list IMAGE=...` to inspect the MBR and `make install-check IMAGE=...` to validate both the image and the built kernel. Add `YES=1` only for non-interactive installation or `BACKUP=...` to save the overwritten boot-sector range before writing. The direct CLI form is `python3 tools/install_image.py install <image> <partition> [--backup <file>] [--yes]`.

The installer accepts regular image files only, not block devices or symlinks. It preserves the MBR and every other partition, writes only the selected partition's boot sector and kernel payload, and verifies the written bytes after installation. Do not select an image containing data you need; the selected partition's boot area and kernel region will be overwritten. To boot MSPC, configure an existing BIOS boot manager to chainload that partition.

Physical disks are supported through `make disk-list DISK=/dev/sdX`, `make disk-check DISK=/dev/sdX`, and `make disk-install DISK=/dev/sdX PARTITION=1`. The disk and partition must be unmounted, and the installer requires root privileges, a backup for rollback is recommended, and both the MBR and write range are verified. Add `YES=1` only for non-interactive use; it still requires typing the exact device path. Raw disk writes are destructive, so verify the target with `disk-list` before installation.

The kernel has no persistent disk filesystem or in-OS installer yet, so files and accounts reset on every boot. BIOS boot only; UEFI and a 16 MB memory guarantee are not implemented.

## USB Boot

Write `build/mspc-usb.img` to a USB drive using a raw/DD image mode in a flashing tool. This overwrites the entire selected USB drive; verify the target carefully. The image is for legacy BIOS USB boot, not UEFI. The kernel has no USB storage driver, so it cannot install itself from its own shell.

## Network

The VirtualBox PCnet driver provides polling Ethernet I/O. `net`/`ifconfig` initializes DHCP and prints the IPv4 address, subnet, gateway, DNS server, and MAC. `dns <host>` resolves A records. `ping <IPv4-or-hostname>` uses ARP, IPv4, ICMP, and DNS. Plain HTTP is available with `download http://host/path output-file`; downloads are limited to 8 KiB by RAMFS. HTTPS, compressed responses, and folder downloads are not implemented. VirtualBox NAT defaults are used if DHCP does not answer.

## USB FAT Reading

Host utilities can list and copy a file from FAT16/FAT32 partitions:

```sh
make usb-read DEVICE=/dev/sdX PARTITION=1
make usb-read DEVICE=/dev/sdX PARTITION=1 NAME=HELLO.TXT OUTPUT=HELLO.TXT
make test-usb
```

This reads the device from the Linux host. It is not yet an in-kernel USB MSC/XHCI storage driver.

## Software 3D and GL Test

Inside MSPC, `gltest`, `3dtest`, or `gfx` runs a fixed-point software 3D wireframe cube. Arrow keys rotate the cube; Esc exits the graphics view and restores the shell. It is a small compatibility subset with point/line/triangle primitives, not an OpenGL 3.x/4.x implementation or GPU driver. Run `make test-gl` for the host-side renderer test.

## Drivers

Run `make test-all` for the network, USB FAT, and software GL host tests.

Driver implementations and manifests are kept in `drivers/`. The build packs the `.drv` files into the kernel image; at boot the kernel validates the pack, activates matching built-in drivers, and copies their manifests into the RAMFS directory `/drivers`. Use `drivers` to see status or `driver load vga-text.drv` as root to re-activate a registered built-in. Adding a manifest alone does not provide new machine code: a driver implementation must also be written and linked into the kernel. Driver manifests and files are volatile until persistent storage is implemented.