# EspFS 實測紀錄（TESTLOG）

> 2026-11-24 從 `PROJECT.md` 原文搬來，只搬不改寫；只更正明列的過時句子與交叉引用（見 PR 說明）。專案現況以 `PROJECT.md` 為準。
> log 檔在 `phase3-drive/logs/`。同一天內依 log 時間排列；沒有 log 的使用者決定放在相關測試之前。

## 日期未記錄於 PROJECT.md（Phase 1、Phase 2）

### 階段一：i-BUS 接收（`phase1-ibus/`）
已完成（使用者確認）：
- ESP32 晶片辨識
- PlatformIO 編譯與燒錄
- Serial Monitor 115200
- FS-iA6B 經 GPIO18 傳輸 i-BUS
- 搖桿操作時通道數值有變化

未驗證：
- CH1～CH10 完整對應（目前只有下方使用者回報的 CH2、CH7；其餘通道未驗證）
- 斷線保護（遙控器關機後接收器持續送舊值；Phase 1 的觀察與失效保護設定見下方；dryrun 在 2026-11-23 已看到 (a)(b)，見階段三）

程式內容（來自程式碼，未實測）：
- 使用 `IBusBM@1.1.4`、`espressif32@6.10.0`，iBus 走 UART1，只指定 RX 腳位 GPIO18。
- 每 100 ms 以 CSV 輸出到 Serial（115200）：`ms,ibus_status,valid_frames,CH1…CH10`。
- 狀態有 `WAITING`、`RECEIVING`、`STALE`；250 ms 沒收到新 frame 判為 `STALE`，並保留最後一筆通道值。
- 程式只讀取與輸出，不控制馬達；`STALE` 只是狀態標示，不等於已驗證的斷線保護。

### 階段二：Modbus RTU 唯讀通訊（設計：`phase2-modbus/DESIGN.md`）
- 已完成：ESP32 與 TTL/RS485、TTL/RS485 與 ZLAC8015D 的接線（驅動器端已依 V4.2 實機 J5 確認，見第 3 節）。
- 已完成：`DESIGN.md` 設計文件。
- 已完成：程式（`phase2-modbus/`）。純邏輯在 `lib/modbus_ro/`，`pio test -e native` 全部通過；兩個 ESP32 環境 `loopback`（A5）與 `main`（A6）可編譯。只允許 FC 0x03，三層防線（組裝層、`guard`、`transport_serial2` 內獨立檢查）。
- 已完成：A5 迴路測試與 A6 真實驅動器唯讀通訊（以下結果由使用者回報，Claude 沒有直接看到序列輸出）。馬達與 Encoder／Hall 未連接。

**A5 迴路測試（模組拆下，GPIO16 與 GPIO17 短接）**
- 結果：**50／50 PASS**。

**A6 真實驅動器唯讀通訊**
| 情境 | 結果 |
|------|------|
| 驅動器無電 | ID 1、ID 4 各 3 次 `TIMEOUT` 後 `LATCHED`，行為正確（與設計一致：只試 1 與 4，不試其他 ID） |
| 驅動器有電 | ID 1 第一次嘗試即成功；rtt 3–5 ms；3.3V 模組供電可用；A/B 極性正確 |
| 設定暫存器 | node_id（2001h）=1；baud_code（2002h）=2（舊手冊編碼 2＝115200，與實際通訊 baud 一致） |
| 故障碼 | error_code（20A5h）=0x0200（Hall 錯誤），與紅燈閃 10 次吻合。因未接 Encoder／Hall，屬預期；舊手冊的對應（RS485 p.18、驅動器 p.13）在這台 V4.2 上同樣成立（僅此一項，其餘仍只作參考） |
| 輪詢 | 週期穩定 200 ms |
| 驅動器斷電 | 電壓 27.83 → 21.26 → 14.03，隨後 3 筆 `TIMEOUT` 並 `LATCHED`，乾淨停止 |

**電壓讀值警告**：dc_voltage 讀到 **27.83 V**，與電池不一致（6S 標稱 22.2 V、滿電約 25.2 V、標示充電上限 25.5 V）。換算係數 0.01 V 是依舊手冊假設，也可能是驅動器 ADC 偏差。使用者手邊沒有三用電表。**在量到實際電壓之前，dc_voltage 不得作為任何保護判斷的依據**（見待確認項 25）。

## 2026-11-23

### 階段一：通道指定與遙控器失效保護（使用者回報）
**使用者回報（2026-11-23，Phase 1 實測；Claude 未見序列輸出）：通道指定與遙控器失效保護**
- 手別 Mode 1（右手油門，CH3 不回中）；前進／後退軸用 **CH2**（左桿上下，推上＝2000、回中＝1500）；解鎖開關 **CH7（SwA）**，解鎖＝2000、關閉＝1000。
- 對應的編譯期常數（0 起算）：`THROTTLE_CH=1`、`ARM_CH=6`；已寫進 `phase3-drive/platformio.ini` 的 `dryrun` 環境；`live` 環境於 2026-11-23 依使用者指示加入（見階段三「live 編譯」）。
- **遙控器關機後 i-BUS 持續送舊值**：狀態仍是 `RECEIVING`、CH2 凍結。**因此 `RECEIVING` 不能單獨當作遙控器在線的依據。**
- 使用者已在遙控器設定失效保護：CH2→1500、CH7→1000，皆 ON，通道數 7。重測 3/3：遙控器關機後 CH7 在 1 秒內回 1000、CH2 回 1500；重新開機後恢復。
- 這是 Phase 1 的觀察；`dryrun` 的驗證結果見階段三「dryrun 實測結果（2026-11-23）」。

### 階段三：遙控器低速驅動（設計：`phase3-drive/DESIGN.md`）
- 狀態：設計經使用者確認；程式與 native 測試已完成（`phase3-drive/`）。通道已由使用者指定（`THROTTLE_CH=1`、`ARM_CH=6`，見階段一），先寫進 `dryrun` 環境；使用者已同意燒錄 `dryrun`（驅動器上電、馬達不接，燒錄與上電由使用者操作；`dryrun` 沒有寫入傳輸，所有寫入只印出）。`live` 環境已依使用者 2026-11-23 的指示加入通道旗標與 `FAILSAFE_VERIFIED=1`，已編譯成功，之後由使用者燒錄並完成 T3/T4（見下）；方向修正後須重新燒錄，燒錄與啟動馬達仍須使用者另行明確確認。範圍：遙控器前進／後退（左右同速），轉向混控預設關。
- **T2（斷線保護，接馬達前的硬門檻）判定已修訂（使用者決定，2026-11-23）**：因接收器在遙控器關機後仍持續送舊值，原本只看「是否進入 STALE」不夠。通過條件：(a) 遙控器關機後 `ARM_CH` 在 1 秒內低於 1700，狀態機因 SWA 關而停機；(b) 拔 i-BUS 訊號線或接收器斷電時進入 `STALE` 並停機。**兩者都要在 `dryrun` 看到才算 T2 完成。2026-11-23 的 `dryrun` 已看到 (a)(b)（使用者判定通過，僅 dryrun、馬達未接，見下方實測結果）**；使用者於 2026-11-23 聲明 T2 (a)(b) 通過並指定 `FAILSAFE_VERIFIED=1`（已寫進 `live` 的 build_flags）。判定細節見 `phase3-drive/DESIGN.md` §7。
- 需先有斷線保護並經確認（CLAUDE.md 規則 11）。

**dryrun 首次燒錄結果（使用者回報，Claude 未見完整 log）：真實暫存器值與一個程式缺陷**
- 20A5h＝0x0200（Hall 故障，無馬達造成，與 A6 相同）。
- **20A2h＝0xC0C0**：V4 RS485 §六、p.18 的「`C0 C0`＝報警」（`00 00` 解軸、`40 40` 鎖軸、`80 80` 急停）。這是真實狀態，不是已使能。
- 缺陷（已修）：舊程式把 20A2h 任何非 `00` 都印成 `driver already enabled`，每 50 ms 擋一次，狀態機永遠進不了 ARMING。現在：dryrun 兩軸皆 `C0C0` 時只顯示 `# would-block: 20A2h=C0C0 (alarm)` 並繼續；`40`、`80`、混合狀態仍擋住且用真實狀態名稱；`live` 任何非 `00` 都擋；擋住時至少等 1 s 才再讀，同一原因只印一次。
- 修正後已重新燒錄並實測，結果見下一段。

