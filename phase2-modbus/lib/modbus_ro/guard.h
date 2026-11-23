// 送出層防線：所有要送出的位元組在 transport 寫入前必經此處。
#pragma once
#include "frame.h"
#include "transport.h"

// 檢查順序：長度不足 2 → REJECTED_LEN；功能碼不是 0x03 → REJECTED_FC；
// 長度不是 8 → REJECTED_LEN；Slave ID／數量／位址（validateReadParams）；CRC → REJECTED_CRC。
Status checkRequest(const uint8_t* bytes, size_t len);

// 第三層防線用：只看功能碼是不是 0x03（長度至少 2）。真實 transport 在 Serial2 寫入前呼叫，
// 刻意與 checkRequest 分開實作，作為雙重保險。
bool isReadFunctionFrame(const uint8_t* bytes, size_t len);

// 送出層檢查的抽象：交易引擎（transactor）只接受這個介面。Phase 2 用唯讀 guard，Phase 3 用自己的寫入 guard。
class IFrameGuard {
public:
    virtual ~IFrameGuard() = default;
    virtual Status check(const uint8_t* bytes, size_t len) const = 0;
};

// 唯讀 guard：就是 checkRequest（只允許 FC 0x03 與 Phase 2 的位址白名單）。
class ReadOnlyGuard final : public IFrameGuard {
public:
    Status check(const uint8_t* bytes, size_t len) const override { return checkRequest(bytes, len); }
};
const IFrameGuard& readOnlyGuard();

// 通過 guard 才寫入 transport；FC 0x03 走 write()，其他功能碼走 writeDrive()（預設拒絕）。
Status guardedWrite(ITransport& transport, const IFrameGuard& guard, const uint8_t* bytes, size_t len);

// 通過 checkRequest 才寫入 transport；否則回 REJECTED_*，且 transport 一個位元組都不會收到。
// transport 若沒有寫完整個封包（第三層防線拒絕、或線路一直忙碌）一律回 TX_FAIL，由呼叫端立即 LATCHED。
Status guardedWrite(ITransport& transport, const uint8_t* bytes, size_t len);
