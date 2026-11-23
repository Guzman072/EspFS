#pragma once
#include <cstddef>
#include <cstdint>

// CRC-16/MODBUS：初值 0xFFFF，多項式 0xA001（反射）。
// 回傳值的低位元組先送出（見 frame）。
uint16_t crc16(const uint8_t* data, size_t len);
