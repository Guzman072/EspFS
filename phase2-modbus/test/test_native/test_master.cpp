#include <unity.h>
#include "master.h"
#include "mock_transport.h"
#include "test_helpers.h"

static MockTransport* t;
static Master* m;
static Result res;

static void setUp_each() {
    delete m;
    delete t;
    t = new MockTransport();
    m = new Master(*t);
    res = Result();
}

static void test_ok_first_attempt() {
    setUp_each();
    t->replies.push_back({okResponse(1, {0x0064, 0x0064}), 4});
    m->readRegisters(1, 0x20AB, 2, res);
    TEST_ASSERT_EQUAL(Status::OK, res.status);
    TEST_ASSERT_EQUAL_UINT8(1, res.attempts);
    TEST_ASSERT_TRUE(res.sent);
    TEST_ASSERT_EQUAL_UINT8(2, res.count);
    TEST_ASSERT_EQUAL_HEX16(0x0064, res.regs[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0064, res.regs[1]);
    TEST_ASSERT_EQUAL_UINT32(4, res.rttMs);
    TEST_ASSERT_EQUAL_UINT32(1, t->writes.size());
    const uint8_t want[8] = {0x01, 0x03, 0x20, 0xAB, 0x00, 0x02, 0xBE, 0x2B};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, t->writes[0].data(), 8);
}
static void test_timeout_after_three_attempts() {
    setUp_each();
    m->readRegisters(1, 0x20A0, 1, res);
    TEST_ASSERT_EQUAL(Status::TIMEOUT, res.status);
    TEST_ASSERT_EQUAL_UINT8(3, res.attempts);
    TEST_ASSERT_EQUAL_UINT32(3, t->writes.size());
    TEST_ASSERT_EQUAL_UINT32(kNoRtt, res.rttMs);
    TEST_ASSERT_TRUE_MESSAGE(t->now >= 340 && t->now <= 370, "最壞情況約 340 ms");
}
static void test_second_attempt_succeeds() {
    setUp_each();
    t->replies.push_back({{}, 0});
    t->replies.push_back({okResponse(1, {0x1234}), 3});
    m->readRegisters(1, 0x20A1, 1, res);
    TEST_ASSERT_EQUAL(Status::OK, res.status);
    TEST_ASSERT_EQUAL_UINT8(2, res.attempts);
    TEST_ASSERT_EQUAL_HEX16(0x1234, res.regs[0]);
}
static void test_crc_error_is_retried() {
    setUp_each();
    auto bad = okResponse(1, {0x0001});
    bad[3] ^= 0x10;
    t->replies.push_back({bad, 2});
    t->replies.push_back({okResponse(1, {0x0001}), 2});
    m->readRegisters(1, 0x20A1, 1, res);
    TEST_ASSERT_EQUAL(Status::OK, res.status);
    TEST_ASSERT_EQUAL_UINT8(2, res.attempts);
}
static void test_short_frame_is_retried() {
    setUp_each();
    t->replies.push_back({{0x01, 0x03}, 2});
    t->replies.push_back({okResponse(1, {0x0001}), 2});
    m->readRegisters(1, 0x20A1, 1, res);
    TEST_ASSERT_EQUAL(Status::OK, res.status);
    TEST_ASSERT_EQUAL_UINT8(2, res.attempts);
}
static void test_exception_not_retried() {
    setUp_each();
    t->replies.push_back({withCrc({0x01, 0x83, 0x02}), 2});
    m->readRegisters(1, 0x20A0, 1, res);
    TEST_ASSERT_EQUAL(Status::EXC_02, res.status);
    TEST_ASSERT_EQUAL_UINT8(1, res.attempts);
    TEST_ASSERT_EQUAL_UINT32(1, t->writes.size());
}
static void test_addr_mismatch_not_retried() {
    setUp_each();
    t->replies.push_back({okResponse(4, {0x0001}), 2});
    m->readRegisters(1, 0x20A1, 1, res);
    TEST_ASSERT_EQUAL(Status::ADDR_MISMATCH, res.status);
    TEST_ASSERT_EQUAL_UINT32(1, t->writes.size());
}
static void test_bad_fc_not_retried() {
    setUp_each();
    t->replies.push_back({withCrc({0x01, 0x04, 0x02, 0x00, 0x01}), 2});
    m->readRegisters(1, 0x20A1, 1, res);
    TEST_ASSERT_EQUAL(Status::BAD_FC, res.status);
    TEST_ASSERT_EQUAL_UINT32(1, t->writes.size());
}
static void test_bad_len_not_retried() {
    setUp_each();
    t->replies.push_back({okResponse(1, {0x0001}), 2});  // 請求 2 個，回 1 個
    m->readRegisters(1, 0x20AB, 2, res);
    TEST_ASSERT_EQUAL(Status::BAD_LEN, res.status);
    TEST_ASSERT_EQUAL_UINT32(1, t->writes.size());
}
struct RejectCase { uint8_t id; uint16_t addr; uint16_t count; Status want; };
static const RejectCase kRejectCases[] = {
    {1, 0x2003, 1, Status::REJECTED_ADDR},
    {1, 0x20AF, 1, Status::REJECTED_ADDR},
    {1, 0x20A0, 17, Status::REJECTED_LEN},
    {1, 0x20A0, 0, Status::REJECTED_LEN},
    {0, 0x20A0, 1, Status::REJECTED_ID},
    {128, 0x20A0, 1, Status::REJECTED_ID},
};

