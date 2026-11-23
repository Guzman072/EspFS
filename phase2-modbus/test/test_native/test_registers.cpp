#include <unity.h>
#include <cstring>
#include "registers.h"

static void test_range_allowed() {
    TEST_ASSERT_TRUE(rangeAllowed(0x2000, 3));
    TEST_ASSERT_TRUE(rangeAllowed(0x2001, 1));
    TEST_ASSERT_TRUE(rangeAllowed(0x20A0, 15));
    TEST_ASSERT_TRUE(rangeAllowed(0x20AE, 1));
}
static void test_range_rejected() {
    TEST_ASSERT_FALSE(rangeAllowed(0x2000, 0));
    TEST_ASSERT_FALSE(rangeAllowed(0x2000, 4));
    TEST_ASSERT_FALSE(rangeAllowed(0x2003, 1));
    TEST_ASSERT_FALSE(rangeAllowed(0x209F, 1));
    TEST_ASSERT_FALSE(rangeAllowed(0x20AF, 1));
    TEST_ASSERT_FALSE(rangeAllowed(0x20AE, 2));
    TEST_ASSERT_FALSE(rangeAllowed(0x20A0, 16));
    TEST_ASSERT_FALSE(rangeAllowed(0x2088, 1));  // 控制字區，不在白名單
    TEST_ASSERT_FALSE(rangeAllowed(0xFFFF, 2));  // 不得溢位繞回
}
static void test_every_whitelisted_address_has_an_entry() {
    for (uint16_t a = 0x2000; a <= 0x2002; ++a) TEST_ASSERT_NOT_NULL(findRegister(a));
    for (uint16_t a = 0x20A0; a <= 0x20AE; ++a) TEST_ASSERT_NOT_NULL(findRegister(a));
    TEST_ASSERT_NULL(findRegister(0x20AF));
    TEST_ASSERT_NULL(findRegister(0x2003));
}
static void test_all_registers_in_table_are_in_whitelist() {
    for (size_t i = 0; i < registerCount(); ++i)
        TEST_ASSERT_TRUE(rangeAllowed(registerAt(i).addr, 1));
}
static void test_table_names() {
    TEST_ASSERT_EQUAL_STRING("dc_voltage", findRegister(0x20A1)->name);
    TEST_ASSERT_EQUAL_STRING("node_id", findRegister(0x2001)->name);
}

