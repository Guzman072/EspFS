#include <unity.h>
#include "frame.h"
#include "guard.h"
#include "report.h"
#include "mock_transport.h"
#include "test_helpers.h"

// 對功能碼 0x00–0xFF 逐一測試：Slave ID、位址、數量、CRC 全部合法，只有功能碼不同。
static void test_whitelist_all_256_function_codes() {
    int passed = 0;
    for (int fc = 0; fc <= 0xFF; ++fc) {
        auto f = withCrc({0x01, static_cast<uint8_t>(fc), 0x20, 0xA0, 0x00, 0x01});
        Status s = checkRequest(f.data(), f.size());
        if (fc == 0x03) {
            TEST_ASSERT_EQUAL(Status::OK, s);
            ++passed;
        } else {
            TEST_ASSERT_EQUAL_MESSAGE(Status::REJECTED_FC, s, "非 0x03 必須被拒絕");
        }
    }
    TEST_ASSERT_EQUAL_INT(1, passed);
}

// 同樣 256 個功能碼，經過 guardedWrite 送進 mock transport：只有 0x03 寫入 8 位元組。
static void test_guarded_write_all_256_function_codes() {
    for (int fc = 0; fc <= 0xFF; ++fc) {
        MockTransport t;
        auto f = withCrc({0x01, static_cast<uint8_t>(fc), 0x20, 0xA0, 0x00, 0x01});
        Status s = guardedWrite(t, f.data(), f.size());
        if (fc == 0x03) {
            TEST_ASSERT_EQUAL(Status::OK, s);
            TEST_ASSERT_EQUAL_UINT32(8, t.totalWritten);
        } else {
            TEST_ASSERT_EQUAL(Status::REJECTED_FC, s);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, t.totalWritten, "被拒絕的封包不得寫入 transport");
        }
    }
}

static void test_write_packets_from_manual_are_rejected() {
    MockTransport t;
    auto w06 = withCrc({0x01, 0x06, 0x20, 0x88, 0x00, 0x64});
    auto w10 = withCrc({0x01, 0x10, 0x20, 0x30, 0x00, 0x02, 0x04, 0x04, 0x00, 0x00, 0x00});
    TEST_ASSERT_EQUAL_HEX8(0x03, w06[6]);  // 確認 CRC 與手冊一致（03 CB）
    TEST_ASSERT_EQUAL_HEX8(0xCB, w06[7]);
    TEST_ASSERT_EQUAL(Status::REJECTED_FC, guardedWrite(t, w06.data(), w06.size()));
    TEST_ASSERT_EQUAL(Status::REJECTED_FC, guardedWrite(t, w10.data(), w10.size()));
    TEST_ASSERT_EQUAL_UINT32(0, t.totalWritten);
}

static void test_guard_rejects_bad_length() {
    auto f = withCrc({0x01, 0x03, 0x20, 0xA0, 0x00, 0x01});
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, checkRequest(f.data(), 7));
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, checkRequest(f.data(), 0));
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, checkRequest(nullptr, 0));
    f.push_back(0x00);
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, checkRequest(f.data(), 9));
}
static void test_guard_rejects_bad_id() {
    auto f0 = withCrc({0x00, 0x03, 0x20, 0xA0, 0x00, 0x01});
    auto f128 = withCrc({0x80, 0x03, 0x20, 0xA0, 0x00, 0x01});
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, checkRequest(f0.data(), 8));
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, checkRequest(f128.data(), 8));
}
static void test_guard_rejects_bad_address_and_count() {
    auto a = withCrc({0x01, 0x03, 0x20, 0x03, 0x00, 0x01});
    auto b = withCrc({0x01, 0x03, 0x20, 0xA0, 0x00, 0x10});
    auto d = withCrc({0x01, 0x03, 0x20, 0xAF, 0x00, 0x01});
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, checkRequest(a.data(), 8));
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, checkRequest(b.data(), 8));
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, checkRequest(d.data(), 8));
}
static void test_guard_rejects_bad_count() {
    auto zero = withCrc({0x01, 0x03, 0x20, 0xA0, 0x00, 0x00});
    auto big = withCrc({0x01, 0x03, 0x20, 0xA0, 0x00, 0x11});
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, checkRequest(zero.data(), 8));
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, checkRequest(big.data(), 8));
}
static void test_guard_rejects_bad_crc() {
    auto f = withCrc({0x01, 0x03, 0x20, 0xA0, 0x00, 0x01});
    f[7] ^= 0xFF;
    MockTransport t;
    TEST_ASSERT_EQUAL(Status::REJECTED_CRC, checkRequest(f.data(), 8));
    TEST_ASSERT_EQUAL(Status::REJECTED_CRC, guardedWrite(t, f.data(), 8));
    TEST_ASSERT_EQUAL_UINT32(0, t.totalWritten);
}
static void test_guard_accepts_everything_the_builder_makes() {
    const uint16_t starts[] = {0x2000, 0x2001, 0x2002, 0x20A0, 0x20A5, 0x20AE};
    for (uint16_t s : starts) {
        Request r;
        if (buildReadRegisters(1, s, 1, r) == Status::OK) {
            TEST_ASSERT_EQUAL(Status::OK, checkRequest(r.bytes, 8));
        } else {
            TEST_FAIL_MESSAGE("builder 應接受單一暫存器");
        }
    }
}

