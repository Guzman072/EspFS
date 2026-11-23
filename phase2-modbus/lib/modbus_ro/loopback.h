// A5 迴路測試（GPIO16 與 GPIO17 短接，模組拆下）：只送 FC 0x03 讀取請求，讀回後逐位元組比對。
// 不經過 master（迴路讀回的是請求本身，master 會把它當回音丟掉）。寫入仍一律經 guardedWrite。
#pragma once
#include "frame.h"
#include "guard.h"
#include "sink.h"
#include "transport.h"

enum class LoopbackVerdict : uint8_t { PASS = 0, NO_BYTES, SHORT, EXTRA, MISMATCH, NOT_SENT };

LoopbackVerdict evaluateLoopback(const uint8_t* tx, size_t txLen, const uint8_t* rx, size_t rxLen);
const char* verdictName(LoopbackVerdict v);  // "PASS"、"FAIL_NO_BYTES"…

// L,ms,index,verdict,rx_len,rx_hex（無資料時 rx_hex 為 -）。回傳字元數。
size_t formatL(char* buf, size_t size, uint32_t ms, uint16_t index, LoopbackVerdict v, const uint8_t* rx, size_t rxLen);

class LoopbackRunner {
public:
    LoopbackRunner(ITransport& transport, ILineSink& sink, uint16_t total = 50, uint32_t periodMs = 200)
        : t_(transport), out_(sink), total_(total), period_(periodMs) {}

    // 到了時間才做一次迴路；全部做完輸出摘要與 S,ms,DONE。
    void step();
    bool done() const { return done_; }
    uint16_t passCount() const { return pass_; }
    uint16_t failCount() const { return fail_; }

private:
    ITransport& t_;
    ILineSink& out_;
    uint16_t total_;
    uint32_t period_;
    uint16_t index_ = 0, pass_ = 0, fail_ = 0;
    uint32_t lastStart_ = 0;
    bool started_ = false, done_ = false;

    void runOnce(uint32_t now);
    void finish();
};
