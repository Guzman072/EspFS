#include "echo_probe.h"
#include <cstring>

size_t EchoProbe::write(const uint8_t* data, size_t len) {
    const size_t n = in_.write(data, len);
    lastLen_ = len <= kMaxFrame ? len : 0;
    if (lastLen_ > 0) std::memcpy(last_, data, lastLen_);
    matched_ = 0;
    watching_ = lastLen_ > 0 && n == len;
    return n;
}

int EchoProbe::readByte() {
    const int b = in_.readByte();
    if (b < 0 || !watching_) return b;
    if (static_cast<uint8_t>(b) == last_[matched_]) {
        if (++matched_ == lastLen_) {
            ++echoes_;
            watching_ = false;
        }
    } else {
        if (matched_ >= kPartialMin) ++partial_;
        watching_ = false;
    }
    return b;
}

namespace {
// T 行：T,ms,slave,fc,addr,count,status,...；回傳第 7 欄（status）的起點，不是 T 行回 nullptr。
const char* statusField(const char* text) {
    if (text[0] != 'T' || text[1] != ',') return nullptr;
    const char* p = text;
    for (int commas = 0; *p != '\0'; ++p) {
        if (*p == ',' && ++commas == 6) return p + 1;
    }
    return nullptr;
}

bool fieldIs(const char* field, const char* name) {
    const size_t n = std::strlen(name);
    return std::strncmp(field, name, n) == 0 && (field[n] == ',' || field[n] == '\0');
}
}  // namespace

void StatusCounter::line(const char* text) {
    if (const char* s = statusField(text)) {
        ++tx_;
        if (!fieldIs(s, "OK")) ++fail_;
        if (fieldIs(s, "CRC_ERR")) ++crc_;
        if (fieldIs(s, "TIMEOUT")) ++timeout_;
    }
    in_.line(text);
}
