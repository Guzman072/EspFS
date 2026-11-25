# Phase 3 設計：遙控器低速前進／後退（ESP32 → ZLAC8015D 速度模式）

> **本檔只有設計**：不寫程式、不燒錄、不開序列埠、不上電。寫完等使用者確認，確認前不進入實作。
> 事實與頁碼以 `../PROJECT.md` 為準；本檔引用手冊一律寫 `V4 RS485 p.N`、`V4 驅動器 p.N`（頁碼寫法見 `PROJECT.md` 開頭）。V4 文件經使用者確認對應 V4.2；舊手冊只作參考。
> 「設計預設」代表手冊沒寫、由本設計暫定的值，需實測調整；對應 `PROJECT.md` 第 8 節的待確認項目。
> 本階段**會讓馬達轉動**：受 `AGENTS.md` 規則 9（Phase 3 白名單）、10（上電／接馬達／寫暫存器前先確認）、11（斷線保護）約束。
> **實作狀態（2026-11-23）**：設計經使用者確認並附決定（見 §9）；程式與 native 測試已完成；`dryrun`、`live` 已由使用者燒錄並完成 T1–T4（§7）；右輪方向修正（`kRightSign`＝−1）已實機驗證（§7 T4）。與原設計的差異以「**實作註記**」標示。