**未被 PROJECT.md 引用的 3 份 log（2026-11-24 復原並提交；以下只描述 log 內實際內容）**
- `device-monitor-261123-142154.log`（107 行）：`dryrun` 開機一次（`BOOT` → 預檢讀值 → `DISARMED`），20A5h／20A6h＝0x0200、20A2h＝`C0C0`，印出 2000h 預定寫入 `# DRY W 01 06 20 00 01 F4 82 1D`；R 行 SWA 全程 1000、油門 1504–1505，沒有進入 `ARMING`。
- `device-monitor-261123-142319.log`（1094 行）：開頭從 R 行（84880 ms）接續、沒有開機段；含 100 次 `# would-block: fault 0x0200` 與 `# ARM blocked: driver already enabled (20A2h)`（20A2h＝`C0C0`），這個訊息就是上段記載的 20A2h 缺陷（修正前）；之後有兩次 `POWERON_RESET` 重開機（`BOOT` → `DISARMED`），全檔沒有 `ARMING`／`ARMED`。
- `device-monitor-261123-144740.log`（14 行）：只有 R 行與 `# T2 stale_seen=1 …` 行（SWA 1000、油門 1504、`values_unchanged_ms` 約 107–118 s），沒有開機段與狀態行。

**dryrun 實測結果（2026-11-23，馬達未接，僅 dryrun；log：`phase3-drive/logs/device-monitor-261123-144822.log`、`logs/device-monitor-261123-150455.log`（2026-11-24 復原並已提交），已與 log 核對）**
- **(d) 開機時 SWA 已開**：印 `# ARM blocked: SWA already on, switch it off first`，撥關再撥開後才解鎖（`ARMING`→`ARMED`）。**通過。**
- **解鎖序列**：`200Dh=3`（`01 06 20 0D 00 03 53 C8`）→ `2088h=0,0`（`01 10 20 88 00 02 04 00 00 00 00 63 A8`）→ `200Eh=0x08`（`01 06 20 0E 00 08 E2 0F`），幀與預期逐字相同。**通過。**
- **速度**：最大 ±30（2088h 寫入 `00 1E 00 1E` 與 `FF E2 FF E2`），斜坡每 50 ms 3.0 r/min，0→30 約 0.45 s（使用者回報，與斜坡步進一致），回中 −30→0 約 0.5 s，回中後歸 0。30 r/min 上限檢查**通過**。
- **(a) 遙控器關機**：`DISARMING` → `2088h=0,0` → `200Eh=0x07`（`01 06 20 0E 00 07 A2 0B`）→ `DISARMED`；從最後一次值變化到 `DISARMING` 約 1.05 s，這是**上限**（關機時刻無法精確得知；log 的 `R` 行每秒一行，無法分辨 1 秒內）。使用者判定**通過**；Phase 1 的 100 ms CSV 重測 3/3 在 1 秒內（使用者回報）。
- **(c) ARMED 時拔 i-BUS 訊號線**：250 ms 後 `STALE`（`# T2 STALE observed after 250 ms without frames`）→ 同一週期（相同時間戳）`DISARMING` → 停止序列 → `DISARMED`；插回後 SWA 仍開 → `# ARM blocked: SWA already on, switch it off first`。**通過。**（使用者回報時把這項與拔線標為 (b)(c)；T2 的 (b) 條件即這項。）
- **T2 判定**：(a)(b) 在 dryrun 均已看到，**使用者判定通過（馬達未接，僅 dryrun）**。**使用者隨後聲明 T2 (a)(b) 通過並指定 `FAILSAFE_VERIFIED=1`（見下方「live 編譯」）。**
- 其他讀值：20A5h＝0x0200、20A2h＝0xC0C0（見上段）；dryrun 每 50 ms 輸出一行 `# DRY W` 的 2088h 寫入，量大屬預期。

### 使用者決定（2026-11-23）：無電表的低速懸空測試例外
- 使用者決定在**沒有電表量測**的情況下，於 2026-11-23 進行遙控器低速、輪子懸空的驅動測試，作為項 22 上電門檻（電池須用電表量測）的**例外**，**風險由使用者承擔**。這是使用者的決定，不是 Claude 的建議。
- 使用者列出的條件：
  1. 數電池平衡接頭針數（6S＝7 針、7S＝8 針）並記錄。**【已記錄】使用者數到 7 針 → 6S（T1，2026-11-23）。**
  2. 馬達接頭與銘牌核對 J2／J6 與動力端子後才接。
  3. 【使用者聲明，2026-11-23】沒有獨立 DC 開關、沒有額外保險絲；供電是手動插拔 XT60，緊急斷電方式是拔 XT60。（原條件「DC 開關額定須不低於電池滿電電壓」不再適用，不再是待確認或阻擋項。）
  4. 輪子離地、人在場、手放在 XT60 上（緊急斷電＝拔 XT60；原寫「手放在 DC 開關上」，依使用者 2026-11-23 聲明改記）。
  5. 【已取消】原「5 A 保險絲不得為了讓馬達轉而換大」不再適用：依使用者 2026-11-23 聲明，供電路徑沒有保險絲。
  6. 接線全程斷電。
- **使用者聲明（2026-11-23）**：沒有獨立 DC 開關、沒有額外保險絲，供電是手動插拔 XT60，緊急斷電方式是拔 XT60。電池 6S，馬達額定 24 V。據此條件 3、4 與項 22、25 改為記錄，不再是待確認或阻擋項。**已依此聲明更新第 2 節電源路徑、項 24、條件 5（取消）與 `phase3-drive/DESIGN.md` §7 準備步驟 3、4、5 與停止條件；供電路徑＝電池 → XT60（手動插拔）→ 驅動器。**
- 使用者將於明天（2026-11-24）以電表量測並補記；**項 22、25 仍保持「待量測」**，在量測之前不得把 dc_voltage 當保護依據。

**T1 實測結果（2026-11-23，`dryrun`，馬達與 Hall 已接、輪子懸空；log：`phase3-drive/logs/device-monitor-261123-152439.log`、`logs/device-monitor-261123-153342.log`（2026-11-24 復原並已提交），已與 log 核對；使用者判定通過）**
- **開機讀值**：20A5h／20A6h＝0000、**20A2h＝0000（解軸，驅動器正常狀態）**，Hall 故障消失（接上馬達與 Hall 後）。預檢讀值：2022h＝1、2008h＝1000、2000h＝0、200Fh＝1、2007h＝0、2011h＝5、**2023h＝0**、20A0h＝26057（原始值）。
- **解鎖**：沒有 `would-block`，`ARMING`→`ARMED`。
- **手轉左輪**：回授（20ABh）取樣到 +34.1、−25.1 r/min，超過門檻 45 r/min（讀到 48.3）→ `FAULT_STOP`（`200Eh=0x05`、`2088h=0,0`，只印出）→ `LATCHED reason=OVERSPEED detail=0x1E3`（483＝48.3 r/min）。
- **手轉右輪**：回授 −54.8 r/min → 同樣 `LATCHED reason=OVERSPEED detail=0x224`（548＝54.8 r/min）。
- **轉矩** 20ADh／20AEh 全程 0.0 A（D 行為每 10 個週期取樣一次）。
- **軟體超速保護在實機驗證有效**：2023h＝0（驅動器本身不檢查超速），保護完全由程式提供。
- **方向正負號當時未知**（手轉無法判斷哪個方向算前進），留待 T4；T4 已確認，見下方「T3/T4 實測結果」。
- log 中沒有 `dc_voltage`（20A1h）的讀值；電壓仍待電表量測（項 22、25）。
- **電池**：平衡線 7 針（使用者數）→ 依規則（6S＝7、7S＝8）為 **6S**（使用者決定例外的條件 1）。與 dc_voltage 27.83 V（6S 滿電約 25.2 V）**仍不一致**，待電表量測（項 22、25）；這與先前的 7S 假說（2026-11-24 已自 PROJECT.md 刪除）不符，27.83 V 目前沒有解釋。
- **T1 判定：通過（`dryrun`）。**

