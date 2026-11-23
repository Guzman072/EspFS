#include "guard.h"
#include "crc16.h"

Status checkRequest(const uint8_t* bytes, size_t len) {
    if (bytes == nullptr || len < 2) return Status::REJECTED_LEN;
    if (bytes[1] != kFcReadRegisters) return Status::REJECTED_FC;  // 白名單：只有 0x03
    if (len != kRequestLen) return Status::REJECTED_LEN;

    const uint16_t start = static_cast<uint16_t>((bytes[2] << 8) | bytes[3]);
    const uint16_t count = static_cast<uint16_t>((bytes[4] << 8) | bytes[5]);
    Status s = validateReadParams(bytes[0], start, count);
    if (s != Status::OK) return s;

    const uint16_t want = crc16(bytes, 6);
    const uint16_t got = static_cast<uint16_t>(bytes[6] | (bytes[7] << 8));
    return (want == got) ? Status::OK : Status::REJECTED_CRC;
}

const IFrameGuard& readOnlyGuard() {
    static const ReadOnlyGuard instance;
    return instance;
}

Status guardedWrite(ITransport& transport, const IFrameGuard& guard, const uint8_t* bytes, size_t len) {
    Status s = guard.check(bytes, len);
    if (s != Status::OK) return s;
    const size_t written = (len >= 2 && bytes[1] == kFcReadRegisters) ? transport.write(bytes, len)
                                                                       : transport.writeDrive(bytes, len);
    return (written == len) ? Status::OK : Status::TX_FAIL;
}

Status guardedWrite(ITransport& transport, const uint8_t* bytes, size_t len) {
    return guardedWrite(transport, readOnlyGuard(), bytes, len);
}

bool isReadFunctionFrame(const uint8_t* bytes, size_t len) {
    return bytes != nullptr && len >= 2 && bytes[1] == kFcReadRegisters;
}
