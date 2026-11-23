#include "failsafe_monitor.h"
#include <cstdio>

const char* remoteStatusName(RemoteStatus s) {
    switch (s) {
        case RemoteStatus::WAITING: return "WAITING";
        case RemoteStatus::RECEIVING: return "RECEIVING";
        case RemoteStatus::STALE: return "STALE";
    }
    return "UNKNOWN";
}

void FailsafeMonitor::update(uint32_t now, IRemote& remote, ILineSink& out) {
    const RemoteStatus st = remote.status();
    const uint32_t frames = remote.frameCount();
    bool changed = false;
    for (int i = 0; i < kRemoteChannels; ++i) {
        const uint16_t v = remote.channel(i);
        if (!init_ || v != snapshot_[i]) {
            snapshot_[i] = v;
            changed = true;
        }
    }
    if (!init_) {
        init_ = true;
        lastFrameCount_ = frames;
        lastFrameAt_ = now;
        lastChangeAt_ = now;
        lastReportAt_ = now;
        lastT2At_ = now;
        lastStatus_ = st;
        sawReceiving_ = (st == RemoteStatus::RECEIVING);
    }
    if (frames != lastFrameCount_) {
        lastFrameCount_ = frames;
        lastFrameAt_ = now;
    }
    if (changed) lastChangeAt_ = now;

    char line[128];
    const bool statusChanged = (st != lastStatus_);
    if (st == RemoteStatus::RECEIVING) sawReceiving_ = true;
    if (st == RemoteStatus::STALE && sawReceiving_ && !staleSeen_) {
        staleSeen_ = true;
        std::snprintf(line, sizeof line, "# T2 STALE observed after %u ms without frames",
                      static_cast<unsigned>(now - lastFrameAt_));
        out.line(line);
    }
    if (statusChanged || now - lastReportAt_ >= 1000) {
        lastReportAt_ = now;
        const unsigned thr = throttleCh_ >= 0 ? remote.channel(throttleCh_) : 0u;
        const unsigned arm = armCh_ >= 0 ? remote.channel(armCh_) : 0u;
        std::snprintf(line, sizeof line, "R,%u,%s,%u,%u,%u,%u", static_cast<unsigned>(now), remoteStatusName(st),
                      static_cast<unsigned>(now - lastFrameAt_), static_cast<unsigned>(now - lastChangeAt_), thr, arm);
        out.line(line);
    }
    if (now - lastT2At_ >= 5000) {
        lastT2At_ = now;
        std::snprintf(line, sizeof line, "# T2 stale_seen=%d frame_age_ms=%u values_unchanged_ms=%u", staleSeen_ ? 1 : 0,
                      static_cast<unsigned>(now - lastFrameAt_), static_cast<unsigned>(now - lastChangeAt_));
        out.line(line);
    }
    lastStatus_ = st;
}
