#include <unity.h>
#include <cstring>
#include <string>
#include <vector>
#include "loopback.h"
#include "mock_transport.h"

class VecSink : public ILineSink {
public:
    std::vector<std::string> lines;
    void line(const char* text) override { lines.emplace_back(text); }
    int count(const char* prefix) const {
        int n = 0;
        for (const auto& l : lines) if (l.rfind(prefix, 0) == 0) ++n;
        return n;
    }
};

static const uint8_t kReq[8] = {0x01, 0x03, 0x20, 0xA0, 0x00, 0x01, 0x8F, 0xE8};

static void runAll(LoopbackRunner& r, MockTransport& t) {
    for (int guard = 0; guard < 100000 && !r.done(); ++guard) {
        r.step();
        t.delayMs(1);
    }
}

static void test_evaluate_verdicts() {
    uint8_t bad[8];
    memcpy(bad, kReq, 8);
    bad[3] ^= 1;
    uint8_t nine[9] = {0x01, 0x03, 0x20, 0xA0, 0x00, 0x01, 0x8F, 0xE8, 0x00};
    TEST_ASSERT_EQUAL(LoopbackVerdict::PASS, evaluateLoopback(kReq, 8, kReq, 8));
    TEST_ASSERT_EQUAL(LoopbackVerdict::NO_BYTES, evaluateLoopback(kReq, 8, kReq, 0));
    TEST_ASSERT_EQUAL(LoopbackVerdict::SHORT, evaluateLoopback(kReq, 8, kReq, 7));
    TEST_ASSERT_EQUAL(LoopbackVerdict::EXTRA, evaluateLoopback(kReq, 8, nine, 9));
    TEST_ASSERT_EQUAL(LoopbackVerdict::MISMATCH, evaluateLoopback(kReq, 8, bad, 8));
}
static void test_format_l_line() {
    char buf[128];
    formatL(buf, sizeof buf, 200, 1, LoopbackVerdict::PASS, kReq, 8);
    TEST_ASSERT_EQUAL_STRING("L,200,1,PASS,8,010320A000018FE8", buf);
    formatL(buf, sizeof buf, 400, 2, LoopbackVerdict::NO_BYTES, nullptr, 0);
    TEST_ASSERT_EQUAL_STRING("L,400,2,FAIL_NO_BYTES,0,-", buf);
    formatL(buf, sizeof buf, 600, 3, LoopbackVerdict::SHORT, kReq, 3);
    TEST_ASSERT_EQUAL_STRING("L,600,3,FAIL_SHORT,3,010320", buf);
}
static void test_full_loopback_passes_50_times() {
    MockTransport t;
    t.echo = true;  // 模擬 GPIO16 與 GPIO17 短接
    VecSink sink;
    LoopbackRunner r(t, sink);
    runAll(r, t);
    TEST_ASSERT_TRUE(r.done());
    TEST_ASSERT_EQUAL_UINT16(50, r.passCount());
    TEST_ASSERT_EQUAL_UINT16(0, r.failCount());
    TEST_ASSERT_EQUAL_INT(50, sink.count("L,"));
    TEST_ASSERT_EQUAL_UINT32(50, t.writes.size());
    for (const auto& w : t.writes) TEST_ASSERT_EQUAL_UINT8_ARRAY(kReq, w.data(), 8);
    TEST_ASSERT_TRUE_MESSAGE(t.now >= 49u * 200u, "每 200 ms 一次");
    TEST_ASSERT_EQUAL_STRING("# SUMMARY pass=50 fail=0 total=50 result=PASS", sink.lines[sink.lines.size() - 2].c_str());
    TEST_ASSERT_TRUE(sink.lines.back().rfind("S,", 0) == 0);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sink.lines.back().find("DONE"));
}
static void test_no_bytes_is_reported_as_fail() {
    MockTransport t;  // 沒短接：沒有任何回音
    VecSink sink;
    LoopbackRunner r(t, sink, 5, 200);
    runAll(r, t);
    TEST_ASSERT_EQUAL_UINT16(0, r.passCount());
    TEST_ASSERT_EQUAL_UINT16(5, r.failCount());
    TEST_ASSERT_EQUAL_INT(5, sink.count("L,"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sink.lines[2].find("FAIL_NO_BYTES"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sink.lines[sink.lines.size() - 2].find("result=FAIL"));
}
static void test_short_extra_and_mismatch_are_fail() {
    for (int mode = 0; mode < 3; ++mode) {
        MockTransport t;
        t.responder = [mode](const std::vector<uint8_t>& req) {
            std::vector<uint8_t> b = req;
            if (mode == 0) b.resize(5);
            if (mode == 1) b.push_back(0x55);
            if (mode == 2) b[4] ^= 0x01;
            return ScriptedReply{b, 1};
        };
        VecSink sink;
        LoopbackRunner r(t, sink, 3, 200);
        runAll(r, t);
        TEST_ASSERT_EQUAL_UINT16(3, r.failCount());
        const char* want[3] = {"FAIL_SHORT", "FAIL_EXTRA", "FAIL_MISMATCH"};
        TEST_ASSERT_NOT_EQUAL(std::string::npos, sink.lines[2].find(want[mode]));
    }
}
static void test_refused_write_is_not_sent_fail() {
    MockTransport t;
    t.refuse = true;
    VecSink sink;
    LoopbackRunner r(t, sink, 2, 200);
    runAll(r, t);
    TEST_ASSERT_EQUAL_UINT16(2, r.failCount());
    TEST_ASSERT_EQUAL_UINT32(0, t.totalWritten);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sink.lines[2].find("FAIL_NOT_SENT"));
}
static void test_header_and_state_lines() {
    MockTransport t;
    t.echo = true;
    VecSink sink;
    LoopbackRunner r(t, sink, 1, 200);
    runAll(r, t);
    TEST_ASSERT_EQUAL_CHAR('#', sink.lines[0][0]);
    TEST_ASSERT_EQUAL_STRING("S,0,LOOPBACK", sink.lines[1].c_str());
}

void run_loopback_tests() {
    RUN_TEST(test_evaluate_verdicts);
    RUN_TEST(test_format_l_line);
    RUN_TEST(test_full_loopback_passes_50_times);
    RUN_TEST(test_no_bytes_is_reported_as_fail);
    RUN_TEST(test_short_extra_and_mismatch_are_fail);
    RUN_TEST(test_refused_write_is_not_sent_fail);
    RUN_TEST(test_header_and_state_lines);
}
