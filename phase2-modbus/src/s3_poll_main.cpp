// JK2501 綠板（ESP32-S3）只讀輪詢：FC 0x03 讀取，RS485 RX=GPIO15、TX=GPIO16（由 s3_poll 的編譯旗標指定），沒有 DE/RE 腳。
// 不寫任何暫存器、不啟動馬達。出錯只記錄、不重送（MODBUS_MAX_ATTEMPTS=1）；連續 3 次失敗沿用 LATCHED 停止。
// 每秒輸出一行 # STAT：交易數、失敗數、CRC 錯誤、逾時、回音（完整／部分）。所有 UART2 存取都在 Serial2Transport 內。
#include <Arduino.h>
#include <stdio.h>
#include "echo_probe.h"
#include "master.h"
#include "poller.h"
#include "transport_serial2.h"

#if !defined(RS485_RX_PIN) || !defined(RS485_TX_PIN)
#error "s3_poll 須以編譯旗標指定 RS485_RX_PIN／RS485_TX_PIN"
#endif
#if MODBUS_MAX_ATTEMPTS != 1
#error "s3_poll 須以 -DMODBUS_MAX_ATTEMPTS=1 編譯（出錯不重送）"
#endif

namespace {
constexpr uint32_t kStatIntervalMs = 1000;

class UsbSink : public ILineSink {
public:
    void line(const char* text) override { Serial.println(text); }
};

Serial2Transport serial2;
EchoProbe probe(serial2);
Master master(probe);
UsbSink usb;
StatusCounter counter(usb);
Poller poller(probe, master, counter);
uint32_t lastStatMs = 0;

void printStat() {
    char line[160];
    snprintf(line, sizeof line,
             "# STAT,%lu,state=%s,tx=%lu,fail=%lu,crc=%lu,timeout=%lu,echo=%lu,partial_echo=%lu,rejected=%lu,busy=%lu",
             static_cast<unsigned long>(millis()), pollStateName(poller.state()),
             static_cast<unsigned long>(counter.transactions()), static_cast<unsigned long>(counter.failures()),
             static_cast<unsigned long>(counter.crcErrors()), static_cast<unsigned long>(counter.timeouts()),
             static_cast<unsigned long>(probe.echoes()), static_cast<unsigned long>(probe.partialEchoes()),
             static_cast<unsigned long>(serial2.rejectedWrites()), static_cast<unsigned long>(serial2.busyWrites()));
    Serial.println(line);
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("# s3_poll JK2501 read-only (FC03), RX=GPIO" + String(RS485_RX_PIN) + " TX=GPIO" + String(RS485_TX_PIN));
    serial2.begin();
}

void loop() {
    if (poller.state() != PollState::LATCHED) poller.step();
    const uint32_t now = millis();
    if (now - lastStatMs >= kStatIntervalMs) {
        lastStatMs = now;
        printStat();
    }
    if (poller.state() == PollState::LATCHED) delay(50);  // LATCHED：不再送出任何請求，等使用者重啟
}
