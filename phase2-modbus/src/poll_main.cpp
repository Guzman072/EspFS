// A6：輪詢主程式。只讀（FC 0x03）；不寫任何暫存器、不啟動馬達。
// 所有 UART2 存取都在 Serial2Transport 內；這個檔案只用 USB Serial 輸出。
#include <Arduino.h>
#include "master.h"
#include "poller.h"
#include "transport_serial2.h"

namespace {
class UsbSink : public ILineSink {
public:
    void line(const char* text) override { Serial.println(text); }
};

Serial2Transport transport;
Master master(transport);
UsbSink sink;
Poller poller(transport, master, sink);
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(500);
    transport.begin();
}

void loop() {
    poller.step();
    if (poller.state() == PollState::LATCHED) delay(50);  // LATCHED：不再送出任何請求，等使用者重啟
}