**live 編譯（2026-11-23，使用者指示；只編譯，未燒錄、未開序列埠）**
- 使用者聲明 T2 (a)(b) 已在 dryrun 通過，並指定 `FAILSAFE_VERIFIED=1`。`platformio.ini` 的 `live` 環境 build_flags 加 `-DTHROTTLE_CH=1 -DARM_CH=6 -DFAILSAFE_VERIFIED=1`（通道與 dryrun 相同：油門 CH2、SWA＝CH7）。
- 靜態測試 `test_platformio_envs` 依使用者同意同步更新：`live` 必須含這三個旗標，`dryrun`、`native` 不得含 `FAILSAFE_VERIFIED`，`live` 沒有 `DRIVE_DRYRUN`。
- 結果：`pio run -e live` 成功；`native` 105／105、`native_dryrun` 84／84；`live` 的 `firmware.elf` 有 `Serial2Transport::writeDrive` 符號、`dryrun` 沒有。
- 編譯當時 `live` 尚未燒錄；之後由使用者燒錄並完成 T3/T4（見下一段）。燒錄、啟動馬達、寫入暫存器仍須使用者依規則 10、11 另行明確確認。

**T3/T4 實測結果（2026-11-23，`live`，馬達已接、輪子懸空；log：`phase3-drive/logs/device-monitor-261123-154855.log`，已與 log 核對）**
- 使用者回報：解鎖與低速正常（±30 r/min 上限、0.5 s 斜坡、回中停止、無錯誤碼）。**左輪方向正確，右輪方向相反（左輪前進時右輪後退）。**
- log 核對：`BOOT`→`DISARMED`→`ARMING`（6068 ms）→`ARMED`（6116 ms）→`DISARMING`（41188 ms）→`DISARMED`，沒有 `FAULT_STOP`／`LATCHED`；`error_code_L`／`error_code_R` 全程 0；T 行記錄到的寫入：FC 0x06 共 3 筆、FC 0x10 共 71 筆，狀態全部 OK（ARMED 期間 T 行每 10 個週期才完整記錄一次、失敗一律記錄，所以這不是實際寫入次數）；`C` 行指令最大 ±30.0；回授 actual_velocity 左 −30.4～+30.4、右 −30.2～+30.3 r/min（D 行取樣）；轉矩 20ADh／20AEh 最大 ±0.6 A（未達 3.0 A 手動停止條件）。
- **方向修正（程式，2026-11-23）**：`kLeftSign`＝+1 維持、**`kRightSign`＝−1**。符號只在兩處成對套用：寫入 2088h／2089h 之前（`DriveFsm::doArmed` 的 `clampRaw`，右輪寫入取反），以及讀回 20ABh／20ACh 之後（`DriveLink` 的 D 行與 `DriveFsm` 的超速判斷，共用 `normalizedVelocity01`）。讀回後油門往上時兩輪的 actual_velocity 都是正值；T 行永遠是未處理的原始暫存器值。
- **超速判斷**（1.5×30＝45.0 r/min）：方向換成「油門往上為正」後取絕對值，符號改動後仍正確；新增測試涵蓋邊界（450 不超速）與 ±451 在左右兩輪、兩個方向都超速。
- 測試與編譯：`native` 110／110、`native_dryrun` 88／88、Phase 2 `native` 116／116；`pio run -e live`、`-e dryrun` 成功（只編譯）。方向修正後的實機驗證見下一段。

**T4 方向修正實機驗證（2026-11-23，`live`，馬達已接、輪子懸空；log：`phase3-drive/logs/device-monitor-261123-160036.log`，已與 log 核對；已驗證）**
- 油門往上：寫入 2088h＝+30、2089h＝−30（`C` 行 `30,-30`），讀回兩輪 actual_velocity 皆為 +30；往下：寫入 `-30,30`，讀回兩輪皆為 −30。log 中所有取樣的兩輪回授都同號，沒有出現異號。
- `error_code_L`／`error_code_R` 全程 0；T 行記錄到的寫入：FC 0x06 共 3 筆、FC 0x10 共 39 筆，狀態全部 OK（ARMED 期間每 10 個週期才記錄一次，不是實際寫入次數）；`BOOT`→`DISARMED`→`ARMING`→`ARMED`→`DISARMING`→`DISARMED`，沒有 `FAULT_STOP`／`LATCHED`。
- 最高實際轉速約 30.6 r/min（D 行取樣：左最大 +30.4、右最大 +30.6，反向最小 −30.2），未觸發超速（門檻 45 r/min）。
- 推到底時指令停在 30 r/min 上限（`C` 行最大 ±30.0），屬預期行為。
- 結論：`kLeftSign`＝+1、`kRightSign`＝−1 已在實機驗證，兩輪同向。

**速度上限調整（使用者決定，2026-11-23；使用者未另述原因，時間點在 T1–T4 於 30 r/min 實測之後）**
- `kSpeedLimitRpm`：30 → **90 r/min**（`static_assert ≤ 100` 不變）。
- 超速倍率 `kOverspeedFactor`：1.5 → **1.2**；門檻由上限與倍率推導＝**108 r/min（1080，單位 0.1 r/min）**，不寫死（原 45 r/min＝450）。注意：108 高於指令上限常數的 100 r/min 上限，那條限制針對指令上限，不針對故障門檻。
- 斜坡原本以**時間**定義（`kSlewSeconds`＝0.5，0 → 上限 0.5 s）；上限改 90 若不動，速率會變成每 50 ms 9.0 r/min。依指示改以**速率**定義：`kSlewRpmPerSec`＝60（每 50 ms 3.0 r/min），0 → 90 約 1.5 s。搖桿換算（死區 ±40、線性到滿格）不變，滿格對應 90。
- 寫入範圍檢查（2088h／2089h）隨上限走（2022h＝1 時 ±90；＝A 時 ±900）。測試全部同步改新值：原 ±450 不超速、±451 超速 → ±1080／±1081；2088h 的 `00 1E`／`FF E2` → `00 5A`／`FF A6`；破壞驗證（倍率改回 1.5、斜坡改回 0.5 s 對應的速率）對應測試都會失敗。
- 測試與編譯：`native` 111／111、`native_dryrun` 89／89、Phase 2 `native` 116／116；`pio run -e live`、`-e dryrun` 成功（只編譯）。
- **上列 dryrun、T1、T3/T4、方向修正的實測結果，都是在上限 30 r/min、超速門檻 45 r/min 下取得；上限 90 r/min、門檻 108 r/min 已於 2026-11-23 在懸空輪實測（見下方「90／108 r/min 懸空輪實測」）。** 3.0 A 轉矩手動停止條件（項目 3.0 A）未更動，未針對 90 r/min 重新評估。

- **90／108 r/min 懸空輪實測（2026-11-23，`live`，輪子懸空；log：`phase3-drive/logs/device-monitor-261123-162402.log`；以下數字由 Claude 自行重算，不沿用使用者回報的數字）**：
  - 狀態：`BOOT`→`DISARMED`（641 ms）→`ARMING`（2795 ms）→`ARMED`（2844 ms）→`DISARMING`（30620 ms）→`DISARMED`（30637 ms）；ARMED 約 27.8 s；沒有 `FAULT_STOP`／`LATCHED`，也沒有任何 `# ARM blocked`。
  - 筆數（T 行）：FC 0x03 共 125 筆、FC 0x06 共 3 筆（`200Dh`、`200Eh=0x08`、`200Eh=0x07`）、FC 0x10 共 56 筆，**全部 OK、沒有任何失敗行**。FC 0x10 的 56 筆不是實際寫入次數：ARMED 期間 T 行每 10 個週期才完整記錄一次（失敗一律記錄）。實際週期數看 `C` 行：548 個，每個週期寫一次 2088h，另加解鎖與解除各 1 次。
  - 指令：搖桿最大 1997、最小 1000；指令最大 +89.4、最小 −90.0 r/min（第一次上升的頂點是 +89.2，後來一次到 +89.4）；寫入值左 −90～+89、右 −89～+90（右輪取反），**每個 `C` 行都滿足右＝−左**。
  - 回授（D 行，每 10 個週期取樣，各 55 筆）：左 −90.5～+89.6、右 −90.5～+89.5 r/min；最大絕對值 90.5，**未達超速門檻 108**。左右同一時間點取樣 46 組、其中 |v|>5 的 35 組，**左右回授全部同號**（往上皆正、往下皆負）。
  - 斜坡：三次完整上升（+89.2、+89.4、−90.0）都是 30 步＝1500 ms（C 行 50 ms 解析度，從 0 的那個週期算起；從第一個非零值算是 29 步＝1450 ms，與使用者回報的約 1.45 s 一致）；放開搖桿回零：−90.0→0 為 30 步＝1500 ms，+89.2→0 為 31 步＝1550 ms。
  - `error_code_L`／`error_code_R` 全程 0（D 行各 58 筆）；轉矩 20ADh／20AEh 左 −0.5～+0.6 A、右 −0.7～+0.7 A（D 行取樣），低於 3.0 A 手動停止條件。
  - **超速門檻 108 r/min 沒有在實機觸發，只由 native 測試驗證。**
  - 結論：上限 90 r/min、斜坡 60 r/min/s、方向在懸空輪上正常。**「90／108 尚未實測、尚未燒錄」已改為「已於懸空輪實測」。** 項 22、25、28 維持待確認；斷線停止延遲在 90 r/min 下未另行實測（DESIGN §4 的前提仍待重新評估）。

