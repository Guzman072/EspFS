// 真實遙控輸入：包 IBusBM，狀態判斷沿用 Phase 1（GPIO18、UART1、250 ms 沒有新封包視為 STALE）。
#pragma once
#include <Arduino.h>
#include <IBusBM.h>
#include "remote_input.h"

class IbusRemote : public IRemote {
public:
    void begin() { receiver_.begin(Serial1, IBUSBM_NOTIMER, kRxPin, -1); }

    void poll() override {
        receiver_.loop();
        const uint32_t now = millis();
        const uint8_t count = receiver_.cnt_rec;
        const uint8_t newFrames = static_cast<uint8_t>(count - previousCount_);  // 函式庫計數每 256 回捲
        previousCount_ = count;
        if (newFrames != 0) {
            frames_ += newFrames;
            lastFrameMs_ = now;
            status_ = RemoteStatus::RECEIVING;
        } else if (status_ == RemoteStatus::RECEIVING && now - lastFrameMs_ >= kFrameTimeoutMs) {
            status_ = RemoteStatus::STALE;
        }
    }
    RemoteStatus status() override { return status_; }
    uint16_t channel(int index) override {
        if (index < 0 || index >= kRemoteChannels || status_ == RemoteStatus::WAITING) return 0;
        return receiver_.readChannel(static_cast<uint8_t>(index));
    }
    uint32_t frameCount() override { return frames_; }

private:
    static constexpr int8_t kRxPin = 18;
    static constexpr uint32_t kFrameTimeoutMs = 250;
    IBusBM receiver_;
    uint8_t previousCount_ = 0;
    uint32_t frames_ = 0;
    uint32_t lastFrameMs_ = 0;
    RemoteStatus status_ = RemoteStatus::WAITING;
};
