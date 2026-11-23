#include <unity.h>
#include "frame.h"
#include "test_helpers.h"

static Request req;

static void test_build_manual_example() {
    TEST_ASSERT_EQUAL(Status::OK, buildReadRegisters(1, 0x20AB, 2, req));
    const uint8_t want[8] = {0x01, 0x03, 0x20, 0xAB, 0x00, 0x02, 0xBE, 0x2B};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, req.bytes, 8);
}
static void test_build_probe_frames() {
    const uint8_t id1[8] = {0x01, 0x03, 0x20, 0xA0, 0x00, 0x01, 0x8F, 0xE8};
    const uint8_t id4[8] = {0x04, 0x03, 0x20, 0xA0, 0x00, 0x01, 0x8F, 0xBD};
    const uint8_t r2001[8] = {0x01, 0x03, 0x20, 0x01, 0x00, 0x01, 0xDE, 0x0A};
    TEST_ASSERT_EQUAL(Status::OK, buildReadRegisters(1, 0x20A0, 1, req));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(id1, req.bytes, 8);
    TEST_ASSERT_EQUAL(Status::OK, buildReadRegisters(4, 0x20A0, 1, req));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(id4, req.bytes, 8);
    TEST_ASSERT_EQUAL(Status::OK, buildReadRegisters(1, 0x2001, 1, req));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(r2001, req.bytes, 8);
}
static void test_build_always_fc03() {
    TEST_ASSERT_EQUAL(Status::OK, buildReadRegisters(127, 0x2000, 3, req));
    TEST_ASSERT_EQUAL_HEX8(0x03, req.bytes[1]);
}
static void test_build_rejects_bad_count() {
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, buildReadRegisters(1, 0x20A0, 0, req));
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, buildReadRegisters(1, 0x20A0, 17, req));
}
static void test_build_rejects_bad_address() {
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, buildReadRegisters(1, 0x2003, 1, req));
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, buildReadRegisters(1, 0x20AF, 1, req));
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, buildReadRegisters(1, 0x2002, 2, req));
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, buildReadRegisters(1, 0x20AE, 2, req));
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, buildReadRegisters(1, 0x20A0, 16, req));
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, buildReadRegisters(1, 0x0000, 1, req));
}
static void test_build_rejects_bad_slave_id() {
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, buildReadRegisters(0, 0x20A0, 1, req));
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, buildReadRegisters(128, 0x20A0, 1, req));
}
static void test_build_accepts_full_ranges() {
    TEST_ASSERT_EQUAL(Status::OK, buildReadRegisters(1, 0x2000, 3, req));
    TEST_ASSERT_EQUAL(Status::OK, buildReadRegisters(1, 0x20A0, 15, req));
}

static Response resp;

