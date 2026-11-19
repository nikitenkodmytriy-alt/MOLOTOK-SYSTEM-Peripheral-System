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

#define ETHERNET_FRAME_SIZE 1514
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV4 0x0800
#define IP_PROTOCOL_ICMP 1
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY 0
#define IP_PROTOCOL_TCP 6
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static uint8_t local_address[4] = { 10, 0, 2, 15 };
static uint8_t subnet_mask[4] = { 255, 255, 255, 0 };
static uint8_t gateway_address[4] = { 10, 0, 2, 2 };
static uint8_t dns_address[4] = { 10, 0, 2, 3 };
static uint16_t next_sequence = 1;
static int network_ready;

typedef struct {
    uint32_t started_at;
    uint32_t duration;
} network_deadline_t;

static void start_deadline(network_deadline_t *deadline, uint32_t duration) {
    deadline->started_at = duck_timer_seconds();
    deadline->duration = duration;
}

static int deadline_expired(const network_deadline_t *deadline) {
    return duck_timer_seconds() - deadline->started_at >= deadline->duration;
}

static uint16_t read_be16(const uint8_t *data) {
    return (uint16_t)data[0] << 8 | data[1];
}

static void write_be16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static uint16_t checksum(const uint8_t *data, size_t length) {
    uint32_t sum = 0;
    while (length >= 2) {
        sum += read_be16(data);
        data += 2;
        length -= 2;
    }
    if (length) sum += (uint16_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void write_ipv4_header(uint8_t *ip, const uint8_t source[4], const uint8_t destination[4],
                              uint8_t protocol, uint16_t payload_length, uint16_t identifier) {
    ip[0] = 0x45;
    ip[1] = 0;
    write_be16(ip + 2, (uint16_t)(20 + payload_length));
    write_be16(ip + 4, identifier);
    write_be16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = protocol;
    write_be16(ip + 10, 0);
    for (size_t i = 0; i < 4; i++) {
        ip[12 + i] = source[i];
        ip[16 + i] = destination[i];
    }
    write_be16(ip + 10, checksum(ip, 20));
}

static size_t make_dhcp_message(uint8_t *message, uint32_t transaction, uint8_t type,
                                const uint8_t *requested_address, const uint8_t *server) {
    for (size_t i = 0; i < 300; i++) message[i] = 0;
    message[0] = 1;
    message[1] = 1;
    message[2] = 6;
    message[4] = (uint8_t)(transaction >> 24);
    message[5] = (uint8_t)(transaction >> 16);
    message[6] = (uint8_t)(transaction >> 8);
    message[7] = (uint8_t)transaction;
    message[10] = 0x80;
    const uint8_t *mac = duck_pcnet_mac();
    for (size_t i = 0; i < 6; i++) message[28 + i] = mac[i];
    message[236] = 99;
    message[237] = 130;
    message[238] = 83;
    message[239] = 99;
    size_t option = 240;
    message[option++] = 53;
    message[option++] = 1;
    message[option++] = type;
    if (requested_address && server) {
        message[option++] = 50;
        message[option++] = 4;
        for (size_t i = 0; i < 4; i++) message[option++] = requested_address[i];
        message[option++] = 54;
        message[option++] = 4;
        for (size_t i = 0; i < 4; i++) message[option++] = server[i];
    }
    message[option++] = 55;
    message[option++] = 5;
    message[option++] = 1;
    message[option++] = 3;
    message[option++] = 6;
    message[option++] = 15;
    message[option++] = 51;
    message[option++] = 255;
    return option;
}

static int send_dhcp(uint32_t transaction, uint8_t type,
                     const uint8_t *requested_address, const uint8_t *server) {
    uint8_t frame[14 + 20 + 8 + 300];
    for (size_t i = 0; i < 6; i++) frame[i] = 0xff;
    const uint8_t *mac = duck_pcnet_mac();
    for (size_t i = 0; i < 6; i++) frame[6 + i] = mac[i];
    write_be16(frame + 12, ETHERTYPE_IPV4);
    uint8_t zero[4] = { 0, 0, 0, 0 };
    uint8_t broadcast[4] = { 255, 255, 255, 255 };
    uint8_t *ip = frame + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bootp = udp + 8;
    size_t bootp_length = make_dhcp_message(bootp, transaction, type, requested_address, server);
    uint16_t udp_length = (uint16_t)(8 + bootp_length);
    write_ipv4_header(ip, zero, broadcast, 17, udp_length, 1);
    write_be16(udp, 68);
    write_be16(udp + 2, 67);
    write_be16(udp + 4, udp_length);
    write_be16(udp + 6, 0);
    return duck_pcnet_send(frame, 14 + 20 + udp_length);
}

static int receive_dhcp(uint32_t transaction, uint8_t wanted_type, uint8_t offered[4],
                        uint8_t mask[4], uint8_t router[4], uint8_t dns[4], uint8_t server[4]) {
    uint8_t frame[1514];
    size_t length;
    network_deadline_t deadline;
    start_deadline(&deadline, 4);
    while (!deadline_expired(&deadline)) {
        if (!duck_pcnet_receive(frame, sizeof(frame), &length)) continue;
        if (length < 14 + 20 + 8 + 240 || read_be16(frame + 12) != ETHERTYPE_IPV4) continue;
        const uint8_t *ip = frame + 14;
        size_t ip_length = (ip[0] & 0x0f) * 4u;
        if (ip_length < 20 || length < 14 + ip_length + 8 + 240 || ip[9] != 17) continue;
        const uint8_t *udp = ip + ip_length;
        uint16_t udp_length = read_be16(udp + 4);
        if (read_be16(udp) != 67 || read_be16(udp + 2) != 68 || udp_length < 248 ||
            udp_length > length - 14 - ip_length) continue;
        const uint8_t *bootp = udp + 8;
        uint32_t received_transaction = (uint32_t)bootp[4] << 24 | (uint32_t)bootp[5] << 16 |
                                        (uint32_t)bootp[6] << 8 | bootp[7];
        if (bootp[0] != 2 || received_transaction != transaction ||
            bootp[236] != 99 || bootp[237] != 130 || bootp[238] != 83 || bootp[239] != 99) continue;
        uint8_t message_type = 0;
        for (size_t i = 0; i < 4; i++) offered[i] = bootp[16 + i];
        size_t options_length = udp_length - 8 - 240;
        const uint8_t *option = bootp + 240;
        for (size_t offset = 0; offset < options_length;) {
            uint8_t code = option[offset++];
            if (code == 0) continue;
            if (code == 255 || offset >= options_length) break;
            uint8_t option_length = option[offset++];
            if (offset + option_length > options_length) break;
            if (code == 53 && option_length == 1) message_type = option[offset];
            else if (code == 1 && option_length == 4) for (size_t i = 0; i < 4; i++) mask[i] = option[offset + i];
            else if (code == 3 && option_length >= 4) for (size_t i = 0; i < 4; i++) router[i] = option[offset + i];
            else if (code == 6 && option_length >= 4) for (size_t i = 0; i < 4; i++) dns[i] = option[offset + i];
            else if (code == 54 && option_length == 4) for (size_t i = 0; i < 4; i++) server[i] = option[offset + i];
            offset += option_length;
        }
        if (message_type == 6) return -1;
        if (message_type == wanted_type) return 1;
    }
    return 0;
}

static void acquire_dhcp_config(void) {
    const uint32_t transaction = 0x4455434bu;
    uint8_t offered[4] = { 0 };
    uint8_t mask[4] = { 255, 255, 255, 0 };
    uint8_t router[4] = { 10, 0, 2, 2 };
    uint8_t dns[4] = { 10, 0, 2, 3 };
    uint8_t server[4] = { 10, 0, 2, 2 };
    if (!send_dhcp(transaction, 1, 0, 0) ||
        receive_dhcp(transaction, 2, offered, mask, router, dns, server) != 1) return;
    if (!send_dhcp(transaction, 3, offered, server) ||
        receive_dhcp(transaction, 5, offered, mask, router, dns, server) != 1) return;
    for (size_t i = 0; i < 4; i++) {
        local_address[i] = offered[i];
        subnet_mask[i] = mask[i];
        gateway_address[i] = router[i];
        dns_address[i] = dns[i];
    }
}

static int parse_ipv4(const char *text, uint8_t address[4]) {
    for (size_t octet = 0; octet < 4; octet++) {
        if (*text < '0' || *text > '9') return 0;
        unsigned value = 0;
        size_t digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (unsigned)(*text++ - '0');
            if (value > 255 || ++digits > 3) return 0;
        }
        address[octet] = (uint8_t)value;
        if (octet < 3) {
            if (*text++ != '.') return 0;
        } else if (*text) return 0;
    }
    return 1;
}

static int same_subnet(const uint8_t left[4], const uint8_t right[4]) {
    for (size_t i = 0; i < 4; i++) {
        if ((left[i] & subnet_mask[i]) != (right[i] & subnet_mask[i])) return 0;
    }
    return 1;
}

static int receive_arp_reply(const uint8_t wanted_ip[4], uint8_t mac[6]) {
    uint8_t frame[ETHERNET_FRAME_SIZE];
    size_t length;
    network_deadline_t deadline;
    start_deadline(&deadline, 3);
    while (!deadline_expired(&deadline)) {
        if (!duck_pcnet_receive(frame, sizeof(frame), &length)) continue;
        if (length < 42 || read_be16(frame + 12) != ETHERTYPE_ARP) continue;
        const uint8_t *arp = frame + 14;
        if (read_be16(arp + 6) != 2 || read_be16(arp + 2) != ETHERTYPE_IPV4 || arp[4] != 6 || arp[5] != 4) continue;
        if (arp[14] != wanted_ip[0] || arp[15] != wanted_ip[1] ||
            arp[16] != wanted_ip[2] || arp[17] != wanted_ip[3]) continue;
        for (size_t i = 0; i < 6; i++) mac[i] = arp[8 + i];
        return 1;
    }
    return 0;
}

static int resolve_mac(const uint8_t address[4], uint8_t mac[6]) {
    uint8_t frame[42] = { 0 };
    const uint8_t *local_mac = duck_pcnet_mac();
    for (size_t i = 0; i < 6; i++) frame[i] = 0xff;
    for (size_t i = 0; i < 6; i++) frame[6 + i] = local_mac[i];
    write_be16(frame + 12, ETHERTYPE_ARP);
    write_be16(frame + 14, 1);
    write_be16(frame + 16, ETHERTYPE_IPV4);
    frame[18] = 6;
    frame[19] = 4;
    write_be16(frame + 20, 1);
    for (size_t i = 0; i < 6; i++) frame[22 + i] = local_mac[i];
    for (size_t i = 0; i < 4; i++) frame[28 + i] = local_address[i];
    for (size_t i = 0; i < 4; i++) frame[38 + i] = address[i];
    if (!duck_pcnet_send(frame, sizeof(frame))) return 0;
    return receive_arp_reply(address, mac);
}

static int dns_skip_name(const uint8_t *message, size_t length, size_t *offset) {
    while (*offset < length) {
        uint8_t label_length = message[(*offset)++];
        if (!label_length) return 1;
        if ((label_length & 0xc0) == 0xc0) {
            if (*offset >= length) return 0;
            (*offset)++;
            return 1;
        }
        if (label_length & 0xc0 || label_length > length - *offset) return 0;
        *offset += label_length;
    }
    return 0;
}

static int resolve_dns(const char *hostname, uint8_t address[4]) {
    const uint8_t *next_hop = same_subnet(dns_address, local_address) ? dns_address : gateway_address;
    uint8_t dns_mac[6];
    if (!resolve_mac(next_hop, dns_mac)) return 0;

    uint8_t query[256] = { 0 };
    uint16_t transaction = 0xd0c1;
    write_be16(query, transaction);
    write_be16(query + 2, 0x0100);
    write_be16(query + 4, 1);
    size_t query_length = 12;
    size_t hostname_length = 0;
    while (hostname[hostname_length]) hostname_length++;
    if (hostname_length && hostname[hostname_length - 1] == '.') hostname_length--;
    size_t position = 0;
    while (position < hostname_length) {
        size_t label_start = position;
        while (position < hostname_length && hostname[position] != '.') position++;
        size_t label_length = position - label_start;
        if (!label_length || label_length > 63 || query_length + label_length + 6 >= sizeof(query)) return 0;
        query[query_length++] = (uint8_t)label_length;
        for (size_t i = 0; i < label_length; i++) {
            char character = hostname[label_start + i];
            query[query_length++] = (uint8_t)(character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character);
        }
        if (position < hostname_length) position++;
    }
    query[query_length++] = 0;
    write_be16(query + query_length, 1);
    query_length += 2;
    write_be16(query + query_length, 1);
    query_length += 2;

    uint8_t frame[14 + 20 + 8 + sizeof(query)];
    for (size_t i = 0; i < 6; i++) frame[i] = dns_mac[i];
    const uint8_t *local_mac = duck_pcnet_mac();
    for (size_t i = 0; i < 6; i++) frame[6 + i] = local_mac[i];
    write_be16(frame + 12, ETHERTYPE_IPV4);
    uint8_t *ip = frame + 14;
    uint8_t *udp = ip + 20;
    uint16_t udp_length = (uint16_t)(8 + query_length);
    write_ipv4_header(ip, local_address, dns_address, 17, udp_length, transaction);
    write_be16(udp, 49153);
    write_be16(udp + 2, 53);
    write_be16(udp + 4, udp_length);
    write_be16(udp + 6, 0);
    for (size_t i = 0; i < query_length; i++) udp[8 + i] = query[i];
    if (!duck_pcnet_send(frame, 14 + 20 + udp_length)) return 0;

    uint8_t incoming[ETHERNET_FRAME_SIZE];
    size_t frame_length;
    network_deadline_t deadline;
    start_deadline(&deadline, 5);
    while (!deadline_expired(&deadline)) {
        if (!duck_pcnet_receive(incoming, sizeof(incoming), &frame_length)) continue;
        if (frame_length < 14 + 20 + 8 + 12 || read_be16(incoming + 12) != ETHERTYPE_IPV4) continue;
        const uint8_t *reply_ip = incoming + 14;
        size_t ip_length = (reply_ip[0] & 0x0f) * 4u;
        if (ip_length < 20 || frame_length < 14 + ip_length + 8 + 12 || reply_ip[9] != 17) continue;
        const uint8_t *reply_udp = reply_ip + ip_length;
        uint16_t reply_udp_length = read_be16(reply_udp + 4);
        if (read_be16(reply_udp) != 53 || read_be16(reply_udp + 2) != 49153 ||
            reply_udp_length < 20 || reply_udp_length > frame_length - 14 - ip_length) continue;
        const uint8_t *response = reply_udp + 8;
        size_t response_length = reply_udp_length - 8;
        if (read_be16(response) != transaction || !(read_be16(response + 2) & 0x8000) ||
            (read_be16(response + 2) & 0x000f) != 0) continue;

        uint16_t questions = read_be16(response + 4);
        uint16_t answers = read_be16(response + 6);
        size_t offset = 12;
        int malformed = 0;
        for (uint16_t i = 0; i < questions; i++) {
            if (!dns_skip_name(response, response_length, &offset) || offset + 4 > response_length) {
                malformed = 1;
                break;
            }
            offset += 4;
        }
        if (malformed) continue;
        for (uint16_t i = 0; i < answers; i++) {
            if (!dns_skip_name(response, response_length, &offset) || offset + 10 > response_length) break;
            uint16_t type = read_be16(response + offset);
            uint16_t class_code = read_be16(response + offset + 2);
            uint16_t data_length = read_be16(response + offset + 8);
            offset += 10;
            if (offset + data_length > response_length) break;
            if (type == 1 && class_code == 1 && data_length == 4) {
                for (size_t j = 0; j < 4; j++) address[j] = response[offset + j];
                return 1;
            }
            offset += data_length;
        }
        return 0;
    }
    return 0;
}

int duck_network_init(void) {
    if (network_ready) return 1;
    network_ready = duck_pcnet_init();
    if (network_ready) acquire_dhcp_config();
    return network_ready;
}

int duck_network_ping(const char *text) {
    uint8_t destination[4];
    uint8_t target_mac[6];
    if (!duck_network_init()) return -2;
    if (!duck_network_resolve(text, destination)) return -5;

    const uint8_t *next_hop = same_subnet(destination, local_address) ? destination : gateway_address;
    if (!resolve_mac(next_hop, target_mac)) return -3;

    uint8_t frame[14 + 20 + 8 + 16] = { 0 };
    const uint8_t *local_mac = duck_pcnet_mac();
    for (size_t i = 0; i < 6; i++) frame[i] = target_mac[i];
    for (size_t i = 0; i < 6; i++) frame[6 + i] = local_mac[i];
    write_be16(frame + 12, ETHERTYPE_IPV4);

    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    write_be16(ip + 2, 44);
    write_be16(ip + 4, next_sequence);
    write_be16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = IP_PROTOCOL_ICMP;
    for (size_t i = 0; i < 4; i++) {
        ip[12 + i] = local_address[i];
        ip[16 + i] = destination[i];
    }
    write_be16(ip + 10, checksum(ip, 20));

    uint8_t *icmp = ip + 20;
    icmp[0] = ICMP_ECHO_REQUEST;
    write_be16(icmp + 4, 0x4455);
    write_be16(icmp + 6, next_sequence);
    static const uint8_t payload[16] = "MSPC-PCNET-PING";
    for (size_t i = 0; i < sizeof(payload); i++) icmp[8 + i] = payload[i];
    write_be16(icmp + 2, checksum(icmp, 24));

    uint16_t sequence = next_sequence++;
    if (!duck_pcnet_send(frame, sizeof(frame))) return -4;

    uint8_t incoming[ETHERNET_FRAME_SIZE];
    size_t length;
    network_deadline_t deadline;
    start_deadline(&deadline, 10);
    while (!deadline_expired(&deadline)) {
        if (!duck_pcnet_receive(incoming, sizeof(incoming), &length)) continue;
        if (length < 42 || read_be16(incoming + 12) != ETHERTYPE_IPV4) continue;
        const uint8_t *reply_ip = incoming + 14;
        size_t header_length = (reply_ip[0] & 0x0f) * 4u;
        if (header_length < 20 || length < 14 + header_length + 8 || reply_ip[9] != IP_PROTOCOL_ICMP) continue;
        if (reply_ip[12] != destination[0] || reply_ip[13] != destination[1] ||
            reply_ip[14] != destination[2] || reply_ip[15] != destination[3] ||
            reply_ip[16] != local_address[0] || reply_ip[17] != local_address[1] ||
            reply_ip[18] != local_address[2] || reply_ip[19] != local_address[3]) continue;
        const uint8_t *reply_icmp = reply_ip + header_length;
        if (reply_icmp[0] == ICMP_ECHO_REPLY && read_be16(reply_icmp + 4) == 0x4455 &&
            read_be16(reply_icmp + 6) == sequence) return 1;
    }
    return 0;
}

static uint32_t read_be32(const uint8_t *data) {
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
           (uint32_t)data[2] << 8 | data[3];
}

static void write_be32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static uint16_t tcp_checksum(const uint8_t source[4], const uint8_t destination[4],
                             const uint8_t *tcp, size_t tcp_length) {
    uint32_t sum = 0;
    for (size_t i = 0; i < 4; i += 2) {
        sum += (uint16_t)source[i] << 8 | source[i + 1];
        sum += (uint16_t)destination[i] << 8 | destination[i + 1];
    }
    sum += IP_PROTOCOL_TCP;
    sum += (uint16_t)tcp_length;
    while (tcp_length >= 2) {
        sum += (uint16_t)tcp[0] << 8 | tcp[1];
        tcp += 2;
        tcp_length -= 2;
    }
    if (tcp_length) sum += (uint16_t)tcp[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static int tcp_send(const uint8_t destination[4], const uint8_t destination_mac[6],
                    uint16_t source_port, uint16_t destination_port,
                    uint32_t sequence, uint32_t acknowledgement,
                    uint8_t flags, const uint8_t *payload, size_t payload_length) {
    uint8_t frame[14 + 20 + 20 + 2048];
    if (payload_length > 2048) return 0;
    const uint8_t *local_mac = duck_pcnet_mac();
    for (size_t i = 0; i < 6; i++) {
        frame[i] = destination_mac[i];
        frame[6 + i] = local_mac[i];
    }
    write_be16(frame + 12, ETHERTYPE_IPV4);
    uint8_t *ip = frame + 14;
    uint8_t *tcp = ip + 20;
    uint16_t tcp_length = (uint16_t)(20 + payload_length);
    write_ipv4_header(ip, local_address, destination, IP_PROTOCOL_TCP, tcp_length, next_sequence++);
    write_be16(tcp, source_port);
    write_be16(tcp + 2, destination_port);
    write_be32(tcp + 4, sequence);
    write_be32(tcp + 8, acknowledgement);
    tcp[12] = 0x50;
    tcp[13] = flags;
    write_be16(tcp + 14, 4096);
    write_be16(tcp + 16, 0);
    write_be16(tcp + 18, 0);
    for (size_t i = 0; i < payload_length; i++) tcp[20 + i] = payload[i];
    write_be16(tcp + 16, tcp_checksum(local_address, destination, tcp, tcp_length));
    return duck_pcnet_send(frame, 14 + 20 + tcp_length);
}

static int tcp_receive(uint16_t source_port, uint16_t destination_port,
                       const uint8_t destination[4],
                       uint32_t *remote_sequence, uint32_t *local_acknowledgement,
                       uint8_t *payload, size_t capacity, size_t *payload_length,
                       uint8_t wanted_flags, const network_deadline_t *deadline) {
    uint8_t frame[ETHERNET_FRAME_SIZE];
    size_t frame_length;
    while (!deadline_expired(deadline)) {
        if (!duck_pcnet_receive(frame, sizeof(frame), &frame_length)) continue;
        if (frame_length < 14 + 20 + 20 || read_be16(frame + 12) != ETHERTYPE_IPV4) continue;
        const uint8_t *ip = frame + 14;
        size_t ip_header = (ip[0] & 0x0f) * 4u;
        if (ip_header < 20 || ip[9] != IP_PROTOCOL_TCP || frame_length < 14 + ip_header + 20) continue;
        for (size_t i = 0; i < 4; i++) {
            if (ip[12 + i] != destination[i] || ip[16 + i] != local_address[i]) {
                ip_header = 0;
                break;
            }
        }
        if (!ip_header) continue;
        uint16_t ip_total_length = read_be16(ip + 2);
        size_t ip_packet_length = (size_t)ip_total_length;
        if (ip_packet_length < ip_header + 20 ||
            frame_length < 14 + ip_header + 20 ||
            frame_length < 14 + ip_packet_length) continue;
        const uint8_t *tcp = ip + ip_header;
        if (read_be16(tcp) != destination_port || read_be16(tcp + 2) != source_port) continue;
        uint8_t tcp_flags = tcp[13];
        if (!(tcp_flags & wanted_flags)) continue;
        size_t tcp_header = (tcp[12] >> 4) * 4u;
        if (tcp_header < 20 || tcp_header > ip_total_length - ip_header) continue;
        size_t tcp_length = ip_total_length - ip_header;
        if (tcp_checksum(ip + 12, ip + 16, tcp, tcp_length) != 0) continue;
        uint32_t sequence = read_be32(tcp + 4);
        uint32_t acknowledgement = read_be32(tcp + 8);
        size_t tcp_payload = 0;
        if (frame_length >= 14 + ip_header + tcp_header) {
            tcp_payload = frame_length - 14 - ip_header - tcp_header;
        }
        if (tcp_payload > capacity) tcp_payload = capacity;
        for (size_t i = 0; i < tcp_payload; i++) payload[i] = tcp[tcp_header + i];
        *payload_length = tcp_payload;
        *remote_sequence = sequence;
        *local_acknowledgement = acknowledgement;
        return 1;
    }
    return 0;
}

int duck_network_http_get(const char *host, const char *path, uint8_t *output,
                          size_t capacity, size_t *received) {
    *received = 0;
    uint8_t server_ip[4];
    uint8_t server_mac[6];
    if (!duck_network_resolve(host, server_ip)) return -1;
    const uint8_t *next_hop = same_subnet(server_ip, local_address) ? server_ip : gateway_address;
    if (!resolve_mac(next_hop, server_mac)) return -2;

    const uint16_t source_port = 40000;
    const uint16_t http_port = 80;
    const uint32_t client_sequence = 0x12345678u;
    uint8_t empty[1] = { 0 };
    if (!tcp_send(server_ip, server_mac, source_port, http_port, client_sequence, 0, TCP_SYN, empty, 0)) return -3;

    uint8_t payload[2048];
    size_t payload_length = 0;
    uint32_t server_sequence = 0;
    uint32_t server_acknowledgement = 0;
    network_deadline_t deadline;
    start_deadline(&deadline, 5);
    if (!tcp_receive(source_port, http_port, server_ip, &server_sequence, &server_acknowledgement,
                     payload, sizeof(payload), &payload_length, TCP_SYN | TCP_ACK, &deadline)) return -4;
    if (server_acknowledgement != client_sequence + 1) return -4;

    uint32_t next_client_sequence = client_sequence + 1;
    uint32_t next_server_sequence = server_sequence + 1;
    if (!tcp_send(server_ip, server_mac, source_port, http_port,
                  next_client_sequence, next_server_sequence, TCP_ACK, empty, 0)) return -5;

    uint8_t request[1024];
    size_t request_length = 0;
    static const char get[] = "GET ";
    static const char host_header[] = " HTTP/1.0\r\nHost: ";
    static const char connection[] = "\r\nConnection: close\r\nUser-Agent: MSPC/1.0\r\n\r\n";
    for (size_t i = 0; i < sizeof(get) - 1; i++) request[request_length++] = (uint8_t)get[i];
    for (size_t i = 0; path[i] && request_length + 1 < sizeof(request); i++) request[request_length++] = (uint8_t)path[i];
    for (size_t i = 0; i < sizeof(host_header) - 1; i++) request[request_length++] = (uint8_t)host_header[i];
    for (size_t i = 0; host[i] && request_length + 1 < sizeof(request); i++) request[request_length++] = (uint8_t)host[i];
    for (size_t i = 0; i < sizeof(connection) - 1; i++) request[request_length++] = (uint8_t)connection[i];

    if (!tcp_send(server_ip, server_mac, source_port, http_port,
                  next_client_sequence, next_server_sequence, TCP_PSH | TCP_ACK,
                  request, request_length)) return -6;

    uint8_t response_header[ETHERNET_FRAME_SIZE];
    size_t output_length = 0;
    uint32_t expected_sequence = next_server_sequence;
    uint32_t total_client_sequence = next_client_sequence + (uint32_t)request_length;
    int header_found = 0;
    size_t body_offset = 0;

    start_deadline(&deadline, 15);
    while (!deadline_expired(&deadline)) {
        size_t segment_length = 0;
        uint32_t segment_sequence = 0;
        uint32_t segment_ack = 0;
        if (!tcp_receive(source_port, http_port, server_ip, &segment_sequence, &segment_ack,
                         response_header, sizeof(response_header), &segment_length,
                         TCP_PSH | TCP_ACK | TCP_FIN, &deadline)) continue;
        if (segment_length && segment_sequence == expected_sequence) {
            if (!header_found) {
                size_t marker = 0;
                for (size_t i = 0; i + 3 < segment_length; i++) {
                    if (response_header[i] == '\r' && response_header[i + 1] == '\n' &&
                        response_header[i + 2] == '\r' && response_header[i + 3] == '\n') {
                        marker = i + 4;
                        break;
                    }
                }
                if (marker) {
                    header_found = 1;
                    body_offset = marker;
                    static const char chunked[] = "transfer-encoding: chunked";
                    int chunked_found = 0;
                    for (size_t i = 0; i + marker <= segment_length; i++) {
                        if (i + sizeof(chunked) - 1 > marker) break;
                        size_t j = 0;
                        while (j < sizeof(chunked) - 1) {
                            char character = (char)response_header[i + j];
                            if (character >= 'A' && character <= 'Z') character += ('a' - 'A');
                            if (character != chunked[j]) break;
                            j++;
                        }
                        if (j == sizeof(chunked) - 1) {
                            chunked_found = 1;
                            break;
                        }
                    }
                    if (chunked_found) return -7;
                }
            } else {
                body_offset = 0;
            }
            size_t copy = segment_length - body_offset;
            if (copy > capacity - output_length) copy = capacity - output_length;
            for (size_t i = 0; i < copy; i++) output[output_length++] = response_header[body_offset + i];
            *received = output_length;
            expected_sequence = segment_sequence + (uint32_t)segment_length;
            tcp_send(server_ip, server_mac, source_port, http_port,
                     total_client_sequence, expected_sequence, TCP_ACK, empty, 0);
            if (copy == 0 && segment_length == 0) break;
            if (output_length >= capacity) break;
        }
    }
    return header_found ? (int)output_length : -7;
}

int duck_network_resolve(const char *hostname, uint8_t address[4]) {
    if (!duck_network_init()) return 0;
    return parse_ipv4(hostname, address) || resolve_dns(hostname, address);
}

void duck_network_get_config(uint8_t address[4], uint8_t mask[4], uint8_t gateway[4], uint8_t dns[4]) {
    for (size_t i = 0; i < 4; i++) {
        address[i] = local_address[i];
        mask[i] = subnet_mask[i];
        gateway[i] = gateway_address[i];
        dns[i] = dns_address[i];
    }
}