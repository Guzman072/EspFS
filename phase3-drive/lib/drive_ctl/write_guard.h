// Phase 3 送出層防線（讀寫總閘）。只允許 Slave ID 1；功能碼 0x03／0x06／0x10 各有自己的白名單。
// 檢查順序：長度不足 2 → REJECTED_LEN；功能碼不在 {03,06,10} → REJECTED_FC；Slave ID ≠ 1 → REJECTED_ID；
// 再依功能碼檢查長度、位址、值、CRC（見 write_guard.cpp）。Phase 2 的唯讀 guard 不受影響。
#pragma once
#include "guard.h"

class WriteGuard : public IFrameGuard {
public:
    // 速度上限的原始值（由 2022h 換算）。未設定時為 0：速度只允許寫 0。
    void setLimitRaw(int16_t limitRaw) { limitRaw_ = limitRaw < 0 ? 0 : limitRaw; }
    int16_t limitRaw() const { return limitRaw_; }
    Status check(const uint8_t* bytes, size_t len) const override;

private:
    int16_t limitRaw_ = 0;
    Status checkRead(const uint8_t* b, size_t len) const;
    Status checkSingle(const uint8_t* b, size_t len) const;
    Status checkMulti(const uint8_t* b, size_t len) const;
};
