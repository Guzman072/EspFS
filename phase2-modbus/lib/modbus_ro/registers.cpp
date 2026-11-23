#include "registers.h"

namespace {
constexpr uint16_t kCfgFirst = 0x2000, kCfgLast = 0x2002;
constexpr uint16_t kStatusFirst = 0x20A0, kStatusLast = 0x20AE;

const RegisterInfo kTable[] = {
    {0x2000, "comm_offline_ms", nullptr, "ms", 0, false, RegKind::Plain},
    {0x2001, "node_id", nullptr, "id", 0, false, RegKind::Plain},
    {0x2002, "baud_code", nullptr, "code", 0, false, RegKind::Plain},
    {0x20A0, "sw_version", nullptr, "raw", 0, false, RegKind::Plain},
    {0x20A1, "dc_voltage", nullptr, "V", 2, false, RegKind::Plain},
    {0x20A2, "motor_state_L", "motor_state_R", "state", 0, false, RegKind::Split},
    {0x20A3, "hall_state_L", "hall_state_R", "state", 0, false, RegKind::Split},
    {0x20A4, "motor_temp_L", "motor_temp_R", "C", 0, false, RegKind::Split},
    {0x20A5, "error_code_L", nullptr, "bits", 0, false, RegKind::Plain},
    {0x20A6, "error_code_R", nullptr, "bits", 0, false, RegKind::Plain},
    {0x20A7, "position_L_tentative", nullptr, "counts", 0, true, RegKind::PosHigh},
    {0x20A8, "position_L_low", nullptr, "counts", 0, true, RegKind::PosLow},
    {0x20A9, "position_R_tentative", nullptr, "counts", 0, true, RegKind::PosHigh},
    {0x20AA, "position_R_low", nullptr, "counts", 0, true, RegKind::PosLow},
    {0x20AB, "actual_velocity_L", nullptr, "r/min", 1, true, RegKind::Plain},
    {0x20AC, "actual_velocity_R", nullptr, "r/min", 1, true, RegKind::Plain},
    {0x20AD, "actual_torque_L", nullptr, "A", 1, true, RegKind::Plain},
    {0x20AE, "actual_torque_R", nullptr, "A", 1, true, RegKind::Plain},
};
constexpr size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

int32_t plainValue(const RegisterInfo& r, uint16_t raw) {
    return r.isSigned ? static_cast<int32_t>(static_cast<int16_t>(raw)) : static_cast<int32_t>(raw);
}
}  // namespace

bool rangeAllowed(uint16_t start, uint16_t count) {
    if (count == 0) return false;
    const uint32_t first = start;
    const uint32_t last = first + count - 1;  // 32 位元運算，避免 0xFFFF 溢位繞回
    const bool inCfg = first >= kCfgFirst && last <= kCfgLast;
    const bool inStatus = first >= kStatusFirst && last <= kStatusLast;
    return inCfg || inStatus;
}

size_t registerCount() { return kTableSize; }
const RegisterInfo& registerAt(size_t index) { return kTable[index]; }

const RegisterInfo* findRegister(uint16_t addr) {
    for (size_t i = 0; i < kTableSize; ++i) {
        if (kTable[i].addr == addr) return &kTable[i];
    }
    return nullptr;
}

size_t decodeBlock(uint16_t start, const uint16_t* regs, size_t count, DecodedValue* out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < count && n < cap; ++i) {
        const RegisterInfo* r = findRegister(static_cast<uint16_t>(start + i));
        if (!r) continue;
        switch (r->kind) {
            case RegKind::Plain:
                out[n++] = {r->name, r->unit, plainValue(*r, regs[i]), r->decimals};
                break;
            case RegKind::Split:
                out[n++] = {r->name, r->unit, static_cast<int32_t>(regs[i] >> 8), 0};
                if (n < cap) out[n++] = {r->name2, r->unit, static_cast<int32_t>(regs[i] & 0xFF), 0};
                break;
            case RegKind::PosHigh:
                // 暫定：高 16 位元在前、低 16 位元在後，組成 32 位元有號數（待確認項目 12）
                if (i + 1 < count) {
                    uint32_t v = (static_cast<uint32_t>(regs[i]) << 16) | regs[i + 1];
                    out[n++] = {r->name, r->unit, static_cast<int32_t>(v), 0};
                }
                break;
            case RegKind::PosLow:
                break;
        }
    }
    return n;
}
