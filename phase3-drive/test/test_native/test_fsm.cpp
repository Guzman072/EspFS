#include <unity.h>
#include <memory>
#include "test_support.h"
#include "write_guard.h"

using Frames = std::vector<std::string>;

static void expectPrefix(const Rig& g, size_t from, const Frames& want) {
    Frames got = describeAll(g.t.writes, from);
    TEST_ASSERT_TRUE_MESSAGE(got.size() >= want.size(), "送出的幀比預期少");
    for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
}
[[maybe_unused]] static size_t countFc(const Rig& g, uint8_t fc) {
    size_t n = 0;
    for (const auto& f : g.t.writes) if (f.size() >= 2 && f[1] == fc) ++n;
    return n;
}
static std::unique_ptr<Rig> armedRig(FakeDriver drv = FakeDriver()) {
    std::unique_ptr<Rig> g(new Rig());
    g->d = drv;
    g->bootOnly();
    g->unlockViaSwitch();
    return g;
}

// ===================== BOOT =====================
static void test_boot_sequence_and_state() {
    Rig g;
    g.bootOnly();
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    TEST_ASSERT_TRUE(g.fsm.enabled());
    TEST_ASSERT_EQUAL_INT(1, g.fsm.resolution());
    TEST_ASSERT_EQUAL_INT16(90, g.fsm.limitRaw());
    TEST_ASSERT_EQUAL_CHAR('#', g.log.lines[0][0]);
    TEST_ASSERT_TRUE(g.log.has("S,0,BOOT"));
    TEST_ASSERT_TRUE(g.log.has(",DISARMED"));
    // 預檢：只探測 ID 1，然後讀各設定暫存器；2000h＝1000 在 200–1000 內，不寫入
    expectPrefix(g, 0, {"R20A0x1", "R2022x1", "R2008x1", "R2000x1", "R200Fx1", "R2007x1", "R2011x1", "R2023x1", "R20A5x2", "R20A2x1"});
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    TEST_ASSERT_TRUE(g.log.has("speed_resolution,1,div"));
}
static void test_boot_probes_only_slave_1() {
    Rig g;
    g.d.dropAll = true;
    g.bootOnly();
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g.fsm.state());
    TEST_ASSERT_EQUAL(FaultReason::PROBE_FAIL, g.fsm.faultReason());
    for (const auto& f : g.t.writes) TEST_ASSERT_EQUAL_UINT8(1, f[0]);
    TEST_ASSERT_EQUAL_UINT32(kMaxAttempts, g.t.writes.size());
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());  // 還沒使能過，不送停止指令
    TEST_ASSERT_TRUE(g.log.has("# LATCHED reason=PROBE_FAIL"));
}
[[maybe_unused]] static void test_boot_writes_500_when_2000h_is_zero_or_too_large() {
    for (uint16_t initial : {0, 1001, 5000, 32000}) {
        Rig g;
        g.d.regs[0x2000] = initial;
        g.bootOnly();
        TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
        TEST_ASSERT_TRUE(g.fsm.enabled());
        TEST_ASSERT_TRUE(g.sawFrame("W2000=01F4"));
        TEST_ASSERT_EQUAL_HEX16(500, g.d.regs[0x2000]);
        TEST_ASSERT_FALSE(g.sawFrame("W2010=0001"));  // 不存檔
        TEST_ASSERT_EQUAL_UINT32(1, g.t.driveWrites.size());
    }
}
static void test_boot_leaves_2000h_alone_when_in_range() {
    for (uint16_t initial : {200, 500, 1000}) {
        Rig g;
        g.d.regs[0x2000] = initial;
        g.bootOnly();
        TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
        TEST_ASSERT_EQUAL_HEX16(initial, g.d.regs[0x2000]);
    }
}
static void test_boot_disables_on_bad_2022h() {
    for (uint16_t v : {0, 11, 255}) {
        Rig g;
        g.d.regs[0x2022] = v;
        g.bootOnly();
        TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
        TEST_ASSERT_FALSE(g.fsm.enabled());
        TEST_ASSERT_TRUE(g.log.has("# DISABLED reason=2022h out of range"));
        g.unlockViaSwitch();
        TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
        TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    }
}
static void test_boot_disables_when_2022h_unreadable() {
    Rig g;
    g.d.dropReadAddrs = {0x2022};
    g.bootOnly();
    TEST_ASSERT_FALSE(g.fsm.enabled());
    TEST_ASSERT_TRUE(g.log.has("# DISABLED reason=read 2022 failed: TIMEOUT"));
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
}
static void test_boot_disables_when_2008h_below_limit() {
    Rig g;
    g.d.regs[0x2008] = 20;
    g.bootOnly();
    TEST_ASSERT_FALSE(g.fsm.enabled());
    TEST_ASSERT_TRUE(g.log.has("speed limit above 2008h"));
}
static void test_boot_resolution_A() {
    Rig g;
    g.d.regs[0x2022] = 10;
    g.bootOnly();
    TEST_ASSERT_EQUAL_INT(10, g.fsm.resolution());
    TEST_ASSERT_EQUAL_INT16(900, g.fsm.limitRaw());
}
static void test_unset_channels_never_arm() {
    FsmConfig c = Rig::defaultConfig();
    c.throttleCh = -1;
    c.armCh = -1;
    Rig g(c);
    g.bootOnly();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
}