static void test_parse_manual_response() {
    auto b = okResponse(1, {0x0064, 0x0064});
    const uint8_t want[9] = {0x01, 0x03, 0x04, 0x00, 0x64, 0x00, 0x64, 0xBA, 0x07};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, b.data(), 9);
    TEST_ASSERT_EQUAL(Status::OK, parseResponse(b.data(), b.size(), 1, 2, resp));
    TEST_ASSERT_EQUAL_UINT8(2, resp.count);
    TEST_ASSERT_EQUAL_HEX16(0x0064, resp.regs[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0064, resp.regs[1]);
}
static void test_parse_exceptions() {
    auto e1 = withCrc({0x01, 0x83, 0x01});
    auto e2 = withCrc({0x01, 0x83, 0x02});
    auto e3 = withCrc({0x01, 0x83, 0x03});
    auto e9 = withCrc({0x01, 0x83, 0x09});
    TEST_ASSERT_EQUAL(Status::EXC_01, parseResponse(e1.data(), e1.size(), 1, 1, resp));
    TEST_ASSERT_EQUAL(Status::EXC_02, parseResponse(e2.data(), e2.size(), 1, 1, resp));
    TEST_ASSERT_EQUAL(Status::EXC_03, parseResponse(e3.data(), e3.size(), 1, 1, resp));
    TEST_ASSERT_EQUAL(Status::EXC_UNKNOWN, parseResponse(e9.data(), e9.size(), 1, 1, resp));
}
static void test_parse_short_frame() {
    const uint8_t b[4] = {0x01, 0x03, 0x02, 0x00};
    TEST_ASSERT_EQUAL(Status::SHORT_FRAME, parseResponse(b, 4, 1, 1, resp));
    TEST_ASSERT_EQUAL(Status::SHORT_FRAME, parseResponse(b, 0, 1, 1, resp));
}
static void test_parse_crc_error() {
    auto b = okResponse(1, {0x0064});
    b[3] ^= 0x01;
    TEST_ASSERT_EQUAL(Status::CRC_ERR, parseResponse(b.data(), b.size(), 1, 1, resp));
}
static void test_parse_truncated_long_frame_is_crc_error() {
    auto b = okResponse(1, {0x0064, 0x0064});
    TEST_ASSERT_EQUAL(Status::CRC_ERR, parseResponse(b.data(), b.size() - 1, 1, 2, resp));
}
static void test_parse_slave_mismatch() {
    auto b = okResponse(4, {0x0064});
    TEST_ASSERT_EQUAL(Status::ADDR_MISMATCH, parseResponse(b.data(), b.size(), 1, 1, resp));
}
static void test_parse_bad_function_code() {
    auto b = withCrc({0x01, 0x04, 0x02, 0x00, 0x64});
    TEST_ASSERT_EQUAL(Status::BAD_FC, parseResponse(b.data(), b.size(), 1, 1, resp));
    auto w = withCrc({0x01, 0x06, 0x20, 0x88, 0x00, 0x64});
    TEST_ASSERT_EQUAL(Status::BAD_FC, parseResponse(w.data(), w.size(), 1, 1, resp));
}
static void test_parse_bad_length() {
    auto wrongCount = withCrc({0x01, 0x03, 0x04, 0x00, 0x64});  // 位元組數 4，但只有 2 個資料位元組
    TEST_ASSERT_EQUAL(Status::BAD_LEN, parseResponse(wrongCount.data(), wrongCount.size(), 1, 2, resp));
    auto okCount = okResponse(1, {0x0064});  // 位元組數與長度一致，但請求要 2 個
    TEST_ASSERT_EQUAL(Status::BAD_LEN, parseResponse(okCount.data(), okCount.size(), 1, 2, resp));
    auto longExc = withCrc({0x01, 0x83, 0x02, 0x00});  // 例外回應多一個位元組
    TEST_ASSERT_EQUAL(Status::BAD_LEN, parseResponse(longExc.data(), longExc.size(), 1, 1, resp));
}
static void test_parse_check_order_crc_before_slave() {
    auto b = okResponse(4, {0x0064});
    b[3] ^= 0x01;
    TEST_ASSERT_EQUAL(Status::CRC_ERR, parseResponse(b.data(), b.size(), 1, 1, resp));
}

void run_frame_tests() {
    RUN_TEST(test_build_manual_example);
    RUN_TEST(test_build_probe_frames);
    RUN_TEST(test_build_always_fc03);
    RUN_TEST(test_build_rejects_bad_count);
    RUN_TEST(test_build_rejects_bad_address);
    RUN_TEST(test_build_rejects_bad_slave_id);
    RUN_TEST(test_build_accepts_full_ranges);
    RUN_TEST(test_parse_manual_response);
    RUN_TEST(test_parse_exceptions);
    RUN_TEST(test_parse_short_frame);
    RUN_TEST(test_parse_crc_error);
    RUN_TEST(test_parse_truncated_long_frame_is_crc_error);
    RUN_TEST(test_parse_slave_mismatch);
    RUN_TEST(test_parse_bad_function_code);
    RUN_TEST(test_parse_bad_length);
    RUN_TEST(test_parse_check_order_crc_before_slave);
}
