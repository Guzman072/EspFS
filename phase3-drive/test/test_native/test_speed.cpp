#include <unity.h>
#include <cmath>
#include "speed.h"

static void test_resolution_validity() {
    TEST_ASSERT_FALSE(validResolution(0));
    for (int d = 1; d <= 10; ++d) TEST_ASSERT_TRUE(validResolution(d));
    TEST_ASSERT_FALSE(validResolution(11));
    TEST_ASSERT_FALSE(validResolution(-1));
}
static void test_conversion_for_value_1_and_A() {
    // 2022h＝1：單位 1 r/min；2022h＝A（10）：單位 0.1 r/min（V4 RS485 p.14、p.7）
    TEST_ASSERT_EQUAL_INT16(30, rpmToRaw(30.0f, 1));
    TEST_ASSERT_EQUAL_INT16(300, rpmToRaw(30.0f, 10));
    TEST_ASSERT_EQUAL_INT16(60, limitRawFor(1));
    TEST_ASSERT_EQUAL_INT16(600, limitRawFor(10));
    TEST_ASSERT_EQUAL_INT16(-15, rpmToRaw(-15.0f, 1));
    TEST_ASSERT_EQUAL_INT16(-150, rpmToRaw(-15.0f, 10));
    TEST_ASSERT_EQUAL_INT16(0, rpmToRaw(0.0f, 10));
}
static void test_conversion_other_divisors_and_rounding() {
    TEST_ASSERT_EQUAL_INT16(60, rpmToRaw(30.0f, 2));
    TEST_ASSERT_EQUAL_INT16(150, rpmToRaw(30.0f, 5));
    TEST_ASSERT_EQUAL_INT16(2, rpmToRaw(0.25f, 8));
    TEST_ASSERT_EQUAL_INT16(32767, rpmToRaw(100000.0f, 10));   // 飽和，不溢位
    TEST_ASSERT_EQUAL_INT16(-32768, rpmToRaw(-100000.0f, 10));
}
static void test_throttle_mapping() {
    const float L = kSpeedLimitRpm;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, throttleToRpm(1500, kDeadband, L));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, throttleToRpm(1500 + kDeadband, kDeadband, L));   // 死區邊界
    TEST_ASSERT_EQUAL_FLOAT(0.0f, throttleToRpm(1500 - kDeadband, kDeadband, L));
    TEST_ASSERT_TRUE(throttleToRpm(1500 + kDeadband + 1, kDeadband, L) > 0.0f);
    TEST_ASSERT_TRUE(throttleToRpm(1500 - kDeadband - 1, kDeadband, L) < 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, L, throttleToRpm(2000, kDeadband, L));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -L, throttleToRpm(1000, kDeadband, L));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, L, throttleToRpm(2400, kDeadband, L));          // 超出範圍被夾住
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -L, throttleToRpm(0, kDeadband, L));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, L / 2.0f, throttleToRpm(1500 + kDeadband + (500 - kDeadband) / 2, kDeadband, L));
}
static void test_mixed_targets_stay_within_limit_and_reverse_has_two_negative_targets() {
    for (int throttle = -60; throttle <= 60; ++throttle) {
        for (int steering = -10; steering <= 10; ++steering) {
            const auto targets = mixWheelTargets(throttle, steering / 10.0f, kSpeedLimitRpm, false);
            const auto boosted = mixWheelTargets(throttle, steering / 10.0f, kSpeedLimitRpm, true);
            TEST_ASSERT_TRUE(std::fabs(targets.left) <= 60.00001f && std::fabs(targets.right) <= 60.00001f);
            TEST_ASSERT_TRUE(std::fabs(boosted.left) <= 60.00001f && std::fabs(boosted.right) <= 60.00001f);
            if (throttle != 0) {
                TEST_ASSERT_EQUAL_FLOAT(targets.left, boosted.left);
                TEST_ASSERT_EQUAL_FLOAT(targets.right, boosted.right);
            }
            if (throttle < 0) TEST_ASSERT_TRUE(targets.left < 0.0f && targets.right < 0.0f);
        }
    }
}

