@AGENTS.md

# Claude 專用

- 規則一律以 `AGENTS.md` 為準（唯一規則檔；規則 9–12 的編號與意思不變）。本檔只放 Claude 專用內容。
- 規則 1–8（模型、代理、context 與交接）是使用者的全域工作原則，在 `~/.claude/CLAUDE.md`，不在本 repo。
- Claude 的記憶與 `/handoff` 筆記 Codex 看不到；交接內容一律寫進 `PROJECT.md`（`AGENTS.md` 規則 24）。
- ECC 工作流程（外掛 `ecc@ecc`）：動手實作前先用 `/plan` 列出步驟、風險與相依項目並經確認；變更完成後用 `/code-review`（C++ 用 `/cpp-review`）審查；遵循 TDD，提交前確認測試通過；提交前做安全檢查（`/security-scan`）。
