#include "poller.h"
#include <cstdio>

namespace {
// 探測：讀 20A0h 共 1 個（DESIGN 第 6 節、B3）。
constexpr PollBlock kProbeBlock = {0x20A0, 1};
// 探測成功後讀一次通訊設定，確認實際 Slave ID 與 Baud（B4，待確認項目 6、7）。
constexpr PollBlock kIdentityBlocks[] = {{0x2001, 2}, {0x2000, 1}};
// 預設輪詢區塊（B5、B6）：單次最多 4 個，在確認單次上限（待確認項目 8）之前保守使用。
constexpr PollBlock kDefaultPlan[] = {{0x20A1, 1}, {0x20A2, 3}, {0x20A5, 2}, {0x20A7, 4}, {0x20AB, 4}};
constexpr size_t kDecodedMax = 8;
}  // namespace

PollerConfig PollerConfig::defaults() {
    PollerConfig c;
    c.plan = kDefaultPlan;
    c.planCount = sizeof(kDefaultPlan) / sizeof(kDefaultPlan[0]);
    return c;
}

const char* pollStateName(PollState s) {
    switch (s) {
        case PollState::BOOT: return "BOOT";
        case PollState::PROBE: return "PROBE";
        case PollState::POLLING: return "POLLING";
        case PollState::LATCHED: return "LATCHED";
    }
    return "UNKNOWN";
}

void Poller::setState(PollState s) {
    state_ = s;
    char line[32];
    formatS(line, sizeof line, t_.nowMs(), pollStateName(s));
    out_.line(line);
}

void Poller::enterLatched(Status reason) {
    if (state_ == PollState::LATCHED) return;
    reason_ = reason;
    setState(PollState::LATCHED);
    char line[64];
    std::snprintf(line, sizeof line, "# LATCHED reason=%s", statusName(reason));
    out_.line(line);
}

bool Poller::latchedByMaster() {
    if (!m_.latched()) return false;
    enterLatched(m_.latchReason());
    return true;
}

Status Poller::transact(uint8_t id, uint16_t addr, uint16_t count) {
    Result r;
    m_.readRegisters(id, addr, count, r);
    const uint32_t now = t_.nowMs();
    char line[160];
    formatT(line, sizeof line, makeTLine(now, id, kFcReadRegisters, addr, count, r));
    out_.line(line);
    if (r.status != Status::OK) return r.status;
    DecodedValue values[kDecodedMax];
    const size_t n = decodeBlock(addr, r.regs, r.count, values, kDecodedMax);
    for (size_t i = 0; i < n; ++i) {
        formatD(line, sizeof line, now, values[i]);
        out_.line(line);
    }
    return Status::OK;
}

void Poller::doProbe() {
    Status last = Status::TIMEOUT;
    for (uint8_t id : cfg_.candidates) {
        const Status s = transact(id, kProbeBlock.addr, kProbeBlock.count);
        if (latchedByMaster()) return;
        if (s == Status::OK) {
            slave_ = id;
            for (const PollBlock& b : kIdentityBlocks) {
                transact(slave_, b.addr, b.count);
                if (latchedByMaster()) return;
            }
            setState(PollState::POLLING);
            return;
        }
        last = s;
    }
    enterLatched(last);
}

void Poller::doPoll() {
    const uint32_t now = t_.nowMs();
    if (tickStarted_ && now - tickStart_ < cfg_.periodMs) return;
    tickStarted_ = true;
    tickStart_ = now;
    for (size_t i = 0; i < cfg_.planCount; ++i) {
        transact(slave_, cfg_.plan[i].addr, cfg_.plan[i].count);
        if (latchedByMaster()) return;  // 同一時間只有一筆交易，LATCHED 後不再送出
    }
    ++cycles_;
}

void Poller::step() {
    switch (state_) {
        case PollState::BOOT: {
            out_.line(headerLine());
            char line[32];
            formatS(line, sizeof line, t_.nowMs(), pollStateName(PollState::BOOT));
            out_.line(line);
            setState(PollState::PROBE);
            break;
        }
        case PollState::PROBE: doProbe(); break;
        case PollState::POLLING: doPoll(); break;
        case PollState::LATCHED: break;
    }
}
