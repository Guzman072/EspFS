// 唯一允許呼叫 Serial2 寫入的檔案（靜態檢查：test_static.cpp）。
// 只在 Arduino 環境編譯；native 環境這個檔案是空的。
#ifdef ARDUINO
#include <Arduino.h>
#include "guard.h"
#include "transport_serial2.h"

namespace {
constexpr uint32_t kQuietUs = 2000;         // 送出前至少靜默 2 ms
constexpr uint32_t kQuietGiveUpMs = 100;    // 線路一直忙碌就放棄，不硬送
}  // namespace

void Serial2Transport::begin() {
    Serial2.setRxBufferSize(256);
    Serial2.begin(115200, SERIAL_8N1, 16, 17);
}

// 清空接收緩衝，並確認連續 2 ms 沒有新位元組。
bool Serial2Transport::waitQuiet() {
    const uint32_t startMs = millis();
    uint32_t quietSince = micros();
    while (micros() - quietSince < kQuietUs) {
        while (Serial2.available() > 0) {
            Serial2.read();
            quietSince = micros();
        }
        if (millis() - startMs > kQuietGiveUpMs) return false;
    }
    return true;
}

size_t Serial2Transport::write(const uint8_t* data, size_t len) {
    // 第三層防線：guard 已檢查過，這裡再檢查一次功能碼，不合格就不寫。
    if (!isReadFunctionFrame(data, len)) {
        ++rejected_;
        return 0;
    }
    if (!waitQuiet()) {
        ++busy_;
        return 0;
    }
    const size_t n = Serial2.write(data, len);
    Serial2.flush();  // 等 TX 完成；回應時限由此刻起算（master 在 flushTx 之後才取時間）
    return n;
}

#ifdef DRIVE_LIVE
#include "drive_frame_check.h"
// Phase 3 寫入通道。第三層防線：獨立、簡化的形狀檢查（功能碼、位址、數量、長度）；不合格就不寫、回 0。
// 數值範圍由 write_guard 檢查。只在 DRIVE_LIVE 編譯，所以 Phase 2 與 dryrun 沒有這個函式。
size_t Serial2Transport::writeDrive(const uint8_t* data, size_t len) {
    if (!isAllowedDriveFrameShape(data, len)) {
        ++rejected_;
        return 0;
    }
    if (!waitQuiet()) {
        ++busy_;
        return 0;
    }
    const size_t n = Serial2.write(data, len);
    Serial2.flush();
    return n;
}
#endif

void Serial2Transport::flushTx() { Serial2.flush(); }

void Serial2Transport::clearRx() {
    while (Serial2.available() > 0) Serial2.read();
}

size_t Serial2Transport::available() { return static_cast<size_t>(Serial2.available()); }
int Serial2Transport::readByte() { return Serial2.read(); }
uint32_t Serial2Transport::nowMs() { return millis(); }
void Serial2Transport::delayMs(uint32_t ms) { delay(ms); }
#endif  // ARDUINO
