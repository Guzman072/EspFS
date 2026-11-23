#include <unity.h>
#include <string>
#include <vector>
#include "mock_transport.h"
#include "poller.h"
#include "test_helpers.h"

class LineLog : public ILineSink {
public:
    std::vector<std::string> lines;
    void line(const char* text) override { lines.emplace_back(text); }
    bool has(const char* needle) const {
        for (const auto& l : lines) if (l.find(needle) != std::string::npos) return true;
        return false;
    }
    int indexOf(const char* needle) const {
        for (size_t i = 0; i < lines.size(); ++i) if (lines[i].find(needle) != std::string::npos) return static_cast<int>(i);
        return -1;
    }
};

// 假驅動器：只回應 aliveId，每個暫存器回 0x0100 + 序號；需要時回例外或不回應。
struct FakeDrive {
    int aliveId = 1;
    bool exceptions = false;
    ScriptedReply operator()(const std::vector<uint8_t>& req) const {
        if (req.size() != 8 || req[0] != aliveId) return ScriptedReply{{}, 0};
        if (exceptions) return ScriptedReply{withCrc({req[0], 0x83, 0x02}), 2};
        uint16_t n = static_cast<uint16_t>((req[4] << 8) | req[5]);
        std::vector<uint16_t> regs;
        for (uint16_t i = 0; i < n; ++i) regs.push_back(static_cast<uint16_t>(0x0100 + i));
        return ScriptedReply{okResponse(req[0], regs), 2};
    }
};

static MockTransport* t;
static Master* m;
static LineLog* log_;
static Poller* p;

static void setUpPoller(FakeDrive d, const PollerConfig& cfg = PollerConfig::defaults()) {
    delete p; delete log_; delete m; delete t;
    t = new MockTransport();
    t->responder = d;
    m = new Master(*t);
    log_ = new LineLog();
    p = new Poller(*t, *m, *log_, cfg);
}
static void steps(int n) { for (int i = 0; i < n; ++i) p->step(); }