// ===================== 解鎖條件 =====================
[[maybe_unused]] static void test_unlock_sequence_frames() {
    Rig g;
    g.bootOnly();
    const size_t mark = g.t.writes.size();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
    expectPrefix(g, mark, {"R20A5x2", "R20A2x1", "W200D=0003", "S2088=0,0", "W200E=0008", "R20A2x1", "R20A5x2"});
    TEST_ASSERT_TRUE(g.log.has(",ARMING"));
    TEST_ASSERT_TRUE(g.log.has(",ARMED"));
}
static void test_swa_already_on_at_boot_does_not_unlock() {
    Rig g;
    g.r.ch[kArm] = 2000;
    g.bootOnly();
    g.run(2000);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    TEST_ASSERT_TRUE(g.log.has("# ARM blocked: SWA already on"));
    g.r.ch[kArm] = 1000;  // 看過關閉之後才能解鎖
    g.run(100);
    g.r.ch[kArm] = 2000;
    g.run(700);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
}
static void test_throttle_must_be_centered_for_half_a_second() {
    Rig g;
    g.bootOnly();
    g.r.ch[kArm] = 1000;
    g.r.ch[kThrottle] = 1700;  // 不在中位
    g.run(1000);
    g.r.ch[kArm] = 2000;
    g.run(300);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    g.r.ch[kThrottle] = 1500 + kDeadband;  // 死區邊界算中位
    g.run(300);                            // 才 300 ms
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    g.run(400);                            // 累計 ≥ 500 ms
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
}
static void test_throttle_leaving_center_restarts_the_timer() {
    Rig g;
    g.bootOnly();
    g.r.ch[kArm] = 1000;
    g.run(100);
    g.r.ch[kThrottle] = 1700;
    g.run(100);
    g.r.ch[kThrottle] = 1500;
    g.r.ch[kArm] = 2000;
    g.run(300);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    g.run(400);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
}
static void test_no_unlock_unless_receiving() {
    for (RemoteStatus st : {RemoteStatus::WAITING, RemoteStatus::STALE}) {
        Rig g;
        g.bootOnly();
        g.r.st = RemoteStatus::RECEIVING;
        g.r.ch[kArm] = 1000;
        g.run(100);
        g.r.st = st;
        g.r.ch[kArm] = 2000;
        g.run(1500);
        TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
        TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    }
}
static void test_no_unlock_when_driver_enabled_already() {
    Rig g;
    g.d.statusWord = 0x4040;
    g.bootOnly();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    TEST_ASSERT_TRUE(g.log.has("# ARM blocked: 20A2h=4040 (locked)"));
    TEST_ASSERT_FALSE(g.log.has("already enabled"));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
}
// ===================== 20A2h 狀態字（V4 RS485 §六：00 解軸、40 鎖軸、80 急停、C0 報警）=====================
// 兩軸皆報警（C0C0）：dryrun 只顯示並放行（未接馬達的 Hall 故障會讓驅動器報警）；live 擋住。
static void test_status_alarm_passes_in_dryrun_and_blocks_in_live() {
    Rig g;
    g.d.statusWord = 0xC0C0;
    g.bootOnly();
    g.unlockViaSwitch();
#ifdef DRIVE_DRYRUN
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
    TEST_ASSERT_EQUAL_INT(1, g.log.count("# would-block: 20A2h=C0C0 (alarm)"));
    TEST_ASSERT_FALSE(g.log.has("# ARM blocked"));
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 0D 00 03 53 C8"));
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 0E 00 08 E2 0F"));
#else
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    TEST_ASSERT_TRUE(g.log.has("# ARM blocked: 20A2h=C0C0 (alarm)"));
    TEST_ASSERT_FALSE(g.log.has("would-block: 20A2h"));
#endif
    TEST_ASSERT_FALSE(g.log.has("already enabled"));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
}
// 鎖軸（40）、急停（80）與混合狀態：dryrun、live 都擋，訊息用真實狀態名稱。
static void test_status_locked_estop_and_mixed_block_in_both_configs() {
    struct Case { uint16_t word; const char* text; };
    const Case cases[] = {
        {0x4040, "# ARM blocked: 20A2h=4040 (locked)"},
        {0x8080, "# ARM blocked: 20A2h=8080 (estop)"},
        {0xC000, "# ARM blocked: 20A2h=C000 (L alarm, R released)"},
        {0xC040, "# ARM blocked: 20A2h=C040 (L alarm, R locked)"},
    };
    for (const Case& c : cases) {
        Rig g;
        g.d.statusWord = c.word;
        g.bootOnly();
        g.unlockViaSwitch();
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(DriveState::DISARMED), static_cast<int>(g.fsm.state()), c.text);
        TEST_ASSERT_TRUE_MESSAGE(g.log.has(c.text), c.text);
        TEST_ASSERT_FALSE_MESSAGE(g.log.has("would-block: 20A2h"), c.text);
        TEST_ASSERT_FALSE_MESSAGE(g.log.has("already enabled"), c.text);
        TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    }
}
// 擋住時約每秒才重讀一次、同一狀態只印一次、狀態改變才再印；原因消失後 ≥1 s 內可解鎖。
static void test_blocked_status_retries_once_per_second_and_prints_once() {
    Rig g;
    g.d.statusWord = 0x4040;
    g.bootOnly();
    const size_t mark = g.t.writes.size();
    g.unlockViaSwitch();  // 約 700 ms
    g.run(2000);          // 共約 2.7 s：重試應在約 0.05、1.05、2.05 s
    size_t reads = 0;
    for (const std::string& s : describeAll(g.t.writes, mark)) if (s == "R20A2x1") ++reads;
    TEST_ASSERT_TRUE_MESSAGE(reads >= 2 && reads <= 3, "擋住時每秒最多重讀一次");
    TEST_ASSERT_EQUAL_INT(1, g.log.count("# ARM blocked: 20A2h=4040 (locked)"));
    g.d.statusWord = 0x8080;
    g.run(1100);
    TEST_ASSERT_EQUAL_INT(1, g.log.count("# ARM blocked: 20A2h=8080 (estop)"));  // 狀態改變才再印
    TEST_ASSERT_EQUAL_INT(1, g.log.count("# ARM blocked: 20A2h=4040 (locked)")); // 沒有重複
    g.d.statusWord = 0x0000;
    g.run(1100);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
}
// SWA 撥關再開：被擋住的原因重新顯示並立即重試。
static void test_switch_cycle_shows_the_blocked_reason_again() {
    Rig g;
    g.d.statusWord = 0x4040;
    g.bootOnly();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL_INT(1, g.log.count("# ARM blocked: 20A2h=4040 (locked)"));
    g.r.ch[kArm] = 1000;
    g.run(100);
    g.r.ch[kArm] = 2000;
    g.run(700);
    TEST_ASSERT_EQUAL_INT(2, g.log.count("# ARM blocked: 20A2h=4040 (locked)"));
}
static void test_no_unlock_without_failsafe_verification() {
    FsmConfig c = Rig::defaultConfig();
    c.failsafeVerified = false;
    Rig g(c);
    g.bootOnly();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    TEST_ASSERT_TRUE(g.log.has("# ARM blocked: T2 failsafe not verified"));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
}
static void test_comm_error_while_checking_unlock_latches() {
    Rig g;
    g.bootOnly();
    g.r.ch[kArm] = 1000;
    g.run(100);
    g.d.dropAll = true;
    g.r.ch[kArm] = 2000;
    g.run(700);
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g.fsm.state());
    TEST_ASSERT_EQUAL(FaultReason::COMM_ERROR, g.fsm.faultReason());
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());  // 沒使能過，不送停止指令
}

#ifndef DRIVE_DRYRUN
static void test_live_driver_fault_blocks_unlock() {
    Rig g;
    g.d.faultL = 0x0200;  // Hall 錯誤
    g.bootOnly();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    TEST_ASSERT_TRUE(g.log.has("# ARM blocked: fault 0x0200"));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    g.d.faultL = 0;
    g.d.faultR = 0x0001;
    g.r.ch[kArm] = 1000; g.run(100); g.r.ch[kArm] = 2000; g.run(700);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    TEST_ASSERT_TRUE(g.log.has("# ARM blocked: fault 0x0001"));
    g.d.faultR = 0;
    g.run(1100);  // 被擋住後至少等 1 s 才再讀驅動器（kBlockRetryMs）
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());  // 故障消失後可解鎖
}
static void test_arming_fails_if_enable_did_not_take_effect() {
    Rig g;
    g.bootOnly();
    g.d.dropWrites = true;  // 寫入沒有回應
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g.fsm.state());
    TEST_ASSERT_EQUAL(FaultReason::COMM_ERROR, g.fsm.faultReason());
    TEST_ASSERT_TRUE(g.sawFrame("W200E=0005"));  // 已進入 ARMING，所以送緊急停止
}
static void test_arming_fails_when_status_not_enabled_afterwards() {
    Rig g;
    g.bootOnly();
    g.t.responder = [&g](const std::vector<uint8_t>& q) {
        FakeDriver& d = g.d;
        if (q.size() >= 6 && q[1] == 0x06 && q[2] == 0x20 && q[3] == 0x0E && q[5] == 0x08) {
            return ScriptedReply{std::vector<uint8_t>(q.begin(), q.end()), 2};  // 回應 OK，但驅動器沒有真的使能
        }
        return d.handle(q);
    };
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g.fsm.state());
    TEST_ASSERT_EQUAL(FaultReason::DRIVER_FAULT, g.fsm.faultReason());
}
#endif

