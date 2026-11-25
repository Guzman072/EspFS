// 綠板（s3_drive_*）的額外安全項（FsmConfig::strictSafety，DESIGN.md §12）：
// (a) 解鎖前重寫 2000h 並讀回；(b) 開機先送停止指令並讀回 20A2h；(c) 通道值超出 1000–2000 → 解除／不解鎖。
// native（live 語意）與 native_dryrun 都會跑；兩邊行為不同的部分各自分開。
#include <unity.h>
#include <memory>
#include "test_support.h"

using Frames = std::vector<std::string>;

namespace {
void expectFrom(const Rig& g, size_t from, const Frames& want) {
    Frames got = describeAll(g.t.writes, from);
    TEST_ASSERT_TRUE_MESSAGE(got.size() >= want.size(), "送出的幀比預期少");
    for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
}
std::unique_ptr<Rig> strictRig(FsmConfig c = Rig::strictConfig()) { return std::unique_ptr<Rig>(new Rig(c)); }
std::unique_ptr<Rig> strictArmed(FsmConfig c = Rig::strictConfig()) {
    auto g = strictRig(c);
    g->bootOnly();
    g->unlockViaSwitch();
    return g;
}
}  // namespace

// ===================== 兩種語意共用 =====================
static void test_strict_off_keeps_original_boot_without_writes() {
    Rig g;  // 預設 strictSafety=false（esp32dev）
    g.bootOnly();
    TEST_ASSERT_FALSE(g.log.has("BOOT STOP"));
    TEST_ASSERT_FALSE(g.log.has("# DRY W"));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    expectFrom(g, 0, {"R20A0x1", "R2022x1"});
}
static void test_strict_boot_confirms_stop_and_reaches_disarmed() {
    auto g = strictRig();
    g->bootOnly();
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
    TEST_ASSERT_TRUE(g->fsm.enabled());
    TEST_ASSERT_TRUE(g->log.has("# BOOT STOP confirmed 20A2h=0000"));
    // 停止指令在預檢讀取之前
    TEST_ASSERT_TRUE(g->log.indexOf("BOOT STOP") < g->log.indexOf(",2022,"));
}
static void test_strict_channel_out_of_range_while_armed_disarms_and_needs_swa_cycle() {
    for (uint16_t bad : {999, 2001, 0, 65535}) {
        auto g = strictArmed();
        TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
        g->r.ch[kThrottle] = bad;
        g->run(60);
        TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
        TEST_ASSERT_TRUE(g->log.has("# DISARM reason=CH_OUT_OF_RANGE CH3="));
        g->r.ch[kThrottle] = 1500;  // 恢復正常值、SWA 仍在開：不得自行重新解鎖（含 3 s 停止取樣）
        g->run(3500);
        TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
        g->unlockViaSwitch();       // 看過 SWA 關閉之後才可以
        TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
    }
}
static void test_strict_arm_channel_out_of_range_while_armed_disarms() {
    auto g = strictArmed();
    g->r.ch[kArm] = 2050;
    g->run(60);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
    TEST_ASSERT_TRUE(g->log.has("# DISARM reason=CH_OUT_OF_RANGE CH5=2050"));
}
static void test_strict_steer_channel_out_of_range_disarms() {
    FsmConfig c = Rig::strictConfig();
    c.steerCh = 3;
    auto g = strictArmed(c);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
    g->r.ch[3] = 2100;
    g->run(60);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
    TEST_ASSERT_TRUE(g->log.has("# DISARM reason=CH_OUT_OF_RANGE CH4=2100"));
}
static void test_strict_range_boundaries_1000_and_2000_stay_armed() {
    auto g = strictArmed();
    g->r.ch[kThrottle] = 2000;
    g->run(300);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
    g->r.ch[kThrottle] = 1000;
    g->run(300);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
    TEST_ASSERT_FALSE(g->log.has("CH_OUT_OF_RANGE"));
}
static void test_strict_out_of_range_throttle_blocks_unlock_and_prints_once() {
    auto g = strictRig();
    g->bootOnly();
    g->r.ch[kThrottle] = 2200;
    g->unlockViaSwitch();
    g->run(1000);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
    TEST_ASSERT_EQUAL_INT(1, g->log.count("# ARM blocked: CH3=2200 out of range 1000-2000"));
    TEST_ASSERT_FALSE(g->log.has(",ARMING"));
}
// SWA 的值超出範圍（例如 0）不算「看過 SWA 關閉」；未開 strict 時會被當成關閉（對照組）。
static void test_strict_out_of_range_arm_value_is_not_seen_as_switch_off() {
    for (bool strict : {true, false}) {
        FsmConfig c = Rig::defaultConfig();
        c.strictSafety = strict;
        Rig g(c);
        g.r.ch[kArm] = 2000;  // 開機時 SWA 已在開
        g.bootOnly();
        g.run(200);
        g.r.ch[kArm] = 0;     // 異常值
        g.run(200);
        g.r.ch[kArm] = 2000;
        g.run(1500);
        TEST_ASSERT_EQUAL_MESSAGE(strict ? DriveState::DISARMED : DriveState::ARMED, g.fsm.state(),
                                  strict ? "strict：異常值不得當成 SWA 關閉" : "對照組：未開 strict 的既有行為");
    }
}

