// s3_poll 的觀測工具（只記錄，不改變交易行為）。
// - EchoProbe：包住 ITransport，比對送出後收到的開頭位元組是否等於剛送出的請求，計數回音。
//   writeDrive 不覆寫（沿用 ITransport 預設回 0），所以任何寫入幀都送不出去。
// - StatusCounter：包住 ILineSink，從 T 行統計交易數、CRC 錯誤與逾時；每一行原樣轉出。
#pragma once
#include <cstddef>
#include <cstdint>
#include "sink.h"
#include "transport.h"

class EchoProbe : public ITransport {
public:
    explicit EchoProbe(ITransport& inner) : in_(inner) {}

    size_t write(const uint8_t* data, size_t len) override;
    void flushTx() override { in_.flushTx(); }
    void clearRx() override { in_.clearRx(); }
    size_t available() override { return in_.available(); }
    int readByte() override;
    uint32_t nowMs() override { return in_.nowMs(); }
    void delayMs(uint32_t ms) override { in_.delayMs(ms); }

    uint32_t echoes() const { return echoes_; }                // 收到與請求完全相同的位元組
    uint32_t partialEchoes() const { return partial_; }        // 開頭至少 kPartialMin 個相同，之後不同

    static constexpr size_t kMaxFrame = 16;
    static constexpr size_t kPartialMin = 4;  // 正常回應的前 2 個位元組（ID、FC）本來就與請求相同

private:
    ITransport& in_;
    uint8_t last_[kMaxFrame] = {};
    size_t lastLen_ = 0;
    size_t matched_ = 0;
    bool watching_ = false;
    uint32_t echoes_ = 0;
    uint32_t partial_ = 0;
};

class StatusCounter : public ILineSink {
public:
    explicit StatusCounter(ILineSink& inner) : in_(inner) {}
    void line(const char* text) override;

    uint32_t transactions() const { return tx_; }
    uint32_t failures() const { return fail_; }
    uint32_t crcErrors() const { return crc_; }
    uint32_t timeouts() const { return timeout_; }

private:
    ILineSink& in_;
    uint32_t tx_ = 0;
    uint32_t fail_ = 0;
    uint32_t crc_ = 0;
    uint32_t timeout_ = 0;
};