// ===================== ARMED =====================
[[maybe_unused]] static void test_armed_cycle_frames() {
    auto g = armedRig();
    g->r.ch[kThrottle] = 1500;
    const size_t mark = g->t.writes.size();
    g->run(100);
    Frames got = describeAll(g->t.writes, mark);
    TEST_ASSERT_TRUE(got.size() >= 3);
    TEST_ASSERT_EQUAL_STRING("S2088=0,0", got[0].c_str());   // 保活寫入
    TEST_ASSERT_EQUAL_STRING("R20A5x2", got[1].c_str());     // 故障碼
    TEST_ASSERT_EQUAL_STRING("R20ABx4", got[2].c_str());     // 速度與轉矩
}
[[maybe_unused]] static void test_slew_to_limit_at_3_rpm_per_50ms_and_keepalive() {
    auto g = armedRig();
    g->r.ch[kThrottle] = 2000;
    const size_t mark = g->t.driveWrites.size();
    g->run(2500);
    int prev = 0, firstNonZero = -1, firstFull = -1;
    uint32_t prevTime = 0;
    for (size_t i = mark; i < g->t.driveWrites.size(); ++i) {
        const auto& f = g->t.driveWrites[i];
        if (f[1] != 0x10) continue;
        const int l = static_cast<int16_t>((f[7] << 8) | f[8]);
        const int r = static_cast<int16_t>((f[9] << 8) | f[10]);
        TEST_ASSERT_EQUAL_INT(l, r);                         // 轉向混控關閉：左右同值
        TEST_ASSERT_TRUE(l >= prev);                         // 單調上升
        // 斜坡是「速率」：相鄰兩次寫入的增量 ≤ 速率 × 實際間隔（再容許 1 個 raw 單位的取整）。名目間隔 50 ms ＝ 3.0 r/min。
        if (prevTime > 0) {
            const float allowed = kSlewRpmPerSec * static_cast<float>(g->t.driveWriteTimes[i] - prevTime) / 1000.0f + 1.0f;
            char why[96];
            std::snprintf(why, sizeof why, "增量超過 60 r/min/s × 間隔：prev=%d l=%d t=%u", prev, l, static_cast<unsigned>(g->t.driveWriteTimes[i]));
            TEST_ASSERT_TRUE_MESSAGE(static_cast<float>(l - prev) <= allowed, why);
        }
        prevTime = g->t.driveWriteTimes[i];
        TEST_ASSERT_TRUE(l <= 90);                           // 不超過上限
        if (l > 0 && firstNonZero < 0) firstNonZero = static_cast<int>(g->t.driveWriteTimes[i]);
        if (l == 90 && firstFull < 0) firstFull = static_cast<int>(g->t.driveWriteTimes[i]);
        prev = l;
    }
    TEST_ASSERT_EQUAL_INT(90, prev);
    TEST_ASSERT_TRUE(firstNonZero > 0 && firstFull > 0);
    TEST_ASSERT_TRUE_MESSAGE(firstFull - firstNonZero >= 1350 && firstFull - firstNonZero <= 1550, "0 → 上限約 1.5 s（每 50 ms 3.0 r/min）");
    uint32_t last = 0;  // 保活：ARMED 期間每個 FC 0x10 寫入的間隔 ≤ 100 ms
    for (size_t i = mark; i < g->t.driveWrites.size(); ++i) {
        if (g->t.driveWrites[i][1] != 0x10) continue;
        if (last != 0) TEST_ASSERT_TRUE_MESSAGE(g->t.driveWriteTimes[i] - last <= kKeepaliveMaxMs, "保活間隔超過 100 ms");
        last = g->t.driveWriteTimes[i];
    }
}
[[maybe_unused]] static void test_reverse_and_resolution_A() {
    Rig g;
    g.d.regs[0x2022] = 10;
    g.bootOnly();
    g.unlockViaSwitch();
    g.r.ch[kThrottle] = 1000;
    g.run(2500);  // 0 → -90 r/min 約 1.5 s
    const auto& f = g.t.driveWrites.back();
    TEST_ASSERT_EQUAL_HEX8(0x10, f[1]);
    TEST_ASSERT_EQUAL_INT16(-900, static_cast<int16_t>((f[7] << 8) | f[8]));    // -90 r/min × 10（2022h＝A）
    TEST_ASSERT_EQUAL_INT16(-900, static_cast<int16_t>((f[9] << 8) | f[10]));
}
[[maybe_unused]] static void test_swa_off_disarms_in_documented_order() {
    auto g = armedRig();
    g->r.ch[kThrottle] = 2000;
    g->run(300);
    const size_t mark = g->t.writes.size();
    g->r.ch[kArm] = 1000;
    g->run(100);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
    expectPrefix(*g, mark, {"S2088=0,0", "W200E=0007"});   // 先速度 0，再停機
    TEST_ASSERT_TRUE(g->log.has(",DISARMING"));
    // 重新解鎖：SWA 關閉過了，但油門要先回中位 ≥ 0.5 s
    g->r.ch[kArm] = 2000;
    g->run(1000);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());   // 油門還在 2000
    g->r.ch[kThrottle] = 1500;
    g->run(300);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
    g->run(400);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
}
[[maybe_unused]] static void test_stale_disarms_and_requires_swa_to_be_seen_off() {
    auto g = armedRig();
    g->r.ch[kThrottle] = 2000;
    g->run(200);
    const size_t mark = g->t.writes.size();
    g->r.st = RemoteStatus::STALE;
    g->run(100);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
    expectPrefix(*g, mark, {"S2088=0,0", "W200E=0007"});
    g->r.st = RemoteStatus::RECEIVING;   // 訊號回來，SWA 仍然打開
    g->r.ch[kThrottle] = 1500;
    g->run(2000);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());   // 沒看過 SWA 關閉，不得重新解鎖
    g->r.ch[kArm] = 1000;
    g->run(100);
    g->r.ch[kArm] = 2000;
    g->run(700);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
}
static void test_waiting_status_also_disarms() {
    auto g = armedRig();
    g->r.st = RemoteStatus::WAITING;
    g->run(100);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g->fsm.state());
}

