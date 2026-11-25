#include <Arduino.h>
#include <IBusBM.h>
#include <inttypes.h>
#include <stdio.h>

#ifndef IBUS_RX_PIN
#define IBUS_RX_PIN 18  // 預設與 esp32dev 原本相同；S3 綠板也是 GPIO18（排針側）
#endif

namespace {
constexpr int8_t kIbusRx = IBUS_RX_PIN;
constexpr uint32_t kReportIntervalMs = 100;
constexpr uint32_t kFrameTimeoutMs = 250;

IBusBM receiver;
uint8_t previousCount = 0;
uint32_t validFrames = 0;
uint32_t lastFrameMs = 0;
uint32_t lastReportMs = 0;

enum class Reception { Waiting, Receiving, Stale };
Reception reception = Reception::Waiting;
}

void setup() {
  Serial.begin(115200);
  // IBusBM 以 115200 8N1 啟動 UART1，僅指定 RX 腳位。
  receiver.begin(Serial1, IBUSBM_NOTIMER, kIbusRx, -1);
  Serial.print("ms,ibus_status,valid_frames,CH1,CH2,CH3,CH4,CH5,CH6,CH7,CH8,CH9,CH10\n");
  lastReportMs = millis();
}

void loop() {
  receiver.loop();
  const uint32_t now = millis();
  const uint8_t count = receiver.cnt_rec;
  // 函式庫的封包計數每 256 筆回捲。
  const uint8_t newFrames = static_cast<uint8_t>(count - previousCount);
  previousCount = count;
  if (newFrames != 0) {
    validFrames += newFrames;
    lastFrameMs = now;
    reception = Reception::Receiving;
  } else if (reception == Reception::Receiving && now - lastFrameMs >= kFrameTimeoutMs) {
    reception = Reception::Stale;
  }

  if (now - lastReportMs < kReportIntervalMs) {
    return;
  }
  lastReportMs = now;

  char line[112];
  if (reception == Reception::Waiting) {
    snprintf(line, sizeof(line), "%" PRIu32 ",WAITING,0,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA\n", now);
  } else {
    // STALE 保留最後一筆通道值，供比對 i-BUS 中斷前的狀態。
    snprintf(line, sizeof(line),
             "%" PRIu32 ",%s,%" PRIu32 ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
             now, reception == Reception::Receiving ? "RECEIVING" : "STALE", validFrames,
             static_cast<unsigned>(receiver.readChannel(0)),
             static_cast<unsigned>(receiver.readChannel(1)),
             static_cast<unsigned>(receiver.readChannel(2)),
             static_cast<unsigned>(receiver.readChannel(3)),
             static_cast<unsigned>(receiver.readChannel(4)),
             static_cast<unsigned>(receiver.readChannel(5)),
             static_cast<unsigned>(receiver.readChannel(6)),
             static_cast<unsigned>(receiver.readChannel(7)),
             static_cast<unsigned>(receiver.readChannel(8)),
             static_cast<unsigned>(receiver.readChannel(9)));
  }
  Serial.print(line);
}
