// T2 斷線保護顯示（DESIGN.md §7，接馬達前的硬門檻）：在所有狀態下印出 i-BUS 狀態與「資料是否還在更新」，
// 讓使用者在 dryrun 就能看到「遙控器關機後 i-BUS 是否進入 STALE」。
//   R,ms,status,frame_age_ms,unchanged_ms,throttle,arm   狀態改變時，以及每 1 秒
//   # T2 STALE observed ...                             第一次從 RECEIVING 進入 STALE 時
//   # T2 stale_seen=0|1 frame_age_ms=N values_unchanged_ms=M   每 5 秒
// 判讀：遙控器關機後若 status 一直是 RECEIVING、frame_age_ms 很小、values_unchanged_ms 持續增加，
// 代表接收器仍持續送舊值（不進 STALE）→ T2 不通過，須先處理遙控器 failsafe 才能進 live。
#pragma once
#include "remote_input.h"
#include "sink.h"

class FailsafeMonitor {
public:
    FailsafeMonitor(int throttleCh, int armCh) : throttleCh_(throttleCh), armCh_(armCh) {}
    void update(uint32_t nowMs, IRemote& remote, ILineSink& out);
    bool staleSeen() const { return staleSeen_; }

private:
    int throttleCh_;
    int armCh_;
    bool init_ = false;
    RemoteStatus lastStatus_ = RemoteStatus::WAITING;
    bool sawReceiving_ = false;
    bool staleSeen_ = false;
    uint32_t lastFrameCount_ = 0;
    uint32_t lastFrameAt_ = 0;
    uint16_t snapshot_[kRemoteChannels] = {};
    uint32_t lastChangeAt_ = 0;
    uint32_t lastReportAt_ = 0;
    uint32_t lastT2At_ = 0;
};
