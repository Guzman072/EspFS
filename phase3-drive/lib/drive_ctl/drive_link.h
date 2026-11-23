// Phase 3 與驅動器的連線：用共用的 Transactor，guard 換成寫入 guard（讀寫總閘）。
// 所有寫入都先過 WriteGuard，再由 transport 第三層防線檢查；回應逐筆核對。
// 狀態碼語意與 Phase 2 相同：REJECTED_*／TX_FAIL 立即 LATCHED（Transactor 層）。
#pragma once
#include "drive_config.h"
#include "drive_registers.h"
#include "report.h"
#include "sink.h"
#include "write_frame.h"
#include "write_guard.h"
#include "write_rules.h"

// 讀回的轉速（0.1 r/min，有號）換成「油門往上時為正」的方向：與寫入時的正負號成對（寫入 raw＝指令×sign，讀回 raw×sign＝指令方向）。
inline int normalizedVelocity01(uint16_t raw, int sign) { return static_cast<int16_t>(raw) * sign; }

class DriveLink {
public:
    DriveLink(ITransport& transport, ILineSink& sink, uint8_t slaveId = kDriveSlaveId)
        : t_(transport), out_(sink), slave_(slaveId), tx_(transport, guard_) {}

    void setLimitRaw(int16_t limitRaw) { guard_.setLimitRaw(limitRaw); }
    // D 行顯示 actual_velocity_L／R 時套用的方向正負號（預設 +1；T 行永遠是未處理的原始暫存器值）。
    void setVelocitySigns(int left, int right) { leftSign_ = left; rightSign_ = right; }
    int16_t limitRaw() const { return guard_.limitRaw(); }

    // 讀取（FC 0x03，Phase 3 讀取白名單）。regs 至少要有 count 個位置。
    Status read(uint16_t start, uint16_t count, uint16_t* regs, uint8_t attempts = kMaxAttempts);

    // 寫入（只有這四種）。emergency=true：即使已 LATCHED 也送出，只嘗試一次（緊急停止序列用）。
    Status writeSpeedMode();
    Status writeControl(uint16_t word, bool emergency = false);
    Status writeOfflineTime(uint16_t ms);
    Status writeSpeeds(int16_t left, int16_t right, bool emergency = false);

    // false：只記錄失敗的交易（ARMED 的高頻週期用，避免塞滿 Serial）。
    void setLogAll(bool on) { logAll_ = on; }

    bool latched() const { return tx_.latched(); }
    Status latchReason() const { return tx_.latchReason(); }

private:
    ITransport& t_;
    ILineSink& out_;
    uint8_t slave_;
    WriteGuard guard_;
    Transactor tx_;
    bool logAll_ = true;
    int leftSign_ = 1;
    int rightSign_ = 1;

    Status sendWrite(const WriteFrame& f, uint16_t addr, uint16_t count, const uint16_t* values, bool emergency);
    void logT(uint8_t fc, uint16_t addr, uint16_t count, Status st, uint8_t attempts, uint32_t rtt, const uint16_t* values,
              uint8_t nValues);
};
