#include "drive_link.h"
#include <cstdio>
#include <cstring>
#include "crc16.h"
#include "reply_handlers.h"

namespace {
// 寫入回應核對：FC 0x06 必須與請求逐位元組相同；FC 0x10 必須是 ID、0x10、起始位址、數量加 CRC。
class WriteReply : public IReplyHandler {
public:
    explicit WriteReply(const WriteFrame& f) : f_(f) {}
    Status handle(const uint8_t* b, size_t len) override {
        if (len < 5) return Status::SHORT_FRAME;
        const uint16_t want = crc16(b, len - 2);
        const uint16_t got = static_cast<uint16_t>(b[len - 2] | (b[len - 1] << 8));
        if (want != got) return Status::CRC_ERR;
        if (b[0] != f_.bytes[0]) return Status::ADDR_MISMATCH;
        const uint8_t fc = f_.bytes[1];
        if (b[1] == static_cast<uint8_t>(fc | 0x80)) {
            if (len != 5) return Status::BAD_LEN;
            switch (b[2]) {
                case 0x01: return Status::EXC_01;
                case 0x02: return Status::EXC_02;
                case 0x03: return Status::EXC_03;
                default: return Status::EXC_UNKNOWN;
            }
        }
        if (b[1] != fc) return Status::BAD_FC;
        if (fc == 0x06) return (len == 8 && std::memcmp(b, f_.bytes, 8) == 0) ? Status::OK : Status::WRITE_MISMATCH;
        // FC 0x10：回應前 6 個位元組要等於請求的前 6 個位元組（ID、FC、起始位址、數量）
        return (len == 8 && std::memcmp(b, f_.bytes, 6) == 0) ? Status::OK : Status::WRITE_MISMATCH;
    }

private:
    const WriteFrame& f_;
};
}  // namespace

void DriveLink::logT(uint8_t fc, uint16_t addr, uint16_t count, Status st, uint8_t attempts, uint32_t rtt,
                     const uint16_t* values, uint8_t nValues) {
    if (!logAll_ && st == Status::OK) return;
    TLine l{};
    l.ms = t_.nowMs();
    l.slave = slave_;
    l.fc = fc;
    l.addr = addr;
    l.count = count;
    l.status = st;
    l.attempts = attempts;
    l.rttMs = rtt;
    l.regCount = st == Status::OK || fc != kFcReadRegisters ? nValues : 0;
    for (uint8_t i = 0; i < l.regCount && i < kMaxCount; ++i) l.regs[i] = values[i];
    char line[160];
    formatT(line, sizeof line, l);
    out_.line(line);
}

Status DriveLink::read(uint16_t start, uint16_t count, uint16_t* regs, uint8_t attempts) {
    if (tx_.latched()) return tx_.latchReason();
    Request rq;
    Status built = buildDriveRead(slave_, start, count, rq);
    if (built != Status::OK) {
        tx_.record(built);
        logT(kFcReadRegisters, start, count, built, 0, kNoRtt, nullptr, 0);
        return built;
    }
    Response resp{};
    ReadReply handler(slave_, count, resp);
    const TxSpec spec{rq.bytes, kRequestLen, kFcReadRegisters, 5u + 2u * count, true, attempts};
    TxOutcome o;
    tx_.run(spec, handler, o);
    if (o.status == Status::OK) {
        std::memcpy(regs, resp.regs, sizeof(uint16_t) * count);
        logT(kFcReadRegisters, start, count, o.status, o.attempts, o.rttMs, regs, static_cast<uint8_t>(count));
        if (logAll_) {
            DecodedValue values[kMaxCount];
            const size_t n = driveDecodeBlock(start, regs, count, values, kMaxCount);
            for (size_t i = 0; i < n; ++i) {
                if (std::strcmp(values[i].name, "actual_velocity_L") == 0) values[i].scaled *= leftSign_;
                else if (std::strcmp(values[i].name, "actual_velocity_R") == 0) values[i].scaled *= rightSign_;
            }
            char line[96];
            for (size_t i = 0; i < n; ++i) {
                formatD(line, sizeof line, t_.nowMs(), values[i]);
                out_.line(line);
            }
        }
    } else {
        logT(kFcReadRegisters, start, count, o.status, o.attempts, o.rttMs, nullptr, 0);
    }
    return o.status;
}

Status DriveLink::sendWrite(const WriteFrame& f, uint16_t addr, uint16_t count, const uint16_t* values, bool emergency) {
    if (tx_.latched() && !emergency) return tx_.latchReason();
#ifdef DRIVE_DRYRUN
    // 乾跑：寫入只印出，不呼叫任何寫入傳輸。仍先過寫入 guard，被拒絕的處置與正式環境相同。
    const Status g = guard_.check(f.bytes, f.len);
    if (g != Status::OK) {
        tx_.record(g);
        logT(f.bytes[1], addr, count, g, 0, kNoRtt, values, static_cast<uint8_t>(count));
        return g;
    }
    char line[96] = "# DRY W";
    size_t n = std::strlen(line);
    for (size_t i = 0; i < f.len && n + 4 < sizeof line; ++i) n += static_cast<size_t>(std::snprintf(line + n, sizeof line - n, " %02X", f.bytes[i]));
    out_.line(line);
    return Status::OK;
#else
    WriteReply handler(f);
    const bool isTen = f.bytes[1] == 0x10;
    const uint8_t attempts = emergency ? 1 : kWriteMaxAttempts;
    const TxSpec spec{f.bytes, f.len, f.bytes[1], 8, isTen, attempts};
    TxOutcome o;
    tx_.run(spec, handler, o, emergency);
    logT(f.bytes[1], addr, count, o.status, o.attempts, o.rttMs, values, static_cast<uint8_t>(count));
    return o.status;
#endif
}

Status DriveLink::writeSpeedMode() {
    WriteFrame f{};
    Status b = buildSetSpeedMode(slave_, f);
    const uint16_t v = kModeSpeed;
    if (b != Status::OK) {
        tx_.record(b);
        return b;
    }
    return sendWrite(f, kRegRunMode, 1, &v, false);
}

Status DriveLink::writeControl(uint16_t word, bool emergency) {
    WriteFrame f{};
    Status b = buildControlWord(slave_, word, f);
    if (b != Status::OK) {
        tx_.record(b);
        return b;
    }
    return sendWrite(f, kRegControlWord, 1, &word, emergency);
}

Status DriveLink::writeOfflineTime(uint16_t ms) {
    WriteFrame f{};
    Status b = buildSetOfflineTime(slave_, ms, f);
    if (b != Status::OK) {
        tx_.record(b);
        return b;
    }
    return sendWrite(f, kRegOfflineTime, 1, &ms, false);
}

Status DriveLink::writeSpeeds(int16_t left, int16_t right, bool emergency) {
    WriteFrame f{};
    Status b = buildSetSpeeds(slave_, left, right, guard_.limitRaw(), f);
    if (b != Status::OK) {
        tx_.record(b);
        return b;
    }
    const uint16_t v[2] = {static_cast<uint16_t>(left), static_cast<uint16_t>(right)};
    return sendWrite(f, kRegTargetSpeedLeft, 2, v, emergency);
}
