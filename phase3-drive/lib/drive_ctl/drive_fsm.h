// 狀態機（DESIGN.md §4）：BOOT → DISARMED → ARMING → ARMED → DISARMING；任何故障 → FAULT_STOP → LATCHED。
// ARMING、DISARMING、FAULT_STOP 的交易序列在一次 step() 內執行。
#pragma once
#include "drive_link.h"
#include "failsafe_monitor.h"
#include "remote_input.h"
#include "speed.h"

enum class DriveState : uint8_t { BOOT = 0, DISARMED, ARMING, ARMED, DISARMING, FAULT_STOP, LATCHED };
const char* driveStateName(DriveState s);

enum class FaultReason : uint8_t {
    NONE = 0,
    PROBE_FAIL,      // 開機探測 Slave ID 1 失敗
    DRIVER_FAULT,    // 20A5h／20A6h 非 0（detail 為故障碼）
    COMM_ERROR,      // TIMEOUT、CRC、例外回應等（detail 為 Status）
    WRITE_MISMATCH,  // 寫入回應與請求不符
    OVERSPEED,       // 實際速度超過指令上限 × kOverspeedFactor（目前 1.2 倍，上限 90 時 108 r/min；detail 為 0.1 r/min 的絕對值）
    REJECTED,        // 任何 REJECTED_*（detail 為 Status）
    TX_FAIL,         // transport 沒寫完整個封包
};
const char* faultReasonName(FaultReason r);
FaultReason faultReasonFor(Status s);

struct FsmConfig {
    int throttleCh = -1;  // 未設定（< 0）時永遠不解鎖
    int armCh = -1;
    float speedLimitRpm = kSpeedLimitRpm;
    int deadband = kDeadband;
    int armOnThreshold = kArmOnThreshold;
    uint32_t controlPeriodMs = kControlPeriodMs;
    uint32_t centerHoldMs = kCenterHoldMs;
    uint32_t blockRetryMs = kBlockRetryMs;
    uint16_t offlineWriteMs = kOfflineWriteMs;
    int leftSign = kLeftSign;
    int rightSign = kRightSign;
    bool failsafeVerified = true;  // live：T2 通過後由使用者以 FAILSAFE_VERIFIED=1 指定；dryrun 不需要
    int steerCh = -1;    // 負值代表只用油門
    // 綠板（s3_drive_*，DRIVE_STRICT_SAFETY）啟用的額外安全項（DESIGN.md §12）。預設關閉：esp32dev 行為不變。
    // (a) 解鎖前重寫 2000h 並讀回，不符就不解鎖；(b) 開機先送停止指令並讀回 20A2h 確認；(c) 通道值超出範圍就解除／不解鎖。
    bool strictSafety = false;
};

class DriveFsm {
public:
    DriveFsm(ITransport& transport, DriveLink& link, IRemote& remote, ILineSink& sink, const FsmConfig& cfg);

    void step();  // 在 loop() 內反覆呼叫

    DriveState state() const { return state_; }
    bool stopSampling() const { return stopSampling_; }
    FaultReason faultReason() const { return fault_; }
    uint32_t faultDetail() const { return faultDetail_; }
    bool enabled() const { return enabled_; }
    int resolution() const { return resolution_; }
    int16_t limitRaw() const { return limitRaw_; }
    const FailsafeMonitor& failsafe() const { return monitor_; }

private:
    ITransport& t_;
    DriveLink& link_;
    IRemote& remote_;
    ILineSink& out_;
    FsmConfig cfg_;
    FailsafeMonitor monitor_;
    SlewLimiter leftSlew_;
    SlewLimiter rightSlew_;

    DriveState state_ = DriveState::BOOT;
    FaultReason fault_ = FaultReason::NONE;
    uint32_t faultDetail_ = 0;
    bool enabled_ = true;
    bool everArmed_ = false;
    int resolution_ = 0;
    int16_t limitRaw_ = 0;

    bool tickInit_ = false;
    uint32_t lastTick_ = 0;
    uint32_t lastCmdAt_ = 0;
    uint32_t armedTicks_ = 0;
    bool sawArmOff_ = false;
    bool centerValid_ = false;
    uint32_t centerSince_ = 0;
    int lastNoteKey_ = -1;
    int statusNoteKey_ = -1;     // 20A2h 擋住（或 dryrun 放行）的訊息，各自記住上一次印過的狀態
    bool retryWait_ = false;     // 解鎖被驅動器狀態擋住後，等 blockRetryMs 才再讀
    uint32_t retryAt_ = 0;
    bool stopSampling_ = false;
    uint32_t disarmingAt_ = 0;
    uint32_t disarmedAt_ = 0;
    uint32_t nextStopSampleMs_ = 0;  // 相對於 DISARMED 的取樣時刻
    bool stopReached_[2] = {};

    bool tickDue(uint32_t now);
    uint32_t setState(DriveState s);
    void disable(const char* reason);
    void note(int key, const char* text);
    void noteStatus(int key, const char* text);
    void blockFor(uint32_t now);
    void clearBlock();
    void updateRemoteFlags(uint32_t now);

    void doBoot();
    void doDisarmed(uint32_t now);
    void doArming(uint32_t now);
    void doArmed(uint32_t now);
    void doDisarm(bool armSwitchOff);
    void sampleStop(uint32_t now);
    void doFault(FaultReason reason, uint32_t detail);
    void faultFromStatus(Status s);
    bool readOne(uint16_t addr, uint16_t& value);
    // strictSafety
    bool channelsInRange(int* badChannel = nullptr, unsigned* badValue = nullptr);
    bool bootStop();                 // (b)；false＝已進入 LATCHED
    bool rewriteOfflineBeforeArm();  // (a)；false＝不解鎖（已停用或已 LATCHED）
};