static void test_rejected_requests_never_touch_transport_and_latch_immediately() {
    for (const RejectCase& c : kRejectCases) {
        setUp_each();
        m->readRegisters(c.id, c.addr, c.count, res);
        TEST_ASSERT_EQUAL(c.want, res.status);
        TEST_ASSERT_FALSE(res.sent);
        TEST_ASSERT_EQUAL_UINT8(0, res.attempts);
        TEST_ASSERT_EQUAL_UINT32(0, t->totalWritten);
        TEST_ASSERT_TRUE_MESSAGE(m->latched(), "任何 REJECTED_* 必須立即 LATCHED");
        TEST_ASSERT_EQUAL(c.want, m->latchReason());
    }
}
static void test_latched_by_rejected_sends_nothing_afterwards() {
    setUp_each();
    m->readRegisters(1, 0x2003, 1, res);  // REJECTED_ADDR → LATCHED
    t->replies.push_back({okResponse(1, {0x0001}), 2});
    for (int i = 0; i < 3; ++i) {
        m->readRegisters(1, 0x20A1, 1, res);  // 合法請求也不得送出
        TEST_ASSERT_FALSE(res.sent);
        TEST_ASSERT_EQUAL_UINT8(0, res.attempts);
        TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, res.status);  // 回報原因
    }
    TEST_ASSERT_EQUAL_UINT32(0, t->writes.size());
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, m->latchReason());  // 原因不被覆蓋
}
static void test_rejected_after_successes_still_latches() {
    setUp_each();
    t->replies.push_back({okResponse(1, {1}), 2});
    m->readRegisters(1, 0x20A1, 1, res);
    TEST_ASSERT_EQUAL(Status::OK, res.status);
    TEST_ASSERT_FALSE(m->latched());
    TEST_ASSERT_EQUAL(Status::OK, m->latchReason());
    m->readRegisters(1, 0x20A1, 17, res);
    TEST_ASSERT_TRUE(m->latched());
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, m->latchReason());
}
static void test_failure_latch_records_reason() {
    setUp_each();
    for (int i = 0; i < 3; ++i) m->readRegisters(1, 0x20A0, 1, res);
    TEST_ASSERT_TRUE(m->latched());
    TEST_ASSERT_EQUAL(Status::TIMEOUT, m->latchReason());
    m->readRegisters(1, 0x20A0, 1, res);
    TEST_ASSERT_EQUAL(Status::TIMEOUT, res.status);
}
static void test_transport_refusal_latches_without_retry() {
    setUp_each();
    t->refuse = true;
    m->readRegisters(1, 0x20A0, 1, res);
    TEST_ASSERT_EQUAL(Status::TX_FAIL, res.status);
    TEST_ASSERT_FALSE(res.sent);
    TEST_ASSERT_EQUAL_UINT8(1, res.attempts);  // 只嘗試一次，不重試
    TEST_ASSERT_EQUAL_UINT32(0, t->totalWritten);
    TEST_ASSERT_TRUE(m->latched());
    TEST_ASSERT_EQUAL(Status::TX_FAIL, m->latchReason());
}
static void test_identical_echo_is_discarded() {
    setUp_each();
    t->echo = true;
    t->replies.push_back({okResponse(1, {0x0064}), 3});
    m->readRegisters(1, 0x20A1, 1, res);
    TEST_ASSERT_EQUAL(Status::OK, res.status);
    TEST_ASSERT_EQUAL_UINT8(1, res.attempts);
    TEST_ASSERT_EQUAL_HEX16(0x0064, res.regs[0]);
}
static void test_echo_alone_then_silence_is_timeout() {
    setUp_each();
    t->echo = true;
    m->readRegisters(1, 0x20A1, 1, res);
    TEST_ASSERT_EQUAL(Status::TIMEOUT, res.status);
    TEST_ASSERT_EQUAL_UINT8(3, res.attempts);
}
static void test_near_echo_is_not_discarded() {
    setUp_each();
    // 與請求只差最後一個位元組：不算回音，照一般回應驗證 → CRC_ERR（不是 TIMEOUT）
    auto near = withCrc({0x01, 0x03, 0x20, 0xA1, 0x00, 0x01});
    near[7] ^= 0x01;
    for (int i = 0; i < 3; ++i) t->replies.push_back({near, 2});  // CRC_ERR 會重試，每次都回同樣的資料
    m->readRegisters(1, 0x20A1, 1, res);
    TEST_ASSERT_EQUAL(Status::CRC_ERR, res.status);
    TEST_ASSERT_EQUAL_UINT8(3, res.attempts);
}
static void test_latched_after_three_failed_transactions() {
    setUp_each();
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_FALSE(m->latched());
        m->readRegisters(1, 0x20A0, 1, res);
        TEST_ASSERT_EQUAL(Status::TIMEOUT, res.status);
    }
    TEST_ASSERT_TRUE(m->latched());
    size_t writesBefore = t->writes.size();
    m->readRegisters(1, 0x20A0, 1, res);
    TEST_ASSERT_FALSE(res.sent);
    TEST_ASSERT_EQUAL_UINT8(0, res.attempts);
    TEST_ASSERT_EQUAL_UINT32(writesBefore, t->writes.size());
}
static void test_success_resets_failure_counter() {
    setUp_each();
    m->readRegisters(1, 0x20A0, 1, res);
    m->readRegisters(1, 0x20A0, 1, res);
    t->replies.push_back({okResponse(1, {1}), 2});
    m->readRegisters(1, 0x20A0, 1, res);
    TEST_ASSERT_EQUAL(Status::OK, res.status);
    m->readRegisters(1, 0x20A0, 1, res);
    m->readRegisters(1, 0x20A0, 1, res);
    TEST_ASSERT_FALSE(m->latched());
}
static void test_exception_counts_as_failed_transaction() {
    setUp_each();
    for (int i = 0; i < 3; ++i) {
        t->replies.push_back({withCrc({0x01, 0x83, 0x02}), 2});
        m->readRegisters(1, 0x20A0, 1, res);
    }
    TEST_ASSERT_TRUE(m->latched());
}
static void test_max_count_response_is_read_in_full() {
    setUp_each();
    std::vector<uint16_t> v;
    for (uint16_t i = 0; i < 15; ++i) v.push_back(static_cast<uint16_t>(0x100 + i));
    t->replies.push_back({okResponse(1, v), 5});
    m->readRegisters(1, 0x20A0, 15, res);
    TEST_ASSERT_EQUAL(Status::OK, res.status);
    TEST_ASSERT_EQUAL_UINT8(15, res.count);
    TEST_ASSERT_EQUAL_HEX16(0x10E, res.regs[14]);
}

