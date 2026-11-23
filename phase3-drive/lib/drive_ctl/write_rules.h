// 寫入白名單與黑名單的規則（DESIGN.md §2）。builder 與 write_guard 共用這些規則。
#pragma once
#include <cstdint>

constexpr uint16_t kRegOfflineTime = 0x2000;
constexpr uint16_t kRegRunMode = 0x200D;
constexpr uint16_t kRegControlWord = 0x200E;
constexpr uint16_t kRegTargetSpeedLeft = 0x2088;
constexpr uint16_t kModeSpeed = 3;

constexpr uint16_t kCtrlEmergencyStop = 0x05;
constexpr uint16_t kCtrlClearFault = 0x06;  // 在白名單內，但狀態機不會自動送出
constexpr uint16_t kCtrlStop = 0x07;
constexpr uint16_t kCtrlEnable = 0x08;

// 黑名單：2031h、2061h（Hall 偏移角）、2010h（存檔）、2009h（恢復出廠）、2001h、2002h、2030h–207Ah 整段。
bool isBlacklistedReg(uint16_t addr);
bool controlWordAllowed(uint16_t word);      // 0x05、0x06、0x07、0x08
bool offlineTimeAllowed(uint16_t ms);        // 200–1000
bool writeRegAllowedForSingle(uint16_t addr);  // FC 0x06 允許的位址：2000h、200Dh、200Eh
