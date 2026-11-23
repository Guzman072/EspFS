// UART 的抽象介面。真實版本（包 Serial2）尚未建立，等使用者確認後才做；
// 測試用 mock 在 test/test_native/mock_transport.h。
#pragma once
#include <cstddef>
#include <cstdint>

class ITransport {
public:
    virtual ~ITransport() = default;
    // 寫入位元組。呼叫端必須先通過 guard（見 guard.h 的 guardedWrite）。
    virtual size_t write(const uint8_t* data, size_t len) = 0;
    virtual void flushTx() = 0;           // 等待 TX 完成
    virtual void clearRx() = 0;           // 清空接收緩衝
    virtual size_t available() = 0;       // 接收緩衝內的位元組數
    virtual int readByte() = 0;           // 無資料回 -1
    // Phase 3 的寫入通道（FC 0x06／0x10）。預設拒絕（回 0）；只有 Phase 3 正式（live）環境編譯的真實 transport 會覆寫。
    // Phase 2 與 dryrun 不會有實作，所以寫入幀永遠送不出去。
    virtual size_t writeDrive(const uint8_t* data, size_t len) {
        (void)data;
        (void)len;
        return 0;
    }
    virtual uint32_t nowMs() = 0;
    virtual void delayMs(uint32_t ms) = 0;
};