static void test_decode_voltage() {
    uint16_t regs[1] = {4800};
    DecodedValue out[4];
    size_t n = decodeBlock(0x20A1, regs, 1, out, 4);
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_STRING("dc_voltage", out[0].name);
    TEST_ASSERT_EQUAL_STRING("V", out[0].unit);
    TEST_ASSERT_EQUAL_INT32(4800, out[0].scaled);
    TEST_ASSERT_EQUAL_UINT8(2, out[0].decimals);
}
static void test_decode_velocity_manual_example() {
    uint16_t regs[2] = {0x0064, 0x0064};
    DecodedValue out[4];
    size_t n = decodeBlock(0x20AB, regs, 2, out, 4);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_STRING("actual_velocity_L", out[0].name);
    TEST_ASSERT_EQUAL_STRING("actual_velocity_R", out[1].name);
    TEST_ASSERT_EQUAL_INT32(100, out[0].scaled);  // 100 × 0.1 = 10.0 r/min
    TEST_ASSERT_EQUAL_UINT8(1, out[0].decimals);
    TEST_ASSERT_EQUAL_STRING("r/min", out[0].unit);
}
static void test_decode_signed_values() {
    uint16_t regs[2] = {0xFFFF, 0xFF38};  // -1、-200
    DecodedValue out[4];
    decodeBlock(0x20AD, regs, 2, out, 4);  // 轉矩 0.1 A
    TEST_ASSERT_EQUAL_INT32(-1, out[0].scaled);
    TEST_ASSERT_EQUAL_INT32(-200, out[1].scaled);
    TEST_ASSERT_EQUAL_STRING("A", out[0].unit);
}
static void test_decode_high_low_byte_split() {
    uint16_t regs[1] = {0x0100};  // 高位元組＝左＝1，低位元組＝右＝0
    DecodedValue out[4];
    size_t n = decodeBlock(0x20A2, regs, 1, out, 4);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_STRING("motor_state_L", out[0].name);
    TEST_ASSERT_EQUAL_INT32(1, out[0].scaled);
    TEST_ASSERT_EQUAL_STRING("motor_state_R", out[1].name);
    TEST_ASSERT_EQUAL_INT32(0, out[1].scaled);
}
static void test_decode_temperature_split() {
    uint16_t regs[1] = {0x2A29};  // 左 42 ℃、右 41 ℃
    DecodedValue out[4];
    decodeBlock(0x20A4, regs, 1, out, 4);
    TEST_ASSERT_EQUAL_INT32(42, out[0].scaled);
    TEST_ASSERT_EQUAL_INT32(41, out[1].scaled);
    TEST_ASSERT_EQUAL_STRING("C", out[0].unit);
}
static void test_decode_position_pair_is_tentative() {
    uint16_t regs[4] = {0x0001, 0x0002, 0xFFFF, 0xFFFE};  // 左 0x00010002、右 0xFFFFFFFE
    DecodedValue out[4];
    size_t n = decodeBlock(0x20A7, regs, 4, out, 4);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_STRING("position_L_tentative", out[0].name);
    TEST_ASSERT_EQUAL_INT32(65538, out[0].scaled);
    TEST_ASSERT_EQUAL_STRING("position_R_tentative", out[1].name);
    TEST_ASSERT_EQUAL_INT32(-2, out[1].scaled);
}
static void test_decode_position_needs_both_halves() {
    uint16_t regs[1] = {0x0001};
    DecodedValue out[4];
    TEST_ASSERT_EQUAL_UINT32(0, decodeBlock(0x20A7, regs, 1, out, 4));
}
static void test_decode_config_registers() {
    uint16_t regs[3] = {1000, 4, 2};
    DecodedValue out[4];
    size_t n = decodeBlock(0x2000, regs, 3, out, 4);
    TEST_ASSERT_EQUAL_UINT32(3, n);
    TEST_ASSERT_EQUAL_STRING("comm_offline_ms", out[0].name);
    TEST_ASSERT_EQUAL_INT32(1000, out[0].scaled);
    TEST_ASSERT_EQUAL_STRING("node_id", out[1].name);
    TEST_ASSERT_EQUAL_INT32(4, out[1].scaled);
    TEST_ASSERT_EQUAL_STRING("baud_code", out[2].name);
    TEST_ASSERT_EQUAL_INT32(2, out[2].scaled);
}
static void test_decode_respects_output_capacity() {
    uint16_t regs[2] = {0x0064, 0x0064};
    DecodedValue out[1];
    TEST_ASSERT_EQUAL_UINT32(1, decodeBlock(0x20AB, regs, 2, out, 1));
}
static void test_decode_skips_unknown_address() {
    uint16_t regs[1] = {7};
    DecodedValue out[2];
    TEST_ASSERT_EQUAL_UINT32(0, decodeBlock(0x2003, regs, 1, out, 2));
}

void run_registers_tests() {
    RUN_TEST(test_range_allowed);
    RUN_TEST(test_range_rejected);
    RUN_TEST(test_every_whitelisted_address_has_an_entry);
    RUN_TEST(test_all_registers_in_table_are_in_whitelist);
    RUN_TEST(test_table_names);
    RUN_TEST(test_decode_voltage);
    RUN_TEST(test_decode_velocity_manual_example);
    RUN_TEST(test_decode_signed_values);
    RUN_TEST(test_decode_high_low_byte_split);
    RUN_TEST(test_decode_temperature_split);
    RUN_TEST(test_decode_position_pair_is_tentative);
    RUN_TEST(test_decode_position_needs_both_halves);
    RUN_TEST(test_decode_config_registers);
    RUN_TEST(test_decode_respects_output_capacity);
    RUN_TEST(test_decode_skips_unknown_address);
}