## 2026-11-24

### 階段三：混控版 dryrun

來源為已核對的下列 log。由 log 推論前四筆測試時驅動器未回應（未上電）。前兩筆是開機後的片段，逾時狀態行見後兩筆。CH4 的實體操作方向及測試判定由使用者回報。時間均為 ESP32 log 的毫秒時間戳。

| Log | 結果 |
|---|---|
| [113113_T2(a)](../phase3-drive/logs/device-monitor-261124-113113_T2%28a%29.log) | 片段只有遙控器 `R`／T2 摘要，沒有開機探測或解鎖狀態行。80,402 ms 的 CH2／CH7／CH4 為 `1999/1000/1999`，81,416 ms 變為 `1500/1000/1501`，仍為 `RECEIVING`。僅記錄輸入行為。 |
| [113316_T2(b)](../phase3-drive/logs/device-monitor-261124-113316_T2%28b%29.log) | 片段沒有開機探測或解鎖狀態行。180,898 ms 在 261 ms 未收到訊框後進入 `STALE`，當時通道值為 `1371/2000/1000`。僅記錄輸入行為。 |
| [113640](../phase3-drive/logs/device-monitor-261124-113640.log) | 531 ms `BOOT`，887 ms 讀 `20A0h` 三次皆逾時，進入 `FAULT_STOP`、`LATCHED`，原因 `PROBE_FAIL`。 |
| [135301](../phase3-drive/logs/device-monitor-261124-135301.log) | 531 ms `BOOT`，887 ms 讀 `20A0h` 三次皆逾時，進入 `FAULT_STOP`、`LATCHED`，原因 `PROBE_FAIL`。 |
| [135557](../phase3-drive/logs/device-monitor-261124-135557.log) | 632 ms 進入 `DISARMED`。23,903–29,903 ms 各次取樣的 CH2 都在死區、CH7 為 2000、CH4 為 2000，期間無 `ARMING` 或 `ARMED`。CH4 偏轉時無法解鎖。 |
| [135943](../phase3-drive/logs/device-monitor-261124-135943.log) | 兩次解鎖、混控和 T2 (a)(b) 停機序列均有記錄。細節見下方。 |
| [140438](../phase3-drive/logs/device-monitor-261124-140438.log) | 7,583、8,338 ms 已出現 `STALE`，首次 `ARMED` 在 16,314 ms，未形成「先解鎖再斷訊」的驗證順序。這次不計。 |
| [140730](../phase3-drive/logs/device-monitor-261124-140730.log) | 步驟 10 通過。`STALE` 解除後 SWA 維持 ON 時拒絕重新解鎖，SWA 重撥後才解鎖。細節見下方。 |

- **135943 解鎖與混控**：5,124 ms `ARMING`，5,134 ms `ARMED`。`C` 行有正負 22.5 r/min 的轉向量。例如 12,811 ms 左右目標為 63.7、18.8 r/min，16 位元寫入值為 `64,-19`。38,835 ms 左輪目標達 90.0 r/min 限幅，右輪目標為 66.5 r/min。CH4 維持約 2000、CH2 回中時，27,024 ms 的 `C` 行兩輪目標與原始輸出都是 0。`# DRY W` 記錄的是預計送出的寫入幀。
- **135943 T2 (a) 遙控器關機**：39,536 ms `DISARMING`，依序印出 2088h／2089h 雙輪零速與 `200Eh=0x07`，同一毫秒回到 `DISARMED`。39,808 ms 的 `R` 仍是 `RECEIVING`，CH2／CH7／CH4 為 `1500/1000/1501`。CH4 失效保護設定為 **1500**，log 原始值 **1501** 在中位死區內。`R` 約每秒取樣，未記錄遙控器實際關機瞬間。
- **135943 T2 (b) i-BUS 訊框中斷**：47,210 ms 再次解鎖。55,534 ms 在 250 ms 未收到訊框後進入 `STALE`，CH7 仍是 2000。55,549 ms（比 `STALE` 晚 15 ms）進入 `DISARMING`，依序印出雙輪零速、`200Eh=0x07`，再回到 `DISARMED`。這是 dryrun 的狀態機與預計幀時序。
- **140730 步驟 10**：4,205 ms `ARMED`。8,876 ms 在 250 ms 未收到訊框後出現 `STALE`，8,880 ms 解除並印出雙輪零速及停機，8,890 ms `DISARMED`。13,923 ms 復訊時 CH7 仍是 2000，印出 `# ARM blocked: SWA already on, switch it off first`。CH7 撥關後在 22,648 ms 記錄為 1000，重新撥開後才在 27,058 ms 再次 `ARMED`。
- **使用者確認（2026-11-24）**：CH4 右推為高值。混控版 T2 (a)(b) 與步驟 10 通過。使用者同意在 `live` 設定 `STEER_CH=3` 並維持 `FAILSAFE_VERIFIED=1`。`live` 混控待輪子懸空實測。

### 階段三：混控版 live 懸空測試

來源為**已與 log 核對＋使用者回報**。[device-monitor-261124-142205.log](../phase3-drive/logs/device-monitor-261124-142205.log)

- 兩次 `ARMED` 分別出現在 10.738 s、99.528 s。兩輪 `error_code` 的取樣全部為 0，最高絕對速度為 90.6 r/min，兩輪轉矩電流均在 ±0.6 A 內。

| 操作 | 取樣時間（s） | 左輪回授（r/min） | 右輪回授（r/min） |
|---|---:|---:|---:|
| 前進右轉 | 25.826 | 84.0 | 44.8 |
| 前進左轉 | 28.328 | 49.9 | 90.2 |
| 倒車右推 | 42.842 | −14.5 | −51.3 |
| 倒車左推 | 46.846 | −89.1 | −43.6 |
| 油門滿加右推 | 77.875 | 89.9 | 66.9 |

- **T2 (a)** 遙控器關機後，82.298 s `DISARMING`、82.317 s `DISARMED`。82.833 s 的 `R` 行仍為 `RECEIVING`，CH2／CH7／CH4 為 `1500/1000/1501`。
- **T2 (b)** i-BUS 訊框中斷 250 ms 後，113.847 s 出現 `STALE`，113.853 s `DISARMING`、113.871 s `DISARMED`。
- 使用者回報兩次輪子都在 1 秒內停止、無異音，轉向方向正確。
- 此版 log 兩次停機的零速與停機寫入都沒有 `T` 行，解除後也沒有速度 `D` 行。可核對狀態轉換，實際停止時間待用新版紀錄量測。

### 90 r/min 斷線停止時間

來源為**已與 log 核對＋使用者回報**。`live`、輪子懸空，指令約 90 r/min 的直行或 90／67 r/min 的轉彎。[device-monitor-261124-144610.log](../phase3-drive/logs/device-monitor-261124-144610.log)

`# STOP` 的時間從 `DISARMING` 起算，到該輪第一次取樣到絕對輪速低於 1.0 r/min 為止。目視時間依使用者回報。審查時依停機後的 `R` 行更正事件對應。CH7 降到 1000、CH2 仍維持推上值時判為 SWA 撥上。CH2／CH7／CH4 維持 `1500/1000/1501` 且 `values_unchanged_ms` 持續增加時判為關遙控器。

