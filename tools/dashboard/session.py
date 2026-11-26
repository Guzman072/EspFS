"""把來源（序列埠或重播）收到的行，交給解析器、記錄器與介面。

執行緒：來源執行緒呼叫 feed()／set_connection()；介面讀取 updates_since()，指令由背景送出。
全部經過同一把鎖。
"""

from __future__ import annotations

import threading
from collections import deque
from datetime import datetime
from typing import Callable, Protocol

from espfs_parser import EspfsMonitor, IngestResult

RAW_HISTORY = 200
EVENT_HISTORY = 500
SAMPLE_HISTORY = 60 * 20 * 2  # 60 秒、C 行 20 Hz，再加 R 行與餘裕

# 唯一允許送到板子的資料。不得加入任何其他字串，也不得有馬達控制功能。
ALLOWED_COMMANDS: dict[str, bytes] = {"dump": b"dump\n", "erase": b"erase\n"}
COMMAND_STATES = frozenset({"DISARMED", "LATCHED"})
ERASE_CONFIRM_TOKEN = "ERASE"


class CommandSink(Protocol):
    def write_command(self, data: bytes) -> bool: ...


class _History:
    """附遞增序號的環形緩衝，讓介面只取尚未顯示的項目。"""

    def __init__(self, maxlen: int) -> None:
        self._items: deque[tuple[int, dict]] = deque(maxlen=maxlen)
        self._seq = 0

    def append(self, item: dict) -> None:
        self._seq += 1
        self._items.append((self._seq, item))

    def since(self, seq: int) -> list[dict]:
        return [{**item, "seq": s} for s, item in self._items if s > seq]

    @property
    def last_seq(self) -> int:
        return self._seq