#ifndef DRIVE_DRYRUN
static void expectLatchedAfterEmergency(Rig& g, size_t mark, FaultReason why) {
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g.fsm.state());
    TEST_ASSERT_EQUAL(why, g.fsm.faultReason());
    Frames got = describeAll(g.t.writes, mark);
    // 故障路徑：先 200Eh＝0x05，再 2088h＝0,0，且之後不再送出任何請求
    int e = -1;
    for (size_t i = 0; i < got.size(); ++i) if (got[i] == "W200E=0005") { e = static_cast<int>(i); break; }
    TEST_ASSERT_TRUE_MESSAGE(e >= 0, "沒有送出急停");
    TEST_ASSERT_TRUE_MESSAGE(static_cast<size_t>(e) + 1 < got.size(), "急停之後沒有再送 2088h＝0,0");
    TEST_ASSERT_EQUAL_STRING("S2088=0,0", got[static_cast<size_t>(e) + 1].c_str());
    TEST_ASSERT_EQUAL_UINT32(static_cast<size_t>(e) + 2, got.size());
    const size_t total = g.t.writes.size();
    g.run(1500);
    TEST_ASSERT_EQUAL_UINT32(total, g.t.writes.size());
    TEST_ASSERT_TRUE(g.log.has("S,"));
    TEST_ASSERT_TRUE(g.log.has(",FAULT_STOP"));
    TEST_ASSERT_TRUE(g.log.has(",LATCHED"));
    TEST_ASSERT_TRUE(g.log.has("# LATCHED reason="));
}
static void test_driver_fault_left_or_right_stops() {
    for (int side = 0; side < 2; ++side) {
        auto g = armedRig();
        g->r.ch[kThrottle] = 2000;
        g->run(200);
        const size_t mark = g->t.writes.size();
        (side == 0 ? g->d.faultL : g->d.faultR) = 0x0004;
        g->run(100);
        expectLatchedAfterEmergency(*g, mark, FaultReason::DRIVER_FAULT);
        TEST_ASSERT_EQUAL_UINT32(0x0004, g->fsm.faultDetail());
        TEST_ASSERT_TRUE(g->log.has("# LATCHED reason=DRIVER_FAULT detail=0x4"));
    }
}
static void test_comm_timeout_stops() {
    auto g = armedRig();
    g->run(200);
    const size_t mark = g->t.writes.size();
    g->d.dropAll = true;
    g->run(100);
    expectLatchedAfterEmergency(*g, mark, FaultReason::COMM_ERROR);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(Status::TIMEOUT), g->fsm.faultDetail());
}
static void test_exception_reply_stops() {
    auto g = armedRig();
    g->run(200);
    const size_t mark = g->t.writes.size();
    g->d.exceptionOnWrite = true;
    g->run(100);
    // 例外回應也視為通訊錯誤；緊急停止的回應同樣是例外，但不影響序列
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g->fsm.state());
    TEST_ASSERT_EQUAL(FaultReason::COMM_ERROR, g->fsm.faultReason());
    TEST_ASSERT_TRUE(g->sawFrame("W200E=0005"));
    (void)mark;
}
static void test_write_mismatch_stops() {
    auto g = armedRig();
    g->run(200);
    const size_t mark = g->t.writes.size();
    g->d.corruptMultiReply = true;
    g->run(100);
    expectLatchedAfterEmergency(*g, mark, FaultReason::WRITE_MISMATCH);
}
// 超速門檻＝上限 90 × 1.2 ＝ 108.0 r/min（1080，單位 0.1 r/min）：1080 不超速、1081 超速。
static void test_overspeed_over_limit_factor_stops_and_boundary_does_not() {
    {
        auto g = armedRig();
        g->d.actualVelL = 1080;  // 108.0 r/min＝上限 90 的 1.2 倍，不超過
        g->d.actualVelR = -1080;
        g->run(300);
        TEST_ASSERT_EQUAL(DriveState::ARMED, g->fsm.state());
    }
    for (int side = 0; side < 2; ++side) {
        auto g = armedRig();
        g->run(100);
        const size_t mark = g->t.writes.size();
        (side == 0 ? g->d.actualVelL : g->d.actualVelR) = side == 0 ? 1081 : -1081;
        g->run(100);
        expectLatchedAfterEmergency(*g, mark, FaultReason::OVERSPEED);
        TEST_ASSERT_EQUAL_UINT32(1081, g->fsm.faultDetail());
    }
}
static void test_rejected_request_stops_and_emergency_still_goes_out() {
    auto g = armedRig();
    g->r.ch[kThrottle] = 2000;
    g->run(100);
    const size_t mark = g->t.writes.size();
    g->link.setLimitRaw(5);  // 模擬程式錯誤：要送出的速度超過寫入 guard 的上限
    g->run(600);
    expectLatchedAfterEmergency(*g, mark, FaultReason::REJECTED);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(Status::REJECTED_VALUE), g->fsm.faultDetail());
}
static void test_tx_fail_stops() {
    auto g = armedRig();
    g->run(100);
    const size_t drv = g->t.driveWrites.size();
    g->t.refuse = true;
    g->run(100);
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g->fsm.state());
    TEST_ASSERT_EQUAL(FaultReason::TX_FAIL, g->fsm.faultReason());
    TEST_ASSERT_EQUAL_UINT32(drv, g->t.driveWrites.size());  // 拒絕寫入：一個位元組都沒送
}
static void test_driver_fault_while_disarmed_is_ignored_until_unlock_attempt() {
    Rig g;
    g.bootOnly();
    g.d.faultL = 0x0200;
    g.run(2000);  // 沒有解鎖企圖，不讀故障碼，也不送任何東西
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
}
static void test_motion_stops_when_comm_dies_even_if_emergency_fails() {
    auto g = armedRig();
    g->r.ch[kThrottle] = 2000;
    g->run(300);
    g->d.dropAll = true;
    g->run(100);
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g->fsm.state());
    // 通訊壞掉時緊急停止也送不到：驅動器靠 2000h 掉線保護停機（這裡只驗證我們有送、且不再繼續送）
    TEST_ASSERT_TRUE(g->sawFrame("W200E=0005"));
}
#endif

