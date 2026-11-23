#include "drive_fsm.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "write_rules.h"

namespace {
constexpr uint16_t kStatusEnabledMask = 0xC0C0;  // 20A2h：左 bit15–14、右 bit7–6；00 00＝解軸（未使能）

int absInt(int v) { return v < 0 ? -v : v; }

// 20A2h 的軸狀態（V4 RS485 §六、p.18）：每軸 2 位元，左 bit15–14、右 bit7–6；00 解軸、40 鎖軸、80 急停、C0 報警。
const char* axisStateName(unsigned twoBits) {
    switch (twoBits & 3u) {
        case 0: return "released";
        case 1: return "locked";
        case 2: return "estop";
        default: return "alarm";
    }
}

// 兩軸狀態相同時只寫一個名稱，不同時寫 L／R。
void describeStatusWord(uint16_t word, char* out, size_t size) {
    const char* left = axisStateName(word >> 14);
    const char* right = axisStateName(word >> 6);
    if (std::strcmp(left, right) == 0) std::snprintf(out, size, "(%s)", left);
    else std::snprintf(out, size, "(L %s, R %s)", left, right);
}
}  // namespace

const char* driveStateName(DriveState s) {
    switch (s) {
        case DriveState::BOOT: return "BOOT";
        case DriveState::DISARMED: return "DISARMED";
        case DriveState::ARMING: return "ARMING";
        case DriveState::ARMED: return "ARMED";
        case DriveState::DISARMING: return "DISARMING";
        case DriveState::FAULT_STOP: return "FAULT_STOP";
        case DriveState::LATCHED: return "LATCHED";
    }
    return "UNKNOWN";
}

const char* faultReasonName(FaultReason r) {
    switch (r) {
        case FaultReason::NONE: return "NONE";
        case FaultReason::PROBE_FAIL: return "PROBE_FAIL";
        case FaultReason::DRIVER_FAULT: return "DRIVER_FAULT";
        case FaultReason::COMM_ERROR: return "COMM_ERROR";
        case FaultReason::WRITE_MISMATCH: return "WRITE_MISMATCH";
        case FaultReason::OVERSPEED: return "OVERSPEED";
        case FaultReason::REJECTED: return "REJECTED";
        case FaultReason::TX_FAIL: return "TX_FAIL";
    }
    return "UNKNOWN";
}

FaultReason faultReasonFor(Status s) {
    switch (s) {
        case Status::OK: return FaultReason::NONE;
        case Status::WRITE_MISMATCH: return FaultReason::WRITE_MISMATCH;
        case Status::TX_FAIL: return FaultReason::TX_FAIL;
        case Status::REJECTED_FC:
        case Status::REJECTED_ADDR:
        case Status::REJECTED_LEN:
        case Status::REJECTED_ID:
        case Status::REJECTED_CRC:
        case Status::REJECTED_REG:
        case Status::REJECTED_VALUE:
            return FaultReason::REJECTED;
        default: return FaultReason::COMM_ERROR;
    }
}

DriveFsm::DriveFsm(ITransport& transport, DriveLink& link, IRemote& remote, ILineSink& sink, const FsmConfig& cfg)
    : t_(transport), link_(link), remote_(remote), out_(sink), cfg_(cfg), monitor_(cfg.throttleCh, cfg.armCh),
      slew_(cfg.speedLimitRpm, kSlewRpmPerSec) {
    link_.setVelocitySigns(cfg.leftSign, cfg.rightSign);  // 讀回端與寫入端（clampRaw）用同一組正負號
}

void DriveFsm::setState(DriveState s) {
    state_ = s;
    char line[48];
    std::snprintf(line, sizeof line, "S,%u,%s", static_cast<unsigned>(t_.nowMs()), driveStateName(s));
    out_.line(line);
}

void DriveFsm::note(int key, const char* text) {
    if (key == lastNoteKey_) return;  // 同一原因只印一次，避免每個週期都洗版
    lastNoteKey_ = key;
    out_.line(text);
}

void DriveFsm::disable(const char* reason) {
    if (!enabled_) return;  // 保留第一個原因
    enabled_ = false;
    char line[96];
    std::snprintf(line, sizeof line, "# DISABLED reason=%s", reason);
    out_.line(line);
}

void DriveFsm::noteStatus(int key, const char* text) {
    if (key == statusNoteKey_) return;  // 同一個狀態只印一次，狀態改變才再印
    statusNoteKey_ = key;
    out_.line(text);
}

void DriveFsm::blockFor(uint32_t now) {
    retryWait_ = true;
    retryAt_ = now + cfg_.blockRetryMs;
}

void DriveFsm::clearBlock() {
    retryWait_ = false;
    statusNoteKey_ = -1;
}

