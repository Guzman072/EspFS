#include "speed.h"
#include <algorithm>
#include <cmath>

bool validResolution(int d) { return d >= 1 && d <= 10; }

int16_t rpmToRaw(float rpm, int d) {
    const float raw = std::round(rpm * static_cast<float>(d));
    if (raw > 32767.0f) return 32767;
    if (raw < -32768.0f) return -32768;
    return static_cast<int16_t>(raw);
}

int16_t limitRawFor(int d) { return rpmToRaw(kSpeedLimitRpm, d); }

int overspeedLimit01(float limitRpm) { return static_cast<int>(std::lround(limitRpm * kOverspeedFactor * 10.0f)); }

float throttleToRpm(int ibusValue, int deadband, float limitRpm) {
    int x = ibusValue - kIbusCenter;
    if (x > kIbusHalfRange) x = kIbusHalfRange;
    if (x < -kIbusHalfRange) x = -kIbusHalfRange;
    const int ax = x < 0 ? -x : x;
    if (ax <= deadband) return 0.0f;
    const float mag = static_cast<float>(ax - deadband) / static_cast<float>(kIbusHalfRange - deadband) * limitRpm;
    return x > 0 ? mag : -mag;
}

WheelTargets mixWheelTargets(float throttleRpm, float steeringInput, float limitRpm, bool turnBoost) {
    if (throttleRpm == 0.0f) {
        if (steeringInput == 0.0f) return {0.0f, 0.0f};
        const float outer = std::min(kInPlaceMaxRpm, limitRpm) * std::min(1.0f, std::fabs(steeringInput));
        const float inner = turnBoost ? outer * kTurnBoostInnerSign : 0.0f;
        return steeringInput < 0.0f ? WheelTargets{inner, outer} : WheelTargets{outer, inner};
    }

    float turn;
    if (throttleRpm > 0.0f) {
        const float requested = steeringInput * kSteeringGain * limitRpm;
        turn = std::max(-throttleRpm, std::min(requested, throttleRpm));
    } else {
        // 倒車時轉向反號，差速隨油門大小變化，兩輪目標均維持後退。
        turn = steeringInput * kSteeringGain * throttleRpm;
    }

    WheelTargets targets{throttleRpm + turn, throttleRpm - turn};
    const float peak = std::max(std::fabs(targets.left), std::fabs(targets.right));
    if (peak > limitRpm) {
        const float scale = limitRpm / peak;
        targets.left *= scale;
        targets.right *= scale;
    }
    return targets;
}

float SlewLimiter::step(float targetRpm, uint32_t dtMs) {
    if (targetRpm > limit_) targetRpm = limit_;
    if (targetRpm < -limit_) targetRpm = -limit_;
    const float maxDelta = rate_ * (static_cast<float>(dtMs) / 1000.0f);
    const float diff = targetRpm - value_;
    if (std::fabs(diff) <= maxDelta) {
        value_ = targetRpm;
    } else {
        value_ += diff > 0 ? maxDelta : -maxDelta;
    }
    return value_;
}
