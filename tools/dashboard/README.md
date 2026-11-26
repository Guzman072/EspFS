# EspFS 儀表板（tools/dashboard）

接上 Type-C 後，在瀏覽器即時顯示遙控數據、輪子轉速與狀態，並在背景持續記錄原始 log。
**只讀顯示＋記錄**：不修改任何韌體檔案；唯一會送到板子的資料只有 `dump\n`、`erase\n` 兩種字串，
而且只在 DISARMED 或 LATCHED 時才送；沒有任何馬達控制功能。

- 需求：Python 3.10 以上、`pyserial`（唯一外部套件；重播與測試不需要）。
- 前端是單頁 HTML/JS，用原生 canvas 畫圖，不使用 CDN，可以離線使用。

## 啟動

```bash
cd ~/projects/EspFS-dashboard        # 或任何含 tools/dashboard 的 checkout
python3 tools/dashboard/dashboard.py                  # 自動偵測序列埠，自動開瀏覽器
python3 tools/dashboard/dashboard.py --port /dev/ttyUSB0
python3 tools/dashboard/dashboard.py --log-dir /tmp/espfs-logs --no-browser
```

| 參數 | 說明 |
|---|---|
| `--port` | 指定序列埠。未指定時先找 `/dev/ttyACM*`，再找 `/dev/ttyUSB*`（綠板 JK2501 的 CH340 是 `/dev/ttyUSB0`）。 |
| `--log-dir` | 記錄目錄，預設 `~/projects/EspFS/phase3-drive/logs`。 |
| `--replay <log> [--speed N]` | 重播既有 log，不開序列埠。`--speed 0` 不等待。重播模式預設**不**寫記錄，給 `--log-dir` 才寫。 |
| `--http-port` | 瀏覽器埠，預設 8765（只綁 127.0.0.1）。 |
| `--no-browser` | 不自動開瀏覽器。 |

瀏覽器網址：<http://127.0.0.1:8765>。拔掉 Type-C 後會顯示「已斷線」，再插上會自動重連，續寫同一個 log 檔。
按 Ctrl+C 結束程式。

## 開啟序列埠不會讓板子重開機

程式用 `serial.Serial()` 建立物件，先設 `dtr=False`、`rts=False`，再設埠號並 `open()`
（`sources.py` 的 `open_without_reset`），ESP32 的自動重置電路就不會把 EN 拉低。

**驗證方式（使用者操作，代理不碰硬體）**：

1. 板子已上電並在跑（例如 DISARMED），啟動儀表板，記下頂列的 uptime（板子的 ms）。
2. 拔掉 Type-C，等幾秒，再插回去。
3. 重連後 uptime 要**接著原本的 ms 繼續往上**（例如 120345 → 128900），「開機次數」維持不變，
   事件列表沒有「偵測到重開機」。原始 log 裡重連前後的 `R,<ms>,…` 也必須連續、沒有倒退。
4. 另外再做一次：關掉儀表板（Ctrl+C）後重新啟動，同樣檢查 ms 接續（這是「開埠」本身的驗證）。
5. 若 ms 從幾百重新開始、開機次數 +1，代表開埠時板子被重置，請停止使用並回報。

自動化測試 `test_dtr_rts_false_before_open` 檢查 dtr／rts 一定在 open 之前設為 False；
`test_unplug_and_replug_keeps_ms_continuous` 用假的序列埠模擬拔插，檢查不會誤判重開機。

## 埠被占用

開埠前會掃描 `/proc/*/fd`，若有其他程式（例如 `pio device monitor`）開著同一個埠，
頂列下方會出現黃色訊息：「/dev/ttyUSB0 已被其他程式占用（pid …: pio device monitor …）。請先關掉它」，
並每秒重試，關掉對方後自動連上。連線中若有別的程式也開了這個埠，同樣會顯示警告（資料可能被搶走）。

## 畫面

1. **頂列**：連線狀態、埠、板子 uptime、開機次數、log 檔名與已寫行數。看到 `# DRY W` 就顯示黃色「DRY 版：不會送速度」。
2. **狀態**：系統狀態與 reason、接收器狀態、frame_age、values_unchanged。LATCHED 時出現紅色大警告；
   reason=PROBE_FAIL 另外提示「請按板子上的 RESET 重新開機」。
3. **遙控**：油門（CH2，縱軸）與轉向（CH4，橫軸）位置，標出 1500 中點與死區 1460–1540（韌體 `kDeadband=40`）；
   SWA（CH7 > 1500 顯示「開」）與原始數值。
4. **輪子**：左右輪實際轉速（actual_velocity，韌體已換成邏輯符號）長條＋目標 tgt 三角形；
   刻度 ±80，虛線 ±60 為速度上限，紅線 ±72 為超速線；另有電流、錯誤碼（非 0 紅底）、motor_state。超過 2 秒沒更新的數值標「（舊）」。
