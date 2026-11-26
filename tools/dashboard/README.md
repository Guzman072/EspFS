# EspFS 桌面儀表板

在 Linux 桌面用 Tkinter 即時查看遙控、輪速、接收器與故障狀態，並在背景記錄原始序列輸出。能送往板子的指令只有 `dump\n` 與 `erase\n`。

## 啟動

需要 Python 3.10 以上、Tkinter（`python3 -c "import tkinter"`）和可用的圖形桌面。實機序列埠需要 `pyserial`，重播與單元測試不需要。程式使用每位使用者一把 lock 檔，只允許開一個視窗。關閉視窗或在啟動終端按 Ctrl+C 即可結束。

```bash
cd ~/projects/EspFS-dashboard
python3 tools/dashboard/dashboard.py
python3 tools/dashboard/dashboard.py --port /dev/ttyUSB0
python3 tools/dashboard/dashboard.py --replay path/to/input.log --speed 10 --log-dir /tmp/espfs-replay
```

| 選項 | 用途 |
|---|---|
| `--port <路徑>` | 指定序列埠。預設搜尋 `/dev/ttyACM*` 再搜尋 `/dev/ttyUSB*`。 |
| `--log-dir <目錄>` | 指定記錄目錄。實機預設 `~/projects/EspFS/phase3-drive/logs`。 |
| `--replay <log>` | 從檔案重播。指定 `--log-dir` 才輸出新的紀錄檔。 |
| `--speed N` | 重播倍率。`0` 跳過板上時間間隔。傾倒開頭會短暫停留，讓橫幅可見。 |

## 開埠與重連驗證

`open_without_reset()` 沿用 pyserial 的 DTR／RTS 預設值，程式在開埠前後都不設定這兩條線。2026-11-26 綠板實測中，V2 舊開法在開埠約 0.05 秒後出現 `rst:0x1 (POWERON)`。維持預設值的 V3 兩次開關埠皆未觀察到重置。同日以 V3 驗證拔插，R 行從 54,585 ms 接續至 111,585 ms，兩筆相隔 57 秒。

1. 使用者確認輪子懸空、驅動器與板子電池供電、遙控器開啟、SWA 撥上為關、兩軸回中。先關閉占用序列埠的其他程式。
2. 啟動儀表板，記下 uptime、開機次數與原始 log 的最後一筆 `R,<ms>,…`。
3. 保持板子電池供電，由使用者拔下 Type-C 約 10 秒並插回。應先顯示「已斷線」，然後自動連上並續寫同一份 log。比對 R 行 ms 與實際經過時間，開機次數應維持原值。
4. 關閉儀表板後再啟動，核對新 log 的第一筆 R 行 ms 是否接續。開機次數以單次程式執行計算，新視窗從 0 起算。
5. 若看到 ms 跌回開機初值、`rst:` 或「板子重開機」提示，記下發生時機與前後 R 行。

## 畫面與記錄

- 頂列顯示連線、埠、板子 uptime、偵測到的重開機次數及 log 檔名與行數。狀態面板顯示系統狀態、原因、接收器與 `frame_age`。`LATCHED` 有紅色警告，`PROBE_FAIL` 另顯示「請按板子上的 RESET」。故障處理依 `AGENTS.md` 規則 21–23。
- 遙控位置以 1500 為中點，灰帶標示 1460–1540 死區。接收器有資料時，CH7 小於 1500 顯示「關」、1500–1700 顯示灰色「未達解鎖門檻」、大於 1700 顯示「開」。這支遙控器撥上＝關、撥下＝開。
- 輪子面板顯示實際轉速、目標、電流、`error_code` 和 `motor_state`。虛線為 ±60 r/min 上限，紅線為 ±72 r/min 超速線。超過兩秒未更新的回授標「舊」。下方分頁可看最近 60 秒曲線、事件或最近 200 行原始輸出。uptime 回退超過 100 ms 時清空曲線並顯示重開機提示。
- `dump` 與 `erase` 只在已連線且處於 `DISARMED` 或 `LATCHED` 時可用，傾倒期間停用。`erase` 先跳確認，再要求輸入 `ERASE`。傾倒時顯示橫幅，即時面板與曲線暫停更新，舊事件標成「傾倒」。
- `dashboard-YYMMDD-HHMMSS.log` 保存原始序列輸出，重連續寫。`dashboard-YYMMDD-HHMMSS-series.csv` 保存每個 C／R 行的最新數值，欄位為 `host_time,ms,tgtL,tgtR,actL,actR,CH2,CH4,CH7,state`。傾倒段另存 `dump-YYMMDD-HHMMSS.log`，不混入 series CSV。

## 已知限制

- USB 驅動與板子電路會影響開埠時 DTR／RTS 的實際行為。上述實測是 JK2501／CH340 的結果。拔下 Type-C 時仍需保持板子供電，才能驗證 uptime 是否接續。
- 占用偵測只能看到目前使用者有權檢查的 `/proc` 行程。另有序列埠 exclusive 鎖，但其他程式須使用相容的鎖才會被擋。
- 開機次數來自即時 uptime 回退，只計本次執行看到的事件。傾倒內容、其中的 `# STATE` 和舊 ms 不更新即時面板。收到 `# DRY W` 才會顯示 DRY 版。
- CH7 顯示按上述三態分界。畫面只反映接收值，實際解鎖由韌體與其回中、SWA 及其他安全條件判斷。

## 測試

```bash
cd tools/dashboard
python3 -m unittest discover -s tests
```

單元測試使用假序列埠及暫存資料，涵蓋解析、記錄、拔插重連、不設定 DTR／RTS、指令限制，以及不用開視窗的 view-model。