// ===================== 全域性質 =====================
static void test_no_frame_outside_the_whitelist_and_never_0x06_clear() {
    auto g = armedRig();
    WriteGuard guard;
    guard.setLimitRaw(90);
    g->r.ch[kThrottle] = 2000;
    g->run(500);
    g->r.ch[kThrottle] = 1000;
    g->run(1000);
    g->r.ch[kArm] = 1000;
    g->run(200);
    g->r.ch[kArm] = 2000;
    g->r.ch[kThrottle] = 1500;
    g->run(800);
    g->d.faultL = 0x0200;
#ifndef DRIVE_DRYRUN
    g->run(300);
#endif
    TEST_ASSERT_TRUE(g->t.writes.size() > 20);
    for (const auto& f : g->t.writes) {
        TEST_ASSERT_EQUAL_MESSAGE(Status::OK, guard.check(f.data(), f.size()), describe(f).c_str());
        TEST_ASSERT_EQUAL_UINT8(1, f[0]);
        if (f[1] == 0x06) {
            const unsigned val = (f[4] << 8) | f[5];
            TEST_ASSERT_TRUE_MESSAGE(!(f[2] == 0x20 && f[3] == 0x0E && val == 0x06), "狀態機不得自動清故障（0x06）");
            const unsigned addr = (f[2] << 8) | f[3];
            TEST_ASSERT_TRUE(addr == 0x200D || addr == 0x200E || addr == 0x2000);
        }
        if (f[1] == 0x10) TEST_ASSERT_EQUAL_HEX8(0x20, f[2]);
    }
}
static void test_state_names() {
    TEST_ASSERT_EQUAL_STRING("BOOT", driveStateName(DriveState::BOOT));
    TEST_ASSERT_EQUAL_STRING("DISARMED", driveStateName(DriveState::DISARMED));
    TEST_ASSERT_EQUAL_STRING("ARMING", driveStateName(DriveState::ARMING));
    TEST_ASSERT_EQUAL_STRING("ARMED", driveStateName(DriveState::ARMED));
    TEST_ASSERT_EQUAL_STRING("DISARMING", driveStateName(DriveState::DISARMING));
    TEST_ASSERT_EQUAL_STRING("FAULT_STOP", driveStateName(DriveState::FAULT_STOP));
    TEST_ASSERT_EQUAL_STRING("LATCHED", driveStateName(DriveState::LATCHED));
    TEST_ASSERT_EQUAL_STRING("OVERSPEED", faultReasonName(FaultReason::OVERSPEED));
    TEST_ASSERT_EQUAL_STRING("TX_FAIL", faultReasonName(FaultReason::TX_FAIL));
    TEST_ASSERT_EQUAL(FaultReason::REJECTED, faultReasonFor(Status::REJECTED_REG));
    TEST_ASSERT_EQUAL(FaultReason::COMM_ERROR, faultReasonFor(Status::CRC_ERR));
    TEST_ASSERT_EQUAL(FaultReason::NONE, faultReasonFor(Status::OK));
}

// ===================== 左右方向正負號（T3/T4 實測 2026-11-23：左輪正確、右輪相反 → kRightSign＝-1）=====================
static void test_real_signs_are_left_plus_and_right_minus() {
    static_assert(kLeftSign == 1 && kRightSign == -1, "T3/T4 實測：左輪不變、右輪取反");
    const FsmConfig c = Rig::realConfig();
    TEST_ASSERT_EQUAL_INT(1, c.leftSign);
    TEST_ASSERT_EQUAL_INT(-1, c.rightSign);
}
// 寫入端：右輪寫入取反。每一個 C 行都是 right＝-left，前進（+90,-90）與後退（-90,+90）都檢查。
static void test_real_signs_write_right_wheel_reversed() {
    Rig g(Rig::realConfig());
    g.bootOnly();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
    g.r.ch[kThrottle] = 2000;
    g.run(2500);                // 0 → +90 約 1.5 s
    g.r.ch[kThrottle] = 1000;
    g.run(4000);                // +90 → -90 約 3.0 s
    int checked = 0;
    bool sawForward = false, sawReverse = false;
    for (const std::string& l : g.log.lines) {
        unsigned ms = 0, thr = 0;
        float cmd = 0;
        int left = 0, right = 0;
        if (std::sscanf(l.c_str(), "C,%u,%u,%f,%d,%d", &ms, &thr, &cmd, &left, &right) != 5) continue;
        TEST_ASSERT_EQUAL_INT_MESSAGE(left, -right, l.c_str());
        sawForward = sawForward || (left == 90 && right == -90);
        sawReverse = sawReverse || (left == -90 && right == 90);
        ++checked;
    }
    TEST_ASSERT_TRUE(checked > 20);
    TEST_ASSERT_TRUE_MESSAGE(sawForward, "沒有看到前進的 +90,-90");
    TEST_ASSERT_TRUE_MESSAGE(sawReverse, "沒有看到後退的 -90,+90");
#ifdef DRIVE_DRYRUN
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 10 20 88 00 02 04 00 5A FF A6"));
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 10 20 88 00 02 04 FF A6 00 5A"));
#else
    TEST_ASSERT_TRUE(g.sawFrame("S2088=90,-90"));
    TEST_ASSERT_TRUE(g.sawFrame("S2088=-90,90"));
#endif
}
// 讀回端：D 行的 actual_velocity 用同一組正負號，油門往上時兩輪都是正值。
static void test_real_signs_normalize_feedback_in_d_lines() {
    Rig g(Rig::realConfig());
    g.d.actualVelL = 900;   // 驅動器的原始回授：左輪寫 +90 → +90.0；右輪寫 -90 → -90.0
    g.d.actualVelR = -900;
    g.bootOnly();
    g.unlockViaSwitch();
    g.run(600);             // ARMED 的 D 行每 10 個週期（500 ms）才完整記錄一次
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
    TEST_ASSERT_TRUE(g.log.has(",actual_velocity_L,90.0,r/min"));
    TEST_ASSERT_TRUE(g.log.has(",actual_velocity_R,90.0,r/min"));
    TEST_ASSERT_FALSE(g.log.has(",actual_velocity_R,-90.0,r/min"));
    // 對照：左右同號的設定下，右輪原始值原樣顯示
    Rig h;
    h.d.actualVelR = -900;
    h.bootOnly();
    h.unlockViaSwitch();
    h.run(600);
    TEST_ASSERT_TRUE(h.log.has(",actual_velocity_R,-90.0,r/min"));
}

// 兩個 native 環境都用假遙控器測混控；既有 Rig 預設仍走直行路徑。
constexpr int kSteer = 3;
static FsmConfig steeringConfig() {
    FsmConfig c = Rig::realConfig();
    c.steerCh = kSteer;
    return c;
}

struct SteeringCommand {
    unsigned ms = 0, throttle = 0;
    float chassisRpm = 0, steerRpm = 0, leftTarget = 0, rightTarget = 0;
    int leftRaw = 0, rightRaw = 0;
};

static int parseSteeringCommand(const std::string& line, SteeringCommand& c) {
    return std::sscanf(line.c_str(), "C,%u,%u,%f,%d,%d,%f,%f,%f", &c.ms, &c.throttle, &c.chassisRpm,
                       &c.leftRaw, &c.rightRaw, &c.steerRpm, &c.leftTarget, &c.rightTarget);
}

static void expectSteeringTarget(const Rig& g, float steer, float left, float right, int leftRaw, int rightRaw) {
    for (auto it = g.log.lines.rbegin(); it != g.log.lines.rend(); ++it) {
        if (it->compare(0, 2, "C,") != 0) continue;
        SteeringCommand c;
        TEST_ASSERT_EQUAL_INT(8, parseSteeringCommand(*it, c));
        TEST_ASSERT_EQUAL_UINT(g.r.ch[kThrottle], c.throttle);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, steer, c.steerRpm);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, left, c.leftTarget);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, right, c.rightTarget);
        TEST_ASSERT_EQUAL_INT(leftRaw, c.leftRaw);
        TEST_ASSERT_EQUAL_INT(rightRaw, c.rightRaw);
        return;
    }
    TEST_FAIL_MESSAGE("找不到指令 C 行");
}

