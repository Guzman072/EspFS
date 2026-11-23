// Serial 輸出格式化（T／D／S 行）。純文字 CSV，欄位內不含逗號與空白。
#pragma once
#include "master.h"
#include "registers.h"

struct TLine {
    uint32_t ms;
    uint8_t slave;
    uint8_t fc;          // 請求的功能碼；被拒絕的請求也要能記錄（例如 04）
    uint16_t addr;
    uint16_t count;
    Status status;
    uint8_t attempts;
    uint32_t rttMs;      // kNoRtt 輸出為 -
    uint8_t regCount;    // regs 內有效筆數；0 輸出為 -
    uint16_t regs[kMaxCount];
};

// 由一次交易結果組出 T 行資料（含被拒絕、未送出的請求）。
TLine makeTLine(uint32_t ms, uint8_t slave, uint8_t fc, uint16_t addr, uint16_t count, const Result& r);

const char* statusName(Status s);
const char* headerLine();

// 以下函式都把結果寫進 buf（含結尾 '\0'，不會超出 size），回傳字元數（不含結尾）。
size_t formatT(char* buf, size_t size, const TLine& line);
size_t formatD(char* buf, size_t size, uint32_t ms, const DecodedValue& v);
size_t formatS(char* buf, size_t size, uint32_t ms, const char* state);