// ===================== live 語意（native） =====================
#ifndef DRIVE_DRYRUN
static void test_strict_boot_sends_stop_before_preflight() {
    auto g = strictRig();
    g->bootOnly();
    expectFrom(*g, 0, {"R20A0x1", "S2088=0,0", "W200E=0007", "R20A2x1", "R2022x1", "R2008x1", "R2000x1"});
    TEST_ASSERT_EQUAL_UINT32(2, g->t.driveWrites.size());
}
static void test_strict_boot_stop_not_confirmed_disables() {
    auto g = strictRig();
    g->d.statusWord = 0x4040;  // 驅動器仍在使能
    g->d.stuckStatus = true;   // 停止指令沒有生效
    g->bootOnly();
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
    TEST_ASSERT_FALSE(g->fsm.enabled());
    TEST_ASSERT_TRUE(g->log.has("# DISABLED reason=boot stop not confirmed 20A2h=4040"));
    g->unlockViaSwitch();
    g->run(1000);
    TEST_ASSERT_FALSE(g->sawFrame("W200E=0008"));
    TEST_ASSERT_FALSE(g->sawFrame("W200D=0003"));
}
static void test_strict_boot_stop_write_failure_latches_and_tries_emergency_stop() {
    auto g = strictRig();
    g->d.dropWrites = true;
    g->bootOnly();
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g->fsm.state());
    TEST_ASSERT_EQUAL(FaultReason::COMM_ERROR, g->fsm.faultReason());
    TEST_ASSERT_TRUE(g->sawFrame("W200E=0005"));  // 故障路徑：急停
    TEST_ASSERT_FALSE(g->log.has("BOOT STOP confirmed"));
}
static void test_strict_unlock_rewrites_2000h_and_reads_back_before_enable() {
    auto g = strictRig();
    g->bootOnly();
    const size_t mark = g->t.writes.size();
    g->unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
    expectFrom(*g, mark, {"R20A5x2", "R20A2x1", "W2000=01F4", "R2000x1", "W200D=0003", "S2088=0,0", "W200E=0008"});
    TEST_ASSERT_EQUAL_HEX16(500, g->d.regs[0x2000]);
    TEST_ASSERT_FALSE(g->sawFrame("W2010=0001"));  // 不存檔
}
static void test_strict_2000h_readback_mismatch_blocks_unlock() {
    auto g = strictRig();
    g->d.ignoreOfflineWrite = true;  // 驅動器回應正常，但 2000h 仍是 1000
    g->bootOnly();
    g->unlockViaSwitch();
    g->run(1500);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
    TEST_ASSERT_FALSE(g->fsm.enabled());
    TEST_ASSERT_TRUE(g->log.has("# DISABLED reason=2000h readback mismatch before arm: 1000 want 500"));
    TEST_ASSERT_FALSE(g->sawFrame("W200D=0003"));
    TEST_ASSERT_FALSE(g->sawFrame("W200E=0008"));
}
static void test_strict_2000h_write_failure_latches() {
    auto g = strictRig();
    g->bootOnly();
    g->d.dropWrites = true;
    g->unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g->fsm.state());
    TEST_ASSERT_FALSE(g->sawFrame("W200E=0008"));
    TEST_ASSERT_TRUE(g->sawFrame("W200E=0005"));
}
#endif