| 觸發方式 | Log 證據 | `# STOP L`（ms） | `# STOP R`（ms） | 使用者目視 |
|---|---|---:|---:|---|
| SWA 撥上 | 19,420 ms `DISARMING`。下一筆 `R` 的 CH7 為 1000，CH2 仍為 1995。 | 377 | 527 | 瞬間停 |
| SWA 撥上 | 26,746 ms `DISARMING`。下一筆 `R` 的 CH7 為 1000，CH2 仍為 1995，接著變成 1882、1504。 | 478 | 578 | 未另回報 |
| 直行時關遙控器 | 38,101 ms `DISARMING`。其後 `R` 的 CH2／CH7／CH4 維持 `1500/1000/1501`，`values_unchanged_ms` 從 160 增至 1,160、2,160。 | 477 | 527 | 約 1 s |
| 拔 i-BUS 線 | 54,021 ms `STALE`，距上一訊框 250 ms。7 ms 後進入 `DISARMING`。 | 427 | 527 | 約 1 s |
| 轉彎 90／67 時關遙控器 | 最後一筆速度指令原始值 `90,-67`，78,528 ms `DISARMING`。下一筆 `R` 的 CH2／CH7／CH4 為 `1500/1000/1501`。 | 527 | 427 | 約 0.5 s |

- log 沒有遙控器實際關機的時間戳，無法量出從關機到失效保護生效的延遲。
- **轉矩電流觀察**：每次停機回到 `DISARMED` 後第一筆回授，左輪為 −5.2 至 −4.9 A，絕對值均超過 3.0 A 手動門檻，右輪為 0.2 至 2.2 A。其後兩輪轉矩電流讀值皆為 0.0 A，輪速仍繼續下降。
- **推論、未驗證**：`200Eh=0x07` 之後馬達可能不再出力，輪子靠滑行減速。

## 2026-11-25

### JK2501 全斷電量測（Issue #8）
來源：使用者回報 2026-11-25，未與照片核對。

| # | 量測 | 結果 |
|---|---|---|
| ① | RS485 第 2 腳（GND）對第 5 腳（GND） | < 1 Ω |
| ② | RS485 第 2 腳對綠板電源負極 | < 1 Ω |
| ③ | RS485 第 2 腳對 MAX3485 第 5 腳（GND） | < 1 Ω |
| ④ | RS485 第 2 腳對 RS232 第 2、5 腳 | < 1 Ω（全板共地） |
| ⑤ | 綠板負極對電池負極 | < 1 Ω |
| ⑥ | J5 Pin 3（SGND）對驅動器 DC− | 電阻檔 OL（隔離） |
| ⑦ | RS485 第 1 腳（+24V）對電源正極 | 0.2 Ω（該腳已封） |
| ⑧ | RS485 第 1 腳（+24V）對第 2 腳（GND） | OL |
| ⑨ | MCU 側 IO18 排針對 RS485 第 1 腳（+24V） | OL |

**另記（使用者回報 2026-11-25，未與照片核對）**
- 電池以電表實測 **25.2 V**（XT60 端）；6S（平衡線 7 針）。Issue #8 所寫的 29.4 V（7S）不適用。
- Phase 0 停止門檻改為 **25.5 V**（使用者同意）。
- 只插 USB-C 不會供電，須先接電池。
- 接收器由 MCU 上方電源排針（+5V／GND／+3.3V）取 5V。
- 24V 路徑沒有限流或保險絲；端子未清空拍照（使用者決定不做）。

### 使用者決定（2026-11-25）：J5 接線
- J5 SGND 接綠板 RS485 **第 5 腳 GND**（B 旁），不用第 2 腳。
- ~~A＝第 3 腳、B＝第 4 腳。~~ **已更正（2026-11-25）**：這樣接 `s3_poll` 會 TIMEOUT。實際有效的接法是 A、B 對調，見下方 `s3_poll` 一節。
- 依據：量測 ⑥ 顯示 SGND 與驅動器 DC− 隔離。

### JK2501 `s3_ibus`：通過
來源：使用者回報＋已與 log 核對。
- **燒錄**：SUCCESS（esptool v4.5.1，ESP32-S3 rev v0.2），不需按 BOOT／EN，連接埠 `/dev/ttyUSB0`。
- **log**：`phase1-ibus/logs/device-monitor-261125-102813.log`、`device-monitor-261125-104338.log`。
- **第一份 log**：
  - 先 WAITING 約 12 s（31,692 ms 起；第一筆 RECEIVING 在 44,092 ms）。
  - 訊框計數每 100 ms 增加 13（約 130 個／s）。
- **通道**：CH2、CH4 隨搖桿變化，CH7 隨開關變化。
- **訊框**：兩份 log 都沒有 STALE，沒有掉訊框。

### JK2501 `s3_poll`：通過（A、B 對調後）
來源：使用者回報＋已與 log 核對。

**第一次執行：未通過（A、B 反接）**
- **log**：`phase2-modbus/logs/device-monitor-261125-104715.log`。
- **過程**：
  - 602 ms BOOT。
  - ID 1、ID 4 讀 20A0h 各 1 次，皆 TIMEOUT（707 ms、812 ms）。
  - `LATCHED reason=TIMEOUT`。
- **STAT**：`tx=2 fail=2 crc=0 timeout=2 echo=0 partial_echo=0`（rejected=0、busy=0）。
- **接線**：SGND→第 5 腳，綠板第 3 腳（標 A）→J5 A，第 4 腳（標 B）→J5 B。
- **驅動器上電與燈號**：待補（使用者未回報）。
- **原因**：A、B 反了。之後只對調 A、B 就通過；程式與腳位（RX＝GPIO15、TX＝GPIO16）都沒改。

**第二次執行：通過（A、B 對調）**
- **log**：`phase2-modbus/logs/device-monitor-261125-105535.log`。
- **更正（2026-11-25）**：先前寫這份 log「STAT 同樣是 tx=2、timeout=2、LATCHED」，是誤讀。前 4 行（`STAT` 8012–11012 ms，LATCHED）是上一次失敗執行（104715）留下的尾巴；接著板子斷電重啟（`rst:0x1 (POWERON)`），之後才是這次執行。
- **有效接法**（使用者回報 2026-11-25）：

  | 綠板 RS485 端子 | 接到驅動器 J5 |
  |---|---|
  | 第 3 腳（板上標 A） | B（Pin 5） |
  | 第 4 腳（板上標 B） | A（Pin 4） |
  | 第 5 腳 GND（B 旁） | SGND（Pin 3） |

  綠板絲印的 A／B 與驅動器 J5 的 A／B 相反。
- **交易**：
  - 598 筆交易全部 OK，全部是 FC 0x03，每筆 1 次就成功。
  - rtt 4–5 ms，平均 4.8 ms（4 ms 121 筆、5 ms 477 筆）。
- **最後一筆 STAT**（24,000 ms）：`state=POLLING tx=588 fail=0 crc=0 timeout=0 echo=0 partial_echo=0 rejected=0 busy=0`。
- **讀值**：
  - 20A0h＝0x65C9（26057）。
  - 2001h＝1（Slave ID）、2002h＝2（115200）。
  - 2000h＝0（`comm_offline_ms`）。
  - `error_code` 左右 119 筆全為 0；`hall_state` 左右皆 1；`motor_state` 左右皆 0；速度與轉矩皆 0。
  - 馬達溫度：左 21 °C、右 20 °C。
  - `dc_voltage` 27.90–27.92 V，與電表的 25.2 V（不同時間量）不一致，見 `PROJECT.md` 項 25。
- **驗收條件達成**：20A0h、2001h、2002h 都讀得回來；echo、crc、timeout 皆為 0。

**第三次：5 分鐘唯讀擷取（代理擷取，2026-11-25 11:04–11:09）**
來源：已與 log 核對。
- **log**：`phase2-modbus/logs/device-monitor-261125-110405.log`。
- **擷取方式**：
  - 先確認沒有程式佔用 `/dev/ttyUSB0`（fuser、lsof 與 /proc 掃描皆為空）。
  - 用 `script -qc "timeout 300 pio device monitor -p /dev/ttyUSB0 -b 115200 --filter log2file" /dev/null` 擷取，輸出有「Logging an output to」。
  - 只讀序列埠，沒有燒錄，也沒有送出任何資料。
  - 板子沒有重置：擷取從開機後 506,636 ms 接續到 806,294 ms，log 內沒有 `rst:`，也沒有 BOOT 行。
- **交易**：
  - T 行 7,495 筆，status 全部是 OK（其他 status 0 筆）。
  - fc 全部是 03，attempts 全部是 1。
  - rtt 最小 4 ms、平均 4.86 ms、最大 5 ms（4 ms 1,079 筆、5 ms 6,416 筆）。