bool DriveFsm::tickDue(uint32_t now) {
    if (!tickInit_) {
        tickInit_ = true;
        lastTick_ = now;
        return true;
    }
    if (now - lastTick_ >= cfg_.controlPeriodMs) {
        lastTick_ = now;
        return true;
    }
    return false;
}

void DriveFsm::updateRemoteFlags(uint32_t now) {
    const bool recv = remote_.status() == RemoteStatus::RECEIVING;
    const bool configured = cfg_.armCh >= 0 && cfg_.throttleCh >= 0;
    if (!recv || !configured) {
        centerValid_ = false;
        return;
    }
    if (remote_.channel(cfg_.armCh) < cfg_.armOnThreshold) sawArmOff_ = true;
    if (absInt(static_cast<int>(remote_.channel(cfg_.throttleCh)) - kIbusCenter) <= cfg_.deadband) {
        if (!centerValid_) {
            centerValid_ = true;
            centerSince_ = now;
        }
    } else {
        centerValid_ = false;
    }
}

void DriveFsm::step() {
    remote_.poll();
    const uint32_t now = t_.nowMs();
    monitor_.update(now, remote_, out_);
    switch (state_) {
        case DriveState::BOOT: doBoot(); break;
        case DriveState::DISARMED: doDisarmed(now); break;
        case DriveState::ARMED: doArmed(now); break;
        default: break;  // ARMING／DISARMING／FAULT_STOP 在一次 step() 內完成；LATCHED 什麼都不做
    }
}

bool DriveFsm::readOne(uint16_t addr, uint16_t& value) {
    uint16_t r[1] = {0};
    const Status s = link_.read(addr, 1, r);
    if (s != Status::OK) {
        char why[48];
        std::snprintf(why, sizeof why, "read %04X failed: %s", addr, statusName(s));
        disable(why);
        if (link_.latched()) faultFromStatus(s);
        return false;
    }
    value = r[0];
    return true;
}

void DriveFsm::doBoot() {
    out_.line(headerLine());
    char line[48];
    std::snprintf(line, sizeof line, "S,%u,BOOT", static_cast<unsigned>(t_.nowMs()));
    out_.line(line);

    // 1. 只探測 Slave ID 1（讀 20A0h 共 1 個）。失敗 → LATCHED（還沒使能過，不必送停止指令）。
    uint16_t probe[1];
    const Status ps = link_.read(0x20A0, 1, probe);
    if (ps != Status::OK) {
        doFault(FaultReason::PROBE_FAIL, static_cast<uint32_t>(ps));
        return;
    }

    // 2. 預檢讀取（全部輸出供記錄）。任一讀取失敗 → 不啟用。
    uint16_t v2022 = 0, v2008 = 0, v2000 = 0, tmp = 0;
    const bool ok2022 = readOne(0x2022, v2022);
    if (state_ == DriveState::LATCHED) return;
    const bool ok2008 = readOne(0x2008, v2008);
    if (state_ == DriveState::LATCHED) return;
    const bool ok2000 = readOne(0x2000, v2000);
    if (state_ == DriveState::LATCHED) return;
    const uint16_t others[] = {0x200F, 0x2007, 0x2011, 0x2023};
    for (uint16_t a : others) {
        readOne(a, tmp);
        if (state_ == DriveState::LATCHED) return;
    }

    if (ok2022) {
        if (validResolution(v2022)) {
            resolution_ = v2022;
        } else {
            disable("2022h out of range 1-10");
        }
    }
    if (ok2008 && cfg_.speedLimitRpm > static_cast<float>(v2008)) disable("speed limit above 2008h max rpm");

    // 3. 2000h 為 0 或超過上限 → 寫 500 ms（不存檔、不寫 2010h），再讀回確認。
    if (ok2000 && (v2000 == 0 || v2000 > kOfflineMaxMs)) {
        const Status ws = link_.writeOfflineTime(cfg_.offlineWriteMs);
        if (ws != Status::OK) {
            faultFromStatus(ws);
            return;
        }
#ifndef DRIVE_DRYRUN
        // 乾跑沒有真的寫入，驅動器上的值不會變，所以不做讀回檢查
        uint16_t back = 0;
        if (readOne(0x2000, back) && back != cfg_.offlineWriteMs) disable("2000h readback mismatch");
        if (state_ == DriveState::LATCHED) return;
#endif
    }

    // 4. 輸出一次故障碼與狀態字
    uint16_t info[2];
    if (link_.read(0x20A5, 2, info) != Status::OK && link_.latched()) {
        faultFromStatus(link_.latchReason());
        return;
    }
    uint16_t st[1];
    link_.read(0x20A2, 1, st);
    if (link_.latched()) {
        faultFromStatus(link_.latchReason());
        return;
    }

    if (enabled_ && ok2022 && ok2008 && ok2000 && resolution_ > 0) {
        limitRaw_ = limitRawFor(resolution_);
        link_.setLimitRaw(limitRaw_);
    } else {
        disable("preflight incomplete");
    }
    setState(DriveState::DISARMED);
}

