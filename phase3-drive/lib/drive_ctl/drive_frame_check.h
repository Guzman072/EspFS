// 第三層防線用的「形狀」檢查：獨立、簡化，刻意不依賴 write_guard。
// 真實 transport（transport_serial2.cpp，只在 live 編譯）在 Serial2 寫入前呼叫；不合格就不寫。
// 只看功能碼、位址、數量與長度；數值範圍由 write_guard 檢查。
#pragma once
#include <cstddef>
#include <cstdint>

bool isAllowedDriveFrameShape(const uint8_t* bytes, size_t len);