- **STAT**：
  - 共 300 行，每一行都是 `fail=0 crc=0 timeout=0 echo=0 partial_echo=0 rejected=0 busy=0`。
  - 最後一筆：`# STAT,806000,state=POLLING,tx=20138,fail=0,crc=0,timeout=0,echo=0,partial_echo=0,rejected=0,busy=0`。
  - 計數從開機累計，所以開機以來 20,138 筆交易都沒有失敗。
- **dc_voltage**：27.90–27.92 V（27.90 V 204 筆、27.91 V 1,218 筆、27.92 V 77 筆）。
- **其他讀值**：各 1,499 筆，全程不變。
  - `error_code` 左右全 0；`hall_state` 左右皆 1；`motor_state` 左右皆 0。
  - 速度、轉矩皆 0.0。
  - 溫度：左 21 °C、右 20 °C。
- **非 0 的 fail／crc／timeout／echo**：無。

### JK2501 `s3_drive_dry`：開機、解鎖與混控、T2 (a)(b)
來源：使用者回報＋已與 log 核對。本節只寫 log 看得到的數字。log 在 `phase3-drive/logs/`。

**`device-monitor-261125-114105.log`：PROBE_FAIL**
- log 內有兩次開機，中間一次 `rst:0x1 (POWERON)`。兩次都是 594 ms `BOOT`、699 ms 讀 20A0h `TIMEOUT`，接著 `FAULT_STOP` → `LATCHED reason=PROBE_FAIL detail=0x1`。
- 沒有任何寫入：沒有 `# DRY W`，也沒有 FC 06／10 的 T 行。
- 使用者回報：當時驅動器斷電。

**`device-monitor-261125-114653.log`：開機、解鎖與混控、T2 (a)**
- **開機**：log 內有兩次開機，中間一次 `rst:0x1 (POWERON)`。兩次都印出 `# BOOT STOP confirmed 20A2h=0000`，然後進入 `DISARMED`（703 ms、712 ms）。
- **交易**：T 行 260 筆全部 OK。
- **解鎖**：
  - 第一次：10,973 ms `ARMING`，10,994 ms `ARMED`。
  - 第二次：66,366 ms `ARMING`，66,387 ms `ARMED`。
  - 每次解鎖前都印出 `# would-block: 2000h readback=0 want 500`。乾跑不會真的寫入，所以驅動器上的 2000h 仍是 0。
- **混控**：C 行 476 筆，都是混控格式（9 欄）；`right_target_rpm` 範圍 −0.7 到 90.0。
- **T2 (a)：遙控器關機**：
  - 32,951 ms 的 R 行讀到 CH2／CH7／CH4＝1500／1000／1501，`values_unchanged_ms`＝110；之後的 R 行一路增加（1110、2110、3110…）。
  - 由此推算，值從 32,841 ms 起不再變動；32,869 ms 進入 `DISARMING`，相差 28 ms。
  - 32,879 ms `DISARMED`；`# STOP L ms=21`、`# STOP R ms=21`。
- **第二次解除**：69,080 ms `DISARMING`，69,090 ms `DISARMED`，`# STOP L／R ms=20`。69,980 ms 的 R 行 CH7＝1000，`values_unchanged_ms`＝63（值仍在變動）。log 看不出是否關機。
- **通道實測範圍**：取自 R 行（約每秒 1 筆，共 75 筆），CH2 另取自 C 行（476 筆）。
  - CH2 1000–1997、CH7 1000–2000、CH4 1000–2000，都沒有超出 1000–2000。
  - 沒有出現 `CH_OUT_OF_RANGE`。

**`device-monitor-261125-115858.log`：T2 (b)（片段，沒有開機段）**
- **解鎖**：734,476 ms `ARMING`，734,507 ms `ARMED`。
- **i-BUS 中斷**：
  - 738,114 ms 印出 `# T2 STALE observed after 250 ms without frames`（R 行 `frame_age_ms`＝250）。
  - 31 ms 後，738,145 ms 進入 `DISARMING`。
  - 10 ms 後，738,155 ms 進入 `DISARMED`；`# STOP L ms=20`、`# STOP R ms=20`。
- **i-BUS 恢復後**：
  - 779,205 ms 恢復 `RECEIVING` 時印出 `# ARM blocked: SWA already on, switch it off first`。
  - 片段內只有一組 `ARMING`／`ARMED`（734,476／734,507 ms），之後沒有再進入 `ARMING`；整段 R 行的 CH7 都是 2000。
- **STALE／RECEIVING 交替**：738,114 ms 第一次 `STALE` 之後，R 行的狀態依序是：

  | ms | 狀態 |
  |---|---|
  | 738,877 | RECEIVING |
  | 739,776 | STALE |
  | 779,205 | RECEIVING |
  | 780,687 | STALE |
  | 780,995 | RECEIVING |
  | 799,755 | STALE |
  | 800,087 | RECEIVING |
  | 800,606 | STALE |
  | 802,447 | RECEIVING |
  | 805,138 | STALE |
  | 820,480 | RECEIVING |
  | 822,439 | STALE（到 log 結尾 838,439 ms 仍是 STALE） |

  原因待使用者確認，使用者推測可能是拔插或接觸不良。

**判定**：使用者聲明（2026-11-25）綠板 T2 (a)(b) 通過，並指定 `s3_drive_live` 加入 `-DFAILSAFE_VERIFIED=1`。

### JK2501 `s3_drive_live` 懸空測試（L1–L6）
來源：log 檔名寫在各小節（`phase3-drive/logs/`）。只記 log 裡看得到的數字；看不到的標「使用者目視，無 log」。

**L1–L4：`device-monitor-261125-130602.log`**
- **開機停止與 2000h**：
  - 603 ms 讀 20A0h OK。
  - 停止指令：612 ms 寫 `2088h＝0,0`、620 ms 寫 `200Eh＝0x07`。628 ms 讀 20A2h＝0000，印出 `# BOOT STOP confirmed 20A2h=0000`。
  - 開機時 652 ms 讀到 `2000h＝0000`，693 ms 寫入 `2000h＝01F4`（500），701 ms 讀回 01F4。718 ms `DISARMED`。
- **解鎖前重寫 2000h**：79,293 ms 寫 `2000h＝01F4`，79,302 ms 讀回 01F4。
- **解鎖序列**：79,310 `200Dh＝3` → 79,319 `2088h＝0,0` → 79,328 `200Eh＝8` → 79,336 讀 20A2h＝4040 → 79,345 `ARMED`。
- **三次解鎖與停止**：

  | 解鎖期間（ms） | 回授最高（左／右，r/min） | 轉矩最大（左／右，A） | 油門範圍 | `DISARMING`（ms） | 解除當下的 log | `# STOP`（左／右，ms） |
  |---|---|---|---|---|---|---|
  | 79,345–130,495 | 90.3／90.3 | 0.7／0.6 | 1000–1997 | 130,495 | 130,695 ms 的 R 行：CH7＝1000，CH2＝1642（值仍在變動）。使用者標示為 SWA | 128／228 |
  | 137,343–144,544 | 63.0／63.0 | 0.7／0.4 | 1504–1860 | 144,544 | 144,744 ms 的 R 行：CH2／CH7／CH4＝1500／1000／1501，`values_unchanged_ms`＝211。使用者標示為遙控器關機 | 327／427 |
  | 155,192–160,892 | 47.0／47.0 | 0.7／0.4 | 1503–1781 | 160,892 | 160,878 ms 的 R 行：STALE，`frame_age_ms`＝250。使用者標示為拔除 i-BUS | 127／277 |

- **整份 log 的交易**：T 行 FC 03 共 461 筆、FC 06 共 14 筆、FC 10 共 134 筆，全部 OK。`error_code` 沒有出現非 0。

**i-BUS 拔除與重新解鎖：`device-monitor-261125-131809.log`**
- **第一次解鎖**：5,567 ms `ARMED`，回授最高 21.4／21.0 r/min。
- **拔除 i-BUS**：
  - 17,331 ms 的 R 行 STALE，`frame_age_ms`＝254；`# T2 STALE observed after 254 ms without frames`。
  - 17,343 ms `DISARMING`，17,361 ms `DISARMED`；`# STOP L ms=277`、`# STOP R ms=277`。
