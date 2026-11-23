#include "report.h"
#include <cstdarg>
#include <cstdio>

namespace {
const char* const kStatusNames[] = {
    "OK", "TIMEOUT", "SHORT_FRAME", "CRC_ERR", "ADDR_MISMATCH", "BAD_FC", "BAD_LEN", "EXC_01",
    "EXC_02", "EXC_03", "EXC_UNKNOWN", "REJECTED_FC", "REJECTED_ADDR", "REJECTED_LEN",
    "REJECTED_ID", "REJECTED_CRC", "TX_FAIL", "REJECTED_REG", "REJECTED_VALUE", "WRITE_MISMATCH",
};

// 有界附加：永遠保留結尾 '\0'，超出就截斷。
class Appender {
public:
    Appender(char* buf, size_t size) : buf_(buf), size_(size) {
        if (size_ > 0) buf_[0] = '\0';
    }
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (size_ == 0 || len_ + 1 >= size_) return;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf_ + len_, size_ - len_, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        len_ = (static_cast<size_t>(n) >= size_ - len_) ? size_ - 1 : len_ + static_cast<size_t>(n);
    }
    size_t length() const { return len_; }

private:
    char* buf_;
    size_t size_;
    size_t len_ = 0;
};

// 定點輸出：scaled 已乘以 10^decimals。0 位小數時不印小數點。
void addScaled(Appender& a, int32_t scaled, uint8_t decimals) {
    const bool neg = scaled < 0;
    uint32_t mag = neg ? static_cast<uint32_t>(-(static_cast<int64_t>(scaled))) : static_cast<uint32_t>(scaled);
    if (decimals == 0) {
        a.add("%s%u", neg ? "-" : "", static_cast<unsigned>(mag));
        return;
    }
    uint32_t div = 1;
    for (uint8_t i = 0; i < decimals; ++i) div *= 10;
    a.add("%s%u.%0*u", neg ? "-" : "", static_cast<unsigned>(mag / div), decimals,
          static_cast<unsigned>(mag % div));
}
}  // namespace

TLine makeTLine(uint32_t ms, uint8_t slave, uint8_t fc, uint16_t addr, uint16_t count, const Result& r) {
    TLine l{};
    l.ms = ms;
    l.slave = slave;
    l.fc = fc;
    l.addr = addr;
    l.count = count;
    l.status = r.status;
    l.attempts = r.attempts;
    l.rttMs = r.rttMs;
    l.regCount = (r.status == Status::OK) ? r.count : 0;
    for (uint8_t i = 0; i < l.regCount; ++i) l.regs[i] = r.regs[i];
    return l;
}

const char* statusName(Status s) {
    size_t i = static_cast<size_t>(s);
    return i < sizeof(kStatusNames) / sizeof(kStatusNames[0]) ? kStatusNames[i] : "UNKNOWN";
}

const char* headerLine() {
    return "# T,ms,slave,fc,addr,count,status,attempts,rtt_ms,values | D,ms,name,value,unit | S,ms,state";
}

size_t formatT(char* buf, size_t size, const TLine& l) {
    Appender a(buf, size);
    a.add("T,%u,%02X,%02X,%04X,%u,%s,%u,", static_cast<unsigned>(l.ms), l.slave, l.fc, l.addr, l.count,
          statusName(l.status), l.attempts);
    if (l.rttMs == kNoRtt) {
        a.add("-,");
    } else {
        a.add("%u,", static_cast<unsigned>(l.rttMs));
    }
    if (l.regCount == 0) {
        a.add("-");
    }
    for (uint8_t i = 0; i < l.regCount && i < kMaxCount; ++i) {
        a.add("%s%04X=%04X", i ? ";" : "", static_cast<uint16_t>(l.addr + i), l.regs[i]);
    }
    return a.length();
}

size_t formatD(char* buf, size_t size, uint32_t ms, const DecodedValue& v) {
    Appender a(buf, size);
    a.add("D,%u,%s,", static_cast<unsigned>(ms), v.name);
    addScaled(a, v.scaled, v.decimals);
    a.add(",%s", v.unit);
    return a.length();
}

size_t formatS(char* buf, size_t size, uint32_t ms, const char* state) {
    Appender a(buf, size);
    a.add("S,%u,%s", static_cast<unsigned>(ms), state);
    return a.length();
}
