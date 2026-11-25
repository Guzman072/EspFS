// Phase 3 常數（設計預設見 DESIGN.md §3、§4）。通道對應不在這裡：THROTTLE_CH、ARM_CH 在 src/config.h，預設未定義。
#pragma once
#include <cstdint>

constexpr uint8_t kDriveSlaveId = 1;  // 只允許 ID 1（A6 實測）

// 速度
// 速度上限（使用者決定，2026-11-23）：30 → 90 r/min；不得超過 100。
constexpr float kSpeedLimitRpm = 90.0f;
static_assert(kSpeedLimitRpm > 0.0f && kSpeedLimitRpm <= 100.0f, "kSpeedLimitRpm 必須在 (0, 100] r/min");
// 斜坡以「速率」定義（使用者決定，2026-11-23）：每秒 60 r/min＝每 50 ms 加減 3.0 r/min（與上限 30 時 0 → 上限 0.5 s 相同的速率）。
// 上限改成 90 後 0 → 上限約 1.5 s；不再用「固定 0.5 s」，否則速率會變成每 50 ms 9.0 r/min。
constexpr float kSlewRpmPerSec = 60.0f;
constexpr int kDeadband = 40;                               // i-BUS 計數，設計預設
constexpr int kIbusCenter = 1500;
constexpr int kIbusHalfRange = 500;                         // 1000–2000
constexpr int kArmOnThreshold = 1700;                       // SWA 視為打開的門檻，設計預設
// 通道值的有效範圍（strictSafety 用，DESIGN.md §12）：i-BUS 標準範圍 1000–2000，超出就視為資料異常。
constexpr int kChannelValidMin = kIbusCenter - kIbusHalfRange;  // 1000
constexpr int kChannelValidMax = kIbusCenter + kIbusHalfRange;  // 2000
// 左右方向正負號（T3/T4 實測，2026-11-23）：左輪方向正確、右輪相反 → 右輪取反。
// 套用位置只有兩處且成對：寫入 2088h／2089h 前（DriveFsm::doArmed 的 clampRaw）與讀回 20ABh／20ACh 後（DriveLink 的 D 行、DriveFsm 的超速判斷）。
constexpr int kLeftSign = 1;
constexpr int kRightSign = -1;
static_assert((kLeftSign == 1 || kLeftSign == -1) && (kRightSign == 1 || kRightSign == -1), "方向正負號只能是 +1 或 -1");
#ifdef STEER_CH
constexpr bool kSteeringEnabled = true;
#else
constexpr bool kSteeringEnabled = false;
#endif
constexpr float kSteeringGain = 0.25f;
constexpr int kSteerInputSign = 1;                          // 假設右推＝2000，待 dryrun 確認
static_assert(kSteerInputSign == 1 || kSteerInputSign == -1, "轉向輸入極性只能是 +1 或 -1");

// 時序
constexpr uint32_t kControlPeriodMs = 50;
constexpr uint32_t kKeepaliveMaxMs = 100;                   // ARMED 時寫入間隔上限
static_assert(kControlPeriodMs <= kKeepaliveMaxMs, "控制週期不得大於保活上限");
constexpr uint32_t kBlockRetryMs = 1000;                    // 解鎖被驅動器狀態擋住（故障碼、20A2h）後，重新讀取驅動器的最短間隔
constexpr uint32_t kCenterHoldMs = 500;                     // 解鎖前油門在中位的最短時間
constexpr uint8_t kWriteMaxAttempts = 2;                    // 寫入（與 ARMED 內的讀取）最多嘗試次數
constexpr uint32_t kLogEveryNthTick = 10;                   // ARMED 時每 N 個週期才完整記錄交易（避免塞滿 Serial）

// 驅動器掉線保護（2000h）
constexpr uint16_t kOfflineMinMs = 200;
constexpr uint16_t kOfflineMaxMs = 1000;
constexpr uint16_t kOfflineWriteMs = 500;                   // 開機若 2000h 為 0 或超過上限就寫這個值
static_assert(kOfflineWriteMs >= kOfflineMinMs && kOfflineWriteMs <= kOfflineMaxMs, "kOfflineWriteMs 必須在白名單內");

// 故障判斷
// 實際速度超過「指令上限 × 倍數」就視為超速（使用者決定，2026-11-23：1.5 → 1.2；上限 90 時門檻 108 r/min）。門檻由上限與倍率推導（speed.h 的 overspeedLimit01），不寫死數字。
constexpr float kOverspeedFactor = 1.2f;
constexpr int kTorqueStopLimit01A = 30;                     // 3.0 A：懸空測試的手動停止條件（單位 0.1 A），程式只顯示不強制
