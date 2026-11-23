#include <unity.h>
#include <cstring>
#include "report.h"
#include "mock_transport.h"

static char buf[256];

static TLine baseLine() {
    TLine l{};
    l.ms = 12345;
    l.slave = 1;
    l.fc = 0x03;
    l.addr = 0x20AB;
    l.count = 2;
    l.status = Status::OK;
    l.attempts = 1;
    l.rttMs = 4;
    l.regCount = 2;
    l.regs[0] = 0x0064;
    l.regs[1] = 0x0064;
    return l;
}

static void test_t_line_ok() {
    formatT(buf, sizeof buf, baseLine());
    TEST_ASSERT_EQUAL_STRING("T,12345,01,03,20AB,2,OK,1,4,20AB=0064;20AC=0064", buf);
}
static void test_t_line_timeout() {
    TLine l = baseLine();
    l.ms = 12600; l.addr = 0x20A0; l.count = 1; l.status = Status::TIMEOUT;
    l.attempts = 3; l.rttMs = kNoRtt; l.regCount = 0;
    formatT(buf, sizeof buf, l);
    TEST_ASSERT_EQUAL_STRING("T,12600,01,03,20A0,1,TIMEOUT,3,-,-", buf);
}
static void test_t_line_rejected_fc() {
    TLine l = baseLine();
    l.ms = 12700; l.fc = 0x04; l.addr = 0x20A0; l.count = 1;
    l.status = Status::REJECTED_FC; l.attempts = 0; l.rttMs = kNoRtt; l.regCount = 0;
    formatT(buf, sizeof buf, l);
    TEST_ASSERT_EQUAL_STRING("T,12700,01,04,20A0,1,REJECTED_FC,0,-,-", buf);
}
static void test_t_line_hex_is_uppercase_and_padded() {
    TLine l = baseLine();
    l.slave = 4; l.addr = 0x2001; l.count = 1; l.regCount = 1; l.regs[0] = 0x000A;
    formatT(buf, sizeof buf, l);
    TEST_ASSERT_EQUAL_STRING("T,12345,04,03,2001,1,OK,1,4,2001=000A", buf);
}
static void test_status_names() {
    const char* names[] = {"OK", "TIMEOUT", "SHORT_FRAME", "CRC_ERR", "ADDR_MISMATCH", "BAD_FC",
                           "BAD_LEN", "EXC_01", "EXC_02", "EXC_03", "EXC_UNKNOWN", "REJECTED_FC",
                           "REJECTED_ADDR", "REJECTED_LEN", "REJECTED_ID", "REJECTED_CRC", "TX_FAIL"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        TEST_ASSERT_EQUAL_STRING(names[i], statusName(static_cast<Status>(i)));
}
static void test_t_line_never_overflows_small_buffer() {
    char small[16];
    memset(small, 'X', sizeof small);
    size_t n = formatT(small, sizeof small, baseLine());
    TEST_ASSERT_TRUE(n < sizeof small);
    TEST_ASSERT_EQUAL_CHAR('\0', small[n]);
}
static void test_d_line_velocity() {
    DecodedValue v{};
    uint16_t regs[1] = {100};
    decodeBlock(0x20AB, regs, 1, &v, 1);
    formatD(buf, sizeof buf, 12345, v);
    TEST_ASSERT_EQUAL_STRING("D,12345,actual_velocity_L,10.0,r/min", buf);
}
static void test_d_line_voltage_two_decimals() {
    DecodedValue v{};
    uint16_t regs[1] = {4805};
    decodeBlock(0x20A1, regs, 1, &v, 1);
    formatD(buf, sizeof buf, 1, v);
    TEST_ASSERT_EQUAL_STRING("D,1,dc_voltage,48.05,V", buf);
}
static void test_d_line_negative_values() {
    DecodedValue v{};
    uint16_t regs[1] = {0xFFFF};  // -1 → -0.1 r/min
    decodeBlock(0x20AB, regs, 1, &v, 1);
    formatD(buf, sizeof buf, 2, v);
    TEST_ASSERT_EQUAL_STRING("D,2,actual_velocity_L,-0.1,r/min", buf);
    uint16_t regs2[1] = {0xFF9C};  // -100 → -10.0
    decodeBlock(0x20AB, regs2, 1, &v, 1);
    formatD(buf, sizeof buf, 2, v);
    TEST_ASSERT_EQUAL_STRING("D,2,actual_velocity_L,-10.0,r/min", buf);
}
static void test_d_line_integer_value_has_no_decimal_point() {
    DecodedValue v{};
    uint16_t regs[1] = {0x2A29};
    decodeBlock(0x20A4, regs, 1, &v, 1);
    formatD(buf, sizeof buf, 3, v);
    TEST_ASSERT_EQUAL_STRING("D,3,motor_temp_L,42,C", buf);
}
static void test_d_line_position_is_marked_tentative() {
    DecodedValue v{};
    uint16_t regs[2] = {0x0001, 0x0002};
    decodeBlock(0x20A7, regs, 2, &v, 1);
    formatD(buf, sizeof buf, 4, v);
    TEST_ASSERT_EQUAL_STRING("D,4,position_L_tentative,65538,counts", buf);
}
static void test_s_line() {
    formatS(buf, sizeof buf, 99, "LATCHED");
    TEST_ASSERT_EQUAL_STRING("S,99,LATCHED", buf);
}
static void test_latched_rejected_outputs_t_line_then_s_line() {
    MockTransport t;
    Master m(t);
    Result r;
    m.readRegisters(1, 0x2003, 1, r);  // REJECTED_ADDR → 立即 LATCHED
    TEST_ASSERT_TRUE(m.latched());
    formatT(buf, sizeof buf, makeTLine(500, 1, 0x03, 0x2003, 1, r));
    TEST_ASSERT_EQUAL_STRING("T,500,01,03,2003,1,REJECTED_ADDR,0,-,-", buf);
    formatS(buf, sizeof buf, 500, "LATCHED");
    TEST_ASSERT_EQUAL_STRING("S,500,LATCHED", buf);
}
static void test_make_t_line_copies_ok_values() {
    Result r;
    r.status = Status::OK; r.attempts = 2; r.sent = true; r.count = 2; r.rttMs = 7;
    r.regs[0] = 0x0064; r.regs[1] = 0x0064;
    formatT(buf, sizeof buf, makeTLine(9, 1, 0x03, 0x20AB, 2, r));
    TEST_ASSERT_EQUAL_STRING("T,9,01,03,20AB,2,OK,2,7,20AB=0064;20AC=0064", buf);
}
static void test_header_is_comment() {
    TEST_ASSERT_EQUAL_CHAR('#', headerLine()[0]);
}

void run_report_tests() {
    RUN_TEST(test_t_line_ok);
    RUN_TEST(test_t_line_timeout);
    RUN_TEST(test_t_line_rejected_fc);
    RUN_TEST(test_t_line_hex_is_uppercase_and_padded);
    RUN_TEST(test_status_names);
    RUN_TEST(test_t_line_never_overflows_small_buffer);
    RUN_TEST(test_d_line_velocity);
    RUN_TEST(test_d_line_voltage_two_decimals);
    RUN_TEST(test_d_line_negative_values);
    RUN_TEST(test_d_line_integer_value_has_no_decimal_point);
    RUN_TEST(test_d_line_position_is_marked_tentative);
    RUN_TEST(test_s_line);
    RUN_TEST(test_latched_rejected_outputs_t_line_then_s_line);
    RUN_TEST(test_make_t_line_copies_ok_values);
    RUN_TEST(test_header_is_comment);
}
