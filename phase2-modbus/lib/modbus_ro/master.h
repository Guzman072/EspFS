// 一次讀取交易：組裝（唯讀）、送出、等待、驗證、重試。收發與重試由 Transactor 負責。
// 預設使用唯讀 guard，所以 Phase 2 的讀取路徑只受唯讀 guard 約束。
#pragma once
#include "frame.h"
#include "guard.h"
#include "transactor.h"
#include "transport.h"

struct Result {
    Status status = Status::OK;
    uint8_t attempts = 0;
    bool sent = false;            // false：沒有任何位元組寫入 transport
    uint8_t count = 0;
    uint16_t regs[kMaxCount] = {};
    uint32_t rttMs = kNoRtt;      // 從 TX 完成到收完回應；沒收到資料為 kNoRtt
};

class Master {
public:
    explicit Master(ITransport& transport, const IFrameGuard& guard = readOnlyGuard())
        : tx_(transport, guard) {}

    // 讀取 count 個暫存器（FC 0x03）。同一時間只會有一筆交易，不會重疊。
    // 已 LATCHED 時不送出任何東西（sent=false），status 回報進入 LATCHED 的原因。
    // 進入 LATCHED 的條件：(1) 任何 REJECTED_*（程式端錯誤請求）或 TX_FAIL（transport 沒寫完）立即 LATCHED；
    // (2) 連續 3 個交易最終失敗（任何最終非 OK，含例外回應）。不自動恢復。
    void readRegisters(uint8_t slaveId, uint16_t startAddr, uint16_t count, Result& out);

    bool latched() const { return tx_.latched(); }
    Status latchReason() const { return tx_.latchReason(); }  // 未 LATCHED 時為 OK
    uint8_t consecutiveFailures() const { return tx_.consecutiveFailures(); }

private:
    Transactor tx_;
};