static void test_boot_prints_header_and_enters_probe() {
    setUpPoller(FakeDrive{});
    p->step();
    TEST_ASSERT_EQUAL(PollState::PROBE, p->state());
    TEST_ASSERT_EQUAL_CHAR('#', log_->lines[0][0]);
    TEST_ASSERT_EQUAL_STRING("S,0,BOOT", log_->lines[1].c_str());
    TEST_ASSERT_EQUAL_STRING("S,0,PROBE", log_->lines[2].c_str());
    TEST_ASSERT_EQUAL_UINT32(0, t->totalWritten);  // BOOT 不送任何東西
}
static void test_probe_id1_found_goes_polling() {
    setUpPoller(FakeDrive{1});
    steps(2);
    TEST_ASSERT_EQUAL(PollState::POLLING, p->state());
    TEST_ASSERT_EQUAL_UINT8(1, p->slaveId());
    const uint8_t first[8] = {0x01, 0x03, 0x20, 0xA0, 0x00, 0x01, 0x8F, 0xE8};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, t->writes[0].data(), 8);
    TEST_ASSERT_TRUE(log_->has("T,"));
    TEST_ASSERT_TRUE(log_->has(",01,03,20A0,1,OK,1,"));
    TEST_ASSERT_TRUE(log_->has("S,"));
    TEST_ASSERT_TRUE(log_->has("POLLING"));
}
static void test_probe_reads_identity_registers() {
    setUpPoller(FakeDrive{1});
    steps(2);
    TEST_ASSERT_TRUE(log_->has(",01,03,2001,2,OK,"));
    TEST_ASSERT_TRUE(log_->has(",01,03,2000,1,OK,"));
    TEST_ASSERT_TRUE(log_->has("node_id"));
}
static void test_probe_falls_back_to_id4() {
    setUpPoller(FakeDrive{4});
    steps(2);
    TEST_ASSERT_EQUAL(PollState::POLLING, p->state());
    TEST_ASSERT_EQUAL_UINT8(4, p->slaveId());
    TEST_ASSERT_TRUE(log_->has(",01,03,20A0,1,TIMEOUT,3,"));
    TEST_ASSERT_TRUE(log_->has(",04,03,20A0,1,OK,1,"));
    TEST_ASSERT_EQUAL_UINT8(4, t->writes.back()[0]);
}
static void test_probe_both_fail_latches_and_stops() {
    setUpPoller(FakeDrive{9});
    steps(2);
    TEST_ASSERT_EQUAL(PollState::LATCHED, p->state());
    TEST_ASSERT_EQUAL(Status::TIMEOUT, p->latchReason());
    TEST_ASSERT_EQUAL_UINT32(6, t->writes.size());  // ID1 ×3、ID4 ×3，沒有試其他 ID
    for (const auto& w : t->writes) TEST_ASSERT_TRUE(w[0] == 1 || w[0] == 4);
    TEST_ASSERT_TRUE(log_->has(",LATCHED"));
    TEST_ASSERT_TRUE(log_->has("# LATCHED reason=TIMEOUT"));
    size_t before = t->writes.size();
    t->now += 5000;
    steps(5);
    TEST_ASSERT_EQUAL_UINT32(before, t->writes.size());
}
static void test_polling_reads_plan_and_decodes() {
    setUpPoller(FakeDrive{1});
    steps(3);  // BOOT、PROBE、第一輪輪詢
    TEST_ASSERT_TRUE(log_->has(",01,03,20A1,1,OK,"));
    TEST_ASSERT_TRUE(log_->has(",01,03,20A2,3,OK,"));
    TEST_ASSERT_TRUE(log_->has(",01,03,20A5,2,OK,"));
    TEST_ASSERT_TRUE(log_->has(",01,03,20A7,4,OK,"));
    TEST_ASSERT_TRUE(log_->has(",01,03,20AB,4,OK,"));
    TEST_ASSERT_TRUE(log_->has("D,"));
    TEST_ASSERT_TRUE(log_->has("dc_voltage,2.56,V"));
    TEST_ASSERT_EQUAL_UINT32(1, p->cycles());
}
static void test_polling_period_is_200ms_and_never_overlaps() {
    setUpPoller(FakeDrive{1});
    steps(3);
    size_t w = t->writes.size();
    steps(5);  // 同一時間點，週期未到
    TEST_ASSERT_EQUAL_UINT32(w, t->writes.size());
    t->now += 250;
    steps(1);
    TEST_ASSERT_EQUAL_UINT32(w + 5, t->writes.size());  // 五個區塊
    TEST_ASSERT_EQUAL_UINT32(2, p->cycles());
}
static void test_only_fc03_and_whitelisted_addresses_are_ever_written() {
    setUpPoller(FakeDrive{1});
    steps(3);
    for (int i = 0; i < 5; ++i) { t->now += 200; steps(1); }
    for (const auto& w : t->writes) {
        TEST_ASSERT_EQUAL_UINT32(8, w.size());
        TEST_ASSERT_EQUAL_HEX8(0x03, w[1]);
        uint16_t a = static_cast<uint16_t>((w[2] << 8) | w[3]);
        TEST_ASSERT_TRUE((a >= 0x2000 && a <= 0x2002) || (a >= 0x20A0 && a <= 0x20AE));
    }
}
static void test_rejected_request_latches_immediately() {
    static const PollBlock bad[] = {{0x20A1, 1}, {0x2003, 1}, {0x20AB, 2}};
    PollerConfig cfg = PollerConfig::defaults();
    cfg.plan = bad;
    cfg.planCount = 3;
    setUpPoller(FakeDrive{1}, cfg);
    steps(3);
    TEST_ASSERT_EQUAL(PollState::LATCHED, p->state());
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, p->latchReason());
    TEST_ASSERT_TRUE(log_->has(",01,03,2003,1,REJECTED_ADDR,0,-,-"));
    TEST_ASSERT_TRUE(log_->has("# LATCHED reason=REJECTED_ADDR"));
    int tIdx = log_->indexOf("REJECTED_ADDR,0,-,-");
    int sIdx = log_->indexOf(",LATCHED");
    TEST_ASSERT_TRUE_MESSAGE(tIdx >= 0 && sIdx > tIdx, "T 行要在 S,LATCHED 之前");
    TEST_ASSERT_FALSE_MESSAGE(log_->has(",20AB,2,"), "LATCHED 後不再送出後面的區塊");
    size_t w = t->writes.size();
    t->now += 1000;
    steps(5);
    TEST_ASSERT_EQUAL_UINT32(w, t->writes.size());
}
static void test_three_exception_transactions_latch() {
    FakeDrive d{1};
    setUpPoller(d);
    steps(2);
    TEST_ASSERT_EQUAL(PollState::POLLING, p->state());
    t->responder = FakeDrive{1, true};  // 之後驅動器一律回例外
    t->now += 200;
    steps(1);
    TEST_ASSERT_EQUAL(PollState::LATCHED, p->state());
    TEST_ASSERT_EQUAL(Status::EXC_02, p->latchReason());
}
static void test_three_timeouts_in_polling_latch() {
    setUpPoller(FakeDrive{1});
    steps(2);
    t->responder = FakeDrive{9};  // 驅動器消失
    t->now += 200;
    steps(1);
    TEST_ASSERT_EQUAL(PollState::LATCHED, p->state());
    TEST_ASSERT_EQUAL(Status::TIMEOUT, p->latchReason());
}
static void test_transport_refusal_latches() {
    setUpPoller(FakeDrive{1});
    t->refuse = true;  // 第三層防線拒絕寫入
    steps(2);
    TEST_ASSERT_EQUAL(PollState::LATCHED, p->state());
    TEST_ASSERT_EQUAL(Status::TX_FAIL, p->latchReason());
    TEST_ASSERT_TRUE(log_->has("TX_FAIL"));
    TEST_ASSERT_EQUAL_UINT32(0, t->totalWritten);
}
static void test_state_names() {
    TEST_ASSERT_EQUAL_STRING("BOOT", pollStateName(PollState::BOOT));
    TEST_ASSERT_EQUAL_STRING("PROBE", pollStateName(PollState::PROBE));
    TEST_ASSERT_EQUAL_STRING("POLLING", pollStateName(PollState::POLLING));
    TEST_ASSERT_EQUAL_STRING("LATCHED", pollStateName(PollState::LATCHED));
}

void run_poller_tests() {
    RUN_TEST(test_boot_prints_header_and_enters_probe);
    RUN_TEST(test_probe_id1_found_goes_polling);
    RUN_TEST(test_probe_reads_identity_registers);
    RUN_TEST(test_probe_falls_back_to_id4);
    RUN_TEST(test_probe_both_fail_latches_and_stops);
    RUN_TEST(test_polling_reads_plan_and_decodes);
    RUN_TEST(test_polling_period_is_200ms_and_never_overlaps);
    RUN_TEST(test_only_fc03_and_whitelisted_addresses_are_ever_written);
    RUN_TEST(test_rejected_request_latches_immediately);
    RUN_TEST(test_three_exception_transactions_latch);
    RUN_TEST(test_three_timeouts_in_polling_latch);
    RUN_TEST(test_transport_refusal_latches);
    RUN_TEST(test_state_names);
}
