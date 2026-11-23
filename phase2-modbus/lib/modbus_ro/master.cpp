#include "master.h"
#include <cstring>
#include "reply_handlers.h"

void Master::readRegisters(uint8_t slaveId, uint16_t startAddr, uint16_t count, Result& out) {
    out = Result();
    if (tx_.latched()) {
        out.status = tx_.latchReason();
        return;
    }
    Request rq;
    Status built = buildReadRegisters(slaveId, startAddr, count, rq);
    if (built != Status::OK) {
        out.status = built;
        tx_.record(built);
        return;
    }

    Response resp{};
    ReadReply handler(slaveId, count, resp);
    const TxSpec spec{rq.bytes, kRequestLen, kFcReadRegisters, 5u + 2u * count, true, kMaxAttempts};
    TxOutcome o;
    tx_.run(spec, handler, o);

    out.status = o.status;
    out.attempts = o.attempts;
    out.sent = o.sent;
    out.rttMs = o.rttMs;
    if (o.status == Status::OK) {
        out.count = resp.count;
        std::memcpy(out.regs, resp.regs, sizeof(uint16_t) * resp.count);
    }
}