## 0. 範圍
- **做**：遙控器油門（前進／後退，左右同速）→ 速度模式。低速（速度上限 90 r/min；原「首次上限 30 r/min」，使用者決定，2026-11-23；使用者未另述原因，調整時間點在 T1–T4 於 30 r/min 實測之後，改為 90）。
- **轉向混控**：使用者於 2026-11-24 確認 §11 與混控 `dryrun` 測試結果，並同意在 `live` 啟用 CH4 混控。`live` 混控已完成懸空驗證，見 [實測紀錄](../docs/TESTLOG.md#2026-11-24)。
- **不做**：位置／轉矩模式、存檔（不寫 2010h）、修改任何驅動參數（Hall 偏移、PID、2022h、2008h 等一律不寫）、自動清故障。
- 前提（來自 `docs/TESTLOG.md` 2026-11-23「無電表的低速懸空測試例外」，原在 `PROJECT.md` 第 5 節）：2026-11-23 使用者決定無電表下做低速**懸空**測試，風險由使用者承擔；條件六項見該節，本設計第 7 節把它們納入流程。

## 1. 架構與檔案配置
沿用 Phase 2 的三層防線（組裝層、`guard`、`transport`），但**寫入與讀取白名單各自獨立成新模組，Phase 2 的唯讀 `guard` 與 `registers` 完全不放寬**（Phase 2 的 `main`、`loopback` 環境行為不變）。

| 位置 | 內容 | 說明 |
|------|------|------|
| `lib/modbus_ro/` | Phase 2（**不變**） | 唯讀 guard、讀位址白名單、master、report |
| `lib/drive_ctl/`（新） | `drive_registers`（Phase 3 讀取白名單）、`write_frame`（只組裝白名單內的四種寫入）、`write_guard`（寫入白名單＋黑名單）、`drive_guard`（讀寫總閘）、`speed`（換算、斜率、死區）、`remote_input`（遙控輸入介面）、`drive_fsm`（狀態機） | 純邏輯，可在 native 測試 |
| `lib/modbus_ro/transport_serial2.cpp` | 真實 transport（仍是唯一碰 `Serial2` 的檔案） | 新增 `writeDrive()`，**只在 `DRIVE_LIVE` 編譯**；第三層防線見 §1.3 |
| `src/drive_main.cpp`（新） | 進入點，只接 USB Serial 與 i-BUS | 與 `poll_main.cpp`、`loopback_main.cpp` 並存 |
| `platformio.ini` | 新增 `dryrun`、`live` 兩個 ESP32 環境 | §5 |

### 1.1 讀取白名單（Phase 3，逐位址）
Phase 2 的範圍（2000h–2002h、20A0h–20AEh）**只增加**：2000h（已含）、**2007h、2008h、200Fh、2011h、2022h、2023h、2088h、2089h**。規則：請求區間內**每個位址**都必須在白名單內（所以 2007h–2008h、2088h–2089h 可以一次讀 2 個，2007h 與 2011h 不能一次跨讀）；數量 1–16；功能碼只有 0x03。20AFh、20B0h 與其他位址仍不在白名單。新增位址必須先更新 `PROJECT.md` 並經使用者確認。

### 1.2 寫入白名單（見 §2）
只有四種寫入幀（200Dh、200Eh、2000h、2088h），只限 Slave ID 1，其餘一律拒絕並單元測試。`write_frame` 只提供四個組裝函式，**沒有接受「功能碼」或「位址」參數的通用函式**：

- `buildSetSpeedMode()`＝FC 0x06、200Dh、值 3（值寫死）
- `buildControlWord(word)`＝FC 0x06、200Eh（`word` 須在白名單）
- `buildSetOfflineTime(ms)`＝FC 0x06、2000h（`ms` 須在 200–1000）
- `buildSetSpeeds(left, right)`＝FC 0x10、起始 2088h、數量 2（左右須在速度上限內）

靜態檢查更新：寫入請求的組裝函式只允許出現在 `lib/drive_ctl/write_frame.*`；`Serial2` 寫入呼叫仍只允許出現在 `transport_serial2.cpp`；巨集 `DRIVE_LIVE` 只允許出現在 `transport_serial2.*`、`drive_main.cpp` 與 `platformio.ini`。

### 1.3 第三層防線（`transport_serial2.cpp`）
`write()`（Phase 2，只送 FC 0x03）**不變**。新增 `writeDrive()`，在 `DRIVE_LIVE` 下才編譯；內部用**獨立、簡化**的檢查（與 `write_guard` 刻意分開實作），不合格就不寫、回 0（上層得到 `TX_FAIL` 並 LATCHED）：
- 功能碼 0x03：同 Phase 2。
- 功能碼 0x06：位址只能是 2000h／200Dh／200Eh，長度 8。
- 功能碼 0x10：起始位址 2088h、數量 2、位元組數 4，長度 13。
- 其他一律不寫。數值範圍由 `write_guard` 檢查，第三層不重複。
- 沿用 Phase 2：寫入前清空接收緩衝並確認線路連續靜默 2 ms，寫入後 flush。

### 1.4 交易引擎（已決定：做法 A，抽共用）
Phase 2 的收發迴圈抽成共用的 `Transactor`（`lib/modbus_ro/transactor.*`），它只接受 `IFrameGuard` 物件：Phase 2 的 `Master` 傳**唯讀 guard**（預設值），Phase 3 的 `DriveLink` 傳 `WriteGuard`。白名單兩邊不共用，Phase 2 的唯讀 guard 與位址表沒有放寬（測試證明 Phase 2 guard 對新位址仍回 `REJECTED_ADDR`）。
- **實作註記**：Phase 2 既有 116 個 native 測試**沒有修改任何斷言、沒有調整呼叫方式**，全數通過（`Master` 的建構子與 `readRegisters`／`latched`／`latchReason`／`consecutiveFailures` 介面不變）。
- **實作註記（專案配置）**：Phase 3 是獨立的 PlatformIO 專案 `phase3-drive/`，用 `lib_extra_dirs = ../phase2-modbus/lib` 取用 Phase 2 函式庫，所以 Phase 2 的 `main`、`loopback` 環境完全不受影響。§1 表格裡的 `lib/drive_ctl/`、`src/drive_main.cpp`、`dryrun`／`live` 環境都在 `phase3-drive/`。
- **實作註記（回音）**：`Transactor` 對 FC 0x06 不丟棄回音（FC 0x06 的正常回應本身就是請求的回音），對讀取與 FC 0x10 照舊丟棄。後果：若通訊模組真的有回音，驅動器沒執行的 FC 0x06 也會看起來成功（測試 `test_fc06_echo_is_indistinguishable_from_acknowledgement` 記錄了這個已知限制）。Phase 2 實測（A6）沒有看到回音現象；FC 0x06 之後的後續讀取（使能狀態、2000h 讀回）與 FC 0x10 的回應核對可以補強。

## 2. 寫入白名單與黑名單
所有寫入先過 `write_guard`，再過 transport 第三層。檢查順序：長度 → Slave ID（只允許 1）→ 黑名單 → 功能碼／位址／數量 → 數值 → CRC。

| 功能碼 | 位址 | 允許的值 | 用途 | 手冊 |
|--------|------|----------|------|------|
| 0x06 | 200Dh | **只允許 3**（速度模式） | 設運行模式 | V4 RS485 p.12、p.5 |
| 0x06 | 200Eh | **只允許 0x05、0x06、0x07、0x08** | 急停／清故障／停機／使能 | V4 RS485 p.12、p.6–7 |
| 0x10 | 起始 2088h，數量 2，位元組數 4 | 左、右各為 I16，**各限制在 ±速度上限原始值內**（§3） | 目標速度 | V4 RS485 p.18、p.5 |
| 0x06 | 2000h | **只允許 200–1000**（ms） | 通訊掉線保護時間 | V4 RS485 p.11 |

- 200Eh 的 0x10／0x11／0x12（位置模式啟動）、0（未定義）**不在白名單**。0x06（清故障）雖在白名單，**狀態機不會自動送出**（§4：故障後不自動清）；它預留給之後使用者明確同意的手動流程，並有測試證明狀態機從不發出 0x06。
- **黑名單（明列、單元測試；即使白名單邏輯有誤也要擋）**：2031h、2061h（Hall 偏移角）、2010h（存檔）、2009h（恢復出廠）、2001h（Node ID）、2002h（Baud）、**2030h–207Ah 整段**（電機參數、PID、加減速參數等）。另外 2022h、2008h、200Fh、2007h、2011h、2023h 不在寫入白名單（只讀）。
- 回應核對：FC 0x06 回應必須與請求**逐位元組完全相同**；FC 0x10 回應必須是 `ID 10 20 88 00 02` 加 CRC（V4 RS485 p.5 例）。不符 → `WRITE_MISMATCH`（§4 的故障路徑）。
- 狀態碼新增：`REJECTED_REG`（位址不在白名單或在黑名單）、`REJECTED_VALUE`（值不在允許集合）、`WRITE_MISMATCH`。`REJECTED_*` 與 `TX_FAIL` 的處理同 Phase 2：立即 LATCHED。

## 3. 速度
- **速度上限 `kSpeedLimitRpm = 90`**（常數；`static_assert(kSpeedLimitRpm <= 100)`，編譯期禁止超過 100，也禁止 ≤0）。**使用者決定（2026-11-23）：原「首次上限 30 r/min」調高為 90 r/min**（使用者決定，2026-11-23；使用者未另述原因，調整時間點在 T1–T4 於 30 r/min 實測之後）。
- **單位換算**：開機先讀 2022h（V4 RS485 p.14：最終分辨率＝1／設定值 r/min，範圍 1–10（0x0A））。設定值 D，則寫給驅動器的原始值 `raw = round(rpm × D)`（上限 90 r/min 時：D＝1 → 90；D＝10 → 900）。**讀不到、回應異常或 D 不在 1–10 → 不啟用**（永遠停在 DISARMED，輸出原因）。原始值上限 `limitRaw = round(kSpeedLimitRpm × D)`（90 × D），`write_guard` 以它檢查左右值；`limitRaw` 未設定時為 0（只允許寫 0）。本程式**不寫 2022h**（手冊說修改後須存檔重啟才有效，V4 RS485 p.7）。
- 開機也讀 2008h（最高轉速，預設 1000 r/min，V4 RS485 p.11）；若 `kSpeedLimitRpm` 大於它 → 不啟用。
- **斜率**：ESP32 端把目標速度做斜率限制；加速與減速都套用。**以「速率」定義：`kSlewRpmPerSec = 60`＝每 50 ms 加減 3.0 r/min（使用者決定，2026-11-23）。** 原設計是時間定義「0 → 上限 0.5 s」（`kSlewSeconds = 0.5`，上限 30 時等於 60 r/min／s）；上限改 90 後若仍用 0.5 s，速率會變成每 50 ms 9.0 r/min，所以改成維持原速率，0 → 上限約 1.5 s。安全停止（§4）不經斜率，直接寫 0（驅動器自己還有 S 形減速，預設 500 ms，V4 RS485 p.17）。
- **死區**：油門離中位 ±`kDeadband` 內視為 0（設計預設 40，約 4%，需實測）。
- **i-BUS 輸入**：範圍 1000–2000，**1500 為中位**。對應：`x = value − 1500`；`|x| ≤ kDeadband` → 0；否則 `target = sign(x) × (|x| − kDeadband) / (500 − kDeadband) × kSpeedLimitRpm`。
- **通道對應做成常數，不假設**：`kThrottleChannel`、`kArmSwitchChannel`（SWA）預設為「未設定」，未設定時狀態機永遠不解鎖。Phase 1 的 CH1～CH10 對應尚未驗證（`PROJECT.md` 第 5 節），必須由使用者確認後才填入。`kArmSwitchOnThreshold`（SWA 視為打開的門檻，設計預設 1700）與 `kLeftSign`、`kRightSign`（左右方向正負號，預設 +1，**實測前不知道**）同為常數。
- 未定義 `STEER_CH` 時，左右輪使用同一車體速度目標。`dryrun`、`live` 均定義 `STEER_CH=3`，並依 §11 分別計算兩輪目標。

## 4. 狀態機

```
BOOT ─(預檢通過)→ DISARMED ─(解鎖條件全滿足)→ ARMING ─(序列成功)→ ARMED
                       ↑                                              │
                       └──────── DISARMING（SWA 關／STALE）←──────────┤
                                                                      │
        任何狀態（含 BOOT 預檢失敗）──(故障)──→ FAULT_STOP → LATCHED（需重開機）
```

**BOOT／預檢**（不使能、不寫速度）：
1. 只探測 **Slave ID 1**（讀 20A0h 共 1 個；ID 已實測為 1，不再試 4）。逾時 → LATCHED。
2. 讀 2022h、2008h、2000h、200Fh、2007h、2011h、2023h，全部輸出到 Serial 供記錄（`T`／`D` 行）；任一讀取失敗 → 不啟用。
3. **2000h 為 0 或大於 1000 → 寫入 500 ms**（`buildSetOfflineTime(500)`，不存檔、**不寫 2010h**），再讀回確認＝500；其餘值（200–1000）不動。理由：驅動器預設 0，0 的意義手冊沒說明（`PROJECT.md` 項 16、26），而 Phase 3 的保活間隔 ≤100 ms，500 ms 的掉線保護可以涵蓋最壞一次交易（約 340 ms）。
4. 讀 20A5h／20A6h、20A2h 輸出一次。預設狀態是 **DISARMED（開機預設）**。

**解鎖條件（全部成立才進入 ARMING）**：
- SWA 打開（`kArmSwitchChannel` 已設定且值 ≥ `kArmSwitchOnThreshold`），**且開機後或上次解除後曾看過 SWA 關閉**（開機時 SWA 已在打開位置不能直接解鎖）。
- 油門在中位（死區內）**連續 ≥ 0.5 s**。
- i-BUS 狀態為 `RECEIVING`（非 `WAITING`、`STALE`）。
- 20A5h＝0 且 20A6h＝0。
- 20A2h 兩軸皆為**解軸**：右 bit7–6 與左 bit15–14 皆為 `00`（V4 RS485 §六、p.18：`00 00` 解軸、`40 40` 鎖軸、`80 80` 急停、`C0 C0` 報警）。任何非 `00` 都擋住，訊息寫真實狀態名稱，例如 `# ARM blocked: 20A2h=4040 (locked)`、`(estop)`、`(alarm)`，兩軸不同時寫 `(L alarm, R locked)`。**dryrun 例外（使用者決定，只在 `DRIVE_DRYRUN` 編譯）**：兩軸皆為 `C0 C0`（報警，例如沒接馬達的 Hall 故障）只顯示 `# would-block: 20A2h=C0C0 (alarm)` 並繼續解鎖；`40`、`80` 與混合狀態仍然擋住。
- **擋住時的重試**：解鎖被驅動器狀態（故障碼、20A2h）擋住後，至少等 1 s（`kBlockRetryMs`）才再讀驅動器；同一個原因（同一個故障碼、同一個 20A2h 值）只印一次，值改變才再印；SWA 撥關再開時重新顯示並立即重試。
- §3 的預檢全部通過（`limitRaw` 已設定）。

**解鎖序列（ARMING）**，每一步都核對回應（§2），任一失敗 → FAULT_STOP：
`200Dh=3` → `2088h=0,0` → `200Eh=0x08`；之後讀 20A2h 確認已使能（bit 不再是 `00`）且 20A5h／20A6h＝0，才進入 ARMED。

**ARMED**：控制週期 50 ms。每個週期：①取遙控輸入、算目標與斜率 → ②寫 `2088h`（**保活：寫入間隔 ≤100 ms**，驅動器看到任何指令就重置 2000h 計時）→ ③讀 20A5h 起 2 個 → ④讀 20ABh 起 2 個（實際速度）。每個交易的最壞時間見 Phase 2（約 340 ms）；速度寫入最多嘗試 2 次（`kWriteMaxAttempts=2`，間隔 20 ms；寫同樣的速度重送是安全的），逾時視為通訊錯誤。

**SWA 關或 STALE（正常解除）**：先 `2088h=0,0`，再 `200Eh=0x07`（停機），核對回應後回 **DISARMED**；要再動必須重新解鎖（含「看過 SWA 關閉」）。

**停機紀錄（2026-11-24 使用者選 B）**：`ARMING` 的三筆與 `DISARMING` 的兩筆寫入一律印 `T` 行。`dryrun` 仍只印出寫入幀，對應的 `T` 行用 `OK,0,-` 表示 guard 通過、未傳送。`ARMED` 仍每 10 個週期完整記錄一次。

正常解除完成、進入 `DISARMED` 後，從第一個可用的迴圈開始，在 3 秒內每 50 ms 以 FC 0x03 讀取 20ABh–20AEh 並印 `D` 行。各輪首次量到絕對速度小於 1.0 r/min 時，印 `# STOP L ms=<距 DISARMING 的毫秒>` 或 `# STOP R ms=...`。在視窗內未達門檻的輪子印 `# STOP L not_reached` 或 `# STOP R not_reached`。若讀取延遲而錯過取樣時段，跳過該時段。取樣完成前禁止重新解鎖，期間仍追蹤遙控器中位和 SWA 狀態。若取樣讀取失敗，回報並進入 `LATCHED`，結束取樣且不再通訊。

**故障路徑（FAULT_STOP → LATCHED，需重開機，不自動清故障）**：下列任一成立：
- 20A5h／20A6h 任一非 0；
- 通訊錯誤：任何交易最終為 `TIMEOUT`／`CRC_ERR`／`SHORT_FRAME`／`TX_FAIL`，或連續失敗達 Phase 2 的 LATCHED 條件；
- 寫入回應與請求不符（`WRITE_MISMATCH`）；
- 實際速度 20ABh／20ACh 的絕對值超過**指令上限 × `kOverspeedFactor`**（單位 0.1 r/min）。**使用者決定（2026-11-23）：倍率由 1.5 改為 1.2；上限 90 r/min 時門檻 108 r/min＝1080**（原：上限 30、1.5 倍、門檻 45 r/min＝450）。門檻由上限與倍率推導（`overspeedLimit01`），不寫死數字；
- 任何 `REJECTED_*`。
動作：先送 `200Eh=0x05`（急停，停止並保持使能，V4 RS485 p.7），再 `2088h=0,0`，**各嘗試一次或兩次後不再等待**（通訊本身壞掉時這兩筆也可能失敗，此時靠驅動器的 2000h 掉線保護停機），然後 LATCHED：**之後不再送出任何請求**。狀態機不送 0x06（不清故障）。

**停機延遲前提（2026-11-24，90 r/min 懸空實測）**：拔 i-BUS 線後，距最後一筆訊框 250 ms 出現 `STALE`，再過 7 ms 進入 `DISARMING`。各輪從 `DISARMING` 到首次量到絕對輪速低於 1.0 r/min 為 377–578 ms。審查更正，SWA 撥上的第二次取樣為 478／578 ms（左／右），直行時關遙控器為 477／527 ms。遙控器關機到失效保護生效的延遲未從 log 量到。這些是懸空樣本，落地前的停機方式仍待使用者決定。[實測紀錄](../docs/TESTLOG.md#90-rmin-斷線停止時間)

## 5. 乾跑與正式環境
- **`dryrun`**：**不編譯任何寫入路徑**（沒有 `DRIVE_LIVE`，`Serial2Transport::writeDrive()` 不存在，二進位檔裡也沒有這個符號；`ITransport::writeDrive()` 的預設實作是拒絕）。照常讀 i-BUS，讀驅動器走 Phase 2 的真實唯讀 transport（FC 0x03）；**所有將要送出的寫入幀只印出**（`# DRY W 01 06 20 0D 00 03 53 C8`），不呼叫任何寫入傳輸。寫入幀仍先過 `WriteGuard`，被拒絕時的處置（`REJECTED_*` → LATCHED）與正式環境相同。
- **dryrun 專用旗標（使用者決定，只在 dryrun 編譯、`live` 不得存在）**——編譯期巨集 `DRIVE_DRYRUN`，原始碼在 `#ifdef DRIVE_DRYRUN` 區塊內，`live` 編譯時這些程式碼不存在（靜態測試檢查 `live` 環境沒有這個旗標、`config.h` 禁止兩者同時定義）：
  - **(a) 故障碼只顯示、不阻擋解鎖**：驅動器未接馬達時 20A5h＝0x0200 為預期。印出 `# would-block: fault 0x0200`（同一個故障碼只印一次）。ARMED 期間故障碼同樣只顯示、不進入故障路徑。其他條件（通訊錯誤、`REJECTED_*`、`TX_FAIL`）在 dryrun 仍然有效。
  - **(b) 不做「寫入後預期驅動器狀態改變」的檢查**：寫入沒送出，驅動器狀態不會變，否則會誤判 LATCHED。具體是：ARMING 之後不讀 20A2h 確認使能、開機寫 2000h 之後不讀回確認。（解鎖前「20A2h 必須是解軸」是對驅動器**真實**狀態的檢查，dryrun 仍然保留，唯一例外是下一條的 C0C0 報警。）
  - **(c) 20A2h 兩軸皆報警（`C0 C0`）只顯示、不阻擋解鎖**：印出 `# would-block: 20A2h=C0C0 (alarm)`（同一個值只印一次）。驅動器沒接馬達時 Hall 故障會讓驅動器報警，這是預期。`40`（鎖軸）、`80`（急停）與兩軸不同的混合狀態仍然擋住。`live` 完全不變：任何非 `00` 都擋。
- **`live`**：定義 `DRIVE_LIVE`，啟用 `writeDrive()` 與寫入路徑；`ARMED` 的故障、使能確認等檢查全部有效。**須使用者另外明確確認才可燒錄**（AGENTS.md 規則 10、11）。
- **通道常數與編譯期擋閘（使用者決定）**：`THROTTLE_CH`、`ARM_CH` 在 `src/config.h` **預設未定義**，未定義時 `#error` 擋住編譯；使用者之後以一句話指定（寫進 `platformio.ini` 對應環境的 `build_flags`，例如 `-DTHROTTLE_CH=2 -DARM_CH=4`）。Claude 不自行假設通道。
- **實作註記（T2 閘門的程式化）**：`live` 另外要求 `-DFAILSAFE_VERIFIED=1`，未定義時 `#error`。使用者在 T2 通過後才指定。這把「T2 是接馬達前的硬門檻」變成編譯期條件（`live` 不能在沒有使用者聲明 T2 通過的情況下編譯）。`dryrun` 不需要。**T2 的判定已於 2026-11-23 修訂為 (a)(b) 兩項，以 §7 為準。**
- 兩個環境都只做 `pio run` 編譯驗證，直到使用者同意燒錄；不執行 upload、不 monitor。native 測試有兩個環境：`native`（live 語意）與 `native_dryrun`（`-DDRIVE_DRYRUN`），都要跑。

## 6. native 測試計畫（TDD，先寫測試）
`mock transport` 沿用 Phase 2；新增假遙控輸入與假驅動器。
| 類別 | 測試內容 |
|------|----------|
| 寫入白名單 | **每一條允許與拒絕**：200Dh 只有 3 通過（0、1、2、4 拒絕）；200Eh 只有 0x05／0x06／0x07／0x08 通過（0、1、0x10、0x11、0x12、0xFFFF 拒絕）；2088h 數量 2 通過、數量 1／3 拒絕、左右各邊界（±limitRaw 通過，±(limitRaw+1) 拒絕）；2000h 只有 200–1000 通過（199、1001、0 拒絕）；Slave ID 非 1（0、2、4、127、128）一律拒絕；FC 0x00–0xFF 逐一測試，只有 0x03／0x06／0x10 進入後續檢查，其餘全拒絕；mock 寫入位元組數為 0 |
| 黑名單 | 2031h、2061h、2010h、2009h、2001h、2002h，以及 2030h–207Ah 全段逐位址：FC 0x06 與 0x10 都拒絕且寫入位元組為 0 |
| 讀取白名單 | Phase 3 新增 9 個位址各通過；跨讀非連續位址（2007h 與 2011h）、20AFh、20B0h 拒絕；Phase 2 的 guard 對這些新位址**仍然拒絕**（證明 Phase 2 沒放寬） |
| 回應核對 | FC 0x06 回應逐位元組相等才算成功；FC 0x10 回應核對起始位址與數量；任何不符 → `WRITE_MISMATCH` → 故障路徑 |
| 2022h 換算 | D＝1、2、5、10 的 `raw` 與 `limitRaw`；D＝0、11、讀取失敗 → 不啟用；2008h 小於上限 → 不啟用；`kSpeedLimitRpm` 的 `static_assert`（以編譯期測試或靜態文字檢查驗證） |
| 斜率與死區 | 斜坡以速率定義（每 50 ms 3.0 r/min，0→上限約 1.5 s；以假時鐘逐步檢查）、減速同；超速門檻由上限與倍率推導（90 × 1.2 → 1080）；死區邊界（±kDeadband）；中位與兩端極值的對應；方向正負號常數 |
| 狀態機轉移 | BOOT 預檢失敗（讀不到、2022h 異常）→ 不啟用；開機即 SWA 打開 → 不解鎖，看過關閉後才解鎖；中位不足 0.5 s、i-BUS 非 RECEIVING、任一故障碼非 0、20A2h 已使能 → 不解鎖；解鎖序列的幀順序（200Dh、2088h、200Eh）與回應核對；ARMED 保活間隔 ≤100 ms；SWA 關／STALE → `2088h=0,0` 後 `200Eh=0x07` → DISARMED；故障路徑 → 先 0x05 再 `2088h=0,0` → LATCHED 且之後 mock 寫入位元組不再增加；實際速度超過上限 1.5 倍 → 故障路徑；狀態機從不發出 0x06、0x10 以外的未允許幀；2000h 為 0 或 >1000 時寫 500，200–1000 時不寫 |
| 輸出 | `T`（含 FC 0x06／0x10）、`C`（指令）、`S` 行逐字比對；`dryrun` 只印 `# DRY W` 而 mock 寫入位元組為 0 |
| 靜態檢查 | 見 §1.2：寫入組裝函式、`Serial2` 寫入、`DRIVE_LIVE` 出現位置；`dryrun` 環境不含 `writeDrive` |
| 回歸 | Phase 2 全部測試仍通過 |

輸出格式沿用 Phase 2 的 `T`／`D`／`S`，新增 `C,ms,throttle,target_rpm,left_raw,right_raw`（指令行），`S` 的狀態值為 `BOOT`、`DISARMED`、`ARMING`、`ARMED`、`DISARMING`、`FAULT_STOP`、`LATCHED`。

## 7. 懸空測試流程與停止條件
前提：使用者決定的例外（`docs/TESTLOG.md` 2026-11-23）；**每個步驟前先向使用者確認**（規則 10）。

**準備（全程斷電）**
1. 數電池平衡接頭針數並記錄（6S＝7、7S＝8）；記下電池型號與標示的充電上限。
2. 馬達接頭與銘牌核對：J2／J6（編碼器與 Hall）與動力端子（V4 驅動器 p.9–10），左右順序鏡像，逐一對照絲印；核對馬達額定電壓（目前只有使用者回報的 DC 24 V，銘牌未核對）。
3. **有直流開關，裝在電池正極，額定未知（使用者 2026-11-25 確認，之後所有板子都會有）。** 舊文（歷史，2026-11-23，已由 2026-11-25 確認取代）：供電路徑是電池 → XT60（手動插拔）→ 驅動器，沒有獨立 DC 開關、沒有額外保險絲（使用者聲明，2026-11-23）；原「DC 開關額定」條件不再適用。
4. **有直流開關，裝在電池正極，額定未知（使用者 2026-11-25 確認，之後所有板子都會有）；緊急停車順序：SWA、遙控器關機、直流開關、最後拔 XT60（使用者 2026-11-25 指示）（馬達轉動時斷開電池正極，再生能量無去處；見 `PROJECT.md`「現況與下一步」）。** 舊句（歷史，2026-11-25，已由同日統一順序取代）：「緊急停車先用 SWA，直流開關為最後手段。」 舊文（歷史，2026-11-23，已由 2026-11-25 確認取代）：緊急斷電方式是**拔 XT60**（取代原「保險絲維持 5 A」條件）。
5. 輪子離地懸空、人在場、**手放在直流開關上**（使用者 2026-11-25 指示）。舊文（歷史，2026-11-23，已由 2026-11-25 指示取代）：「手放在 XT60 上，隨時準備拔掉。」
6. 接線全程斷電；通電時不得接線或拆線。

**步驟**
- **T1 乾跑**：`dryrun` 環境，驅動器上電、馬達已接；確認 20A5h／20A6h＝0（Hall 故障應消失）、20A2h 為解軸、2022h／2008h／200Fh／2007h／2000h 的實際值，記錄 dc_voltage 讀值；轉動油門與 SWA，檢查印出的指令與換算。
  - **實測結果（2026-11-23，dryrun，馬達與 Hall 已接、懸空；log：`logs/device-monitor-261123-152439.log`、`logs/device-monitor-261123-153342.log`（2026-11-24 復原並已提交）；使用者判定通過）**：開機 20A5h／20A6h＝0000、20A2h＝0000（解軸），Hall 故障消失；解鎖無 `would-block`，`ARMING`→`ARMED`；預檢讀值 2022h＝1、2008h＝1000、2000h＝0、200Fh＝1、2007h＝0、2011h＝5、2023h＝0。手轉左輪回授最大 +34.1／−25.1 r/min，超過 45 r/min（讀到 48.3）→ `FAULT_STOP`（`200Eh=0x05`、`2088h=0,0` 只印出）→ `LATCHED reason=OVERSPEED detail=0x1E3`；手轉右輪 −54.8 r/min → `LATCHED detail=0x224`。轉矩全程 0.0 A。**軟體超速保護在實機有效**（2023h＝0，驅動器本身不檢查超速）。方向正負號仍未知，留待 T4。log 沒有 dc_voltage 讀值，電壓仍待電表。電池平衡線 7 針（使用者數）→ 6S，與 dc_voltage 27.83 V 仍不一致（`PROJECT.md` 項 22、25）。
- **T2 斷線保護驗證（規則 11；接馬達之前的硬門檻，使用者決定；判定已於 2026-11-23 修訂）**：
  - 背景（使用者回報的 Phase 1 觀察，Claude 未見輸出）：遙控器關機後，FS-iA6B 持續送舊值，`status` 仍是 `RECEIVING`、CH2 凍結，**所以 `RECEIVING` 不能單獨當遙控器在線的依據**。使用者已在 FS-i6X 設定失效保護（CH2→1500、CH7→1000，皆 ON，通道數 7），Phase 1 重測 3/3：關機後 CH7 在 1 秒內回 1000、CH2 回 1500；重開機後恢復。**這只是 Phase 1 觀察，`dryrun` 尚未驗證，不得寫成已通過。**
  - **通過條件（兩者都要在 `dryrun` 看到才算 T2 完成）**：
    - **(a)** 遙控器關機後，`ARM_CH`（`R` 行最後一欄）在 1 秒內低於 1700，且狀態機因 SWA 關而停機：ARMED 時依序印出 `S,…,DISARMING`、`# DRY W 01 10 20 88 …`（2088h＝0,0）、`# DRY W 01 06 20 0E 00 07 …`（200Eh＝0x07）、`S,…,DISARMED`。
    - **(b)** 拔 i-BUS 訊號線或接收器斷電時，`status` 進入 `STALE`（`R` 行、`# T2 STALE observed …`）並停機：ARMED 時同樣印出上述停止序列。
  - 輸出行：`dryrun` 在所有狀態下輸出 `R,ms,status,frame_age_ms,unchanged_ms,throttle,arm`（狀態改變時與每 1 秒）、`# T2 STALE observed after N ms without frames`（第一次從 RECEIVING 進入 STALE 時；字面刻意不含 PASS）、`# T2 stale_seen=0|1 frame_age_ms=N values_unchanged_ms=M`（每 5 秒）。**注意：`# T2 STALE observed` 這一行只代表 (b) 的 STALE 觀察，不代表 (a) 或整個 T2 通過**（字面刻意不含 PASS，T2 是否通過以本節 (a)(b) 為準）。(a) 靠 `R` 行的 `arm` 欄與 `S`／`# DRY W` 行判讀；`R` 行每 1 秒才印一次，「1 秒內」的時間精度以 Phase 1 的 100 ms CSV 觀察為準，`dryrun` 驗證的是狀態機確實因此停機。
  - **不通過**：(a) 遙控器關機後 `arm` 欄沒有低於 1700（或狀態機沒有停機）；(b) 拔線或接收器斷電後 `status` 一直是 `RECEIVING`。任何一項不通過，須先處理失效保護或接收器設定才能進 live；通過之前不得接馬達、不得指定 `FAILSAFE_VERIFIED`。
  - **實測結果（2026-11-23，dryrun，馬達未接；log：`logs/device-monitor-261123-144822.log`、`logs/device-monitor-261123-150455.log`（2026-11-24 復原並已提交）；使用者判定 T2 通過）**：
    - **(a)** 遙控器關機 → `S,…,DISARMING` → `# DRY W 01 10 20 88 00 02 04 00 00 00 00 63 A8` → `# DRY W 01 06 20 0E 00 07 A2 0B` → `S,…,DISARMED`；從最後一次值變化到 `DISARMING` 約 1.05 s，是**上限**（關機時刻無法精確得知；`R` 行每秒一行，分辨不出 1 秒內）。Phase 1 的 100 ms CSV 重測 3/3 在 1 秒內（使用者回報）。
    - **(b)** ARMED 時拔 i-BUS 訊號線 → 250 ms 後 `STALE`（`# T2 STALE observed after 250 ms without frames`）→ 同一週期（相同時間戳）`DISARMING` → 停止序列 → `DISARMED`；插回後 SWA 仍開 → `# ARM blocked: SWA already on, switch it off first`。
    - 其他：(d) 開機時 SWA 已開被擋、撥關再開才解鎖；解鎖序列三個幀逐字正確；速度最大 ±30（`00 1E 00 1E`／`FF E2 FF E2`），斜坡每 50 ms 3.0 r/min，回中歸 0。
    - **使用者隨後（2026-11-23）聲明 T2 (a)(b) 通過並指定 `FAILSAFE_VERIFIED=1`，已寫進 `live` 的 build_flags；`live` 已編譯成功，尚未燒錄（燒錄與啟動馬達仍須使用者另行確認，規則 10、11）。**
- **dryrun 測試清單（驅動器上電、馬達不接；使用者操作，Claude 只提供判讀；以下為依狀態機程式推導的預期，尚未在硬體上看過）**：
  - **(a) 解鎖→推 CH2→關遙控器**：ARMED 後遙控器關機，失效保護使 `arm`＝1000、`throttle`＝1500，`status` 仍是 `RECEIVING`；預期 `S,…,DISARMING`、`# DRY W 01 10 20 88 00 02 04 00 00 00 00 63 A8`、`# DRY W 01 06 20 0E 00 07 A2 0B`、`S,…,DISARMED`。此時「看過 SWA 關閉」成立。
  - **(b) 遙控器重新開機、SWA 仍在 ON、CH2 在中位**：依 `doDisarmed`，**會自動重新解鎖**：失效保護期間 CH2 已是 1500（`centerValid_` 成立且已超過 0.5 s）、`sawArmOff_` 已成立，遙控器恢復後 SWA 讀到 2000、`RECEIVING`，條件全滿足，下一個控制週期就會走 `ARMING`（`200Dh=3`→`2088h=0,0`→`200Eh=0x08` 只印出）→`ARMED`。若 CH2 不在中位則要等回中 ≥0.5 s。是否加規則（例如要求重新開機後必須再撥一次 SWA 關閉）由使用者決定（`PROJECT.md` 項 31）；程式未改。**實測觀察（使用者回報，2026-11-23）**：FS-i6X 開機時若 SWA 不在原位，開機開關警告會鳴叫且不發送訊號，接收器維持失效保護值（`arm`＝1000），所以遙控器重開後**不會**自動解鎖；**不得關閉遙控器的開機開關警告**。
  - **(c) 拔 i-BUS 訊號線**：≤250 ms 後 `status` 變 `STALE`，印 `# T2 STALE observed after N ms without frames`（只代表 (b) 的 STALE 觀察）；若當時 ARMED，印出與 (a) 相同的停止序列。與 (a) 不同：STALE 造成的解除不算「看過 SWA 關閉」，接回線後若 SWA 仍在 ON，會印 `# ARM blocked: SWA already on, switch it off first`，必須撥關再開。
  - **(d) 開機時 SWA 已在 ON**：`sawArmOff_` 初值為 false，收到 `RECEIVING` 且 SWA≥1700 時印 `# ARM blocked: SWA already on, switch it off first`，不得解鎖；撥關（`arm`<1700）後才可解鎖。
  - 通用：dryrun 的 20A5h 若為 0x0200，解鎖時印 `# would-block: fault 0x0200`（只顯示、不阻擋）。20A2h 的處理見 §4（dryrun 實測讀到 `0xC0C0`，見 `docs/TESTLOG.md` 2026-11-23）。
- **dryrun 重測程序（你做什麼 → 你會看到什麼；驅動器上電、馬達不接，先把 SWA 撥到關）**：
  - 開機後看到 `# would-block: fault 0x0200` 與 `# would-block: 20A2h=C0C0 (alarm)` 是正常的（沒接馬達）。
  - **(a)** 撥 SWA 開、CH2 在中位，等出現 `ARMED`，把 CH2 推到底，再關遙控器 → 推到底（約 1.5 s 後）`C` 行速度最大 90.0、`# DRY W 01 10 20 88` 的數值最大 `00 5A 00 5A`（上限 90 r/min）；關遙控器後出現 `DISARMING`、兩行停止指令（`# DRY W …`）、`DISARMED`。
  - **(b)** 再開遙控器（SWA 還在開）→ 遙控器開機開關警告會鳴叫、不發送訊號，螢幕維持 `arm`＝1000，不會自己解鎖；不得關閉這個警告（關掉後會在約 0.1 秒內自動再解鎖，那是你決定的、僅限懸空測試，落地前要重新評估）。
  - **(c)** 解鎖後拔掉 i-BUS 訊號線 → 約 0.25 秒內出現 `STALE` 與 `# T2 STALE observed …`，接著出現停止指令與 `DISARMED`；接回線後 SWA 還開著，會顯示 `# ARM blocked: SWA already on`，要先撥關。
  - **(d)** 開機時 SWA 就在開的位置 → 一直不會解鎖，螢幕顯示 `# ARM blocked: SWA already on, switch it off first`；撥關再撥開才會解鎖。
- **T3 live 空載解鎖**：T2 通過、使用者指定通道常數與 `FAILSAFE_VERIFIED=1` 並確認後，編譯並燒錄 `live`；SWA 解鎖，油門保持中位，確認序列與回應核對無誤、馬達不動。
- **T4 低速**：油門小幅推動，觀察左右輪方向與速度，記錄方向正負號（`kLeftSign`、`kRightSign` 只有使用者確認後才改）。
  - **實測結果（2026-11-23，`live`，馬達已接、懸空；log：`logs/device-monitor-261123-154855.log`）**：解鎖與低速正常（±30 r/min 上限、0.5 s 斜坡、回中停止、無錯誤碼；log 轉矩最大 ±0.6 A）。**左輪方向正確、右輪相反** → `kLeftSign`＝+1、**`kRightSign`＝−1**（使用者確認）。符號只在寫入 2088h／2089h 之前與讀回 20ABh／20ACh 之後成對套用，超速判斷取絕對值不受影響。
  - **方向修正實機驗證（2026-11-23，`live`，log：`logs/device-monitor-261123-160036.log`；已驗證）**：油門往上寫入 2088h＝+30、2089h＝−30，讀回兩輪 actual_velocity 皆 +30；往下皆 −30。`error_code` 全程 0，最高實際轉速約 30.6 r/min，未觸發超速（門檻 45 r/min）；推到底指令停在 30 r/min 上限，屬預期。
  - **注意（2026-11-23）**：§7 以上各項實測（dryrun、T1–T4、方向修正）都是在上限 30 r/min、超速門檻 45 r/min 下取得；上限改為 90 r/min、門檻 108 r/min（使用者決定，2026-11-23）後已於懸空輪實測，見下一項。
  - **90／108 r/min 懸空輪實測（2026-11-23，`live`，輪子懸空；log：`logs/device-monitor-261123-162402.log`；數字由 Claude 自行重算）**：`ARMING`→`ARMED`（約 27.8 s）→`DISARMING`→`DISARMED`，沒有 `FAULT_STOP`／`LATCHED`；T 行 FC 0x03 共 125、FC 0x06 共 3、FC 0x10 共 56（ARMED 期間每 10 個週期才記錄一次，不是實際寫入次數），全部 OK；`C` 行 548 個週期，每行右＝−左。指令最大 +89.4／最小 −90.0 r/min（搖桿 1997／1000）；回授左 −90.5～+89.6、右 −90.5～+89.5 r/min，未達超速門檻 108，左右回授（35 組 |v|>5 的取樣）全部同號。斜坡 60 r/min/s：三次完整上升都是 30 步＝1500 ms，放開搖桿回零 1500～1550 ms。`error_code` 全程 0，轉矩左 −0.5～+0.6 A、右 −0.7～+0.7 A（低於 3.0 A）。**108 r/min 門檻沒有在實機觸發，只由 native 測試驗證。**斷線停止延遲在 90 r/min 下未另行實測。
- **T5 停止測試**：SWA 關、遙控器關機（經失效保護使 SWA 讀到關閉）、拔 i-BUS 線或接收器斷電（`STALE`）各一次，確認停止；每次之後須重新解鎖。
- **T6 記錄**：把實測值寫回 `PROJECT.md`（待確認項目 16、26 等）。

**停止條件（任一發生就停止並回報，不自行繼續）**：
- 任何故障碼（20A5h／20A6h 非 0）、LATCHED、`REJECTED_*`、`TX_FAIL`、`WRITE_MISMATCH`。
- 馬達在沒有指令時轉動、速度明顯超過指令、方向與預期不符、異常聲音、異味、過熱、冒煙。
- 電源異常：供電突然中斷、XT60 或線材發熱、電壓與預期差距大（電表量測值與 dc_voltage 的差異超出合理誤差，量測前的 dc_voltage 不可當依據）。停止並回報，先查原因。
- T2 的斷線保護驗證失敗（(a) 遙控器關機後 `ARM_CH` 沒有在 1 秒內低於 1700，或 (b) 拔線／接收器斷電後不進入 `STALE`、狀態機沒有停機）。
- 實際轉矩電流（20ADh／20AEh，單位 0.1 A）超過 **3.0 A**（讀值 30，使用者已確認）：手動停止條件。**實作註記**：ARMED 每個週期讀 20ABh 起 **4 個**暫存器（左右速度與左右轉矩，原設計是 2 個），並以 D 行輸出，讓你看得到轉矩；程式只顯示、不強制。

## 8. 已知未知
| 項目 | 狀況 | 影響與處理 |
|------|------|------------|
| 2000h＝0 的意義 | V4 沒說明（`PROJECT.md` 項 16） | 因此開機若為 0 就寫 500 ms；寫入是否掉電保存手冊沒寫（項 26），不寫 2010h |
| 讀取是否重置 2000h 計時 | V4 沒寫 | 保活用**寫入**，不依賴讀取 |
| 20A0h 格式 | V4 沒說明（項 11、27） | 只當作探測用的讀值，不解讀 |
| 200Fh 實際預設 | 暫存器表寫 0（異步），§4.1 文字寫「默認出貨為同步」（項 26） | 開機讀取並記錄；本設計只用 FC 0x10 同時寫左右，不依賴任一模式的語意；若為異步，T3／T4 需特別留意左右是否同時生效 |
| 左右馬達方向正負號 | **已確認（T4，2026-11-23）**：左輪正確、右輪相反 | `kLeftSign`＝+1、`kRightSign`＝−1；已於 2026-11-23 實機驗證（兩輪同向） |
| 2008h 與 0x2000 故障的關係 | V4 沒有明說（項 26） | 讀 2008h 只用於檢查上限；速度超出額定仍可能報 0x2000，故障路徑會處理 |
| 20A2h 在停機（0x07）、急停（0x05）後的值 | V4 沒寫 | 狀態機只用「解軸＝`00`」判斷未使能；停機後是否回到解軸不假設，T5 實測記錄 |
| FS-iA6B 斷線時的 i-BUS 行為 | 未驗證（Phase 1 的斷線保護未驗證） | T2 專門驗證；失敗則不得進入 T3 |
| dc_voltage 讀值（27.83 V） | 與電池規格不一致（項 25，7S 假說未驗證） | 本階段不使用電壓做任何保護判斷 |
| 泄放電阻是否已安裝 | 未知（項 28） | 上限原為 30 r/min（反電動勢風險低）；**使用者決定（2026-11-23）改為 90 r/min，接近 V4 驅動器 p.16 建議加泄放電阻的 100 RPM**；是否已安裝仍待確認；測試時注意 20A5h 是否出現過壓 0x0001 |

## 9. 使用者已確認（2026-11-23）
1. 設計整體（範圍、白名單、狀態機、停止條件）。
2. 交易引擎做法 A（見 §1.4）。
3. `dryrun` 的語意與兩個專用旗標（見 §5）。
4. 實際轉矩電流 3.0 A 的手動停止門檻。
5. 死區 40、SWA 門檻 1700、控制週期 50 ms。
6. 通道常數 `THROTTLE_CH`、`ARM_CH` 預設未定義並以 `#error` 擋住，使用者之後以一句話指定。
7. T2 是接馬達之前的硬門檻，dryrun 就要顯示結果（見 §7）。
8. **（2026-11-23，使用者決定）速度上限 30 → 90 r/min、超速倍率 1.5 → 1.2（門檻 108 r/min＝1080）、斜坡改以速率定義（每 50 ms 3.0 r/min）。**

## 10. 實作註記（與原設計的差異）
- `ARMED` 讀 20ABh 起 4 個（原 2 個），為了顯示轉矩（§7）。
- 新增 `FAILSAFE_VERIFIED` 編譯期條件（§5）。
- 速度上限 90 r/min（使用者決定，2026-11-23）：`kSlewSeconds` 改為速率常數 `kSlewRpmPerSec`（60 r/min/s）；`SlewLimiter(limitRpm, rpmPerSecond)`；新增 `overspeedLimit01(limitRpm)`（`kOverspeedFactor`＝1.2，門檻 1080）；測試中的 30／45／450／451 與 `00 1E`／`FF E2` 一併改為 90／108／1080／1081 與 `00 5A`／`FF A6`。
- 緊急停止序列使用 `Transactor::run(..., ignoreLatch=true)`：即使 Transactor 已因 `REJECTED_*`、`TX_FAIL`、連續失敗而 LATCHED，故障路徑的 `200Eh=0x05` 與 `2088h=0,0` 仍會送出（各只嘗試一次）；狀態機本身進入 LATCHED 之後不再送出任何請求。
- 開機探測失敗（PROBE_FAIL）與預檢階段的通訊錯誤直接 LATCHED，**不送停止指令**（還沒有使能過，`everArmed` 為 false）。
- 狀態機的 `C` 行（指令）每個週期都輸出；ARMED 期間的 T／D 行每 10 個週期才完整記錄一次，失敗的交易一律記錄（避免塞滿 USB Serial）。
- 新增狀態碼 `REJECTED_REG`、`REJECTED_VALUE`、`WRITE_MISMATCH`（§2）。
- `ITransport` 新增 `writeDrive()`，預設拒絕；寫入幀（FC 0x06／0x10）走這條，讀取幀走 `write()`。

## 11. 轉向混控設計

2026-11-24 使用者指定 FS-i6X Mode 1。左搖桿上下是 CH2（前後，`THROTTLE_CH=1`），左搖桿左右是 CH4（轉向，`STEER_CH=3`）。SWA 沿用 CH7（`ARM_CH=6`）。使用者確認本節的五項設計選擇。`dryrun` 與 `live` 懸空結果見 [2026-11-24 實測紀錄](../docs/TESTLOG.md#2026-11-24)。

### 輸入與每輪輸出

1. 油門 `T` 沿用 §3 的換算，前進為正，範圍為 ±90 r/min。CH4 比照 CH2，先將原始值 `v` 夾在 1000–2000，以 1500 為中點，1460–1540（含端點）為死區。以 `x = v − 1500` 計算，死區內 `u = 0`，其餘 `u = sign(x) × (|x| − 40) / 460`，滿行程為 ±1。CH4 右推為高值（使用者回報），`kSteerInputSign=+1` 對應往右為正。
2. `S = clamp(u × kSteerInputSign × kSteeringGain × kSpeedLimitRpm, −abs(T), +abs(T))`，單位 r/min。`kSteeringGain=0.25`，滿行程限幅前為 22.5 r/min。以車體方向計算 `左 = T + S`、`右 = T − S`。油門歸零時兩輪目標為零。倒車也沿用 `S` 的方向，右推時車頭仍朝右。
3. 左右目標各自夾在 `±kSpeedLimitRpm`（±90 r/min），單側飽和時另一側依自己的目標輸出。例如 `T = 85`、`S = 20` 時，限幅後為 90、65 r/min。各輪在限幅後套用自己的斜坡，沿用 `kSlewRpmPerSec=60`，正常 50 ms 週期每輪最多變動 3.0 r/min。直接前後換向時，兩輪通過零速的時間可能不同。解除或故障停機沿用 §4 的路徑，零速指令跳過斜坡，兩輪的斜坡狀態都在停機及重新解鎖時歸零。
4. 斜坡後才乘 `kLeftSign=+1`、`kRightSign=−1`，再依 2022h 的解析度換算、取整，並各自夾在 `±limitRaw`。用現有 FC 0x10 一次寫入 2088h／2089h。方向符號只處理馬達安裝差異，讀回速度時成對套用。每輪仍獨立比較實際速度絕對值，超過 108 r/min 走既有故障路徑，倍率維持 1.2。

### 解鎖與失效保護

- CH2、CH4 必須同時在各自死區內連續 ≥0.5 s。`DISARMED` 每次更新遙控輸入時檢查，任一軸離開死區或 i-BUS 進入 `WAITING`／`STALE` 就重計。沿用 §4 的 SWA 條件。失效保護值也可能滿足回中與「看過 SWA 關閉」的條件，因此雙軸回中無法解決自動重新解鎖。保留 FS-i6X 開機開關警告，落地前依 `PROJECT.md` 項 31 決定重啟後的解鎖行為。
- 遙控器關機後，接收器仍可能送出 i-BUS 訊框並維持 `RECEIVING`，尚未設定失效保護的 CH4 會凍結在舊值。只要 CH7 確實降到 1000，SWA 解除優先於混控，§4 的雙輪零速與停機序列即會執行。若 CH7 仍停在 ON，單靠 SWA 就無法停機。CH4 回中也只能消除轉向量，停機仍依賴 CH7 解除，CH2 回中則消除油門指令。
- FS-i6X 的 CH4 失效保護設定為 1500，CH2 為 1500、CH7 為 1000。混控 `dryrun` 關遙控器後讀到 CH4＝1501，落在中位死區。已驗證遙控器關機、i-BUS 訊框中斷及 `STALE` 後須重撥 SWA 才能解鎖。

### 使用者決定與驗證

使用者於 2026-11-24 確認禁止原地旋轉、`kSteeringGain=0.25`、倒車右推時車頭仍朝右、雙軸同時回中 ≥0.5 s 才解鎖，以及維持 3.0 A 手動停止門檻。

- `[env:dryrun]`、`[env:live]` 均定義 `-DSTEER_CH=3`，`kSteeringEnabled` 隨定義啟用。`src/config.h` 檢查通道範圍與通道重複。使用者於 2026-11-24 聲明混控 `dryrun` 的 T2 (a)(b) 與步驟 10 通過，依規則 19 同意在 `live` 啟用混控並維持 `FAILSAFE_VERIFIED=1`。
- Phase 3 `native`／`native_dryrun` 以模擬輸入驗證通道換算與死區、前進與倒車方向、原地旋轉限制、單輪飽和、獨立斜坡、原始值換算、雙軸回中計時、失聯停機與復訊、每輪超速邊界。靜態測試檢查 `dryrun`、`live` 都有 `STEER_CH=3`，且只有 `live` 有 `FAILSAFE_VERIFIED=1`。
- 混控版 `R,ms,status,frame_age_ms,unchanged_ms,throttle,arm,steer_raw` 的最後一欄是 CH4 原始值。右推預期大於 1500，左推預期小於 1500。關遙控器後讀 CH2、CH7、CH4 是否到 1500、1000、1500，並用 `S` 狀態行及 `# DRY W` 確認 `DISARMING`、雙輪零速、停機、`DISARMED`。拔 i-BUS 或接收器失電時看 `STALE` 與同一停機序列。
- 混控版 `C,ms,throttle,target_rpm,left_raw,right_raw,steer_rpm,left_target_rpm,right_target_rpm` 附加限於 ±|T| 的 `S` 和各輪經 ±90 限幅、尚未套斜坡及馬達符號的目標。既有 `target_rpm` 欄在混控時代表兩輪斜坡後的平均指令。`left_raw`、`right_raw` 已套用斜坡、馬達符號及解析度換算。這些新增欄位適用於兩個啟用混控的環境。
- **斷線停止延遲**：直行與轉向的懸空停機時間及量測限制見 §4。RS485 中斷時驅動器是否按 `2000h` 停機仍待驗證，500 ms 讀回值只確認設定。落地前依規則 26 處理轉向停止時的位移與車身轉角。
- **3.0 A 的適用性也需要重新評估。** 轉向、方向反轉及雙輪不同步減速時，各輪電流峰值可能不同。經同意的懸空驗證應分輪記錄 20ADh／20AEh 的絕對峰值與對應動作，任一輪超過現行 3.0 A 手動門檻即停止並回報。落地負載待另外評估。

## 12. 綠板 JK2501（ESP32-S3）移植

使用者於 2026-11-25 授權綠板做第三階段寫入（來源：使用者本人；記錄見 `PROJECT.md`）。新增兩個環境，程式沿用 `drive_main.cpp` 與同一個狀態機，只有旗標不同。

| 環境 | 寫入 | 主要旗標 |
|---|---|---|
| `s3_drive_dry` | 只印出（`# DRY W`），沒有寫入傳輸 | `DRIVE_DRYRUN` |
| `s3_drive_live` | 啟用寫入傳輸 | `DRIVE_LIVE`、`FAILSAFE_VERIFIED=1`（使用者聲明 2026-11-25，見下方 (d)） |

- **腳位**：沿用 2026-11-25 實測通過的設定（`docs/TESTLOG.md`）：i-BUS RX＝GPIO18（排針側，X6 空著）；RS485 RX＝GPIO15、TX＝GPIO16，沒有 DE/RE 腳；`MODBUS_MAX_ATTEMPTS=1`。
- **通道**：沿用使用者 2026-11-23 指定的同一組遙控器與接收器：油門 CH2（`THROTTLE_CH=1`）、SWA CH7（`ARM_CH=6`）、轉向 CH4（`STEER_CH=3`）。`s3_ibus` 實測 CH2、CH4、CH7 跟著動。
- **板型**：`boards/jk2501_n16r8.json`（16 MB flash、OPI PSRAM），不使用 GPIO35–37。
- **不變的部分**：速度上限、超速倍率、左右正負號、轉向參數、寫入白名單（規則 16）與黑名單、第三層防線、停機序列。第三層防線 `isAllowedDriveFrameShape` 對 FC 0x06 只放行 2000h、200Dh、200Eh，對 FC 0x10 只放行 2088h 數量 2；數值範圍由 `WriteGuard` 檢查。

### 額外安全項（`DRIVE_STRICT_SAFETY` → `FsmConfig::strictSafety`）

只在 s3 兩個環境開啟。esp32dev 的 `dryrun`／`live` 不開，行為不變；是否擴大到 esp32dev 待使用者決定。對應 `docs/CI.md` 的待決檢查：

| 項目 | live 行為 | dryrun 行為 |
|---|---|---|
| (a) 解鎖前重寫 2000h 並讀回 | SWA 解鎖條件與驅動器狀態檢查都通過後，先寫 `2000h＝500` 再讀回；不等於 500 → `# DISABLED`（不解鎖，須重開機）。寫入或讀取失敗 → 故障路徑 | 寫入只印出；讀回不符只顯示 `# would-block`，照常解鎖 |
| (b) 開機先送停止並讀回 | 探測 20A0h 成功後，先送 `2088h＝0,0`、`200Eh＝0x07`，再讀 20A2h；兩軸都是解軸才印 `# BOOT STOP confirmed`，否則 `# DISABLED`。寫入失敗 → 故障路徑（急停） | 寫入只印出；讀回不是解軸只顯示 `# would-block` |
| (c) 通道值超出範圍 | 油門、SWA、轉向任一通道不在 1000–2000（`kChannelValidMin`／`kChannelValidMax`）：`ARMED` 時印 `# DISARM reason=CH_OUT_OF_RANGE` 並走正常解除序列；`DISARMED` 時不解鎖。恢復後須再看過 SWA 關閉才能解鎖；範圍外的 SWA 值不算「關閉」 | 同左 |

- **i-BUS 逾時或失聯**：沿用 §4，250 ms 沒有新訊框即 `STALE` → 解除，`WAITING` 也視為沒有資料。驅動器端另有 2000h 掉線保護（500 ms）。
- **FC 0x06 回音**：綠板 `s3_poll` 2026-11-25 在 20,138 筆讀取中量到 `echo=0`、`partial_echo=0`（`docs/TESTLOG.md`）。所以 FC 0x06 的回應視為驅動器的真正確認，沿用 §1.4「實作註記（回音）」的處理方式，沒有回音不算故障。回音與確認仍無法在協定上區分，因此 (a)(b) 與解鎖後的 20A2h 讀回仍保留。
- **不需要 USB 序列埠**：`Serial` 是 UART0（CH340），`setup()` 不等待序列埠連線，拔掉 Type-C 照常執行；靜態測試檢查沒有等待序列埠的迴圈，也沒有 `ARDUINO_USB_CDC_ON_BOOT`。

### 待使用者決定

- **(d) 已決定（2026-11-25）**：使用者聲明綠板 T2 (a)(b) 已在 `s3_drive_dry` 看到並判定通過，指定 `s3_drive_live` 加入 `-DFAILSAFE_VERIFIED=1`（規則 19）。實測紀錄見 `docs/TESTLOG.md` 2026-11-25。`test_s3_drive_envs` 改為檢查 `s3_drive_live` 有此旗標、`s3_drive_dry` 沒有。燒錄 live 仍須使用者另行明確確認。
- **(c) 的範圍**：目前嚴格採用 1000–2000，沒有容差。若遙控器的端點或微調讓值略超出，會造成解除（偏安全）。
- **esp32dev 是否也開啟 strictSafety**：開啟會改變已驗證的 `dryrun`／`live` 行為。

## 13. 綠板板上 log

`ONBOARD_LOG` 預設關閉，只支援 JK2501 的 `s3_drive_*` 環境。單次啟用編譯可在 `phase3-drive/` 執行 `PLATFORMIO_BUILD_FLAGS=-DONBOARD_LOG pio run -e s3_drive_dry`。

- 原有 log 行原樣存入 128 KiB PSRAM 環形緩衝。緩衝滿時以完整行淘汰最舊資料。`ARMED` 只緩存。進入 `DISARMED` 或 `LATCHED` 後追加至 LittleFS 的 `/onboard.log`。正常解除須先完成 §4 的三秒停機取樣，Flash 作業與控制迴圈在同一執行緒，落盤時狀態無法切換為 `ARMED`。
- 每批附開機序號與當下狀態、故障原因。開機序號由 NVS 保存。LittleFS 掛載失敗時只初始化全空的分割區。log 達 2 MiB 時從最新一批重新記錄，並輸出 `# ONBOARD_LOG rollover`。寫入失敗會保留 PSRAM 資料供下一次狀態切換時追加。尚未落盤的資料會隨斷電消失。
- Type-C 序列埠為 115200 baud，每 5 秒輸出 `# STATE <狀態> reason=<原因>`。在 `DISARMED` 且停機取樣完成，或在 `LATCHED`，可送 `dump` 或 `erase` 加換行。`dump` 逐行輸出已存檔的原始紀錄，狀態切換會中斷傳輸。`erase` 清除單一 log 檔及當前緩存，開機序號繼續累計。
