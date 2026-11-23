#include <unity.h>
#include "failsafe_monitor.h"
#include "test_support.h"

static void test_status_change_prints_r_line_and_t2_pass_on_stale() {
    FakeRemote r;
    LineLog log;
    FailsafeMonitor m(kThrottle, kArm);
    m.update(0, r, log);
    TEST_ASSERT_FALSE(m.staleSeen());
    r.frames = 5;
    m.update(100, r, log);
    r.st = RemoteStatus::STALE;
    m.update(400, r, log);
    TEST_ASSERT_TRUE(m.staleSeen());
    TEST_ASSERT_TRUE(log.has("R,400,STALE,300,"));
    TEST_ASSERT_TRUE(log.has("# T2 STALE observed after 300 ms without frames"));
    TEST_ASSERT_FALSE_MESSAGE(log.has("PASS"), "輸出行字面不得含 PASS（T2 是否通過以 DESIGN §7 (a)(b) 為準）");
    const int before = log.count("# T2 STALE observed");
    m.update(500, r, log);
    TEST_ASSERT_EQUAL_INT(before, log.count("# T2 STALE observed"));  // 只印一次
}
static void test_stale_without_ever_receiving_is_not_a_pass() {
    FakeRemote r;
    r.st = RemoteStatus::WAITING;
    LineLog log;
    FailsafeMonitor m(kThrottle, kArm);
    m.update(0, r, log);
    r.st = RemoteStatus::STALE;
    m.update(100, r, log);
    TEST_ASSERT_FALSE(m.staleSeen());
    TEST_ASSERT_FALSE(log.has("# T2 STALE observed"));
}
static void test_frozen_values_with_live_frames_show_unchanged_time() {
    // 接收器在遙控器關機後仍持續送舊值：狀態一直 RECEIVING、frame_age 很小、values_unchanged 持續增加 → 不通過
    FakeRemote r;
    LineLog log;
    FailsafeMonitor m(kThrottle, kArm);
    m.update(0, r, log);
    for (uint32_t t = 50; t <= 5000; t += 50) {
        r.frames += 1;
        m.update(t, r, log);
    }
    TEST_ASSERT_FALSE(m.staleSeen());
    TEST_ASSERT_TRUE(log.has("# T2 stale_seen=0 frame_age_ms=0 values_unchanged_ms=5000"));
}
static void test_changing_values_reset_the_unchanged_timer() {
    FakeRemote r;
    LineLog log;
    FailsafeMonitor m(kThrottle, kArm);
    m.update(0, r, log);
    r.ch[kThrottle] = 1600;
    m.update(4000, r, log);
    m.update(5000, r, log);
    TEST_ASSERT_TRUE(log.has("values_unchanged_ms=1000"));
}
static void test_periodic_r_line_every_second() {
    FakeRemote r;
    LineLog log;
    FailsafeMonitor m(kThrottle, kArm);
    m.update(0, r, log);
    TEST_ASSERT_EQUAL_INT(0, log.count("R,"));
    m.update(1000, r, log);
    TEST_ASSERT_EQUAL_INT(1, log.count("R,"));
    TEST_ASSERT_TRUE(log.has("R,1000,RECEIVING,1000,1000,1500,1000"));
    TEST_ASSERT_EQUAL_STRING("WAITING", remoteStatusName(RemoteStatus::WAITING));
    TEST_ASSERT_EQUAL_STRING("RECEIVING", remoteStatusName(RemoteStatus::RECEIVING));
}
static void test_unset_channels_print_zero() {
    FakeRemote r;
    LineLog log;
    FailsafeMonitor m(-1, -1);
    m.update(0, r, log);
    m.update(1000, r, log);
    TEST_ASSERT_TRUE(log.has("R,1000,RECEIVING,1000,1000,0,0"));
}

void run_failsafe_tests() {
    RUN_TEST(test_status_change_prints_r_line_and_t2_pass_on_stale);
    RUN_TEST(test_stale_without_ever_receiving_is_not_a_pass);
    RUN_TEST(test_frozen_values_with_live_frames_show_unchanged_time);
    RUN_TEST(test_changing_values_reset_the_unchanged_timer);
    RUN_TEST(test_periodic_r_line_every_second);
    RUN_TEST(test_unset_channels_print_zero);
}