static void test_third_layer_fc_check() {
    const uint8_t ok[2] = {0x01, 0x03};
    TEST_ASSERT_TRUE(isReadFunctionFrame(ok, 2));
    TEST_ASSERT_FALSE(isReadFunctionFrame(ok, 1));
    TEST_ASSERT_FALSE(isReadFunctionFrame(nullptr, 8));
    for (int fc = 0; fc <= 0xFF; ++fc) {
        const uint8_t f[8] = {0x01, static_cast<uint8_t>(fc), 0, 0, 0, 0, 0, 0};
        TEST_ASSERT_EQUAL(fc == 0x03, isReadFunctionFrame(f, 8));
    }
}
static void test_guarded_write_reports_transport_refusal() {
    MockTransport t;
    t.refuse = true;  // 模擬 transport 第三層拒絕：回 0、不寫入
    auto f = withCrc({0x01, 0x03, 0x20, 0xA0, 0x00, 0x01});
    TEST_ASSERT_EQUAL(Status::TX_FAIL, guardedWrite(t, f.data(), f.size()));
    TEST_ASSERT_EQUAL_UINT32(0, t.totalWritten);
}
static void test_guarded_write_reports_incomplete_write_as_tx_fail() {
    MockTransport t;
    t.shortWrite = true;  // 只寫了 len-1 個位元組
    auto f = withCrc({0x01, 0x03, 0x20, 0xA0, 0x00, 0x01});
    TEST_ASSERT_EQUAL(Status::TX_FAIL, guardedWrite(t, f.data(), f.size()));
}
static void test_tx_fail_is_distinct_from_rejected_fc() {
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(Status::REJECTED_FC), static_cast<int>(Status::TX_FAIL));
    TEST_ASSERT_EQUAL_STRING("TX_FAIL", statusName(Status::TX_FAIL));
}

void run_guard_tests() {
    RUN_TEST(test_third_layer_fc_check);
    RUN_TEST(test_guarded_write_reports_transport_refusal);
    RUN_TEST(test_guarded_write_reports_incomplete_write_as_tx_fail);
    RUN_TEST(test_tx_fail_is_distinct_from_rejected_fc);
    RUN_TEST(test_whitelist_all_256_function_codes);
    RUN_TEST(test_guarded_write_all_256_function_codes);
    RUN_TEST(test_write_packets_from_manual_are_rejected);
    RUN_TEST(test_guard_rejects_bad_length);
    RUN_TEST(test_guard_rejects_bad_id);
    RUN_TEST(test_guard_rejects_bad_address_and_count);
    RUN_TEST(test_guard_rejects_bad_count);
    RUN_TEST(test_guard_rejects_bad_crc);
    RUN_TEST(test_guard_accepts_everything_the_builder_makes);
}
