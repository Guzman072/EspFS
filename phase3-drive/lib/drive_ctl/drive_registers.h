// Phase 3 讀取白名單（逐位址）與解碼。Phase 2 的 registers／guard 不變、不放寬。
#pragma once
#include "frame.h"
#include "registers.h"

// 白名單：2000h–2002h、2007h、2008h、200Fh、2011h、2022h、2023h、2088h、2089h、20A0h–20AEh。
// 請求區間內每個位址都必須在白名單內；數量 1–16。
bool driveReadAllowed(uint16_t start, uint16_t count);

// Slave ID 只允許 1（REJECTED_ID）、數量 1–16（REJECTED_LEN）、位址（REJECTED_ADDR）。
Status validateDriveRead(uint8_t slaveId, uint16_t start, uint16_t count);
Status buildDriveRead(uint8_t slaveId, uint16_t start, uint16_t count, Request& out);

// 解碼一段讀回的暫存器：Phase 3 新增的位址在這裡解，其餘交給 Phase 2 的 decodeBlock。
size_t driveDecodeBlock(uint16_t start, const uint16_t* regs, size_t count, DecodedValue* out, size_t cap);
