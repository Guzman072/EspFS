// A5：迴路測試程式。需先拆下通訊模組，GPIO16 與 GPIO17 以杜邦線短接。
// 只組裝 FC 0x03 請求並經 guard 與 transport 送出；不經過 master。
#include <Arduino.h>
#include "loopback.h"
#include "transport_serial2.h"

namespace {
class UsbSink : public ILineSink {
public:
    void line(const char* text) override { Serial.println(text); }
};

Serial2Transport transport;
UsbSink sink;
LoopbackRunner runner(transport, sink);
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(500);
    transport.begin();
}

void loop() {
    runner.step();
    if (runner.done()) delay(100);
}
