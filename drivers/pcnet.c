/*
 * MSPC (MOLOTOK SYSTEM Peripheral System)
 * Copyright (C) 2026 Molotok
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but annoying to plagiarists, WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 */

#include "driver.h"

#define PCNET_VENDOR 0x1022
#define PCNET_DEVICE 0x2000
#define PCI_CONFIG_ADDRESS 0x0cf8
#define PCI_CONFIG_DATA 0x0cfc
#define PCNET_RX_COUNT 8
#define PCNET_TX_COUNT 4
#define PCNET_BUFFER_SIZE 1536
#define DESC_OWN 0x8000
#define DESC_STP 0x0200
#define DESC_ENP 0x0100
#define DESC_ERR 0x4000

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t address;
    int16_t buffer_length;
    uint16_t status;
    uint32_t message_length;
    uint32_t reserved;
} pcnet_descriptor_t;

typedef struct __attribute__((packed, aligned(16))) {
    uint16_t mode;
    uint8_t receive_transmit_lengths;
    uint8_t physical_address[6];
    uint16_t logical_filter[4];
    uint32_t receive_ring;
    uint32_t transmit_ring;
} pcnet_init_block_t;

static volatile pcnet_descriptor_t receive_descriptors[PCNET_RX_COUNT] __attribute__((aligned(16)));
static volatile pcnet_descriptor_t transmit_descriptors[PCNET_TX_COUNT] __attribute__((aligned(16)));
static uint8_t receive_buffers[PCNET_RX_COUNT][PCNET_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t transmit_buffers[PCNET_TX_COUNT][PCNET_BUFFER_SIZE] __attribute__((aligned(16)));
static pcnet_init_block_t initialization_block __attribute__((aligned(16)));
static uint8_t hardware_address[6];
static uint16_t io_base;
static size_t receive_index;
static size_t transmit_index;
static int initialized;

static void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000u | (uint32_t)bus << 16 | (uint32_t)device << 11 |
                       (uint32_t)function << 8 | (offset & 0xfcu);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    uint32_t address = 0x80000000u | (uint32_t)bus << 16 | (uint32_t)device << 11 |
                       (uint32_t)function << 8 | (offset & 0xfcu);
    outl(PCI_CONFIG_ADDRESS, address);
    uint16_t shift = (uint16_t)(offset & 2u) * 8u;
    uint32_t current = inl(PCI_CONFIG_DATA);
    current = (current & ~(0xffffu << shift)) | (uint32_t)value << shift;
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, current);
}

static int find_pcnet(void) {
    for (uint8_t device = 0; device < 32; device++) {
        uint32_t identity = pci_read32(0, device, 0, 0);
        if ((identity & 0xffffu) != PCNET_VENDOR || (identity >> 16) != PCNET_DEVICE) continue;
        uint32_t bar = pci_read32(0, device, 0, 0x10);
        uint32_t command = pci_read32(0, device, 0, 0x04);
        if (!(bar & 1u)) return 0;
        pci_write16(0, device, 0, 0x04, (uint16_t)command | 0x0005u);
        io_base = (uint16_t)(bar & 0xfffcu);
        return io_base != 0;
    }
    return 0;
}

static uint16_t csr_read(uint16_t index) {
    outw(io_base + 0x12, index);
    return inw(io_base + 0x10);
}

static void csr_write(uint16_t index, uint16_t value) {
    outw(io_base + 0x12, index);
    outw(io_base + 0x10, value);
}

static void bcr_write(uint16_t index, uint16_t value) {
    outw(io_base + 0x12, index);
    outw(io_base + 0x16, value);
}