5. **即時曲線**：最近 60 秒的 tgtL／tgtR（虛線）、actL／actR（實線），以及油門、轉向。偵測到重開機時曲線清空重畫。
6. **事件**：狀態切換、STOP、ARM blocked、LATCHED、BOOT、Modbus TIMEOUT、非 0 錯誤碼、連線變化等，附主機時間與板子秒數。
   傾倒內容裡的事件標「傾倒」並以淡色顯示（可勾選隱藏）。可展開「原始輸出」看最近 200 行。
7. **dump／erase 按鈕**：只在已連線、DISARMED 或 LATCHED、且不在傾倒中時可按。erase 先跳確認視窗，再要求輸入 `ERASE`。
   伺服器端同樣檢查狀態與確認字串，只接受這兩個指令；指令端點要求 JSON 與自訂標頭並檢查 Host／Origin，其他網頁無法代送。

## 傾倒（dump）

`# ONBOARD_LOG dump begin` 到 `dump end`（或 `dump interrupted`、`dump read failed`）之間是板上舊資料重播：

- 即時面板與曲線凍結，頂列與橫幅顯示「傾倒中」；
- 所有行照常寫入主 log；
- 視窗內的行（含開頭、結尾標記）另存成 `dump-YYMMDD-HHMMSS.log`（同一記錄目錄）；
- 傾倒內容的 ms 倒退不算重開機；連線中斷、重播檔結束、或傾倒中出現 ESP ROM 開機訊息時，視窗自動結束。

## 背景記錄

- 程式一啟動就開始記錄，與瀏覽器是否開著無關。啟動時終端機印出 `Logging an output to <檔名>`，介面頂列也顯示檔名（滑鼠移上去看完整路徑）。
- `dashboard-YYMMDD-HHMMSS.log`：序列埠收到的原始位元組，一字不改（含 `\r\n`），所以既有的分析方式照樣能用。
  每行立即 flush，約每秒 fsync。重連時續寫同一個檔；開機事件留在內容裡。
  唯一的例外：拔線時若剩半行，會補一個 `\n` 再寫入，避免和重連後的第一行黏在一起。
- `dashboard-YYMMDD-HHMMSS-series.csv`：欄位 `host_time,ms,tgtL,tgtR,actL,actR,CH2,CH4,CH7,state`，
  每個 C 行（約 50 ms）與 R 行（約 1 秒）各一列，值為當下最新的數值；`host_time` 為 ISO 8601 本機時間（到毫秒）。傾倒內容不寫入。
- ms 倒退（超過 100 ms）記為一次開機事件，開機次數加 1。

## 測試

```bash
cd tools/dashboard
python3 -m unittest discover -s tests -v
```

- `test_parser.py`：R／C／D／S／T、`#` 事件行、傾倒視窗（凍結、結尾三種標記、傾倒中重開機）、重開機偵測；
  `tests/fixtures/dump-window-excerpt.log` 取自實測 log `device-monitor-261125-165625.log` 的開頭與傾倒結尾。
- `test_logging_session.py`：原始 log 與輸入逐位元組相同（重播 `phase3-drive/logs/` 內全部已追蹤 log）、
  series CSV、dump 另存、指令白名單與狀態限制、重開機計數、COMM_ERROR LATCHED。
- `test_sources_web.py`：dtr／rts 在 open 前設為 False、`/proc` 占用偵測、拔插重連、HTTP 靜態檔、SSE、指令端點的防護。

測試全部用假的序列埠與暫存目錄，不開任何真實序列埠、不需要硬體。

## 已知限制

- Linux 開啟 tty 時，核心會在 pyserial 設定 DTR/RTS 之前短暫拉高 DTR/RTS（兩條同時）。ESP32 的雙電晶體自動重置電路在兩條同時變化時通常不會重置，
  但這取決於板子電路，所以務必用上面的方法實際驗證一次。
- 占用偵測只看得到同一使用者的行程；其他使用者或 root 的程式開著埠時，只能靠 `exclusive` 鎖（對方也用 flock 時才有效）。
- 傾倒期間韌體只即時送 `S,` 行（會中斷傾倒）與 `# STATE` 心跳；心跳和傾倒內容的 `# STATE` 無法區分，所以傾倒期間整個面板凍結。
- DRY 標示以「本次開機後看過 `# DRY W`」為準。DRY 版在 DISARMED 時不會印 `# DRY W`，所以中途接上時要等到開機或 ARMED 才會出現。
- SWA 的「開」依規格以 CH7 > 1500 判斷；韌體解鎖門檻是 `kArmOnThreshold=1700`，兩者只在 1501–1700 之間的值不一致。
- 介面顯示的是板子送出的數值，不是安全依據；停止與故障處理仍依 `AGENTS.md` 規則 21–23。
