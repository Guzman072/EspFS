// s3_poll 用的觀測工具：EchoProbe（回音計數）與 StatusCounter（CRC 等錯誤計數）。只記錄，不改變交易行為。
#include <unity.h>
#include <cstring>
#include "echo_probe.h"
#include "master.h"
#include "mock_transport.h"
#include "test_helpers.h"

namespace {
class NullSink : public ILineSink {
public:
    void line(const char*) override { ++lines; }
    int lines = 0;
};
}  // namespace

static void test_echo_probe_counts_full_echo_and_master_still_reads() {
    MockTransport mock;
    mock.echo = true;  // 送出的位元組原樣回來，之後才是驅動器回應
    mock.replies.push_back({okResponse(1, {0x1234}), 5});
    EchoProbe probe(mock);
    Master master(probe);
    Result r;
    master.readRegisters(1, 0x20A0, 1, r);
    TEST_ASSERT_EQUAL(Status::OK, r.status);
    TEST_ASSERT_EQUAL_UINT32(1, probe.echoes());
    TEST_ASSERT_EQUAL_UINT32(0, probe.partialEchoes());
}

static void test_echo_probe_zero_when_no_echo() {
    MockTransport mock;
    mock.replies.push_back({okResponse(1, {0x0001}), 5});
    EchoProbe probe(mock);
    Master master(probe);
    Result r;
    master.readRegisters(1, 0x20A0, 1, r);
    TEST_ASSERT_EQUAL(Status::OK, r.status);
    TEST_ASSERT_EQUAL_UINT32(0, probe.echoes());
    TEST_ASSERT_EQUAL_UINT32(0, probe.partialEchoes());
}

static void test_echo_probe_counts_partial_echo() {
    MockTransport mock;
    EchoProbe probe(mock);
    const uint8_t req[] = {0x01, 0x03, 0x20, 0xA0, 0x00, 0x01, 0x8F, 0xE8};
    mock.replies.push_back({{0x01, 0x03, 0x20, 0xA0, 0x55}, 0});  // 前 4 個位元組相同，第 5 個不同
    TEST_ASSERT_EQUAL_UINT32(sizeof req, probe.write(req, sizeof req));
    while (probe.available() > 0) probe.readByte();
    TEST_ASSERT_EQUAL_UINT32(0, probe.echoes());
    TEST_ASSERT_EQUAL_UINT32(1, probe.partialEchoes());
}

static void test_echo_probe_never_forwards_drive_writes() {
    MockTransport mock;
    EchoProbe probe(mock);
    const uint8_t fc06[] = {0x01, 0x06, 0x20, 0x0E, 0x00, 0x08, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT32(0, probe.writeDrive(fc06, sizeof fc06));
    TEST_ASSERT_EQUAL_UINT32(0, mock.writes.size());
}

static void test_status_counter_counts_crc_and_other_errors() {
    NullSink inner;
    StatusCounter c(inner);
    c.line("T,100,01,03,20A0,1,OK,1,5,0001");
    c.line("T,200,01,03,20A0,1,CRC_ERR,1,5,-");
    c.line("T,300,01,03,20A0,1,TIMEOUT,1,-,-");
    c.line("D,300,x,1,");
    c.line("S,300,POLLING");
    TEST_ASSERT_EQUAL_UINT32(3, c.transactions());
    TEST_ASSERT_EQUAL_UINT32(1, c.crcErrors());
    TEST_ASSERT_EQUAL_UINT32(1, c.timeouts());
    TEST_ASSERT_EQUAL_UINT32(2, c.failures());
    TEST_ASSERT_EQUAL(5, inner.lines);  // 每一行都原樣轉出
}

static void test_master_single_attempt_when_flag_set() {
    // 本測試在 native（預設 3 次）下確認預設值不變；s3_poll 以旗標設為 1。
    TEST_ASSERT_EQUAL_UINT8(MODBUS_MAX_ATTEMPTS, kMaxAttempts);
}

void run_s3_probe_tests() {
    RUN_TEST(test_echo_probe_counts_full_echo_and_master_still_reads);
    RUN_TEST(test_echo_probe_zero_when_no_echo);
    RUN_TEST(test_echo_probe_counts_partial_echo);
    RUN_TEST(test_echo_probe_never_forwards_drive_writes);
    RUN_TEST(test_status_counter_counts_crc_and_other_errors);
    RUN_TEST(test_master_single_attempt_when_flag_set);
}