static void test_steering_maps_deadband_and_turns_both_ways_in_forward_and_reverse() {
    Rig g(steeringConfig());
    g.d.regs[0x2022] = 10;
    g.bootOnly();
    g.unlockViaSwitch();
    struct Case { unsigned throttle, steer; float diff, left, right; int rawL, rawR; };
    const Case cases[] = {
        {1770, 1500, 0, 45, 45, 450, -450},
        {1770, 1460, 0, 45, 45, 450, -450},
        {1770, 1540, 0, 45, 45, 450, -450},
        {1770, 1542, 0.1f, 45.1f, 44.9f, 451, -449},
        {1770, 1458, -0.1f, 44.9f, 45.1f, 449, -451},
        {1770, 2000, 22.5f, 67.5f, 22.5f, 675, -225},
        {1770, 1000, -22.5f, 22.5f, 67.5f, 225, -675},
        {1230, 2000, 22.5f, -22.5f, -67.5f, -225, 675},
        {1230, 1000, -22.5f, -67.5f, -22.5f, -675, 225},
    };
    for (const auto& c : cases) {
        g.r.ch[kThrottle] = c.throttle;
        g.r.ch[kSteer] = c.steer;
        g.run(2500);
        TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
        expectSteeringTarget(g, c.diff, c.left, c.right, c.rawL, c.rawR);
    }
    // 確認混控經過現有 2088h／2089h 路徑，右輪符號只在送出前套用。
#ifdef DRIVE_DRYRUN
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 10 20 88 00 02 04 FD 5D 00 E1"));
    TEST_ASSERT_TRUE(g.t.driveWrites.empty());
#else
    TEST_ASSERT_TRUE(g.sawFrame("S2088=-675,225"));
#endif
}

static void test_steering_limits_each_wheel_and_disallows_pivot() {
    Rig g(steeringConfig());
    g.bootOnly();
    g.unlockViaSwitch();
    g.r.ch[kSteer] = 2400;  // 超出 CH4 上界時仍只用滿行程
    g.run(100);
    expectSteeringTarget(g, 0, 0, 0, 0, 0);
    g.r.ch[kThrottle] = 1586;  // 9 r/min，S 限於 9 r/min
    g.run(600);
    expectSteeringTarget(g, 9, 18, 0, 18, 0);
    g.r.ch[kThrottle] = 2000;
    g.run(2000);
    expectSteeringTarget(g, 22.5f, 90, 67.5f, 90, -68);
    g.r.ch[kThrottle] = 1000;
    g.run(3300);
    expectSteeringTarget(g, 22.5f, -67.5f, -90, -68, 90);
    g.r.ch[kThrottle] = 1500;
    g.run(1700);
    expectSteeringTarget(g, 0, 0, 0, 0, 0);
}

static void test_steering_each_wheel_slews_and_holds_its_own_target() {
    Rig g(steeringConfig());
    g.d.regs[0x2022] = 10;
    g.bootOnly();
    g.unlockViaSwitch();
    g.r.ch[kThrottle] = 1770;
    g.r.ch[kSteer] = 2000;
    g.run(1400);
    SteeringCommand prev;
    bool havePrev = false;
    bool rightAtTargetFirst = false;
    for (const auto& line : g.log.lines) {
        SteeringCommand c;
        if (parseSteeringCommand(line, c) != 8) continue;
        if (havePrev) {
            const float maxStep = kSlewRpmPerSec * (c.ms - prev.ms) / 100.0f + 1.0f;  // 原始值，含取整誤差
            TEST_ASSERT_FLOAT_WITHIN(maxStep, prev.leftRaw, c.leftRaw);
            TEST_ASSERT_FLOAT_WITHIN(maxStep, prev.rightRaw, c.rightRaw);
            rightAtTargetFirst |= c.rightRaw == -225 && c.leftRaw > prev.leftRaw && c.leftRaw < 675;
        }
        prev = c;
        havePrev = true;
    }
    TEST_ASSERT_TRUE(rightAtTargetFirst);
    expectSteeringTarget(g, 22.5f, 67.5f, 22.5f, 675, -225);
}

static void test_steering_requires_continuous_overlap_of_both_centered_axes() {
    Rig g(steeringConfig());
    g.bootOnly();
    g.r.ch[kSteer] = 1541;
    g.run(100);
    g.r.ch[kArm] = 2000;
    g.run(600);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    g.r.ch[kThrottle] = 1541;
    g.r.ch[kSteer] = 1460;
    g.run(600);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    g.r.ch[kThrottle] = 1540;
    g.run(450);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    g.run(100);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
}

static void test_steering_restarts_center_timer_on_brief_excursion_or_signal_loss() {
    for (int event : {kThrottle, kSteer, -1, -2}) {
        Rig g(steeringConfig());
        g.bootOnly();
        g.run(400);
        if (event >= 0) g.r.ch[event] = 1541;
        else g.r.st = event == -1 ? RemoteStatus::STALE : RemoteStatus::WAITING;
        g.run(10);  // 短於 50 ms 控制週期，仍須重新計時
        if (event >= 0) g.r.ch[event] = 1500;
        g.r.st = RemoteStatus::RECEIVING;
        g.r.ch[kArm] = 2000;
        g.run(450);
        TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
        g.run(100);
        TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
    }
}

static void test_steering_disarms_with_frozen_ch4_and_with_stale_then_rearms_from_zero() {
    for (bool stale : {false, true}) {
        Rig g(steeringConfig());
        g.bootOnly();
        g.unlockViaSwitch();
        g.r.ch[kThrottle] = 1770;
        g.r.ch[kSteer] = 2000;
        g.run(1800);
        expectSteeringTarget(g, 22.5f, 67.5f, 22.5f, 68, -23);
        const size_t frameMark = g.t.writes.size();
        const size_t logMark = g.log.lines.size();
        if (stale) g.r.st = RemoteStatus::STALE;
        else g.r.ch[kArm] = 1000;  // CH7 failsafe 生效，CH4 仍保留舊值
        g.r.ch[kThrottle] = 1500;
        g.run(1200);
        TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
        TEST_ASSERT_TRUE(g.log.indexOf(",DISARMING", logMark) >= 0);
        if (stale) TEST_ASSERT_TRUE(g.log.has("# T2 STALE observed"));
#ifdef DRIVE_DRYRUN
        const int stop = g.log.indexOf("# DRY W 01 06 20 0E 00 07", logMark);
        TEST_ASSERT_TRUE(stop > 0);
        TEST_ASSERT_TRUE(g.log.lines[static_cast<size_t>(stop) - 1].find("# DRY W 01 10 20 88 00 02 04 00 00 00 00") == 0);
        TEST_ASSERT_EQUAL_UINT(frameMark, g.t.writes.size());
#else
        expectPrefix(g, frameMark, {"S2088=0,0", "W200E=0007"});
#endif
        g.r.st = RemoteStatus::RECEIVING;
        g.r.ch[kArm] = 2000;
        g.run(100);
        TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
        g.r.ch[kSteer] = 1500;
        if (stale) {
            g.run(700);
            TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
            g.r.ch[kArm] = 1000;
            g.run(100);
            g.r.ch[kArm] = 2000;
        }
        g.run(700);
        TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
        expectSteeringTarget(g, 0, 0, 0, 0, 0);
    }
}

