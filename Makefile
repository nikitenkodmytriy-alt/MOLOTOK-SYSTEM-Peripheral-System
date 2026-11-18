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

CC = gcc
NASM = nasm
OBJCOPY = objcopy
NM = nm
CFLAGS = -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin -mno-sse -mno-sse2 -mno-mmx -msoft-float -Wall -Wextra -O2
LDFLAGS = -m32 -nostdlib -no-pie -Wl,-T,linker.ld -Wl,--build-id=none

.PHONY: all iso usb-image install install-image test-net test-usb test-gl test-all usb-read clean

all: build/kernel.elf

iso: build/mspc-os.iso

usb-image: build/mspc-usb.img

install-image: build/kernel.bin build/kernel.elf
	python3 tools/install_image.py

install: install-image

test-net: | build
	$(CC) -std=gnu11 -Wall -Wextra -I. tests/net_stack_test.c -o build/net_stack_test
	build/net_stack_test

test-usb:
	python3 tests/usb_read_test.py

test-gl:
	$(CC) -std=gnu11 -Wall -Wextra -I. tests/mspc_gl_test.c -o build/mspc_gl_test
	build/mspc_gl_test

test-all: test-net test-usb test-gl

usb-read:
	python3 tools/usb_read.py "$(DEVICE)" "$(PARTITION)" $(if $(NAME),copy $(NAME) $(if $(OUTPUT),$(OUTPUT),$(NAME)),ls)

build:
	mkdir -p build

build/boot.o: boot.asm | build
	$(NASM) -f elf32 $< -o $@

build/kernel.o: kernel.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/vga_text.o: drivers/vga_text.c drivers/driver.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/ps2_keyboard.o: drivers/ps2_keyboard.c drivers/driver.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/pcnet.o: drivers/pcnet.c drivers/driver.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/net_stack.o: drivers/net_stack.c drivers/driver.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/mspc_gl.o: drivers/mspc_gl.c drivers/mspc_gl.h drivers/driver.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/drivers.pack: $(wildcard drivers/*.drv) tools/mkdrivers.py | build
	python3 tools/mkdrivers.py drivers $@

build/drivers_blob.o: driver_archive.asm build/drivers.pack
	$(NASM) -f elf32 driver_archive.asm -o $@

build/kernel.elf: build/boot.o build/kernel.o build/vga_text.o build/ps2_keyboard.o build/pcnet.o build/net_stack.o build/mspc_gl.o build/drivers_blob.o linker.ld
	$(CC) $(LDFLAGS) -o $@ build/boot.o build/kernel.o build/vga_text.o build/ps2_keyboard.o build/pcnet.o build/net_stack.o build/mspc_gl.o build/drivers_blob.o

build/kernel.bin: build/kernel.elf
	$(OBJCOPY) -O binary $< $@

build/boot.bin: boot_bios.asm build/kernel.bin
	@kernel_size=$$(stat -c%s build/kernel.bin); \
	kernel_sectors=$$(((kernel_size + 511) / 512)); \
	kernel_entry=$$($(NM) -n build/kernel.elf | awk '$$3 == "_start" { print $$1 }'); \
	$(NASM) -f bin -D KERNEL_SECTORS=$$kernel_sectors -D KERNEL_ENTRY=0x$$kernel_entry $< -o $@

build/mspc-os.iso: build/boot.bin build/kernel.bin build/kernel.elf tools/mkiso.py
	python3 tools/mkiso.py build/boot.bin build/kernel.bin build/kernel.elf $@

build/usb-boot.bin: boot_partition.asm build/kernel.bin build/kernel.elf | build
	@kernel_size=$$(stat -c%s build/kernel.bin); \
	kernel_sectors=$$(((kernel_size + 511) / 512)); \
	kernel_entry=$$($(NM) -n build/kernel.elf | awk '$$3 == "_start" { print $$1 }'); \
	$(NASM) -f bin -D KERNEL_SECTORS=$$kernel_sectors -D KERNEL_LBA=2049 -D KERNEL_ENTRY=0x$$kernel_entry $< -o $@

build/mspc-usb.img: build/usb-boot.bin build/kernel.bin tools/mkusb.py
	python3 tools/mkusb.py build/usb-boot.bin build/kernel.bin $@

clean:
	rm -rf build