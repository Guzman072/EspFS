// 速度換算、死區與斜率。純邏輯。
#pragma once
#include <cstdint>
#include "drive_config.h"

bool validResolution(int d);                         // 2022h 的設定值需在 1–10（0x0A）
int16_t rpmToRaw(float rpm, int d);                  // 寫給驅動器的原始值 = round(rpm × D)
int16_t limitRawFor(int d);                          // round(kSpeedLimitRpm × D)

// i-BUS 通道值（1000–2000，1500 為中位）→ r/min（-limit..+limit）。死區內為 0。
float throttleToRpm(int ibusValue, int deadband, float limitRpm);

// 超速門檻（單位 0.1 r/min，與 20ABh／20ACh 的讀值同單位）＝round(上限 × kOverspeedFactor × 10)。
int overspeedLimit01(float limitRpm);

// 斜率限制：每秒最多變化 rpmPerSecond（r/min）；加速與減速都套用，目標被夾在 ±limit。
class SlewLimiter {
public:
    SlewLimiter(float limitRpm, float rpmPerSecond) : limit_(limitRpm), rate_(rpmPerSecond) {}
    float step(float targetRpm, uint32_t dtMs);
    void reset() { value_ = 0.0f; }
    float value() const { return value_; }

private:
    float limit_;
    float rate_;
    float value_ = 0.0f;
};
