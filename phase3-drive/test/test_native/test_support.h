// Phase 3 測試共用：假傳輸、假驅動器、假遙控器、輸出收集。沿用 Phase 2 的 mock_transport.h 與 test_helpers.h。
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "drive_fsm.h"
#include "mock_transport.h"
#include "test_helpers.h"

class DriveMock : public MockTransport {
public:
    std::vector<std::vector<uint8_t>> driveWrites;  // 經 writeDrive 送出的幀
    std::vector<uint32_t> driveWriteTimes;
    size_t writeDrive(const uint8_t* d, size_t n) override {
        if (refuse) return 0;
        driveWrites.emplace_back(d, d + n);
        driveWriteTimes.push_back(now);
        return write(d, n);
    }
};

class LineLog : public ILineSink {
public:
    std::vector<std::string> lines;
    void line(const char* text) override { lines.emplace_back(text); }
    bool has(const char* needle) const { return indexOf(needle) >= 0; }
    int indexOf(const char* needle, size_t from = 0) const {
        for (size_t i = from; i < lines.size(); ++i)
            if (lines[i].find(needle) != std::string::npos) return static_cast<int>(i);
        return -1;
    }
    int count(const char* needle) const {
        int n = 0;
        for (const auto& l : lines) if (l.find(needle) != std::string::npos) ++n;
        return n;
    }
};

class FakeRemote : public IRemote {
public:
    RemoteStatus st = RemoteStatus::RECEIVING;
    uint16_t ch[kRemoteChannels] = {1500, 1500, 1500, 1500, 1000, 1500, 1500, 1500, 1500, 1500};
    uint32_t frames = 1;
    void poll() override {}
    RemoteStatus status() override { return st; }
    uint16_t channel(int i) override { return (i >= 0 && i < kRemoteChannels) ? ch[i] : 0; }
    uint32_t frameCount() override { return frames; }
};

constexpr int kThrottle = 2;
constexpr int kArm = 4;

// 一個很小的 ZLAC8015D 模擬：能回應讀取、FC 0x06、FC 0x10，並可注入各種故障。
struct FakeDriver {
    int slave = 1;
    std::map<uint16_t, uint16_t> regs;
    uint16_t statusWord = 0x0000;  // 20A2h
    uint16_t faultL = 0, faultR = 0;
    int16_t speedL = 0, speedR = 0;
    int16_t actualVelL = 0, actualVelR = 0;  // 20ABh／20ACh（0.1 r/min）
    bool followTarget = false;
    bool dropAll = false;
    bool dropWrites = false;
    bool corruptMultiReply = false;
    bool exceptionOnWrite = false;
    std::vector<uint16_t> dropReadAddrs;

    FakeDriver() {
        regs[0x2022] = 1; regs[0x2008] = 1000; regs[0x2000] = 1000; regs[0x200F] = 1;
        regs[0x2007] = 0; regs[0x2011] = 5; regs[0x2023] = 1; regs[0x20A0] = 0x0100;
    }

    uint16_t value(uint16_t a) const {
        switch (a) {
            case 0x20A2: return statusWord;
            case 0x20A5: return faultL;
            case 0x20A6: return faultR;
            case 0x20AB: return static_cast<uint16_t>(actualVelL);
            case 0x20AC: return static_cast<uint16_t>(actualVelR);
            case 0x2088: return static_cast<uint16_t>(speedL);
            case 0x2089: return static_cast<uint16_t>(speedR);
            default: {
                auto it = regs.find(a);
                return it == regs.end() ? 0 : it->second;
            }
        }
    }