class Session:
    def __init__(self, logger=None, now: Callable[[], datetime] = datetime.now, mode: str = "serial") -> None:
        self._lock = threading.Lock()
        self._now = now
        self.logger = logger
        self.mode = mode
        self.monitor = EspfsMonitor()
        self.raw = _History(RAW_HISTORY)
        self.events = _History(EVENT_HISTORY)
        self.samples = _History(SAMPLE_HISTORY)
        self.connection = {"status": "starting", "port": None, "message": ""}
        self.last_line_at: datetime | None = None
        self.lines_seen = 0
        self._sink: CommandSink | None = None

    # ---- 來源端 ----
    def set_sink(self, sink: CommandSink | None) -> None:
        with self._lock:
            self._sink = sink

    def set_connection(self, status: str, port: str | None = None, message: str = "") -> None:
        with self._lock:
            old = self.connection
            # 只在狀態或埠改變時記事件；busy 另看訊息（占用者可能換人）。
            changed = (status, port) != (old["status"], old["port"]) or (status == "busy" and message != old["message"])
            self.connection = {"status": status, "port": port, "message": message}
            if changed and status in ("connected", "disconnected", "busy", "replay_done"):
                text = {"connected": f"已連線 {port}", "disconnected": f"連線中斷 {port or ''}",
                        "busy": message, "replay_done": "重播完畢"}[status]
                self._add_event({"kind": "conn", "text": text,
                                 "level": "info" if status in ("connected", "replay_done") else "warn",
                                 "ms": self.monitor.last_ms, "dump": False})
            if status in ("disconnected", "replay_done"):
                why = "連線中斷" if status == "disconnected" else "重播檔結束"
                self._apply(self.monitor.end_dump(why), None)
            if status == "disconnected":
                # 斷線期間板子狀態可能已變（例如被遙控器 ARM）；重連後收到新的 S／# STATE 前一律視為未知，指令被擋。
                self.monitor.sys_state = None

    def feed(self, raw_line: bytes) -> None:
        """raw_line 是一整行（含行尾），原封不動寫進 log。"""
        with self._lock:
            now = self._now()
            self.last_line_at = now
            self.lines_seen += 1
            if self.logger is not None:
                self.logger.write_raw(raw_line)
            text = raw_line.decode("utf-8", errors="replace").rstrip("\r\n")
            try:
                result = self.monitor.ingest(text)
            except Exception as exc:  # noqa: BLE001 — 解析器的 bug 不該讓序列埠被當成斷線
                result = IngestResult(in_dump=self.monitor.dumping)
                result.events.append({"kind": "error", "text": f"解析錯誤（{type(exc).__name__}）：{text[:80]}",
                                      "level": "warn", "ms": self.monitor.last_ms, "dump": False})
            self._apply(result, raw_line)
            self.raw.append({"text": text, "dump": result.in_dump, "host": now.strftime("%H:%M:%S")})

    def tick(self) -> None:
        with self._lock:
            if self.logger is not None:
                self.logger.tick()

    def _apply(self, result: IngestResult, raw_line: bytes | None) -> None:
        if self.logger is not None:
            if result.dump_begin:
                self.logger.begin_dump()
            if result.in_dump and raw_line is not None:
                self.logger.write_dump(raw_line)
            if result.dump_end:
                self.logger.end_dump()
        for ev in result.events:
            self._add_event(ev)
        if result.sample is not None:
            self.samples.append(result.sample)
            if self.logger is not None:
                self.logger.write_series(result.sample, self._now())

    def _add_event(self, ev: dict) -> None:
        self.events.append({**ev, "host": self._now().strftime("%H:%M:%S")})

    # ---- 介面端 ----
    def _status_locked(self) -> dict:
        logger = self.logger
        state = self.monitor.snapshot()
        state.update(
            connection=dict(self.connection),
            mode=self.mode,
            log_file=str(logger.raw_path) if logger else None,
            log_lines=logger.lines_written if logger else 0,
            dump_file=str(logger.dump_path) if logger and logger.dump_path else None,
            last_line_age_s=(self._now() - self.last_line_at).total_seconds() if self.last_line_at else None,
            lines_seen=self.lines_seen,
            commands_enabled=self._command_block_reason() is None,
            command_block_reason=self._command_block_reason(),
        )
        return state

    def updates_since(self, cursor: dict | None) -> tuple[dict, dict]:
        cursor = cursor or {"raw": 0, "events": 0, "samples": 0}
        with self._lock:
            payload = {
                "state": self._status_locked(),
                "raw": self.raw.since(cursor["raw"]),
                "events": self.events.since(cursor["events"]),
                "samples": self.samples.since(cursor["samples"]),
            }
            new_cursor = {"raw": self.raw.last_seq, "events": self.events.last_seq, "samples": self.samples.last_seq}
        return payload, new_cursor

    def _command_block_reason(self) -> str | None:
        if self.mode != "serial":
            return "重播模式不能送指令"
        if self._sink is None or self.connection["status"] != "connected":
            return "未連線"
        if self.monitor.dumping:
            return "傾倒中"
        if self.monitor.sys_state not in COMMAND_STATES:
            return f"只在 DISARMED 或 LATCHED 可用（目前 {self.monitor.sys_state or '未知'}）"
        return None

    def send_command(self, cmd: str, confirm: str | None = None) -> tuple[bool, str]:
        data = ALLOWED_COMMANDS.get(cmd)
        if data is None:
            return False, "不允許的指令"
        if cmd == "erase" and confirm != ERASE_CONFIRM_TOKEN:
            return False, "erase 需要二次確認"
        with self._lock:
            reason = self._command_block_reason()
            if reason is not None:
                return False, reason
            sink = self._sink
        # 序列寫入放在鎖外，讓來源與介面在等待寫入時仍能更新狀態。
        ok = sink.write_command(data)
        with self._lock:
            self._add_event({"kind": "cmd", "text": f"送出 {cmd}" if ok else f"送出 {cmd} 失敗",
                             "level": "info" if ok else "warn", "ms": self.monitor.last_ms, "dump": False})
        return ok, "已送出" if ok else "寫入失敗"
