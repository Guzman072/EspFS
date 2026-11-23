// 共用型別與常數。純邏輯，不碰硬體。
#pragma once
#include <cstddef>
#include <cstdint>

// 交易狀態。順序與 report 的 statusName() 對應，不要任意調整。
// REJECTED_CRC 是實作時新增的（設計文件的 status 清單沒有）：送出前 guard 發現 CRC 不符。
enum class Status : uint8_t {
    OK = 0,
    TIMEOUT,
    SHORT_FRAME,
    CRC_ERR,
    ADDR_MISMATCH,
    BAD_FC,
    BAD_LEN,
    EXC_01,
    EXC_02,
    EXC_03,
    EXC_UNKNOWN,
    REJECTED_FC,
    REJECTED_ADDR,
    REJECTED_LEN,
    REJECTED_ID,
    REJECTED_CRC,
    TX_FAIL,  // transport 沒有寫完整個封包（第三層防線拒絕、線路忙碌或寫入不完整）；與 REJECTED_* 同樣立即 LATCHED
    // 以下三個由 Phase 3 使用（phase3-drive/DESIGN.md §2）：寫入白名單拒絕與回應核對。
    REJECTED_REG,   // 位址不在白名單，或在黑名單
    REJECTED_VALUE, // 值不在允許的集合內（含超過速度上限）
    WRITE_MISMATCH, // 寫入回應與請求不符（請求已送出）
};

// 白名單：只有 0x03。編譯期常數，沒有任何執行期修改途徑。
constexpr uint8_t kFcReadRegisters = 0x03;
constexpr uint8_t kFcReadException = 0x83;
constexpr size_t kRequestLen = 8;
constexpr uint16_t kMaxCount = 16;       // 設計預設，待確認項目 8
constexpr uint8_t kMinSlaveId = 1;
constexpr uint8_t kMaxSlaveId = 127;