int duck_pcnet_init(void) {
    if (initialized) return 1;
    if (!find_pcnet()) return 0;
    (void)inw(io_base + 0x14);
    bcr_write(20, 2);
    csr_write(0, 0x0004);

    for (size_t i = 0; i < PCNET_RX_COUNT; i++) {
        receive_descriptors[i].address = (uint32_t)(uintptr_t)receive_buffers[i];
        receive_descriptors[i].buffer_length = -(int16_t)PCNET_BUFFER_SIZE;
        receive_descriptors[i].message_length = 0;
        receive_descriptors[i].reserved = 0;
        receive_descriptors[i].status = DESC_OWN;
    }
    for (size_t i = 0; i < PCNET_TX_COUNT; i++) {
        transmit_descriptors[i].address = (uint32_t)(uintptr_t)transmit_buffers[i];
        transmit_descriptors[i].buffer_length = 0;
        transmit_descriptors[i].message_length = 0;
        transmit_descriptors[i].reserved = 0;
        transmit_descriptors[i].status = 0;
    }

    initialization_block.mode = 0;
    initialization_block.receive_transmit_lengths = 0x33;
    for (size_t i = 0; i < 6; i++) {
        initialization_block.physical_address[i] = inb(io_base + (uint16_t)i);
        hardware_address[i] = initialization_block.physical_address[i];
    }
    for (size_t i = 0; i < 4; i++) initialization_block.logical_filter[i] = 0;
    initialization_block.receive_ring = (uint32_t)(uintptr_t)receive_descriptors;
    initialization_block.transmit_ring = (uint32_t)(uintptr_t)transmit_descriptors;

    uint32_t init_address = (uint32_t)(uintptr_t)&initialization_block;
    csr_write(1, (uint16_t)init_address);
    csr_write(2, (uint16_t)(init_address >> 16));
    csr_write(0, 0x0001);
    uint32_t timeout = 10000000;
    while (timeout-- && !(csr_read(0) & 0x0100)) { }
    if (!timeout) return 0;
    csr_write(0, 0x0142);
    if (!(csr_read(0) & 0x0001)) return 0;
    receive_index = 0;
    transmit_index = 0;
    initialized = 1;
    return 1;
}

const uint8_t *duck_pcnet_mac(void) {
    return hardware_address;
}

int duck_pcnet_send(const uint8_t *frame, size_t length) {
    if (!initialized || !frame || !length || length > PCNET_BUFFER_SIZE) return 0;
    volatile pcnet_descriptor_t *descriptor = &transmit_descriptors[transmit_index];
    if (descriptor->status & DESC_OWN) return 0;
    for (size_t i = 0; i < length; i++) transmit_buffers[transmit_index][i] = frame[i];
    size_t transmitted_length = length < 60 ? 60 : length;
    for (size_t i = length; i < transmitted_length; i++) transmit_buffers[transmit_index][i] = 0;
    descriptor->buffer_length = -(int16_t)transmitted_length;
    descriptor->message_length = 0;
    descriptor->status = DESC_OWN | DESC_STP | DESC_ENP;
    csr_write(0, 0x0048);
    uint32_t timeout = 1000000;
    while (timeout-- && (descriptor->status & DESC_OWN)) { }
    if (!timeout || (descriptor->status & DESC_ERR)) return 0;
    transmit_index = (transmit_index + 1) % PCNET_TX_COUNT;
    return 1;
}

int duck_pcnet_receive(uint8_t *frame, size_t capacity, size_t *length) {
    if (!initialized || !frame || !length) return 0;
    volatile pcnet_descriptor_t *descriptor = &receive_descriptors[receive_index];
    uint16_t status = descriptor->status;
    if (status & DESC_OWN) return 0;

    uint32_t received_length = descriptor->message_length & 0x0fffu;
    int valid = !(status & DESC_ERR) && (status & (DESC_STP | DESC_ENP)) == (DESC_STP | DESC_ENP) &&
                received_length >= 4 && received_length - 4 <= capacity;
    if (valid) {
        received_length -= 4;
        for (size_t i = 0; i < received_length; i++) frame[i] = receive_buffers[receive_index][i];
        *length = received_length;
    }
    descriptor->buffer_length = -(int16_t)PCNET_BUFFER_SIZE;
    descriptor->message_length = 0;
    descriptor->status = DESC_OWN;
    receive_index = (receive_index + 1) % PCNET_RX_COUNT;
    csr_write(0, 0x0400);
    return valid;
}