- **插回後**：39,999 ms 恢復 RECEIVING（CH7＝2000），印出 `# ARM blocked: SWA already on, switch it off first`。
- **重新解鎖**：53,344 ms `ARMING`，53,387 ms `ARMED`，回授最高 54.1／54.1 r/min。
- **SWA 解除**：61,271 ms `DISARMING`，61,289 ms `DISARMED`；`# STOP L／R ms=30`。62,013 ms 的 R 行 CH7＝1000。
- **交易**：T 行 260 筆全部 OK。

**`device-monitor-261125-132054.log`**
- **第一次解鎖**：4,489–6,328 ms，油門一直是 1504，回授 0。6,328 ms `DISARMING`，`# STOP L／R ms=29`；7,018 ms 的 R 行 CH7＝1000。
- **第二次解鎖**：9,427 ms `ARMED`，到 log 結尾（32,375 ms）都沒有解除。
  - 指令最高 71.8 r/min，回授最高 70.8／70.8 r/min，油門範圍 1000–1991。
- **交易**：T 行 245 筆全部 OK，沒有 TIMEOUT。

**L5：`device-monitor-261125-132455.log`（開頭幾行是上一次執行留下的尾巴，之後有一次 `rst:`）**
- **開機到解鎖**：719 ms `DISARMED`，印出 `# ARM blocked: SWA already on`；3,875 ms `ARMED`。
- **通訊中斷前**：
  - 回授最高 53.9／53.9 r/min，轉矩最大 0.7／0.5 A，油門範圍 1180–1818。
  - 最後一筆回授：14,597 ms，左右都是 53.9 r/min。最後一筆 C 行：14,870 ms，指令 54.2 r/min。
- **故障序列**：

  | ms | 事件 |
  |---|---|
  | 15,100 | `T,…,10,2088,2,TIMEOUT,2,-,2088=0036;2089=FFCA` → `FAULT_STOP` |
  | 15,204 | 急停 `200Eh＝0x05`：TIMEOUT（attempts＝1） |
  | 15,309 | `2088h＝0,0`：TIMEOUT（attempts＝1）→ `LATCHED reason=COMM_ERROR detail=0x1` |

- **LATCHED 之後**：沒有再送出任何交易，沒有再進入 `ARMING`。i-BUS 一直是 RECEIVING，CH7＝2000，到 log 結尾（47,511 ms）。
- **輪子 1 秒內停下**：使用者目視，無 log（LATCHED 之後沒有讀取回授）。
- **為什麼 attempts＝2**（查程式，沒有改）：
  - `MODBUS_MAX_ATTEMPTS=1` 只設定 `kMaxAttempts`，也就是 `DriveLink::read` 的預設嘗試次數，用在開機與 DISARMED 的讀取。
  - 寫入另由 `drive_config.h` 的 `kWriteMaxAttempts = 2` 決定：`DriveLink::sendWrite` 的寫法是 `attempts = emergency ? 1 : kWriteMaxAttempts`。ARMED 週期內的 20A5h／20ABh 讀取也明確傳入 `kWriteMaxAttempts`。
  - 所以 ARMED 時的 `2088h` 保活寫入最多 2 次；故障路徑的急停寫入（emergency）只試 1 次，與 15,204、15,309 ms 的 attempts＝1 相符。
  - 時間也對得上：14,870 ms 那個週期的 2088h 寫入，第 1 次等滿 100 ms 逾時，隔 20 ms 重試，第 2 次再等 100 ms，加上兩次送出前的 2 ms 靜默，約 14,870＋224 ms，在 15,100 ms 記錄。中間沒有下一筆 C 行，與「同一個週期」相符。

**L6：電池供電**：使用者目視，無 log。

### 60 r/min 混控版桌測

以下數字已與 log 核對。dry 片段有 `# DRY W`，可見的 FC 0x06／0x10 `T` 行均為 `attempts=0`，代表模擬寫入。15:52 片段沒有 `# DRY W`，4 筆 FC 0x06 與 190 筆 FC 0x10 均為 `OK`、`attempts=1`，有回應時間，判定為 live。

| Log | 模式 | `C` 行目標絕對值最大（左／右，r/min） | 雙輪寫入值絕對值最大（左／右，r/min） | 回授絕對值最大（左／右，r/min） | `# STOP`（左／右，ms） |
|---|---|---|---|---|---|
| [15:30:20](../phase3-drive/logs/device-monitor-261125-153020.log) | dry | 60.0／60.0 | 60／60（預計） | 0.0／0.0 | 10／10（3 組） |
| [15:39:23](../phase3-drive/logs/device-monitor-261125-153923.log) | dry | 42.9／52.1 | 42／52（預計） | 0.0／0.0 | 10／10（2 組） |
| [15:45:35](../phase3-drive/logs/device-monitor-261125-154535.log) | dry，含完整傾倒 | 60.0／60.0 | 60／60（預計） | 0.0／0.0 | 10／10（3 組） |
| [15:52:50](../phase3-drive/logs/device-monitor-261125-155250.log) | live | 60.0／60.0 | 60／60（實際） | 60.1／60.5 | 28／28（1 組） |
| [16:52:14](../phase3-drive/logs/device-monitor-261125-165214.log) | dry，傾倒中斷 | 59.3／59.3 | 59／59（預計） | 0.0／0.0 | 未見 |

- **資料範圍**：左右各自取峰值，可能發生在不同時間。15:45 的傾倒重複收錄前兩份 dry log 的部分 `C` 行，16:52 可見的 `C` 行也都在 15:45 傾倒裡。16:52 缺少 `# ONBOARD_LOG dump end`，表中數字只涵蓋截斷前的資料。
- **混控與停止**：15:30 的 dry log 在 15,993 ms 記錄 CH2＝1503、左右目標 −45.0／45.0 r/min。15:52 的 live log 在 73,344 與 89,844 ms 記錄相反方向的 ±45.0 r/min 左右目標。該次從 128,544 ms `DISARMING` 到首筆雙輪低於 1.0 r/min 為 28 ms，後續左輪回授仍出現 −3.2 r/min（128,620 ms）。15:52 的最高回授為左 ±60.1 r/min、右 −60.5 至 60.3 r/min。
- **錯誤與逾時**：五份 log 中可見的左右 `error_code` 均為 0，未見 `TIMEOUT` 交易行。16:52 有 `LATCHED reason=PROBE_FAIL` 狀態行，該份沒有對應的逾時交易。探測逾時另見下節。
- **使用者回報（2026-11-26）**：使用者已無法回憶各場次是否接馬達、輪子是否懸空、由誰操作硬體，也無法確認各場次燒錄的 commit、分支、PlatformIO 環境或 `-DONBOARD_LOG` 設定。使用者記得當時由 Claude 或 Codex CLI 協助，無法對應到個別場次。

### JK2501 開機探測逾時

來源為板載 log dump（本機 `phase3-drive/logs/device-monitor-261125-165625.log`）及使用者回報。開機序號 5、7、9、11 在 `S,…,BOOT` 約 105 ms 後記錄 `01,03,20A0,1,TIMEOUT,1`，同一毫秒出現 `FAULT_STOP`、`LATCHED`，原因為 `PROBE_FAIL detail=0x1`。序號 6、8、10 的首筆 20A0h 均成功，RTT 3–4 ms。

使用者判斷逾時發生於冷開機，成功發生於 RESET（EN）重啟。這段 dump 沒有重啟原因，也沒有線上的 TX／RX 波形。啟動等待及重試的修正尚待實機驗證。

## 2026-11-26

### 儀表板舊版實機測試

使用者在場操作，Codex 回報結果，已與[原始序列 log](../phase3-drive/logs/dashboard-261126-093641.log)核對。

- **Type-C 拔插驗證未通過**。拔插前最後一筆 R 為 247,961 ms，插回後記到 `rst:0x1 (POWERON)`、`S,612,BOOT`，第一筆 R 為 822 ms，儀表板開機次數由 1 增至 2。此前另一次重置由使用者確認是手動按 RESET。
- **SWA 方向指示寫反**。板子在 24,229 ms 進入 `ARMED`，使用者將 SWA 撥上關閉後，165,035 ms 回到 `DISARMED`，左右輪各印 `# STOP … ms=28`。拔插驗證失敗後結束測試，未做關閉再啟動儀表板或實機 dump。

### 開埠實驗 V1–V5 與 Type-C 拔插

使用者在場操作 Type-C，Codex 執行開關埠並比對主機時間與 R 行，已與本節的序列 log 核對。每輪開埠觀察 5 秒，關埠後等 5 秒。各輪 `RECEIVING` 行的 CH7 均為 1000。

