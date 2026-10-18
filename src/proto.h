// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <stdint.h>
#include <stddef.h>

// v Wire format:
// slip framed (0xC0 delimiters), body = type : payload : crc16 big endian
// crc16 (CCITT-FALSE) covers type and payload Garbage between frames is ignored.

constexpr uint8_t OI_SLIP_END     = 0xC0;
constexpr uint8_t OI_SLIP_ESC     = 0xDB;
constexpr uint8_t OI_SLIP_ESC_END = 0xDC;
constexpr uint8_t OI_SLIP_ESC_ESC = 0xDD;

constexpr uint8_t OI_T_DATA  = 0x01;  // one raw ethernet frame
constexpr uint8_t OI_T_HELLO = 0x02;  // esp > host: [ver][mac x6][link]e
constexpr uint8_t OI_T_PING  = 0x04;  // host > esp: keepalive
constexpr uint8_t OI_T_EXIT  = 0x05;  // host > esp: leave bridge mode
constexpr uint8_t OI_T_STATS = 0x06;  // esp > host: 5x u32 big endian [drops][wifi tx fail][uart tx bytes][uart rx bytes][uart baud]

constexpr uint8_t  OI_PROTO_VERSION = 1;
constexpr size_t   OI_MAX_ETH  = 1600;
constexpr size_t   OI_MAX_BODY = 1 + OI_MAX_ETH + 2;

static inline uint16_t oi_crc16(const uint8_t *d, size_t n, uint16_t crc = 0xFFFF) {
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
