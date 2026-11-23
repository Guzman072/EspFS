#include "drive_frame_check.h"

bool isAllowedDriveFrameShape(const uint8_t* b, size_t len) {
    if (b == nullptr || len < 2) return false;
    const unsigned addr = len >= 4 ? static_cast<unsigned>((b[2] << 8) | b[3]) : 0u;
    switch (b[1]) {
        case 0x03:
            return len == 8;
        case 0x06:
            return len == 8 && (addr == 0x2000 || addr == 0x200D || addr == 0x200E);
        case 0x10:
            return len == 13 && addr == 0x2088 && b[4] == 0x00 && b[5] == 0x02 && b[6] == 0x04;
        default:
            return false;
    }
}
