#include <unity.h>
#include <cstring>
#include <vector>
#include "crc16.h"

namespace {
struct Vec { const char* name; std::vector<uint8_t> data; uint16_t crc; };

void expectCrc(const std::vector<uint8_t>& d, uint16_t want) {
    TEST_ASSERT_EQUAL_HEX16(want, crc16(d.data(), d.size()));
}
}  // namespace

static void test_ascii_check_value() {
    const char* s = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x4B37, crc16(reinterpret_cast<const uint8_t*>(s), strlen(s)));
}
static void test_manual_read_request() { expectCrc({0x01, 0x03, 0x20, 0xAB, 0x00, 0x02}, 0x2BBE); }
static void test_manual_read_response() { expectCrc({0x01, 0x03, 0x04, 0x00, 0x64, 0x00, 0x64}, 0x07BA); }
static void test_probe_20a0_id1() { expectCrc({0x01, 0x03, 0x20, 0xA0, 0x00, 0x01}, 0xE88F); }
static void test_probe_20a0_id4() { expectCrc({0x04, 0x03, 0x20, 0xA0, 0x00, 0x01}, 0xBD8F); }
static void test_read_2001() { expectCrc({0x01, 0x03, 0x20, 0x01, 0x00, 0x01}, 0x0ADE); }
// 以下兩個是寫入封包，只用來驗 CRC 與白名單拒絕，不會被送出。
static void test_manual_write_single() { expectCrc({0x01, 0x06, 0x20, 0x88, 0x00, 0x64}, 0xCB03); }
static void test_manual_write_multi() {
    expectCrc({0x01, 0x10, 0x20, 0x30, 0x00, 0x02, 0x04, 0x04, 0x00, 0x00, 0x00}, 0x4A68);
}
static void test_empty_is_initial_value() { TEST_ASSERT_EQUAL_HEX16(0xFFFF, crc16(nullptr, 0)); }
static void test_single_bit_flip_changes_crc() {
    std::vector<uint8_t> d{0x01, 0x03, 0x20, 0xAB, 0x00, 0x02};
    uint16_t base = crc16(d.data(), d.size());
    for (size_t i = 0; i < d.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            d[i] ^= static_cast<uint8_t>(1u << bit);
            TEST_ASSERT_NOT_EQUAL(base, crc16(d.data(), d.size()));
            d[i] ^= static_cast<uint8_t>(1u << bit);
        }
    }
}

void run_crc_tests() {
    RUN_TEST(test_ascii_check_value);
    RUN_TEST(test_manual_read_request);
    RUN_TEST(test_manual_read_response);
    RUN_TEST(test_probe_20a0_id1);
    RUN_TEST(test_probe_20a0_id4);
    RUN_TEST(test_read_2001);
    RUN_TEST(test_manual_write_single);
    RUN_TEST(test_manual_write_multi);
    RUN_TEST(test_empty_is_initial_value);
    RUN_TEST(test_single_bit_flip_changes_crc);
}