void run_master_tests() {
    RUN_TEST(test_ok_first_attempt);
    RUN_TEST(test_timeout_after_three_attempts);
    RUN_TEST(test_second_attempt_succeeds);
    RUN_TEST(test_crc_error_is_retried);
    RUN_TEST(test_short_frame_is_retried);
    RUN_TEST(test_exception_not_retried);
    RUN_TEST(test_addr_mismatch_not_retried);
    RUN_TEST(test_bad_fc_not_retried);
    RUN_TEST(test_bad_len_not_retried);
    RUN_TEST(test_rejected_requests_never_touch_transport_and_latch_immediately);
    RUN_TEST(test_latched_by_rejected_sends_nothing_afterwards);
    RUN_TEST(test_rejected_after_successes_still_latches);
    RUN_TEST(test_failure_latch_records_reason);
    RUN_TEST(test_transport_refusal_latches_without_retry);
    RUN_TEST(test_identical_echo_is_discarded);
    RUN_TEST(test_echo_alone_then_silence_is_timeout);
    RUN_TEST(test_near_echo_is_not_discarded);
    RUN_TEST(test_latched_after_three_failed_transactions);
    RUN_TEST(test_success_resets_failure_counter);
    RUN_TEST(test_exception_counts_as_failed_transaction);
    RUN_TEST(test_max_count_response_is_read_in_full);
}