void DriveFsm::doDisarmed(uint32_t now) {
    if (!tickDue(now)) return;
    updateRemoteFlags(now);
    if (!enabled_ || cfg_.armCh < 0 || cfg_.throttleCh < 0) return;

    const bool armOn = remote_.channel(cfg_.armCh) >= cfg_.armOnThreshold;
    const bool recv = remote_.status() == RemoteStatus::RECEIVING;
    if (!(armOn && recv)) {
        clearBlock();  // SWA 關或沒有資料：下一次重新打開時，被擋住的原因要重新顯示、立刻重試
        return;
    }
    if (!sawArmOff_) {
        note(1, "# ARM blocked: SWA already on, switch it off first");
        return;
    }
    if (!centerValid_ || now - centerSince_ < cfg_.centerHoldMs) return;
    if (!cfg_.failsafeVerified) {
        note(2, "# ARM blocked: T2 failsafe not verified");
        return;
    }

    // 解鎖條件裡需要讀驅動器的部分：20A5h／20A6h 為 0、20A2h 兩軸皆為解軸。被擋住後至少等 blockRetryMs 才再讀。
    if (retryWait_ && static_cast<int32_t>(now - retryAt_) < 0) return;
    uint16_t faults[2];
    Status s = link_.read(0x20A5, 2, faults);
    if (s != Status::OK) {
        faultFromStatus(s);
        return;
    }
    uint16_t sw[1];
    s = link_.read(0x20A2, 1, sw);
    if (s != Status::OK) {
        faultFromStatus(s);
        return;
    }
    const uint16_t code = static_cast<uint16_t>(faults[0] | faults[1]);
    if (code != 0) {
        char text[64];
        std::snprintf(text, sizeof text, "# would-block: fault 0x%04X", code);
#ifdef DRIVE_DRYRUN
        // 乾跑：故障碼只顯示、不阻擋解鎖（驅動器未接馬達時 20A5h＝0x0200 為預期）
        note(100 + code, text);
#else
        std::snprintf(text, sizeof text, "# ARM blocked: fault 0x%04X", code);
        note(100 + code, text);
        blockFor(now);
        return;
#endif
    }
    const uint16_t axes = static_cast<uint16_t>(sw[0] & kStatusEnabledMask);
    if (axes != 0) {
        char names[32];
        char text[80];
        describeStatusWord(sw[0], names, sizeof names);
        const int key = axes;  // 以兩軸狀態位元當作「狀態」，改變才再印
#ifdef DRIVE_DRYRUN
        // 乾跑：兩軸皆為報警（C0C0）只顯示、不阻擋（未接馬達時 Hall 故障會讓驅動器報警）。40（鎖軸）、80（急停）與混合狀態照擋。
        if (axes == kStatusEnabledMask) {
            std::snprintf(text, sizeof text, "# would-block: 20A2h=%04X %s", static_cast<unsigned>(sw[0]), names);
            noteStatus(key, text);
        } else
#endif
        {
            std::snprintf(text, sizeof text, "# ARM blocked: 20A2h=%04X %s", static_cast<unsigned>(sw[0]), names);
            noteStatus(key, text);
            blockFor(now);
            return;
        }
    }
    clearBlock();
    doArming(now);
}

void DriveFsm::doArming(uint32_t now) {
    setState(DriveState::ARMING);
    everArmed_ = true;  // 從第一筆寫入開始，任何失敗都要送停止指令
    slew_.reset();
    Status s = link_.writeSpeedMode();
    if (s != Status::OK) return faultFromStatus(s);
    s = link_.writeSpeeds(0, 0);
    if (s != Status::OK) return faultFromStatus(s);
    s = link_.writeControl(kCtrlEnable);
    if (s != Status::OK) return faultFromStatus(s);
#ifndef DRIVE_DRYRUN
    // 乾跑沒有真的寫入，驅動器狀態不會改變，所以不做「寫入後預期狀態改變」的檢查
    uint16_t sw[1];
    uint16_t faults[2];
    s = link_.read(0x20A2, 1, sw);
    if (s != Status::OK) return faultFromStatus(s);
    s = link_.read(0x20A5, 2, faults);
    if (s != Status::OK) return faultFromStatus(s);
    if ((sw[0] & kStatusEnabledMask) == 0) return doFault(FaultReason::DRIVER_FAULT, 0);  // 使能沒有生效
    if ((faults[0] | faults[1]) != 0) return doFault(FaultReason::DRIVER_FAULT, static_cast<uint32_t>(faults[0] | faults[1]));
#endif
    lastCmdAt_ = now;
    armedTicks_ = 0;
    setState(DriveState::ARMED);
}

