// 請求組裝與回應解析。只有 FC 0x03；不存在任何寫入請求的組裝函式。
#pragma once
#include "types.h"

struct Request {
    uint8_t bytes[kRequestLen];
};

struct Response {
    uint8_t slave;
    uint8_t count;
    uint16_t regs[kMaxCount];
};

// 檢查讀取參數：Slave ID 1–127（REJECTED_ID）、數量 1–16（REJECTED_LEN）、位址白名單（REJECTED_ADDR）。
Status validateReadParams(uint8_t slaveId, uint16_t startAddr, uint16_t count);

// 唯一的請求組裝函式，功能碼寫死為 0x03。參數不合法時回對應的 REJECTED_*，且不改動 out。
Status buildReadRegisters(uint8_t slaveId, uint16_t startAddr, uint16_t count, Request& out);

// 解析回應。驗證順序：長度 → CRC → Slave ID → 功能碼 → 例外碼 → 位元組數與長度。
// out 只在回傳 OK 時有效。
Status parseResponse(const uint8_t* buf, size_t len, uint8_t expectSlave, uint16_t expectCount, Response& out);
