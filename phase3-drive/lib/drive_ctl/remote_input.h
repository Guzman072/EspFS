// 遙控輸入的抽象介面。真實版本包 IBusBM（src/ibus_remote.h，沿用 Phase 1 的狀態判斷），測試版本是假資料。
#pragma once
#include <cstdint>

enum class RemoteStatus : uint8_t { WAITING = 0, RECEIVING, STALE };
const char* remoteStatusName(RemoteStatus s);

constexpr int kRemoteChannels = 10;

class IRemote {
public:
    virtual ~IRemote() = default;
    virtual void poll() = 0;                       // 在 loop() 內反覆呼叫，更新狀態
    virtual RemoteStatus status() = 0;
    virtual uint16_t channel(int index) = 0;       // 0 起算；範圍外回 0
    virtual uint32_t frameCount() = 0;             // 累計收到的有效封包數
};
