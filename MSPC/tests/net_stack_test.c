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

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../drivers/driver.h"

static uint8_t queued_frame[1514];
static size_t queued_size;
static int queued_ready;
static uint8_t transmitted_frame[1514];
static size_t transmitted_size;
static uint8_t last_arp_target[4];
static uint32_t test_seconds;

#define static
#include "../drivers/net_stack.c"
#undef static

static const uint8_t local_mac[6] = { 0x08, 0x00, 0x27, 0x12, 0x34, 0x56 };
static const uint8_t peer_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

const uint8_t *duck_pcnet_mac(void) {
    return local_mac;
}

uint32_t duck_timer_seconds(void) {
    return test_seconds++;
}

int duck_pcnet_init(void) {
    return 1;
}

static void queue_arp_reply(const uint8_t *request) {
    memset(queued_frame, 0, sizeof(queued_frame));
    memcpy(queued_frame, local_mac, 6);
    memcpy(queued_frame + 6, peer_mac, 6);
    write_be16(queued_frame + 12, ETHERTYPE_ARP);
    write_be16(queued_frame + 14, 1);
    write_be16(queued_frame + 16, ETHERTYPE_IPV4);
    queued_frame[18] = 6;
    queued_frame[19] = 4;
    write_be16(queued_frame + 20, 2);
    memcpy(queued_frame + 22, peer_mac, 6);
    memcpy(queued_frame + 28, request + 38, 4);
    memcpy(queued_frame + 32, local_mac, 6);
    memcpy(queued_frame + 38, request + 28, 4);
    queued_size = 42;
    queued_ready = 1;
}

static void queue_dns_reply(const uint8_t *request) {
    const uint8_t *query = request + 42;
    size_t question_end = 12;
    while (query[question_end]) question_end += 1 + query[question_end];
    question_end += 5;

    uint8_t dns[256] = { 0 };
    write_be16(dns, read_be16(query));
    write_be16(dns + 2, 0x8180);
    write_be16(dns + 4, 1);
    write_be16(dns + 6, 1);
    memcpy(dns + 12, query + 12, question_end - 12);
    size_t offset = question_end;
    dns[offset++] = 0xc0;
    dns[offset++] = 0x0c;
    write_be16(dns + offset, 1);
    offset += 2;
    write_be16(dns + offset, 1);
    offset += 2;
    dns[offset++] = 0;
    dns[offset++] = 0;
    dns[offset++] = 0;
    dns[offset++] = 60;
    write_be16(dns + offset, 4);
    offset += 2;
    dns[offset++] = 93;
    dns[offset++] = 184;
    dns[offset++] = 216;
    dns[offset++] = 34;

    memset(queued_frame, 0, sizeof(queued_frame));
    memcpy(queued_frame, local_mac, 6);
    memcpy(queued_frame + 6, peer_mac, 6);
    write_be16(queued_frame + 12, ETHERTYPE_IPV4);
    uint8_t *ip = queued_frame + 14;
    uint8_t *udp = ip + 20;
    uint16_t udp_size = (uint16_t)(8 + offset);
    write_ipv4_header(ip, dns_address, local_address, 17, udp_size, 2);
    write_be16(udp, 53);
    write_be16(udp + 2, 49153);
    write_be16(udp + 4, udp_size);
    memcpy(udp + 8, dns, offset);
    queued_size = 14 + 20 + udp_size;
    queued_ready = 1;
}

static void queue_dhcp_message(uint32_t transaction, uint8_t message_type) {
    memset(queued_frame, 0, sizeof(queued_frame));
    write_be16(queued_frame + 12, ETHERTYPE_IPV4);
    uint8_t *ip = queued_frame + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bootp = udp + 8;
    bootp[0] = 2;
    bootp[4] = (uint8_t)(transaction >> 24);
    bootp[5] = (uint8_t)(transaction >> 16);
    bootp[6] = (uint8_t)(transaction >> 8);
    bootp[7] = (uint8_t)transaction;
    bootp[236] = 99;
    bootp[237] = 130;
    bootp[238] = 83;
    bootp[239] = 99;
    bootp[240] = 53;
    bootp[241] = 1;
    bootp[242] = message_type;
    bootp[243] = 255;
    uint16_t udp_length = 8 + 244;
    write_ipv4_header(ip, dns_address, local_address, 17, udp_length, 3);
    write_be16(udp, 67);
    write_be16(udp + 2, 68);
    write_be16(udp + 4, udp_length);
    queued_size = 14 + 20 + udp_length;
    queued_ready = 1;
}

