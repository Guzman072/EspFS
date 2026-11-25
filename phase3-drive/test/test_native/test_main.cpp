#include <unity.h>

void run_speed_tests();
void run_write_guard_tests();
void run_link_tests();
void run_failsafe_tests();
void run_fsm_tests();
void run_static_tests();
void run_strict_tests();
void run_max3485_echo_tests();

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    run_speed_tests();
    run_write_guard_tests();
    run_link_tests();
    run_failsafe_tests();
    run_fsm_tests();
    run_static_tests();
    run_strict_tests();
    run_max3485_echo_tests();
    return UNITY_END();
}
