#include "write_guard.h"
#include <cstdlib>
#include "crc16.h"
#include "drive_config.h"
#include "drive_registers.h"
#include "write_frame.h"
#include "write_rules.h"

namespace {
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

Status checkCrc(const uint8_t* b, size_t len) {
    const uint16_t want = crc16(b, len - 2);
    const uint16_t got = static_cast<uint16_t>(b[len - 2] | (b[len - 1] << 8));
    return want == got ? Status::OK : Status::REJECTED_CRC;
}
}  // namespace

Status WriteGuard::check(const uint8_t* b, size_t len) const {
    if (b == nullptr || len < 2) return Status::REJECTED_LEN;
    const uint8_t fc = b[1];
    if (fc != 0x03 && fc != 0x06 && fc != 0x10) return Status::REJECTED_FC;
    if (b[0] != kDriveSlaveId) return Status::REJECTED_ID;
    if (fc == 0x03) return checkRead(b, len);
    if (fc == 0x06) return checkSingle(b, len);
    return checkMulti(b, len);
}

Status WriteGuard::checkRead(const uint8_t* b, size_t len) const {
    if (len != 8) return Status::REJECTED_LEN;
    const uint16_t count = be16(b + 4);
    if (count < 1 || count > kMaxCount) return Status::REJECTED_LEN;
    if (!driveReadAllowed(be16(b + 2), count)) return Status::REJECTED_ADDR;
    return checkCrc(b, len);
}

Status WriteGuard::checkSingle(const uint8_t* b, size_t len) const {
    if (len != 8) return Status::REJECTED_LEN;
    const uint16_t addr = be16(b + 2);
    const uint16_t value = be16(b + 4);
    if (isBlacklistedReg(addr) || !writeRegAllowedForSingle(addr)) return Status::REJECTED_REG;
    bool ok = false;
    if (addr == kRegRunMode) ok = (value == kModeSpeed);
    else if (addr == kRegControlWord) ok = controlWordAllowed(value);
    else ok = offlineTimeAllowed(value);  // 2000h
    if (!ok) return Status::REJECTED_VALUE;
    return checkCrc(b, len);
}

Status WriteGuard::checkMulti(const uint8_t* b, size_t len) const {
    if (len < 7) return Status::REJECTED_LEN;
    const uint16_t start = be16(b + 2);
    if (isBlacklistedReg(start) || start != kRegTargetSpeedLeft) return Status::REJECTED_REG;
    if (be16(b + 4) != 2 || b[6] != 4 || len != kMaxWriteFrame) return Status::REJECTED_LEN;
    const int left = static_cast<int16_t>(be16(b + 7));
    const int right = static_cast<int16_t>(be16(b + 9));
    if (std::abs(left) > limitRaw_ || std::abs(right) > limitRaw_) return Status::REJECTED_VALUE;
    return checkCrc(b, len);
}
