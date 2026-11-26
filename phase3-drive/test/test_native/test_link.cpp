#include <unity.h>
#include "test_support.h"

static void test_read_logs_t_and_d_lines() {
    Rig g;
    g.d.regs[0x2022] = 10;
    uint16_t v[1];
    TEST_ASSERT_EQUAL(Status::OK, g.link.read(0x2022, 1, v));
    TEST_ASSERT_EQUAL_UINT16(10, v[0]);
    TEST_ASSERT_TRUE(g.log.has(",01,03,2022,1,OK,1,"));
    TEST_ASSERT_TRUE(g.log.has("speed_resolution,10,div"));
}
static void test_quiet_mode_logs_only_failures() {
    Rig g;
    g.link.setLogAll(false);
    uint16_t v[1];
    TEST_ASSERT_EQUAL(Status::OK, g.link.read(0x2022, 1, v));
    TEST_ASSERT_EQUAL_UINT32(0, g.log.lines.size());
    g.d.dropAll = true;
    TEST_ASSERT_EQUAL(Status::TIMEOUT, g.link.read(0x2022, 1, v, 1));
    TEST_ASSERT_EQUAL_INT(1, g.log.count("TIMEOUT"));
}
static void test_read_rejects_addresses_outside_phase3_whitelist() {
    Rig g;
    uint16_t v[2];
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, g.link.read(0x2010, 1, v));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.totalWritten);
    TEST_ASSERT_TRUE(g.link.latched());
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, g.link.read(0x2022, 1, v));  // LATCHED 後什麼都不送
    TEST_ASSERT_EQUAL_UINT32(0, g.t.totalWritten);
}