static void queue_icmp_reply(const uint8_t *request) {
    memset(queued_frame, 0, sizeof(queued_frame));
    memcpy(queued_frame, request + 6, 6);
    memcpy(queued_frame + 6, request, 6);
    write_be16(queued_frame + 12, ETHERTYPE_IPV4);
    const uint8_t *request_ip = request + 14;
    uint8_t *ip = queued_frame + 14;
    uint16_t payload_length = (uint16_t)(read_be16(request_ip + 2) - 20);
    write_ipv4_header(ip, request_ip + 16, request_ip + 12, IP_PROTOCOL_ICMP, payload_length,
                      read_be16(request_ip + 4));
    uint8_t *icmp = ip + 20;
    const uint8_t *request_icmp = request_ip + 20;
    for (size_t i = 0; i < payload_length; i++) icmp[i] = request_icmp[i];
    icmp[0] = ICMP_ECHO_REPLY;
    write_be16(icmp + 2, 0);
    write_be16(icmp + 2, checksum(icmp, payload_length));
    queued_size = 14 + 20 + payload_length;
    queued_ready = 1;
}

int duck_pcnet_send(const uint8_t *frame, size_t length) {
    assert(length <= sizeof(transmitted_frame));
    memcpy(transmitted_frame, frame, length);
    transmitted_size = length;
    uint16_t type = read_be16(frame + 12);
    if (type == ETHERTYPE_ARP) {
        memcpy(last_arp_target, frame + 38, sizeof(last_arp_target));
        queue_arp_reply(frame);
    }
    else if (type == ETHERTYPE_IPV4 && frame[23] == 1) queue_icmp_reply(frame);
    else if (type == ETHERTYPE_IPV4 && read_be16(frame + 36) == 53) queue_dns_reply(frame);
    return 1;
}

int duck_pcnet_receive(uint8_t *frame, size_t capacity, size_t *length) {
    if (!queued_ready || queued_size > capacity) return 0;
    memcpy(frame, queued_frame, queued_size);
    *length = queued_size;
    queued_ready = 0;
    return 1;
}

static void test_dhcp_discover(void) {
    assert(send_dhcp(0x4455434b, 1, 0, 0));
    assert(read_be16(transmitted_frame + 12) == ETHERTYPE_IPV4);
    const uint8_t *udp = transmitted_frame + 14 + 20;
    assert(read_be16(udp) == 68 && read_be16(udp + 2) == 67);
    const uint8_t *bootp = udp + 8;
    assert(bootp[0] == 1 && bootp[1] == 1 && bootp[2] == 6);
    assert(bootp[236] == 99 && bootp[237] == 130 && bootp[238] == 83 && bootp[239] == 99);
}

static void test_network_helpers(void) {
    uint8_t address[4];
    assert(parse_ipv4("1.2.3.4", address));
    assert(address[0] == 1 && address[1] == 2 && address[2] == 3 && address[3] == 4);
    assert(!parse_ipv4("1.2.300.4", address));
    assert(!parse_ipv4("1.2.3", address));

    uint8_t header[20] = { 0x45, 0, 0, 20, 0, 1, 0x40, 0, 64, 1 };
    write_be16(header + 10, checksum(header, sizeof(header)));
    assert(checksum(header, sizeof(header)) == 0);

    uint8_t name[4] = { 0xc0, 0x0c, 0, 0 };
    size_t offset = 0;
    assert(dns_skip_name(name, sizeof(name), &offset) && offset == 2);

    assert(resolve_dns("example.com", address));
    assert(address[0] == 93 && address[1] == 184 && address[2] == 216 && address[3] == 34);
    assert(duck_network_ping("1.2.3.4") == 1);
}

static void test_dhcp_nak(void) {
    uint8_t offered[4] = { 0 };
    uint8_t mask[4] = { 0 };
    uint8_t router[4] = { 0 };
    uint8_t dns[4] = { 0 };
    uint8_t server[4] = { 0 };
    queue_dhcp_message(0x12345678, 6);
    assert(receive_dhcp(0x12345678, 5, offered, mask, router, dns, server) == -1);
}

static void test_external_dns_uses_gateway(void) {
    uint8_t address[4];
    const uint8_t expected_gateway[4] = { 10, 0, 2, 2 };
    dns_address[0] = 8;
    dns_address[1] = 8;
    dns_address[2] = 8;
    dns_address[3] = 8;
    memset(last_arp_target, 0, sizeof(last_arp_target));
    assert(resolve_dns("example.com", address));
    assert(memcmp(last_arp_target, expected_gateway, sizeof(expected_gateway)) == 0);
    assert(transmitted_frame[30] == 8 && transmitted_frame[31] == 8 &&
           transmitted_frame[32] == 8 && transmitted_frame[33] == 8);
    dns_address[0] = 10;
    dns_address[1] = 0;
    dns_address[2] = 2;
    dns_address[3] = 3;
}

int main(void) {
    test_dhcp_discover();
    test_dhcp_nak();
    test_network_helpers();
    test_external_dns_uses_gateway();
    return 0;
}