| 試驗 | 首筆 → 末筆 R（ms） | 開埠重置 | 關埠重置 |
|---|---:|---|---|
| [V1 `stty -hupcl`＋`cat`](../phase3-drive/logs/serial-open-V1.log) | 5,618（`WAITING`）→ 259,517，首筆 `RECEIVING` 為 255,497 | 未見 | 無法單獨確認，下一輪 V2 開埠時重置。 |
| [V2 舊儀表板開法](../phase3-drive/logs/serial-open-V2.log) | 1,585 → 4,585 | 有，`rst:0x1 (POWERON)` | 未見，下一筆 10,585，接續 6 秒。 |
| [V3 維持 pyserial 的 DTR／RTS 預設](../phase3-drive/logs/serial-open-V3.log) | 10,585 → 14,585 | 未見 | 未見，下一筆 20,585。 |
| [V4 開埠後依序將 RTS、DTR 設為 `False`](../phase3-drive/logs/serial-open-V4.log) | 20,585 → 24,585 | 未見 | 未見，下一筆 30,585。 |
| [V5 重做 V3](../phase3-drive/logs/serial-open-V5-V3.log) | 30,585 → 34,585 | 未見 | 未見，下一筆 40,585。 |
| [V5 重做 V4](../phase3-drive/logs/serial-open-V5-V4.log) | 40,585 → 44,585 | 未見 | 未見，下一筆 50,585。 |

- **V2 重置時點**約在開埠後 0.05 秒，距 V1 關埠約 5.05 秒，依首筆 R 的 ms 與主機時間推算。舊程式在呼叫 `open()` 前設定 DTR、RTS 為 `False`，pyserial 在開埠時套用。V1 一次開埠未見重置，V3 與 V4 各兩次開關埠均未見重置。
- V1 開埠時板子處於 `LATCHED reason=PROBE_FAIL`。[serial-open-V1.log 第 2 行](../phase3-drive/logs/serial-open-V1.log#L2)
- **V3 的 Type-C 拔插未見重置**。`/dev/ttyUSB0` 消失約 12.9 秒，R 行由拔插前最後一筆 54,585 ms 接續到重插後第一筆 111,585 ms，增量 57,000 ms 與主機經過時間相符，未見 `rst:` 或 `# BOOT seq=`。[拔插前 log](../phase3-drive/logs/serial-open-USB-before.log) · [拔插後 log](../phase3-drive/logs/serial-open-USB-after.log)

### Tkinter 桌面版實機 T1–T5

使用者在場操作與確認畫面，Codex 回報並以當次本機暫存的實機 log 和畫面狀態紀錄核對。桌面版採用 V3；單元測試 55/55 通過。兩份指定 log 均在圖形桌面完成重播，`165625` 可見傾倒橫幅及 PROBE_FAIL 的 RESET 提示，`093641` 可見兩軸、輪速、曲線及重開機提示。本機沒有 `xvfb-run`。

| 試驗 | 結果 |
|---|---|
| T1 十次開關埠 | **通過**。R 行從 797,585 到 901,585 ms，無 `rst:` 或 `# BOOT seq=`，相鄰輪次的 ms 接續。原第 6 次在 831,166 ms 出現 `ARMING` 後中斷。使用者確認當時操作了 SWA，待 SWA 關閉、輪子停止後重做第 6 次。 |
| T2 桌面畫面 | **通過**。使用者確認兩軸跟隨、SWA 顯示「關」。CH2 取樣為 1,000–1,943，CH4 為 1,000–1,502，CH7 維持 1,000。 |
| T3 Type-C 拔插三次 | **通過**。前後 R 行分別為 944,585 → 963,585、987,585 → 1,008,585、1,030,585 → 1,044,585 ms。三次都自動重連、續寫同一份 log，開機次數均為 0 → 0。 |
| T4 關閉再啟動程式三次 | **通過**。前後 R 行分別為 1,052,585 → 1,059,585、1,061,585 → 1,063,585、1,065,585 → 1,067,585 ms。各次 uptime 均接續，新視窗的開機次數皆為 0。 |
| T5 板上傾倒 | **通過**。使用者確認「傾倒中」橫幅出現與消失，log 記到 `dump begin` 與 `dump end`，另存 15,011 行，開機次數維持 0。使用者無法確認第一次點擊是否就送出。未執行 `erase`。 |

### 遙控器 SWA 實體方向

使用者回報這支遙控器 **撥上＝關閉（CH7＝1000），撥下＝開啟（CH7＝2000）**。

### PR #21 冷開機探測重試實機驗證（b2caf5d）

紀錄來自使用者在場操作、Codex 回報，已與 log 核對。綠板使用 `s3_drive_dry` 加 `-DONBOARD_LOG`，Flash 用量 340,861 B、RAM 用量 19,292 B。輪子懸空，SWA 全程撥上為關，未解鎖。[開機 log 節錄](../phase3-drive/logs/probe-verify-261126-excerpts.log)

| 編號 | 操作 | 最終狀態 | PROBE 嘗試次數 | 首筆 20A0h 狀態與 RTT |
|---|---|---|---|---|
| A1 | 冷上電 | `DISARMED` | 1/3 | `OK`，3 ms |
| A2 | 冷上電 | `DISARMED` | 1/3 | `OK`，4 ms |
| A3 | 冷上電 | `DISARMED` | 1/3 | `OK`，4 ms |
| A4 | 冷上電 | `DISARMED` | 1/3 | `OK`，3 ms |
| A5 | 冷上電 | `DISARMED` | 1/3 | `OK`，3 ms |
| B1 | RESET | `DISARMED` | 1/3 | `OK`，4 ms |
| B2 | RESET | `DISARMED` | 1/3 | `OK`，3 ms |
| B3 | RESET | `DISARMED` | 1/3 | `OK`，4 ms |
| B4 | RESET | `DISARMED` | 1/3 | `OK`，4 ms |
| B5 | RESET | `DISARMED` | 1/3 | `OK`，3 ms |

10/10 到 `DISARMED`，皆於第 1/3 次探測成功。A2–A5 的即時 log 漏了部分開機輸出，已由板上傾倒補齊。B4 期間遙控器關閉，接收器顯示 `WAITING`。B5 前已重開遙控器並確認 `RECEIVING`、CH7＝1000。

驅動器兩組電源端子在總電源關閉時拔下，三輪皆由 ESP32 單獨冷開機。每輪的 `# PROBE attempt 1/3`、`2/3`、`3/3` 均各出現一次。

| 輪次 | PROBE 1／2／3 開始時間（推算，ms） | 20A0h T 行（板上 ms） | 最終狀態 | 首次探測至鎖定（推算） |
|---|---:|---|---|---:|
| 1 | 1,111／1,716／2,321 | 1,216／1,821／2,426，皆 `TIMEOUT` | `LATCHED reason=PROBE_FAIL`，2,426 ms | 約 1,315 ms |
| 2 | 1,112／1,717／2,322 | 1,217／1,822／2,427，皆 `TIMEOUT` | `LATCHED reason=PROBE_FAIL`，2,427 ms | 約 1,315 ms |
| 3 | 1,114／1,719／2,324 | 1,219／1,824／2,429，皆 `TIMEOUT` | `LATCHED reason=PROBE_FAIL`，2,429 ms | 約 1,315 ms |

`# PROBE attempt` 行沒有板上時間戳，開始時間由 `S,…,BOOT`、T 行時間與韌體的 500 ms 等待推算。三輪從 `S,…,BOOT` 到 `S,…,LATCHED` 均為 1,815 ms。第 2、3 輪的開頭由板上傾倒補齊。三輪皆依規則 22 復位。驅動器接回後，最後一次正常冷上電確認 `DISARMED`。

節錄共 13 段。有 ROM 開機訊息時從該行開始，其餘從原檔最早可用的開機輸出或板上傾倒的 `# BOOT seq=` 開始。

**限制。** 正常供電的 10 次皆在第一次探測成功，實機未觸發「失敗後重試成功」。使用者決定跳過舊版 `92a4295` 的同環境對照，因此無法判斷成功歸因於修正，或是問題當天未重現。2026-11-25 的 `165625` 紀錄中，4 次探測失敗均發生於舊版。[11-25 探測逾時紀錄](#jk2501-開機探測逾時)
