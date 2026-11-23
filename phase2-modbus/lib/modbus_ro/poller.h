// 輪詢狀態機：BOOT → PROBE → POLLING → LATCHED（DESIGN.md 第 6 節）。
// 純邏輯：只透過 Master（含 guard）與 ITransport 的時鐘、ILineSink 的輸出運作，可在電腦上用 mock 測試。
#pragma once
#include "master.h"
#include "report.h"
#include "sink.h"

enum class PollState : uint8_t { BOOT = 0, PROBE, POLLING, LATCHED };
const char* pollStateName(PollState s);

struct PollBlock {
    uint16_t addr;
    uint16_t count;
};

struct PollerConfig {
    uint8_t candidates[2] = {1, 4};        // Slave ID：先 1 再 4，不試其他 ID
    const PollBlock* plan = nullptr;       // 每個輪詢週期依序讀取的區塊
    size_t planCount = 0;
    uint32_t periodMs = 200;               // 5 Hz；上一輪沒結束就不會開始下一輪
    static PollerConfig defaults();
};

class Poller {
public:
    Poller(ITransport& transport, Master& master, ILineSink& sink,
           const PollerConfig& cfg = PollerConfig::defaults())
        : t_(transport), m_(master), out_(sink), cfg_(cfg) {}

    // 在 loop() 內反覆呼叫。一次呼叫最多做一個狀態的工作（探測或一輪輪詢）。
    void step();

    PollState state() const { return state_; }
    uint8_t slaveId() const { return slave_; }
    Status latchReason() const { return reason_; }
    uint32_t cycles() const { return cycles_; }

private:
    ITransport& t_;
    Master& m_;
    ILineSink& out_;
    PollerConfig cfg_;
    PollState state_ = PollState::BOOT;
    uint8_t slave_ = 0;
    Status reason_ = Status::OK;
    uint32_t cycles_ = 0;
    uint32_t tickStart_ = 0;
    bool tickStarted_ = false;

    void setState(PollState s);
    void enterLatched(Status reason);
    bool latchedByMaster();
    Status transact(uint8_t id, uint16_t addr, uint16_t count);  // 讀取、輸出 T／D 行；回傳最終狀態
    void doProbe();
    void doPoll();
};
