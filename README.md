# EspFS

ESP32 雙輪差速無線控制：FlySky FS-i6X 遙控器 → ESP32 → RS485（Modbus RTU）→ ZLTECH ZLAC8015D 驅動兩顆輪轂馬達。

## 目前階段
Phase 3（遙控器油門控制速度）已完成，並在**輪子懸空**下實測到 90 r/min；輪子著地尚未測試。最新現況、已驗證與未驗證項目見 [`PROJECT.md`](PROJECT.md) 開頭的「現況與下一步」。

## 資料夾
| 資料夾 | 內容 |
|--------|------|
| `phase1-ibus/` | Phase 1：接收遙控器 i-BUS 訊號（FS-iA6B → ESP32） |
| `phase2-modbus/` | Phase 2：Modbus RTU 唯讀通訊（只送 FC 0x03），含共用函式庫 `lib/modbus_ro/` |
| `phase3-drive/` | Phase 3：遙控器油門 → 速度模式驅動（`dryrun` 只印出寫入、`live` 真的寫入），實測 log 在 `logs/` |
| `docs/` | `TESTLOG.md`（實測紀錄）、`REGISTERS.md`（暫存器與 CRC）、`MANUALS.md`（手冊清單；PDF 只在本機、不提交） |

## 從哪裡開始讀
- [`PROJECT.md`](PROJECT.md)：專案狀態的唯一事實來源。
- [`AGENTS.md`](AGENTS.md)：唯一規則檔（Codex、Claude 與其他代理共用）。這個專案會讓馬達轉動，燒錄、上電、接馬達、寫入暫存器之前都必須先經使用者確認。
- **接線以 [`PROJECT.md`](PROJECT.md) §3 為準。**