static void test_steering_logs_append_fields_and_straight_line_keeps_original_format() {
    Rig steering(steeringConfig());
    Rig straight(Rig::realConfig());
    for (Rig* g : {&steering, &straight}) {
        g->bootOnly();
        g->unlockViaSwitch();
        g->r.ch[kSteer] = 1623;
        g->run(1100);
        const bool mixed = g == &steering;
        bool sawRemote = false, sawCommand = false;
        for (const auto& line : g->log.lines) {
            unsigned ms, age, unchanged, throttle, arm, raw;
            char status[16];
            const int fields = std::sscanf(line.c_str(), "R,%u,%15[^,],%u,%u,%u,%u,%u", &ms, status, &age, &unchanged,
                                           &throttle, &arm, &raw);
            if (fields > 0) {
                TEST_ASSERT_EQUAL_INT(mixed ? 7 : 6, fields);
                TEST_ASSERT_EQUAL_UINT(1500, throttle);
                TEST_ASSERT_EQUAL_UINT(2000, arm);
                sawRemote |= !mixed || raw == 1623;
            }
            SteeringCommand c;
            const int commands = parseSteeringCommand(line, c);
            if (commands > 0) {
                TEST_ASSERT_EQUAL_INT(mixed ? 8 : 5, commands);
                TEST_ASSERT_EQUAL_UINT(1500, c.throttle);
                TEST_ASSERT_EQUAL_INT(0, c.leftRaw);
                TEST_ASSERT_EQUAL_INT(0, c.rightRaw);
                sawCommand = true;
            }
        }
        TEST_ASSERT_TRUE(sawRemote && sawCommand);
    }
}

// 超速判斷：方向換成「油門往上為正」後取絕對值，108.0 r/min 邊界不超過、1081 超過，任何輪、任何方向都一樣。
static void expect_symmetric_overspeed(const FsmConfig& config) {
    struct Case { int16_t velL, velR; };
    const Case ok[] = {{1080, -1080}, {-1080, 1080}, {1080, 1080}, {-1080, -1080}};
    for (const Case& c : ok) {
        Rig g(config);
        g.bootOnly();
        g.unlockViaSwitch();
        g.d.actualVelL = c.velL;
        g.d.actualVelR = c.velR;
        g.run(300);
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(DriveState::ARMED), static_cast<int>(g.fsm.state()), "邊界值 108.0 r/min（上限 90 的 1.2 倍）不得超速");
    }
    const Case bad[] = {{1081, 0}, {-1081, 0}, {0, 1081}, {0, -1081}};
    for (const Case& c : bad) {
        Rig g(config);
        g.bootOnly();
        g.unlockViaSwitch();
        g.d.actualVelL = c.velL;
        g.d.actualVelR = c.velR;
        g.run(300);
        TEST_ASSERT_EQUAL(DriveState::LATCHED, g.fsm.state());
        TEST_ASSERT_EQUAL(FaultReason::OVERSPEED, g.fsm.faultReason());
        TEST_ASSERT_EQUAL_UINT32(1081, g.fsm.faultDetail());
    }
}
static void test_overspeed_is_symmetric_with_real_signs() { expect_symmetric_overspeed(Rig::realConfig()); }
static void test_overspeed_is_symmetric_with_steering() { expect_symmetric_overspeed(steeringConfig()); }
#ifndef DRIVE_DRYRUN
// 端到端（只有 live 有真的寫入）：假驅動器的回授跟著寫入值走，油門往上時左右回授都是正的。
static void test_real_signs_end_to_end_feedback_positive_on_both_wheels() {
    Rig g(Rig::realConfig());
    g.d.followTarget = true;
    g.bootOnly();
    g.unlockViaSwitch();
    g.r.ch[kThrottle] = 2000;
    g.run(2500);  // 0 → 90 r/min 約 1.5 s
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
    TEST_ASSERT_EQUAL_INT16(900, g.d.actualVelL);
    TEST_ASSERT_EQUAL_INT16(-900, g.d.actualVelR);  // 原始回授跟著寫入值（右輪寫 -90）
    TEST_ASSERT_TRUE(g.log.has(",actual_velocity_L,90.0,r/min"));
    TEST_ASSERT_TRUE(g.log.has(",actual_velocity_R,90.0,r/min"));
    TEST_ASSERT_FALSE(g.log.has(",actual_velocity_R,-90.0,r/min"));
}
#endif

