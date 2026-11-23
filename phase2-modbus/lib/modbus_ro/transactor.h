// 交易引擎：送出、等待、驗證、重試與 LATCHED。Phase 2 的 Master 與 Phase 3 的 DriveLink 共用。
// 引擎只接受 IFrameGuard 物件；Phase 2 傳唯讀 guard，所以讀取路徑仍然只受唯讀 guard 約束。
#pragma once
#include "guard.h"
#include "transport.h"

constexpr uint32_t kNoRtt = 0xFFFFFFFFu;

// 以下都是設計預設（phase2-modbus/DESIGN.md 第 6 節），手冊沒有規定，需實測調整。
constexpr uint32_t kQuietBeforeTxMs = 2;
constexpr uint32_t kResponseTimeoutMs = 100;
constexpr uint32_t kSilenceEndMs = 2;
constexpr uint32_t kRetryIntervalMs = 20;
constexpr uint8_t kMaxAttempts = 3;
constexpr uint8_t kLatchAfterFailures = 3;

struct TxSpec {
    const uint8_t* req;
    size_t reqLen;
    uint8_t fc;          // 請求的功能碼，用來辨識例外回應（fc | 0x80）
    size_t normalLen;    // 正常回應的長度（讀取 5+2N；FC 0x06 與 0x10 為 8）
    bool discardEcho;    // 讀取與 FC 0x10 為 true；FC 0x06 的正常回應本身就是回音，必須為 false
    uint8_t maxAttempts;
};

struct TxOutcome {
    Status status = Status::OK;
    uint8_t attempts = 0;
    bool sent = false;           // false：沒有任何位元組寫入 transport
    uint32_t rttMs = kNoRtt;
};

// 收到位元組後的解析（讀取：parseResponse；寫入：核對回應）。
class IReplyHandler {
public:
    virtual ~IReplyHandler() = default;
    virtual Status handle(const uint8_t* buf, size_t len) = 0;
};

class Transactor {
public:
    Transactor(ITransport& transport, const IFrameGuard& guard) : t_(transport), guard_(guard) {}

    // 做一筆交易（含重試）。已 LATCHED 時不送出（sent=false，status 為 LATCHED 的原因）。
    // ignoreLatch=true 只給 Phase 3 的緊急停止序列使用：即使已 LATCHED 也會送出。
    void run(const TxSpec& spec, IReplyHandler& handler, TxOutcome& out, bool ignoreLatch = false);

    // 請求根本沒組成（例如參數被拒絕）時，由呼叫端回報結果。
    void record(Status s);

    bool latched() const { return latched_; }
    Status latchReason() const { return latchReason_; }
    uint8_t consecutiveFailures() const { return failures_; }
    ITransport& transport() { return t_; }

private:
    ITransport& t_;
    const IFrameGuard& guard_;
    uint8_t failures_ = 0;
    bool latched_ = false;
    Status latchReason_ = Status::OK;

    Status attemptOnce(const TxSpec& spec, IReplyHandler& handler, uint32_t& rtt, bool& gotBytes);
    void latch(Status reason);
};

bool isRetryableStatus(Status s);
bool isNotSentStatus(Status s);  // REJECTED_*（含 Phase 3 的 REJECTED_REG／VALUE）與 TX_FAIL
