// 寫入請求的組裝：只有四個函式，位址與功能碼都寫死，沒有接受「功能碼」或「位址」參數的通用函式。
// 靜態檢查：寫入組裝函式只允許出現在這個檔案（test_static.cpp）。
#pragma once
#include <cstddef>
#include <cstdint>
#include "types.h"

constexpr size_t kMaxWriteFrame = 13;

struct WriteFrame {
    uint8_t bytes[kMaxWriteFrame];
    size_t len;
};

// 參數不合法時回 REJECTED_ID／REJECTED_VALUE，且不改動 out。
Status buildSetSpeedMode(uint8_t slaveId, WriteFrame& out);                          // FC 0x06、200Dh = 3
Status buildControlWord(uint8_t slaveId, uint16_t word, WriteFrame& out);            // FC 0x06、200Eh
Status buildSetOfflineTime(uint8_t slaveId, uint16_t ms, WriteFrame& out);           // FC 0x06、2000h（200–1000）
Status buildSetSpeeds(uint8_t slaveId, int16_t left, int16_t right, int16_t limitRaw, WriteFrame& out);  // FC 0x10、2088h ×2
