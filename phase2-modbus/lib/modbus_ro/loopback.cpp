#include "loopback.h"
#include <cstdio>
#include <cstring>
#include "master.h"  // 時序常數（靜默、回應時限）

namespace {
constexpr size_t kRxMax = 64;
constexpr uint16_t kProbeAddr = 0x20A0;
constexpr uint8_t kLoopbackSlave = 1;
}  // namespace

LoopbackVerdict evaluateLoopback(const uint8_t* tx, size_t txLen, const uint8_t* rx, size_t rxLen) {
    if (rxLen == 0) return LoopbackVerdict::NO_BYTES;
    if (rxLen < txLen) return LoopbackVerdict::SHORT;
    if (rxLen > txLen) return LoopbackVerdict::EXTRA;
    return std::memcmp(tx, rx, txLen) == 0 ? LoopbackVerdict::PASS : LoopbackVerdict::MISMATCH;
}

const char* verdictName(LoopbackVerdict v) {
    switch (v) {
        case LoopbackVerdict::PASS: return "PASS";
        case LoopbackVerdict::NO_BYTES: return "FAIL_NO_BYTES";
        case LoopbackVerdict::SHORT: return "FAIL_SHORT";
        case LoopbackVerdict::EXTRA: return "FAIL_EXTRA";
        case LoopbackVerdict::MISMATCH: return "FAIL_MISMATCH";
        case LoopbackVerdict::NOT_SENT: return "FAIL_NOT_SENT";
    }
    return "FAIL_UNKNOWN";
}

size_t formatL(char* buf, size_t size, uint32_t ms, uint16_t index, LoopbackVerdict v, const uint8_t* rx, size_t rxLen) {
    if (size == 0) return 0;
    int n = std::snprintf(buf, size, "L,%u,%u,%s,%u,", static_cast<unsigned>(ms), static_cast<unsigned>(index),
                          verdictName(v), static_cast<unsigned>(rxLen));
    size_t len = (n < 0) ? 0 : (static_cast<size_t>(n) >= size ? size - 1 : static_cast<size_t>(n));
    if (rxLen == 0 || rx == nullptr) {
        if (len + 1 < size) buf[len++] = '-';
    } else {
        static const char kHex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < rxLen && len + 2 < size; ++i) {
            buf[len++] = kHex[rx[i] >> 4];
            buf[len++] = kHex[rx[i] & 0x0F];
        }
    }
    buf[len] = '\0';
    return len;
}

void LoopbackRunner::step() {
    if (done_) return;
    const uint32_t now = t_.nowMs();
    if (!started_) {
        started_ = true;
        out_.line("# A5 loopback: GPIO16/GPIO17 shorted, module removed. L,ms,index,verdict,rx_len,rx_hex");
        char s[32];
        std::snprintf(s, sizeof s, "S,%u,LOOPBACK", static_cast<unsigned>(now));
        out_.line(s);
    }
    if (index_ > 0 && now - lastStart_ < period_) return;
    runOnce(now);
    if (index_ >= total_) finish();
}

void LoopbackRunner::runOnce(uint32_t now) {
    lastStart_ = now;
    ++index_;
    uint8_t rx[kRxMax];
    size_t rxLen = 0;
    LoopbackVerdict v = LoopbackVerdict::NOT_SENT;

    Request rq;
    if (buildReadRegisters(kLoopbackSlave, kProbeAddr, 1, rq) == Status::OK) {
        t_.delayMs(kQuietBeforeTxMs);
        t_.clearRx();
        if (guardedWrite(t_, rq.bytes, kRequestLen) == Status::OK) {
            t_.flushTx();
            const uint32_t start = t_.nowMs();
            uint32_t lastByteAt = start;
            for (;;) {
                while (t_.available() > 0) {
                    int b = t_.readByte();
                    if (b < 0) break;
                    lastByteAt = t_.nowMs();
                    if (rxLen < kRxMax) rx[rxLen++] = static_cast<uint8_t>(b);
                }
                const uint32_t n = t_.nowMs();
                if (n - start >= kResponseTimeoutMs) break;
                if (rxLen > 0 && n - lastByteAt >= kSilenceEndMs) break;
                t_.delayMs(1);
            }
            v = evaluateLoopback(rq.bytes, kRequestLen, rx, rxLen);
        }
    }

    if (v == LoopbackVerdict::PASS) ++pass_; else ++fail_;
    char line[224];
    formatL(line, sizeof line, now, index_, v, rx, rxLen);
    out_.line(line);
}

void LoopbackRunner::finish() {
    char s[96];
    std::snprintf(s, sizeof s, "# SUMMARY pass=%u fail=%u total=%u result=%s", pass_, fail_, total_,
                  (fail_ == 0 && pass_ == total_) ? "PASS" : "FAIL");
    out_.line(s);
    std::snprintf(s, sizeof s, "S,%u,DONE", static_cast<unsigned>(t_.nowMs()));
    out_.line(s);
    done_ = true;
}
