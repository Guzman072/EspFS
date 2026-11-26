#include <unity.h>
#include <string>
#include "onboard_log.h"

namespace {
struct MemoryStore {
    std::string contents;
    unsigned calls = 0;
    bool accept = true;

    void update(OnboardLog& log, DriveState state, bool sampling = false) {
        log.update(state, sampling, [&](const char* a, size_t aSize, const char* b, size_t bSize) {
            ++calls;
            if (!accept) return false;
            contents.append(a, aSize);
            contents.append(b, bSize);
            return true;
        });
    }
};

void test_onboard_ring_keeps_latest_complete_lines() {
    char memory[12];
    OnboardLog log(memory, sizeof memory);
    log.capture("T,1");
    log.capture("D,2");
    log.capture("S,3");  // 12 bytes，剛好滿
    log.capture("R,444"); // 跨過末端，淘汰兩個完整行
    log.capture("C,line too long");
    MemoryStore store;
    store.update(log, DriveState::DISARMED);
    TEST_ASSERT_EQUAL_STRING("S,3\nR,444\n", store.contents.c_str());
}

void test_onboard_flushes_at_safe_states_and_waits_for_stop_sampling() {
    char memory[128];
    OnboardLog log(memory, sizeof memory);
    MemoryStore store;
    log.capture("S,0,BOOT");
    store.update(log, DriveState::BOOT);
    TEST_ASSERT_EQUAL_UINT(0, store.calls);
    log.capture("S,1,DISARMED");
    store.update(log, DriveState::DISARMED);
    TEST_ASSERT_EQUAL_UINT(1, store.calls);
    store.contents.clear();

    log.capture("S,2,ARMED");
    store.update(log, DriveState::ARMED);
    log.capture("C,50,1500,0.0,0,0");
    store.update(log, DriveState::ARMED);
    store.update(log, DriveState::DISARMING);
    TEST_ASSERT_EQUAL_UINT(1, store.calls);
    TEST_ASSERT_FALSE(OnboardLog::storageAllowed(DriveState::ARMED, false));

    log.capture("S,51,DISARMED");
    for (int i = 0; i < 60; ++i) store.update(log, DriveState::DISARMED, true);
    TEST_ASSERT_EQUAL_UINT(1, store.calls);
    log.capture("# STOP L ms=20");
    store.update(log, DriveState::DISARMED);
    store.update(log, DriveState::DISARMED);
    TEST_ASSERT_EQUAL_UINT(2, store.calls);
    TEST_ASSERT_EQUAL_STRING("S,2,ARMED\nC,50,1500,0.0,0,0\nS,51,DISARMED\n# STOP L ms=20\n", store.contents.c_str());
}

void test_onboard_latched_flush_is_immediate_and_preserves_reason() {
    char memory[96];
    OnboardLog log(memory, sizeof memory);
    MemoryStore store;
    log.capture("S,1,ARMED");
    store.update(log, DriveState::ARMED);
    log.capture("S,2,LATCHED");
    log.capture("# LATCHED reason=COMM_ERROR detail=0x1");
    store.update(log, DriveState::LATCHED);
    TEST_ASSERT_EQUAL_UINT(1, store.calls);
    TEST_ASSERT_EQUAL_STRING("S,1,ARMED\nS,2,LATCHED\n# LATCHED reason=COMM_ERROR detail=0x1\n", store.contents.c_str());
}

void test_onboard_failed_flush_keeps_records_for_next_transition() {
    char memory[32];
    OnboardLog log(memory, sizeof memory);
    MemoryStore store;
    store.accept = false;
    log.capture("S,0,DISARMED");
    store.update(log, DriveState::DISARMED);
    store.update(log, DriveState::DISARMED);
    TEST_ASSERT_EQUAL_UINT(1, store.calls);
    TEST_ASSERT_TRUE(store.contents.empty());
    store.update(log, DriveState::ARMED);
    store.accept = true;
    log.capture("S,2,LATCHED");
    store.update(log, DriveState::LATCHED);
    TEST_ASSERT_EQUAL_STRING("S,0,DISARMED\nS,2,LATCHED\n", store.contents.c_str());
}
}  // namespace

void run_onboard_log_tests() {
    RUN_TEST(test_onboard_ring_keeps_latest_complete_lines);
    RUN_TEST(test_onboard_flushes_at_safe_states_and_waits_for_stop_sampling);
    RUN_TEST(test_onboard_latched_flush_is_immediate_and_preserves_reason);
    RUN_TEST(test_onboard_failed_flush_keeps_records_for_next_transition);
}
