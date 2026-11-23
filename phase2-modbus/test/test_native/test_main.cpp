#include <unity.h>

void run_crc_tests();
void run_frame_tests();
void run_guard_tests();
void run_registers_tests();
void run_master_tests();
void run_report_tests();
void run_loopback_tests();
void run_poller_tests();
void run_static_tests();

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    run_crc_tests();
    run_frame_tests();
    run_guard_tests();
    run_registers_tests();
    run_master_tests();
    run_report_tests();
    run_loopback_tests();
    run_poller_tests();
    run_static_tests();
    return UNITY_END();
}
