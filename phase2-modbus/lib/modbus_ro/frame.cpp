#include "frame.h"
#include "crc16.h"
#include "registers.h"

namespace {
constexpr size_t kMinFrame = 5;  // 例外回應的長度，也是任何合法回應的最小長度

uint16_t frameCrc(const uint8_t* b, size_t bodyLen) {
    return static_cast<uint16_t>(b[bodyLen] | (b[bodyLen + 1] << 8));  // 低位元組在前
}
}  // namespace

Status validateReadParams(uint8_t slaveId, uint16_t startAddr, uint16_t count) {
    if (slaveId < kMinSlaveId || slaveId > kMaxSlaveId) return Status::REJECTED_ID;
    if (count < 1 || count > kMaxCount) return Status::REJECTED_LEN;
    if (!rangeAllowed(startAddr, count)) return Status::REJECTED_ADDR;
    return Status::OK;
}

Status buildReadRegisters(uint8_t slaveId, uint16_t startAddr, uint16_t count, Request& out) {
    Status s = validateReadParams(slaveId, startAddr, count);
    if (s != Status::OK) return s;
    Request r;
    r.bytes[0] = slaveId;
    r.bytes[1] = kFcReadRegisters;
    r.bytes[2] = static_cast<uint8_t>(startAddr >> 8);
    r.bytes[3] = static_cast<uint8_t>(startAddr & 0xFF);
    r.bytes[4] = static_cast<uint8_t>(count >> 8);
    r.bytes[5] = static_cast<uint8_t>(count & 0xFF);
    uint16_t c = crc16(r.bytes, 6);
    r.bytes[6] = static_cast<uint8_t>(c & 0xFF);
    r.bytes[7] = static_cast<uint8_t>(c >> 8);
    out = r;
    return Status::OK;
}

Status parseResponse(const uint8_t* buf, size_t len, uint8_t expectSlave, uint16_t expectCount, Response& out) {
    if (buf == nullptr || len < kMinFrame) return Status::SHORT_FRAME;
    if (crc16(buf, len - 2) != frameCrc(buf, len - 2)) return Status::CRC_ERR;
    if (buf[0] != expectSlave) return Status::ADDR_MISMATCH;
    const uint8_t fc = buf[1];
    if (fc != kFcReadRegisters && fc != kFcReadException) return Status::BAD_FC;

    if (fc == kFcReadException) {
        if (len != kMinFrame) return Status::BAD_LEN;
        switch (buf[2]) {
            case 0x01: return Status::EXC_01;
            case 0x02: return Status::EXC_02;
            case 0x03: return Status::EXC_03;
            default: return Status::EXC_UNKNOWN;
        }
    }

    const size_t byteCount = buf[2];
    if (expectCount < 1 || expectCount > kMaxCount) return Status::BAD_LEN;
    if (byteCount != static_cast<size_t>(expectCount) * 2) return Status::BAD_LEN;
    if (len != kMinFrame + byteCount) return Status::BAD_LEN;

    out.slave = buf[0];
    out.count = static_cast<uint8_t>(expectCount);
    for (size_t i = 0; i < expectCount; ++i) {
        out.regs[i] = static_cast<uint16_t>((buf[3 + 2 * i] << 8) | buf[4 + 2 * i]);
    }
    return Status::OK;
}