// ===================== dryrun 語意（native_dryrun） =====================
#ifdef DRIVE_DRYRUN
static void test_strict_dryrun_boot_stop_is_printed_only() {
    auto g = strictRig();
    g->bootOnly();
    TEST_ASSERT_TRUE(g->log.has("# DRY W 01 10 20 88 00 02 04 00 00 00 00"));
    TEST_ASSERT_TRUE(g->log.has("# DRY W 01 06 20 0E 00 07"));
    TEST_ASSERT_EQUAL_UINT32(0, g->t.driveWrites.size());
    for (const auto& f : g->t.writes) TEST_ASSERT_EQUAL_UINT8(0x03, f[1]);
}
static void test_strict_dryrun_unconfirmed_boot_stop_is_only_noted() {
    auto g = strictRig();
    g->d.statusWord = 0xC0C0;  // 未接馬達時的報警
    g->bootOnly();
    TEST_ASSERT_TRUE(g->fsm.enabled());
    TEST_ASSERT_TRUE(g->log.has("# would-block: boot stop not confirmed 20A2h=C0C0"));
}
static void test_strict_dryrun_2000h_mismatch_is_only_noted_and_arms() {
    auto g = strictRig();  // 乾跑不會真的寫入，2000h 仍是 1000
    g->bootOnly();
    g->unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
    TEST_ASSERT_TRUE(g->log.has("# would-block: 2000h readback=1000 want 500"));
    TEST_ASSERT_TRUE(g->log.has("# DRY W 01 06 20 00 01 F4"));
    TEST_ASSERT_EQUAL_UINT32(0, g->t.driveWrites.size());
}
#endif

void run_strict_tests() {
    RUN_TEST(test_strict_off_keeps_original_boot_without_writes);
    RUN_TEST(test_strict_boot_confirms_stop_and_reaches_disarmed);
    RUN_TEST(test_strict_channel_out_of_range_while_armed_disarms_and_needs_swa_cycle);
    RUN_TEST(test_strict_arm_channel_out_of_range_while_armed_disarms);
    RUN_TEST(test_strict_steer_channel_out_of_range_disarms);
    RUN_TEST(test_strict_range_boundaries_1000_and_2000_stay_armed);
    RUN_TEST(test_strict_out_of_range_throttle_blocks_unlock_and_prints_once);
    RUN_TEST(test_strict_out_of_range_arm_value_is_not_seen_as_switch_off);
#ifndef DRIVE_DRYRUN
    RUN_TEST(test_strict_boot_sends_stop_before_preflight);
    RUN_TEST(test_strict_boot_stop_not_confirmed_disables);
    RUN_TEST(test_strict_boot_stop_write_failure_latches_and_tries_emergency_stop);
    RUN_TEST(test_strict_unlock_rewrites_2000h_and_reads_back_before_enable);
    RUN_TEST(test_strict_2000h_readback_mismatch_blocks_unlock);
    RUN_TEST(test_strict_2000h_write_failure_latches);
#endif
#ifdef DRIVE_DRYRUN
    RUN_TEST(test_strict_dryrun_boot_stop_is_printed_only);
    RUN_TEST(test_strict_dryrun_unconfirmed_boot_stop_is_only_noted);
    RUN_TEST(test_strict_dryrun_2000h_mismatch_is_only_noted_and_arms);
#endif
}
