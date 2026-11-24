# Native 測試 CI

GitHub Actions 在每次 push 和 pull request 時於 `ubuntu-latest` 執行下列三組 PlatformIO 主機測試。任一測試失敗，對應的工作就會失敗。工作流程只執行主機測試。

## 本機執行

先在 Python 虛擬環境安裝 PlatformIO，確認 `pio` 在 `PATH` 上，再從 repo 根目錄執行：

```bash
(cd phase2-modbus && pio test -e native)
(cd phase3-drive && pio test -e native)
(cd phase3-drive && pio test -e native_dryrun)
```

## 待納入 CI 的檢查

- (a) 解鎖前重寫 `2000h` 並讀回確認。
- (b) 開機時送出停止命令並讀回確認。
- (c) CH 超出範圍時觸發 `DISARM`。

(a)～(c) 與 watchdog 檢查會隨各自的修正 PR 納入 CI。(d) 是否新增已提交的 INI 須含 `FAILSAFE_VERIFIED=1` 的檢查，待使用者決定。現有的旗標檢查由 Phase 3 native 測試執行。[現有測試](../phase3-drive/test/test_native/test_static.cpp#L174-L182)

已知會失敗的情境會收錄於 CI 門檻外的 `known-gaps`，不得用 `TEST_IGNORE` 跳過測試，也不得放寬斷言。