#ifndef DRIVE_DRYRUN
// ---------- live：寫入與回應核對 ----------
static void test_fc06_write_is_acknowledged_by_exact_echo() {
    Rig g;
    g.link.setLimitRaw(60);
    TEST_ASSERT_EQUAL(Status::OK, g.link.writeControl(kCtrlStop));
    TEST_ASSERT_EQUAL_UINT32(1, g.t.driveWrites.size());
    TEST_ASSERT_TRUE(g.log.has(",01,06,200E,1,OK,1,"));
    TEST_ASSERT_TRUE(g.log.has("200E=0007"));
    TEST_ASSERT_EQUAL_HEX16(0x0000, g.d.statusWord);
    TEST_ASSERT_EQUAL(Status::OK, g.link.writeSpeedMode());
    TEST_ASSERT_EQUAL_HEX16(3, g.d.regs[0x200D]);
}
static void test_fc10_write_checked_against_request() {
    Rig g;
    g.link.setLimitRaw(60);
    TEST_ASSERT_EQUAL(Status::OK, g.link.writeSpeeds(-12, 60));
    TEST_ASSERT_EQUAL_INT16(-12, g.d.speedL);
    TEST_ASSERT_EQUAL_INT16(60, g.d.speedR);
    TEST_ASSERT_TRUE(g.log.has(",01,10,2088,2,OK,1,"));
}
static void test_fc10_reply_mismatch_is_not_retried() {
    Rig g;
    g.link.setLimitRaw(60);
    g.d.corruptMultiReply = true;
    TEST_ASSERT_EQUAL(Status::WRITE_MISMATCH, g.link.writeSpeeds(1, 1));
    TEST_ASSERT_EQUAL_UINT32(1, g.t.driveWrites.size());
    TEST_ASSERT_FALSE(g.link.latched());
}
static void test_fc06_reply_mismatch_detected() {
    Rig g;
    g.t.responder = [](const std::vector<uint8_t>& q) {
        std::vector<uint8_t> b(q.begin(), q.begin() + 6);
        b[5] ^= 0x01;  // 回應的值與請求不同
        return ScriptedReply{withCrc(b), 2};
    };
    TEST_ASSERT_EQUAL(Status::WRITE_MISMATCH, g.link.writeControl(kCtrlStop));
    TEST_ASSERT_EQUAL_UINT32(1, g.t.driveWrites.size());
}
static void test_exception_reply_to_write() {
    Rig g;
    g.link.setLimitRaw(60);
    g.d.exceptionOnWrite = true;
    TEST_ASSERT_EQUAL(Status::EXC_02, g.link.writeControl(kCtrlEnable));
    TEST_ASSERT_EQUAL_UINT32(1, g.t.driveWrites.size());
}
static void test_write_retries_once_then_times_out() {
    Rig g;
    g.d.dropWrites = true;
    TEST_ASSERT_EQUAL(Status::TIMEOUT, g.link.writeControl(kCtrlStop));
    TEST_ASSERT_EQUAL_UINT32(kWriteMaxAttempts, g.t.driveWrites.size());
    TEST_ASSERT_TRUE(g.log.has("TIMEOUT,2,"));
}
static void test_emergency_write_ignores_latch_and_tries_once() {
    Rig g;
    g.link.setLimitRaw(60);
    g.d.dropAll = true;
    uint16_t v[1];
    for (int i = 0; i < 3; ++i) g.link.read(0x20A0, 1, v, 1);
    TEST_ASSERT_TRUE(g.link.latched());
    const size_t before = g.t.totalWritten;
    TEST_ASSERT_EQUAL(Status::TIMEOUT, g.link.writeControl(kCtrlStop));   // 一般寫入：LATCHED 後不送
    TEST_ASSERT_EQUAL_UINT32(before, g.t.totalWritten);
    g.link.writeControl(kCtrlEmergencyStop, true);                         // 緊急：照送，只試一次
    TEST_ASSERT_EQUAL_UINT32(1, g.t.driveWrites.size());
}
static void test_refused_write_is_tx_fail_and_latches() {
    Rig g;
    g.t.refuse = true;
    TEST_ASSERT_EQUAL(Status::TX_FAIL, g.link.writeControl(kCtrlStop));
    TEST_ASSERT_TRUE(g.link.latched());
    TEST_ASSERT_EQUAL(Status::TX_FAIL, g.link.latchReason());
}
static void test_speed_over_limit_is_rejected_before_sending() {
    Rig g;
    g.link.setLimitRaw(10);
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, g.link.writeSpeeds(11, 0));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.totalWritten);
    TEST_ASSERT_TRUE(g.link.latched());
}
static void test_limit_unset_only_allows_zero_speed() {
    Rig g;
    TEST_ASSERT_EQUAL(Status::OK, g.link.writeSpeeds(0, 0));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, g.link.writeSpeeds(1, 0));
}
static void test_fc06_echo_is_indistinguishable_from_acknowledgement() {
    // 已知限制（寫進測試以免被忽略）：FC 0x06 的正常回應本身就是請求的回音，
    // 所以若通訊模組真的有回音，驅動器沒執行的 FC 0x06 也會看起來成功。FC 0x10 的回應不同，回音會被丟掉。
    Rig g;
    g.link.setLimitRaw(60);
    g.t.echo = true;
    g.d.dropWrites = true;
    TEST_ASSERT_EQUAL(Status::OK, g.link.writeControl(kCtrlStop));
    TEST_ASSERT_EQUAL(Status::TIMEOUT, g.link.writeSpeeds(0, 0));
}
#else
// ---------- dryrun：寫入只印出 ----------
static void test_dryrun_prints_instead_of_sending() {
    Rig g;
    g.link.setLimitRaw(60);
    TEST_ASSERT_EQUAL(Status::OK, g.link.writeSpeedMode());
    TEST_ASSERT_EQUAL(Status::OK, g.link.writeControl(kCtrlEnable));
    TEST_ASSERT_EQUAL(Status::OK, g.link.writeSpeeds(60, -60));
    TEST_ASSERT_EQUAL(Status::OK, g.link.writeOfflineTime(500));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.totalWritten);
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 0D 00 03 53 C8"));
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 0E 00 08 E2 0F"));
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 10 20 88 00 02 04 00 3C FF C4"));
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 00 01 F4"));
}
static void test_dryrun_still_applies_the_write_guard() {
    Rig g;
    g.link.setLimitRaw(10);
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, g.link.writeSpeeds(11, 0));
    TEST_ASSERT_TRUE(g.link.latched());
    TEST_ASSERT_EQUAL_INT(0, g.log.count("# DRY W"));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.totalWritten);
}
static void test_dryrun_emergency_write_prints_even_when_latched() {
    Rig g;
    g.link.setLimitRaw(60);
    g.link.writeSpeeds(91, 0);  // REJECTED_VALUE → LATCHED
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, g.link.writeControl(kCtrlStop));
    g.link.writeControl(kCtrlEmergencyStop, true);
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 0E 00 05 23 CA"));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.totalWritten);
}
#endif

void run_link_tests() {
    RUN_TEST(test_read_logs_t_and_d_lines);
    RUN_TEST(test_quiet_mode_logs_only_failures);
    RUN_TEST(test_read_rejects_addresses_outside_phase3_whitelist);
#ifndef DRIVE_DRYRUN
    RUN_TEST(test_fc06_write_is_acknowledged_by_exact_echo);
    RUN_TEST(test_fc10_write_checked_against_request);
    RUN_TEST(test_fc10_reply_mismatch_is_not_retried);
    RUN_TEST(test_fc06_reply_mismatch_detected);
    RUN_TEST(test_exception_reply_to_write);
    RUN_TEST(test_write_retries_once_then_times_out);
    RUN_TEST(test_emergency_write_ignores_latch_and_tries_once);
    RUN_TEST(test_refused_write_is_tx_fail_and_latches);
    RUN_TEST(test_speed_over_limit_is_rejected_before_sending);
    RUN_TEST(test_limit_unset_only_allows_zero_speed);
    RUN_TEST(test_fc06_echo_is_indistinguishable_from_acknowledgement);
#else
    RUN_TEST(test_dryrun_prints_instead_of_sending);
    RUN_TEST(test_dryrun_still_applies_the_write_guard);
    RUN_TEST(test_dryrun_emergency_write_prints_even_when_latched);
#endif
}
