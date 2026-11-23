// 讀取回應的解析器（IReplyHandler 的讀取版本）。Phase 2 的 Master 與 Phase 3 的 DriveLink 共用。
#pragma once
#include "frame.h"
#include "transactor.h"

class ReadReply : public IReplyHandler {
public:
    ReadReply(uint8_t slave, uint16_t count, Response& out) : slave_(slave), count_(count), out_(out) {}
    Status handle(const uint8_t* buf, size_t len) override { return parseResponse(buf, len, slave_, count_, out_); }

private:
    uint8_t slave_;
    uint16_t count_;
    Response& out_;
};