#ifdef DRIVE_DRYRUN
// ===================== dryrun 專屬 =====================
static void test_dryrun_never_sends_a_write() {
    Rig g;
    g.d.faultL = 0x0200;  // 未接馬達時 20A5h＝0x0200 為預期
    g.bootOnly();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());   // 故障碼只顯示，不阻擋解鎖
    g.r.ch[kThrottle] = 2000;
    g.run(2500);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());   // 沒有「寫入後預期狀態改變」，所以不會誤判 LATCHED
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    TEST_ASSERT_EQUAL_UINT32(0, countFc(g, 0x06));
    TEST_ASSERT_EQUAL_UINT32(0, countFc(g, 0x10));
    TEST_ASSERT_TRUE(countFc(g, 0x03) > 0);                // 讀取仍走真實唯讀 transport
    TEST_ASSERT_EQUAL_HEX16(0x0000, g.d.statusWord);       // 驅動器狀態沒變
    TEST_ASSERT_TRUE(g.log.has("# would-block: fault 0x0200"));
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 0D 00 03 53 C8"));
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 0E 00 08 E2 0F"));
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 10 20 88 00 02 04 00 5A 00 5A"));   // 90 r/min，左右同值
}
static void test_dryrun_would_block_printed_once_per_code() {
    Rig g;
    g.d.faultL = 0x0200;
    g.bootOnly();
    g.unlockViaSwitch();
    g.run(500);
    TEST_ASSERT_EQUAL_INT(1, g.log.count("# would-block: fault 0x0200"));
}
static void test_dryrun_still_blocks_when_driver_already_enabled() {
    Rig g;
    g.d.statusWord = 0x4040;  // 這是驅動器的真實狀態，不是寫入造成的
    g.bootOnly();
    g.unlockViaSwitch();
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
}
static void test_dryrun_boot_prints_offline_write_without_readback_check() {
    Rig g;
    g.d.regs[0x2000] = 0;
    g.bootOnly();
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 00 01 F4"));
    TEST_ASSERT_TRUE(g.fsm.enabled());                     // 沒有讀回檢查，所以不會因為值沒變而停用
    TEST_ASSERT_EQUAL_HEX16(0, g.d.regs[0x2000]);
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
}
static void test_dryrun_disarm_and_fault_paths_print_only() {
    Rig g;
    g.bootOnly();
    g.unlockViaSwitch();
    g.r.ch[kArm] = 1000;
    g.run(100);
    TEST_ASSERT_EQUAL(DriveState::DISARMED, g.fsm.state());
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 0E 00 07 A2 0B"));
    const int stopIdx = g.log.indexOf("# DRY W 01 06 20 0E 00 07 A2 0B");
    TEST_ASSERT_TRUE_MESSAGE(stopIdx > 0 && g.log.lines[static_cast<size_t>(stopIdx) - 1].find("# DRY W 01 10 20 88 00 02 04 00 00 00 00") != std::string::npos,
                             "正常解除：先 2088h＝0,0 再 200Eh＝0x07");
    g.r.ch[kArm] = 2000;
    g.run(700);
    TEST_ASSERT_EQUAL(DriveState::ARMED, g.fsm.state());
    g.d.dropAll = true;   // 讀取逾時仍會觸發故障路徑（通訊錯誤在 dryrun 也有效）
    g.run(100);
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g.fsm.state());
    TEST_ASSERT_TRUE(g.log.has("# DRY W 01 06 20 0E 00 05 23 CA"));
    TEST_ASSERT_EQUAL_UINT32(0, g.t.driveWrites.size());
    TEST_ASSERT_EQUAL_UINT32(0, countFc(g, 0x06) + countFc(g, 0x10));
}
static void test_dryrun_rejected_still_latches() {
    Rig g;
    g.bootOnly();
    g.unlockViaSwitch();
    g.link.setLimitRaw(5);
    g.r.ch[kThrottle] = 2000;
    g.run(600);
    TEST_ASSERT_EQUAL(DriveState::LATCHED, g.fsm.state());
    TEST_ASSERT_EQUAL(FaultReason::REJECTED, g.fsm.faultReason());
}
#endif

void run_fsm_tests() {
    RUN_TEST(test_boot_sequence_and_state);
    RUN_TEST(test_boot_probes_only_slave_1);
    RUN_TEST(test_boot_leaves_2000h_alone_when_in_range);
    RUN_TEST(test_boot_disables_on_bad_2022h);
    RUN_TEST(test_boot_disables_when_2022h_unreadable);
    RUN_TEST(test_boot_disables_when_2008h_below_limit);
    RUN_TEST(test_boot_resolution_A);
    RUN_TEST(test_unset_channels_never_arm);
    RUN_TEST(test_swa_already_on_at_boot_does_not_unlock);
    RUN_TEST(test_throttle_must_be_centered_for_half_a_second);
    RUN_TEST(test_throttle_leaving_center_restarts_the_timer);
    RUN_TEST(test_no_unlock_unless_receiving);
    RUN_TEST(test_no_unlock_when_driver_enabled_already);
    RUN_TEST(test_status_alarm_passes_in_dryrun_and_blocks_in_live);
    RUN_TEST(test_status_locked_estop_and_mixed_block_in_both_configs);
    RUN_TEST(test_blocked_status_retries_once_per_second_and_prints_once);
    RUN_TEST(test_switch_cycle_shows_the_blocked_reason_again);
    RUN_TEST(test_no_unlock_without_failsafe_verification);
    RUN_TEST(test_comm_error_while_checking_unlock_latches);
#ifndef DRIVE_DRYRUN
    RUN_TEST(test_boot_writes_500_when_2000h_is_zero_or_too_large);
    RUN_TEST(test_unlock_sequence_frames);
    RUN_TEST(test_armed_cycle_frames);
    RUN_TEST(test_slew_to_limit_at_3_rpm_per_50ms_and_keepalive);
    RUN_TEST(test_reverse_and_resolution_A);
    RUN_TEST(test_swa_off_disarms_in_documented_order);
    RUN_TEST(test_stale_disarms_and_requires_swa_to_be_seen_off);
    RUN_TEST(test_live_driver_fault_blocks_unlock);
    RUN_TEST(test_arming_fails_if_enable_did_not_take_effect);
    RUN_TEST(test_arming_fails_when_status_not_enabled_afterwards);
#endif
    RUN_TEST(test_waiting_status_also_disarms);
#ifndef DRIVE_DRYRUN
    RUN_TEST(test_driver_fault_left_or_right_stops);
    RUN_TEST(test_comm_timeout_stops);
    RUN_TEST(test_exception_reply_stops);
    RUN_TEST(test_write_mismatch_stops);
    RUN_TEST(test_overspeed_over_limit_factor_stops_and_boundary_does_not);
    RUN_TEST(test_rejected_request_stops_and_emergency_still_goes_out);
    RUN_TEST(test_tx_fail_stops);
    RUN_TEST(test_driver_fault_while_disarmed_is_ignored_until_unlock_attempt);
    RUN_TEST(test_motion_stops_when_comm_dies_even_if_emergency_fails);
#endif
    RUN_TEST(test_no_frame_outside_the_whitelist_and_never_0x06_clear);
    RUN_TEST(test_state_names);
    RUN_TEST(test_real_signs_are_left_plus_and_right_minus);
    RUN_TEST(test_real_signs_write_right_wheel_reversed);
    RUN_TEST(test_real_signs_normalize_feedback_in_d_lines);
    RUN_TEST(test_steering_maps_deadband_and_turns_both_ways_in_forward_and_reverse);
    RUN_TEST(test_steering_limits_each_wheel_and_disallows_pivot);
    RUN_TEST(test_steering_each_wheel_slews_and_holds_its_own_target);
    RUN_TEST(test_steering_requires_continuous_overlap_of_both_centered_axes);
    RUN_TEST(test_steering_restarts_center_timer_on_brief_excursion_or_signal_loss);
    RUN_TEST(test_steering_disarms_with_frozen_ch4_and_with_stale_then_rearms_from_zero);
    RUN_TEST(test_steering_logs_append_fields_and_straight_line_keeps_original_format);
    RUN_TEST(test_overspeed_is_symmetric_with_steering);
    RUN_TEST(test_overspeed_is_symmetric_with_real_signs);
#ifndef DRIVE_DRYRUN
    RUN_TEST(test_real_signs_end_to_end_feedback_positive_on_both_wheels);
#endif
#ifdef DRIVE_DRYRUN
    RUN_TEST(test_dryrun_never_sends_a_write);
    RUN_TEST(test_dryrun_would_block_printed_once_per_code);
    RUN_TEST(test_dryrun_still_blocks_when_driver_already_enabled);
    RUN_TEST(test_dryrun_boot_prints_offline_write_without_readback_check);
    RUN_TEST(test_dryrun_disarm_and_fault_paths_print_only);
    RUN_TEST(test_dryrun_rejected_still_latches);
#endif
}
