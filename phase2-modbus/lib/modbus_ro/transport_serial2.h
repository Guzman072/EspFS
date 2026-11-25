// 真實 transport：包 ESP32 的 Serial2（UART2）。只在 Arduino 環境編譯；native 測試不包含。
#pragma once
#ifdef ARDUINO
#include "transport.h"

class Serial2Transport : public ITransport {
public:
    void begin();  // 115200 8N1，RX=RS485_RX_PIN、TX=RS485_TX_PIN（預設 GPIO16、GPIO17）

    // 第三層防線：功能碼不是 0x03 就不寫，回 0 並計數。
    // 寫入前清空接收緩衝並確認線路至少靜默 2 ms；寫入後 flush（等 TX 完成）。
    size_t write(const uint8_t* data, size_t len) override;
#ifdef DRIVE_LIVE
    // Phase 3 寫入通道（FC 0x06／0x10），只在 DRIVE_LIVE 編譯。第三層防線：drive_frame_check 的形狀檢查。
    size_t writeDrive(const uint8_t* data, size_t len) override;
#endif
    void flushTx() override;
    void clearRx() override;
    size_t available() override;
    int readByte() override;
    uint32_t nowMs() override;
    void delayMs(uint32_t ms) override;

    uint32_t rejectedWrites() const { return rejected_; }  // 被第三層防線擋下的次數
    uint32_t busyWrites() const { return busy_; }          // 線路一直有資料、無法靜默而放棄的次數

private:
    uint32_t rejected_ = 0;
    uint32_t busy_ = 0;
    bool waitQuiet();
};
#endif
