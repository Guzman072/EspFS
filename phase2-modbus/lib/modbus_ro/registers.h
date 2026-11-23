// 候選暫存器表與位址白名單。來源：PROJECT.md 第 4 節（RS485 手冊 §4，對應舊版硬體，僅供參考）。
// 新增任何位址之前，必須先更新 PROJECT.md 並經使用者確認。
#pragma once
#include <cstddef>
#include <cstdint>

enum class RegKind : uint8_t {
    Plain,    // 單一數值（無號或有號由 isSigned 決定）
    Split,    // 高位元組＝左（name）、低位元組＝右（name2）
    PosHigh,  // 32 位元位置的高 16 位元，與下一個暫存器組合（暫定）
    PosLow,   // 單獨不輸出，由 PosHigh 一併處理
};

struct RegisterInfo {
    uint16_t addr;
    const char* name;
    const char* name2;  // 僅 Split 使用
    const char* unit;
    uint8_t decimals;   // 縮放：scaled / 10^decimals
    bool isSigned;
    RegKind kind;
};

struct DecodedValue {
    const char* name;
    const char* unit;
    int32_t scaled;     // 已乘以 10^decimals 的整數，例如 0.1 r/min 單位的 100 代表 10.0
    uint8_t decimals;
};

// 位址白名單：0x2000–0x2002、0x20A0–0x20AE（不含 0x20AF）。count 為 0 或越界回 false。
bool rangeAllowed(uint16_t start, uint16_t count);

size_t registerCount();
const RegisterInfo& registerAt(size_t index);
const RegisterInfo* findRegister(uint16_t addr);  // 找不到回 nullptr

// 把一段讀回的暫存器解成可顯示的值。未知位址略過；輸出不超過 cap。回傳實際輸出數。
size_t decodeBlock(uint16_t start, const uint16_t* regs, size_t count, DecodedValue* out, size_t cap);
