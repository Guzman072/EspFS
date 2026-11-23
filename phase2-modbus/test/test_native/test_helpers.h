#pragma once
#include <cstdint>
#include <vector>
#include "crc16.h"

// 在資料後面附上 CRC16（低位元組在前），供測試組裝回應或偽造請求用。
inline std::vector<uint8_t> withCrc(std::vector<uint8_t> body) {
    uint16_t c = crc16(body.data(), body.size());
    body.push_back(static_cast<uint8_t>(c & 0xFF));
    body.push_back(static_cast<uint8_t>(c >> 8));
    return body;
}

// 正常 0x03 回應：Slave ID、0x03、位元組數、各暫存器（高位元組在前）、CRC。
inline std::vector<uint8_t> okResponse(uint8_t slave, const std::vector<uint16_t>& regs) {
    std::vector<uint8_t> b{slave, 0x03, static_cast<uint8_t>(regs.size() * 2)};
    for (uint16_t r : regs) {
        b.push_back(static_cast<uint8_t>(r >> 8));
        b.push_back(static_cast<uint8_t>(r & 0xFF));
    }
    return withCrc(b);
}