    ScriptedReply handle(const std::vector<uint8_t>& q) {
        const ScriptedReply none{{}, 0};
        if (dropAll || q.size() < 8 || q[0] != slave) return none;
        const uint8_t fc = q[1];
        const uint16_t addr = static_cast<uint16_t>((q[2] << 8) | q[3]);
        if (fc == 0x03) {
            for (uint16_t a : dropReadAddrs) if (a == addr) return none;
            const uint16_t n = static_cast<uint16_t>((q[4] << 8) | q[5]);
            std::vector<uint16_t> v;
            for (uint16_t i = 0; i < n; ++i) v.push_back(value(static_cast<uint16_t>(addr + i)));
            return {okResponse(q[0], v), 2};
        }
        if (dropWrites) return none;
        if (exceptionOnWrite) return {withCrc({q[0], static_cast<uint8_t>(fc | 0x80), 0x02}), 2};
        if (fc == 0x06) {
            const uint16_t val = static_cast<uint16_t>((q[4] << 8) | q[5]);
            if (addr == 0x200E) {
                if (val == 8) statusWord = 0x4040;
                else if (val == 7) statusWord = 0x0000;
                else if (val == 5) statusWord = 0x8080;
                else if (val == 6) faultL = faultR = 0;
            } else {
                regs[addr] = val;
            }
            return {std::vector<uint8_t>(q.begin(), q.end()), 2};
        }
        if (fc == 0x10) {
            speedL = static_cast<int16_t>((q[7] << 8) | q[8]);
            speedR = static_cast<int16_t>((q[9] << 8) | q[10]);
            if (followTarget) {
                const int d = regs[0x2022] ? regs[0x2022] : 1;
                actualVelL = static_cast<int16_t>(speedL * 10 / d);
                actualVelR = static_cast<int16_t>(speedR * 10 / d);
            }
            std::vector<uint8_t> body{q[0], 0x10, q[2], q[3], q[4], q[5]};
            if (corruptMultiReply) body[3] ^= 0x01;
            return {withCrc(body), 2};
        }
        return none;
    }
};

// 把一個幀描述成短字串：R20A5x2、W200E=0008、S2088=90,90。
inline std::string describe(const std::vector<uint8_t>& f) {
    char b[64];
    const unsigned addr = f.size() >= 4 ? static_cast<unsigned>((f[2] << 8) | f[3]) : 0u;
    if (f.size() >= 2 && f[1] == 0x03) std::snprintf(b, sizeof b, "R%04Xx%u", addr, f[5]);
    else if (f.size() >= 2 && f[1] == 0x06) std::snprintf(b, sizeof b, "W%04X=%04X", addr, (f[4] << 8) | f[5]);
    else if (f.size() >= 11 && f[1] == 0x10)
        std::snprintf(b, sizeof b, "S%04X=%d,%d", addr, static_cast<int16_t>((f[7] << 8) | f[8]), static_cast<int16_t>((f[9] << 8) | f[10]));
    else std::snprintf(b, sizeof b, "?");
    return b;
}

inline std::vector<std::string> describeAll(const std::vector<std::vector<uint8_t>>& frames, size_t from = 0) {
    std::vector<std::string> out;
    for (size_t i = from; i < frames.size(); ++i) out.push_back(describe(frames[i]));
    return out;
}

// 整套系統：假傳輸＋假驅動器＋假遙控器＋DriveLink＋DriveFsm。
struct Rig {
    DriveMock t;
    FakeDriver d;
    FakeRemote r;
    LineLog log;
    DriveLink link;
    DriveFsm fsm;

    // 一般測試用左右同號（+1／+1），讓既有斷言維持「左右寫同一個值」；真實的方向正負號（kLeftSign／kRightSign）
    // 由 realConfig() 另外測，見 test_fsm.cpp 的「左右方向正負號」。
    static FsmConfig defaultConfig() {
        FsmConfig c;
        c.throttleCh = kThrottle;
        c.armCh = kArm;
        c.leftSign = 1;
        c.rightSign = 1;
        return c;
    }
    // 不覆寫正負號：用 FsmConfig 的預設值，也就是 drive_config.h 的 kLeftSign／kRightSign。
    static FsmConfig realConfig() {
        FsmConfig c;
        c.throttleCh = kThrottle;
        c.armCh = kArm;
        return c;
    }
    explicit Rig(const FsmConfig& cfg = defaultConfig()) : link(t, log), fsm(t, link, r, log, cfg) {
        t.responder = [this](const std::vector<uint8_t>& q) { return d.handle(q); };
    }

    // 以 10 ms 為單位推進時間並呼叫 step()。
    void run(uint32_t ms) {
        const uint32_t end = t.now + ms;
        while (t.now < end) {
            t.now += 10;
            fsm.step();
        }
    }
    void bootOnly() { fsm.step(); }
    // SWA 先關閉一下再打開、油門在中位，經過 centerHold 之後狀態機應該解鎖。
    void unlockViaSwitch() {
        r.ch[kArm] = 1000;
        run(100);
        r.ch[kArm] = 2000;
        run(700);
    }
    bool sawFrame(const char* d_) const {
        for (const auto& f : t.writes) if (describe(f) == d_) return true;
        return false;
    }
};
