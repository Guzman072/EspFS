#include <unity.h>
#include "test_support.h"

// 以下皆為模擬注入。JK2501 的晶片型號（C3）與 DE／RE 控制方式（C4）仍待實機確認。
static_assert(kMaxAttempts == 3, "native 使用預設 Modbus 重試設定");

static void test_fc03_echo_only_latches_link_after_three_failed_transactions() {
    Rig g;
    g.t.echo = true;
    g.d.dropAll = true;

    uint16_t version = 0;
    for (unsigned failure = 1; failure <= 3; ++failure) {
        TEST_ASSERT_EQUAL(Status::TIMEOUT, g.link.read(0x20A0, 1, &version));
        TEST_ASSERT_EQUAL_UINT(failure * kMaxAttempts, g.t.writes.size());
        TEST_ASSERT_EQUAL(failure == 3, g.link.latched());
    }
    TEST_ASSERT_EQUAL(Status::TIMEOUT, g.link.latchReason());
    TEST_ASSERT_EQUAL(Status::TIMEOUT, g.link.read(0x20A0, 1, &version));
    TEST_ASSERT_EQUAL_UINT(3 * kMaxAttempts, g.t.writes.size());
}

#ifndef DRIVE_DRYRUN
static void prepareArmedRig(Rig& g) {
    g.bootOnly();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
    g.r.ch[kThrottle] = 2000;
    g.run(200);
    TEST_ASSERT_TRUE(g.d.speedL > 0);
    TEST_ASSERT_TRUE(g.d.speedR > 0);
}

static void expectEchoFaultStop(Rig& g, size_t failedRequest, uint8_t fc, Status status) {
    const size_t logMark = g.log.lines.size();
    g.run(1000);

    TEST_ASSERT_EQUAL(DriveState::LATCHED, g.fsm.state());
    TEST_ASSERT_EQUAL(FaultReason::COMM_ERROR, g.fsm.faultReason());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(status), g.fsm.faultDetail());
    const int fault = g.log.indexOf(",FAULT_STOP", logMark);
    TEST_ASSERT_TRUE(fault >= 0);
    TEST_ASSERT_TRUE(g.log.indexOf(",LATCHED", fault + 1) > fault);
    TEST_ASSERT_EQUAL_INT(1, g.log.count(",FAULT_STOP"));

    // ARMED 在第一筆交易用完重試後就停機。兩筆急停寫入各送一次，回應可能仍受模擬故障影響。
    const size_t emergency = failedRequest + kWriteMaxAttempts;
    const auto frames = describeAll(g.t.writes);
    TEST_ASSERT_EQUAL_UINT(emergency + 2, frames.size());
    for (size_t i = failedRequest; i < emergency; ++i) {
        TEST_ASSERT_EQUAL_HEX8(fc, g.t.writes[i][1]);
        TEST_ASSERT_EQUAL_STRING(frames[failedRequest].c_str(), frames[i].c_str());
    }
    TEST_ASSERT_EQUAL_STRING("W200E=0005", frames[emergency].c_str());
    TEST_ASSERT_EQUAL_STRING("S2088=0,0", frames[emergency + 1].c_str());
    g.run(1500);
    TEST_ASSERT_EQUAL_UINT(frames.size(), g.t.writes.size());
}

static void test_fc03_echo_only_stops_armed_fsm() {
    Rig g;
    prepareArmedRig(g);
    const size_t mark = g.t.writes.size();
    g.t.echo = true;
    g.d.dropReadAddrs = {0x20A5};

    // ARMED 每個週期先寫速度，再讀故障碼。只讓後者收到純回音。
    expectEchoFaultStop(g, mark + 1, 0x03, Status::TIMEOUT);
    TEST_ASSERT_EQUAL_STRING("R20A5x2", describe(g.t.writes[mark + 1]).c_str());
}

static void test_fc10_echo_only_stops_armed_fsm() {
    Rig g;
    prepareArmedRig(g);
    g.t.echo = true;
    g.d.dropWrites = true;

    expectEchoFaultStop(g, g.t.writes.size(), 0x10, Status::TIMEOUT);
}

static void probe_fc06_echo_with_unexecuted_stop() {
    Rig g;
    g.d.statusWord = 0x4040;  // 模擬驅動器仍處於使能狀態
    g.d.dropWrites = true;
    g.t.echo = true;

    const Status status = g.link.writeControl(kCtrlStop);
    TEST_ASSERT_TRUE(g.sawFrame("W200E=0007"));
    TEST_ASSERT_EQUAL_HEX16(0x4040, g.d.statusWord);
    if (status == Status::OK) {
        TEST_MESSAGE("FINDING M4 (模擬): FC 0x06 純回音回報 OK，假驅動器未執行停止。");
    }
}

static void test_garbage_echo_stops_armed_fsm() {
    Rig g;
    prepareArmedRig(g);
    g.t.garbageEcho = true;

    expectEchoFaultStop(g, g.t.writes.size(), 0x10, Status::CRC_ERR);
}

static void test_first_reply_byte_drop_stops_armed_fsm() {
    Rig g;
    prepareArmedRig(g);
    g.t.dropFirstReplyByte = true;

    expectEchoFaultStop(g, g.t.writes.size(), 0x10, Status::CRC_ERR);
}

static void test_idle_bias_error_stops_armed_fsm() {
    Rig g;
    prepareArmedRig(g);
    g.t.idleBiasError = true;

    expectEchoFaultStop(g, g.t.writes.size(), 0x10, Status::CRC_ERR);
}
#endif

void run_max3485_echo_tests() {
    RUN_TEST(test_fc03_echo_only_latches_link_after_three_failed_transactions);
#ifndef DRIVE_DRYRUN
    RUN_TEST(test_fc03_echo_only_stops_armed_fsm);
    RUN_TEST(test_fc10_echo_only_stops_armed_fsm);
    RUN_TEST(probe_fc06_echo_with_unexecuted_stop);
    RUN_TEST(test_garbage_echo_stops_armed_fsm);
    RUN_TEST(test_first_reply_byte_drop_stops_armed_fsm);
    RUN_TEST(test_idle_bias_error_stops_armed_fsm);
#endif
}
