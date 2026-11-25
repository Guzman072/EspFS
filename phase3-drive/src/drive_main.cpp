// Phase 3 進入點（dryrun 與 live 共用）。USB Serial 輸出；UART2 一律經 Serial2Transport。
// 不等待 USB 序列埠連線：綠板的 Serial 是 UART0（CH340），拔掉 Type-C 也照常執行。
// dryrun：寫入只印出（# DRY W ...），沒有寫入傳輸。live：啟用寫入，須使用者確認才可燒錄。
#include <Arduino.h>
#include "config.h"
#include "drive_fsm.h"
#include "ibus_remote.h"
#include "transport_serial2.h"

namespace {
class UsbSink : public ILineSink {
public:
    void line(const char* text) override { Serial.println(text); }
};

Serial2Transport transport;
UsbSink sink;
IbusRemote remote;
DriveLink link(transport, sink);

FsmConfig makeConfig() {
    FsmConfig c;
    c.throttleCh = THROTTLE_CH;
    c.armCh = ARM_CH;
    if (kSteeringEnabled) c.steerCh = kSteeringChannel;
    c.failsafeVerified = kFailsafeVerified;
    c.strictSafety = kStrictSafety;
    return c;
}

DriveFsm fsm(transport, link, remote, sink, makeConfig());
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(500);
    transport.begin();
    remote.begin();
}

void loop() {
    fsm.step();
    if (fsm.state() == DriveState::LATCHED) delay(20);  // LATCHED：不再送出任何請求，等使用者重開機
}
