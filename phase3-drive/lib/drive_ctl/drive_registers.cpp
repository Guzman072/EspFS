#include "drive_registers.h"
#include "crc16.h"
#include "drive_config.h"

namespace {
struct Extra {
    uint16_t addr;
    const char* name;
    const char* unit;
};
const Extra kExtras[] = {
    {0x2007, "power_on_lock_mode", "mode"}, {0x2008, "max_rpm", "r/min"},        {0x200F, "sync_flag", "flag"},
    {0x2011, "quick_stop_code", "code"},    {0x2022, "speed_resolution", "div"}, {0x2023, "overspeed_check", "flag"},
    {0x2088, "target_speed_L", "raw"},      {0x2089, "target_speed_R", "raw"},
};

const Extra* findExtra(uint32_t addr) {
    for (const Extra& e : kExtras)
        if (e.addr == addr) return &e;
    return nullptr;
}

bool addrReadable(uint32_t a) {
    if (a >= 0x2000 && a <= 0x2002) return true;
    if (a >= 0x20A0 && a <= 0x20AE) return true;
    return findExtra(a) != nullptr;
}
}  // namespace

bool driveReadAllowed(uint16_t start, uint16_t count) {
    if (count < 1 || count > kMaxCount) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!addrReadable(static_cast<uint32_t>(start) + i)) return false;
    return true;
}

Status validateDriveRead(uint8_t slaveId, uint16_t start, uint16_t count) {
    if (slaveId != kDriveSlaveId) return Status::REJECTED_ID;
    if (count < 1 || count > kMaxCount) return Status::REJECTED_LEN;
    if (!driveReadAllowed(start, count)) return Status::REJECTED_ADDR;
    return Status::OK;
}

Status buildDriveRead(uint8_t slaveId, uint16_t start, uint16_t count, Request& out) {
    Status s = validateDriveRead(slaveId, start, count);
    if (s != Status::OK) return s;
    Request r;
    r.bytes[0] = slaveId;
    r.bytes[1] = kFcReadRegisters;
    r.bytes[2] = static_cast<uint8_t>(start >> 8);
    r.bytes[3] = static_cast<uint8_t>(start & 0xFF);
    r.bytes[4] = static_cast<uint8_t>(count >> 8);
    r.bytes[5] = static_cast<uint8_t>(count & 0xFF);
    const uint16_t c = crc16(r.bytes, 6);
    r.bytes[6] = static_cast<uint8_t>(c & 0xFF);
    r.bytes[7] = static_cast<uint8_t>(c >> 8);
    out = r;
    return Status::OK;
}

size_t driveDecodeBlock(uint16_t start, const uint16_t* regs, size_t count, DecodedValue* out, size_t cap) {
    size_t n = 0;
    size_t i = 0;
    while (i < count && n < cap) {
        const uint32_t addr = static_cast<uint32_t>(start) + i;
        if (const Extra* e = findExtra(addr)) {
            out[n++] = {e->name, e->unit, static_cast<int32_t>(regs[i]), 0};
            ++i;
        } else {
            size_t j = i;
            while (j < count && findExtra(static_cast<uint32_t>(start) + j) == nullptr) ++j;
            n += decodeBlock(static_cast<uint16_t>(start + i), regs + i, j - i, out + n, cap - n);
            i = j;
        }
    }
    return n;
}