static void test_slew_rises_linearly_at_3_rpm_per_50ms_to_limit_in_1_s() {
    SlewLimiter s(kSpeedLimitRpm, kSlewRpmPerSec);
    float v = 0;
    v = s.step(kSpeedLimitRpm, 50);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 3.0f, v);                       // 每 50 ms 加 3.0 r/min
    for (int ms = 100; ms <= 500; ms += 50) v = s.step(kSpeedLimitRpm, 50);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 30.0f, v);                      // 500 ms：一半
    for (int ms = 550; ms <= 1000; ms += 50) v = s.step(kSpeedLimitRpm, 50);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, kSpeedLimitRpm, v);             // 1000 ms：到達上限
    v = s.step(kSpeedLimitRpm, 50);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, kSpeedLimitRpm, v);             // 不會超過
}
static void test_slew_decelerates_at_same_rate_and_handles_reverse() {
    SlewLimiter s(kSpeedLimitRpm, kSlewRpmPerSec);
    for (int i = 0; i < 40; ++i) s.step(kSpeedLimitRpm, 50);
    float v = s.step(0.0f, 500);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 30.0f, v);                      // 500 ms 減少一半
    v = s.step(0.0f, 500);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, v);
    v = s.step(-kSpeedLimitRpm, 1000);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, -kSpeedLimitRpm, v);
}
static void test_slew_clamps_target_and_resets() {
    SlewLimiter s(kSpeedLimitRpm, kSlewRpmPerSec);
    float v = s.step(500.0f, 100000);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, kSpeedLimitRpm, v);
    s.reset();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.value());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.step(0.0f, 0));
}
static void test_overspeed_threshold_is_derived_from_limit_and_factor() {
    TEST_ASSERT_EQUAL_INT(720, overspeedLimit01(kSpeedLimitRpm));  // 60 × 1.2 × 10
    TEST_ASSERT_EQUAL_INT(1080, overspeedLimit01(90.0f));
    TEST_ASSERT_EQUAL_INT(360, overspeedLimit01(30.0f));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(std::lround(kSpeedLimitRpm * kOverspeedFactor * 10.0f)), overspeedLimit01(kSpeedLimitRpm));
}
static void test_constants_are_the_agreed_defaults() {
    TEST_ASSERT_EQUAL_FLOAT(60.0f, kSpeedLimitRpm);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, kInPlaceMaxRpm);
    TEST_ASSERT_EQUAL_INT(2, kTurnBoostChannel);
    TEST_ASSERT_EQUAL_UINT(1900, kTurnBoostOnThreshold);
    TEST_ASSERT_EQUAL_INT(-1, kTurnBoostInnerSign);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, kSlewRpmPerSec);                 // 每 50 ms 3.0 r/min
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3.0f, kSlewRpmPerSec * (kControlPeriodMs / 1000.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.2f, kOverspeedFactor);               // 使用者決定（2026-11-23）：1.5 → 1.2
    TEST_ASSERT_EQUAL_INT(40, kDeadband);
    TEST_ASSERT_EQUAL_INT(1700, kArmOnThreshold);
    TEST_ASSERT_EQUAL_UINT32(50, kControlPeriodMs);
    TEST_ASSERT_EQUAL_UINT32(100, kKeepaliveMaxMs);
    TEST_ASSERT_EQUAL_UINT16(500, kOfflineWriteMs);
    TEST_ASSERT_EQUAL_INT(30, kTorqueStopLimit01A);
    TEST_ASSERT_FALSE(kSteeringEnabled);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, kSteeringGain);
    TEST_ASSERT_EQUAL_INT(1, kSteerInputSign);  // 假設右推＝2000，待 dryrun 確認
}

void run_speed_tests() {
    RUN_TEST(test_resolution_validity);
    RUN_TEST(test_conversion_for_value_1_and_A);
    RUN_TEST(test_conversion_other_divisors_and_rounding);
    RUN_TEST(test_throttle_mapping);
    RUN_TEST(test_mixed_targets_stay_within_limit_and_reverse_has_two_negative_targets);
    RUN_TEST(test_slew_rises_linearly_at_3_rpm_per_50ms_to_limit_in_1_s);
    RUN_TEST(test_slew_decelerates_at_same_rate_and_handles_reverse);
    RUN_TEST(test_slew_clamps_target_and_resets);
    RUN_TEST(test_overspeed_threshold_is_derived_from_limit_and_factor);
    RUN_TEST(test_constants_are_the_agreed_defaults);
}