void DriveFsm::doArmed(uint32_t now) {
    if (!tickDue(now)) return;
    const bool recv = remote_.status() == RemoteStatus::RECEIVING;
    const bool armOn = recv && remote_.channel(cfg_.armCh) >= cfg_.armOnThreshold;
    if (!recv || !armOn) return doDisarm(recv);  // SWA 關或 STALE（WAITING 也視為沒有資料）

    link_.setLogAll(armedTicks_ % kLogEveryNthTick == 0);
    ++armedTicks_;

    const float want = throttleToRpm(remote_.channel(cfg_.throttleCh), cfg_.deadband, cfg_.speedLimitRpm);
    const uint32_t dt = now - lastCmdAt_;
    lastCmdAt_ = now;
    const float cmd = slew_.step(want, dt);
    auto clampRaw = [this](float rpm, int sign) {
        int raw = rpmToRaw(rpm * static_cast<float>(sign), resolution_);
        if (raw > limitRaw_) raw = limitRaw_;
        if (raw < -limitRaw_) raw = -limitRaw_;
        return static_cast<int16_t>(raw);
    };
    const int16_t left = clampRaw(cmd, cfg_.leftSign);
    const int16_t right = clampRaw(cmd, cfg_.rightSign);

    char line[80];
    std::snprintf(line, sizeof line, "C,%u,%u,%.1f,%d,%d", static_cast<unsigned>(now),
                  static_cast<unsigned>(remote_.channel(cfg_.throttleCh)), static_cast<double>(cmd), left, right);
    out_.line(line);

    Status s = link_.writeSpeeds(left, right, false);  // 保活：每個週期都寫
    if (s != Status::OK) return faultFromStatus(s);

    uint16_t faults[2];
    s = link_.read(0x20A5, 2, faults, kWriteMaxAttempts);
    if (s != Status::OK) return faultFromStatus(s);
    const uint16_t code = static_cast<uint16_t>(faults[0] | faults[1]);
    if (code != 0) {
#ifdef DRIVE_DRYRUN
        char text[64];
        std::snprintf(text, sizeof text, "# would-block: fault 0x%04X", code);
        note(100 + code, text);
#else
        return doFault(FaultReason::DRIVER_FAULT, code);
#endif
    }

    uint16_t fb[4];
    s = link_.read(0x20AB, 4, fb, kWriteMaxAttempts);  // 速度（左右）與轉矩（左右）
    if (s != Status::OK) return faultFromStatus(s);
    const int limit01 = overspeedLimit01(cfg_.speedLimitRpm);
    // 方向換成「油門往上為正」之後取絕對值：任何方向超過「上限 × kOverspeedFactor」都算超速，正負號不影響判斷
    const int vl = absInt(normalizedVelocity01(fb[0], cfg_.leftSign));
    const int vr = absInt(normalizedVelocity01(fb[1], cfg_.rightSign));
    if (vl > limit01 || vr > limit01) return doFault(FaultReason::OVERSPEED, static_cast<uint32_t>(vl > vr ? vl : vr));
}

void DriveFsm::doDisarm(bool armSwitchOff) {
    setState(DriveState::DISARMING);
    // 正常解除：先 2088h＝0,0，再 200Eh＝0x07（停機）
    Status s = link_.writeSpeeds(0, 0);
    if (s != Status::OK) return faultFromStatus(s);
    s = link_.writeControl(kCtrlStop);
    if (s != Status::OK) return faultFromStatus(s);
    slew_.reset();
    sawArmOff_ = armSwitchOff;  // STALE 造成的解除：必須再看過 SWA 關閉才能重新解鎖
    centerValid_ = false;
    lastNoteKey_ = -1;
    clearBlock();
    setState(DriveState::DISARMED);
}

void DriveFsm::faultFromStatus(Status s) {
    if (state_ == DriveState::FAULT_STOP || state_ == DriveState::LATCHED) return;
    doFault(faultReasonFor(s), static_cast<uint32_t>(s));
}

void DriveFsm::doFault(FaultReason reason, uint32_t detail) {
    if (state_ == DriveState::LATCHED || state_ == DriveState::FAULT_STOP) return;
    setState(DriveState::FAULT_STOP);
    if (everArmed_) {
        // 故障路徑：先 200Eh＝0x05（急停），再 2088h＝0,0；各嘗試一次，不管結果。
        // 通訊本身壞掉時這兩筆也可能失敗，此時靠驅動器的 2000h 掉線保護停機。
        link_.writeControl(kCtrlEmergencyStop, true);
        link_.writeSpeeds(0, 0, true);
    }
    fault_ = reason;
    faultDetail_ = detail;
    setState(DriveState::LATCHED);
    char line[96];
    std::snprintf(line, sizeof line, "# LATCHED reason=%s detail=0x%X", faultReasonName(reason), static_cast<unsigned>(detail));
    out_.line(line);
}
