#include "transactor.h"
#include <cstring>

namespace {
constexpr size_t kRxBufSize = 64;  // 最長回應 5+2×16 = 37 位元組，另留回音空間

// 依已收到的位元組推算完整回應長度；還無法判斷回 0。
size_t expectedLength(const TxSpec& spec, const uint8_t* b, size_t len) {
    if (len < 2) return 0;
    if (b[1] == static_cast<uint8_t>(spec.fc | 0x80)) return 5;
    if (b[1] == spec.fc) return spec.normalLen;
    return 0;
}
}  // namespace

bool isRetryableStatus(Status s) {
    return s == Status::TIMEOUT || s == Status::CRC_ERR || s == Status::SHORT_FRAME;
}

bool isNotSentStatus(Status s) {
    return s == Status::REJECTED_FC || s == Status::REJECTED_ADDR || s == Status::REJECTED_LEN ||
           s == Status::REJECTED_ID || s == Status::REJECTED_CRC || s == Status::REJECTED_REG ||
           s == Status::REJECTED_VALUE || s == Status::TX_FAIL;
}

Status Transactor::attemptOnce(const TxSpec& spec, IReplyHandler& handler, uint32_t& rtt, bool& gotBytes) {
    uint8_t buf[kRxBufSize];
    size_t len = 0;
    gotBytes = false;
    rtt = kNoRtt;

    t_.delayMs(kQuietBeforeTxMs);
    t_.clearRx();
    Status g = guardedWrite(t_, guard_, spec.req, spec.reqLen);
    if (g != Status::OK) return g;
    t_.flushTx();

    const uint32_t start = t_.nowMs();
    uint32_t lastByteAt = start;
    for (;;) {
        while (t_.available() > 0) {
            int b = t_.readByte();
            if (b < 0) break;
            lastByteAt = t_.nowMs();
            if (len < kRxBufSize) buf[len++] = static_cast<uint8_t>(b);
            // 回音：收到的位元組與剛送出的請求逐位元組完全相同才丟棄（FC 0x06 的正常回應就是回音，不丟）
            if (spec.discardEcho && len == spec.reqLen && std::memcmp(buf, spec.req, spec.reqLen) == 0) len = 0;
        }
        const size_t want = expectedLength(spec, buf, len);
        if (want > 0 && len >= want) break;
        const uint32_t now = t_.nowMs();
        if (now - start >= kResponseTimeoutMs) break;
        if (len > 0 && now - lastByteAt >= kSilenceEndMs) break;
        t_.delayMs(1);
    }

    if (len == 0) return Status::TIMEOUT;
    gotBytes = true;
    rtt = t_.nowMs() - start;
    return handler.handle(buf, len);
}

void Transactor::latch(Status reason) {
    if (latched_) return;  // 保留第一個原因
    latched_ = true;
    latchReason_ = reason;
}

void Transactor::record(Status s) {
    if (isNotSentStatus(s)) {  // 程式端的錯誤請求或傳輸層失敗：立即 LATCHED，記錄原因
        latch(s);
        return;
    }
    if (s == Status::OK) {
        failures_ = 0;
        return;
    }
    if (failures_ < 255) ++failures_;
    if (failures_ >= kLatchAfterFailures) latch(s);
}

void Transactor::run(const TxSpec& spec, IReplyHandler& handler, TxOutcome& out, bool ignoreLatch) {
    out = TxOutcome();
    if (latched_ && !ignoreLatch) {
        out.status = latchReason_;
        return;
    }
    Status s = Status::TIMEOUT;
    const uint8_t maxAttempts = spec.maxAttempts == 0 ? 1 : spec.maxAttempts;
    for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt) {
        uint32_t rtt;
        bool gotBytes;
        s = attemptOnce(spec, handler, rtt, gotBytes);
        out.attempts = attempt;
        out.sent = !isNotSentStatus(s);
        out.rttMs = gotBytes ? rtt : kNoRtt;
        if (s == Status::OK || !isRetryableStatus(s) || attempt == maxAttempts) break;
        t_.delayMs(kRetryIntervalMs);
    }
    out.status = s;
    record(s);
}
