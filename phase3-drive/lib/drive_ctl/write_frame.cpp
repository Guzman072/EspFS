#include "write_frame.h"
#include <cstdlib>
#include "crc16.h"
#include "drive_config.h"
#include "write_rules.h"

namespace {
void appendCrc(WriteFrame& f) {
    const uint16_t c = crc16(f.bytes, f.len);
    f.bytes[f.len++] = static_cast<uint8_t>(c & 0xFF);
    f.bytes[f.len++] = static_cast<uint8_t>(c >> 8);
}

WriteFrame single(uint8_t id, uint16_t addr, uint16_t value) {
    WriteFrame f{};
    f.bytes[0] = id;
    f.bytes[1] = 0x06;
    f.bytes[2] = static_cast<uint8_t>(addr >> 8);
    f.bytes[3] = static_cast<uint8_t>(addr & 0xFF);
    f.bytes[4] = static_cast<uint8_t>(value >> 8);
    f.bytes[5] = static_cast<uint8_t>(value & 0xFF);
    f.len = 6;
    appendCrc(f);
    return f;
}
}  // namespace

Status buildSetSpeedMode(uint8_t slaveId, WriteFrame& out) {
    if (slaveId != kDriveSlaveId) return Status::REJECTED_ID;
    out = single(slaveId, kRegRunMode, kModeSpeed);
    return Status::OK;
}

Status buildControlWord(uint8_t slaveId, uint16_t word, WriteFrame& out) {
    if (slaveId != kDriveSlaveId) return Status::REJECTED_ID;
    if (!controlWordAllowed(word)) return Status::REJECTED_VALUE;
    out = single(slaveId, kRegControlWord, word);
    return Status::OK;
}

Status buildSetOfflineTime(uint8_t slaveId, uint16_t ms, WriteFrame& out) {
    if (slaveId != kDriveSlaveId) return Status::REJECTED_ID;
    if (!offlineTimeAllowed(ms)) return Status::REJECTED_VALUE;
    out = single(slaveId, kRegOfflineTime, ms);
    return Status::OK;
}

Status buildSetSpeeds(uint8_t slaveId, int16_t left, int16_t right, int16_t limitRaw, WriteFrame& out) {
    if (slaveId != kDriveSlaveId) return Status::REJECTED_ID;
    if (limitRaw < 0 || std::abs(static_cast<int>(left)) > limitRaw || std::abs(static_cast<int>(right)) > limitRaw)
        return Status::REJECTED_VALUE;
    WriteFrame f{};
    f.bytes[0] = slaveId;
    f.bytes[1] = 0x10;
    f.bytes[2] = static_cast<uint8_t>(kRegTargetSpeedLeft >> 8);
    f.bytes[3] = static_cast<uint8_t>(kRegTargetSpeedLeft & 0xFF);
    f.bytes[4] = 0x00;
    f.bytes[5] = 0x02;  // 數量固定 2
    f.bytes[6] = 0x04;  // 位元組數固定 4
    f.bytes[7] = static_cast<uint8_t>(static_cast<uint16_t>(left) >> 8);
    f.bytes[8] = static_cast<uint8_t>(static_cast<uint16_t>(left) & 0xFF);
    f.bytes[9] = static_cast<uint8_t>(static_cast<uint16_t>(right) >> 8);
    f.bytes[10] = static_cast<uint8_t>(static_cast<uint16_t>(right) & 0xFF);
    f.len = 11;
    appendCrc(f);
    out = f;
    return Status::OK;
}
