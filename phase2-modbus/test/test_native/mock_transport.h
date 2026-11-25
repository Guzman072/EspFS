// 測試用假 transport：假時鐘、腳本化回應、記錄所有寫入位元組。不碰任何硬體。
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include "transport.h"

struct ScriptedReply {
    std::vector<uint8_t> bytes;  // 空 = 驅動器沒有回應
    uint32_t latencyMs;          // 寫入完成後經過多久，整包位元組同時到達
};

class MockTransport : public ITransport {
public:
    std::vector<std::vector<uint8_t>> writes;
    size_t totalWritten = 0;
    std::deque<ScriptedReply> replies;  // 每次 write 取用一筆
    bool echo = false;                  // 為 true 時，寫入的位元組立刻原樣回到接收端（模擬 GPIO16／17 短接）
    bool garbageEcho = false;           // 模擬送出後立即收到一段與請求不同的短序列
    bool dropFirstReplyByte = false;    // 模擬 DE／RE 切換延遲吃掉驅動器回應的第一個位元組
    bool idleBiasError = false;         // 模擬閒置線路反覆產生 0xFF
    bool shortWrite = false;            // 為 true 時，只寫入 len-1 個位元組（模擬寫入不完整）
    bool refuse = false;                // 為 true 時，模擬真實 transport 的第三層防線拒絕寫入（回 0、不記錄）
    // 設定後，每次寫入都由它產生回應（優先於 replies 佇列）；回傳空 bytes 表示不回應。
    std::function<ScriptedReply(const std::vector<uint8_t>&)> responder;
    uint32_t now = 0;

    size_t write(const uint8_t* data, size_t len) override {
        if (refuse) return 0;
        if (shortWrite && len > 0) {
            writes.emplace_back(data, data + len - 1);
            totalWritten += len - 1;
            return len - 1;
        }
        writes.emplace_back(data, data + len);
        totalWritten += len;
        if (echo) rx_.insert(rx_.end(), data, data + len);
        if (garbageEcho) rx_.insert(rx_.end(), {0xAA, 0x55, 0xAA});
        if (responder) {
            ScriptedReply r = responder(writes.back());
            if (!r.bytes.empty()) pending_.push_back({now + r.latencyMs, r.bytes});
        } else if (!replies.empty()) {
            ScriptedReply r = replies.front();
            replies.pop_front();
            if (!r.bytes.empty()) pending_.push_back({now + r.latencyMs, r.bytes});
        }
        return len;
    }
    void flushTx() override {}
    void clearRx() override { rx_.clear(); }
    size_t available() override { deliver(); return rx_.size(); }
    int readByte() override {
        deliver();
        if (rx_.empty()) return -1;
        int b = rx_.front();
        rx_.pop_front();
        return b;
    }
    uint32_t nowMs() override { return now; }
    void delayMs(uint32_t ms) override { now += ms; }

private:
    struct Pending { uint32_t at; std::vector<uint8_t> bytes; };
    std::deque<uint8_t> rx_;
    std::vector<Pending> pending_;
    uint32_t nextBiasByteAt_ = 0;
    void deliver() {
        // 每個模擬毫秒最多產生一個位元組，讓接收迴圈能排空 RX；clearRx() 後仍會繼續產生。
        if (idleBiasError && now >= nextBiasByteAt_) {
            rx_.push_back(0xFF);
            nextBiasByteAt_ = now + 1;
        }
        for (size_t i = 0; i < pending_.size();) {
            if (pending_[i].at <= now) {
                const auto& bytes = pending_[i].bytes;
                const auto begin = bytes.begin() + (dropFirstReplyByte ? 1 : 0);
                rx_.insert(rx_.end(), begin, bytes.end());
                pending_.erase(pending_.begin() + static_cast<long>(i));
            } else {
                ++i;
            }
        }
    }
};
