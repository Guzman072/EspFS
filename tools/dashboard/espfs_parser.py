"""EspFS Phase 3 序列埠輸出的解析器與即時狀態模型（純邏輯，不碰 I/O）。

輸入：一次一行（已去掉行尾的文字）。輸出：IngestResult，說明這行是否在傾倒視窗內、
是否產生曲線樣本、事件，以及是否偵測到重開機。

格式來源（韌體，不得修改）：
- R：lib/drive_ctl/failsafe_monitor.cpp  R,ms,status,frame_age_ms,values_unchanged_ms,CH2,CH7[,CH4]
- C：lib/drive_ctl/drive_fsm.cpp         C,ms,CH2raw,rampedBase,wireL,wireR[,steer,tgtL,tgtR]
- D/S/T：lib/drive_ctl/drive_link.cpp、drive_fsm.cpp
- # ONBOARD_LOG：src/onboard_log_serial.h（dump end／interrupted／read failed 都會結束傾倒）
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field

# 板子 ms 倒退超過此值視為重開機。實測 34 個 log 內，傾倒視窗以外沒有任何非重開機的倒退。
REBOOT_TOLERANCE_MS = 100

DUMP_BEGIN = "# ONBOARD_LOG dump begin"
DUMP_END_MARKERS = (
    "# ONBOARD_LOG dump end",
    "# ONBOARD_LOG dump interrupted",
    "# ONBOARD_LOG dump read failed",
)
# ESP ROM 開機訊息；出現在傾倒中代表板子在傾倒途中重開機，傾倒不會再有結尾。
ROM_BOOT_PREFIXES = ("rst:", "ESP-ROM:", "ets ")

DRIVE_KEYS = (
    "actual_velocity_L",
    "actual_velocity_R",
    "actual_torque_L",
    "actual_torque_R",
    "error_code_L",
    "error_code_R",
    "motor_state_L",
    "motor_state_R",
)
# 這些狀態下沒有目標轉速；切換到這些狀態時清掉 tgt，避免介面顯示過時的目標。
NO_TARGET_STATES = frozenset({"BOOT", "DISARMED", "FAULT_STOP", "LATCHED"})

_MS_LINE = re.compile(r"^([RCDST]),(\d+),")
_STATE_LINE = re.compile(r"^# STATE (\S+) reason=(\S+)")
_LATCHED_LINE = re.compile(r"^# LATCHED reason=(\S+)(?: detail=(\S+))?")
_BOOT_LINE = re.compile(r"^# BOOT seq=(\d+)")
_STOP_LINE = re.compile(r"^# STOP ([LR])\b(.*)")


@dataclass
class IngestResult:
    in_dump: bool = False
    dump_begin: bool = False
    dump_end: bool = False
    reboot: bool = False
    sample: dict | None = None
    events: list[dict] = field(default_factory=list)


def _event(kind: str, text: str, level: str = "info", ms: int | None = None, dump: bool = False) -> dict:
    return {"kind": kind, "text": text, "level": level, "ms": ms, "dump": dump}


def _to_int(text: str) -> int | None:
    try:
        return int(text)
    except ValueError:
        return None


def _to_float(text: str) -> float | None:
    """nan／inf 視為無效（否則後面的 int() 會丟例外）。"""
    try:
        value = float(text)
    except ValueError:
        return None
    return value if math.isfinite(value) else None


class EspfsMonitor:
    """保存板子的即時狀態。傾倒視窗內的行不更新即時狀態（凍結），只產生標記為 dump 的事件。"""

    def __init__(self) -> None:
        self.last_ms: int | None = None
        self.boot_count = 0
        self.dry = False
        self.sys_state: str | None = None
        self.reason: str | None = None
        self.latched_reason: str | None = None
        self.latched_detail: str | None = None
        self.rx: dict = {}
        self.control: dict = {}
        self.drive: dict = {}
        self.dumping = False
        self.dump_lines = 0
        self.chart_epoch = 0

    # ---- 對外 ----
    def ingest(self, line: str) -> IngestResult:
        text = line.strip()
        if self.dumping:
            return self._ingest_dump(text)
        if text == DUMP_BEGIN:
            self.dumping = True
            self.dump_lines = 1
            return IngestResult(in_dump=True, dump_begin=True,
                                events=[_event("dump", "傾倒開始（舊資料重播）", "info", self.last_ms)])
        return self._ingest_live(text)

    def end_dump(self, why: str) -> IngestResult:
        """連線中斷等外部原因結束傾倒視窗。"""
        if not self.dumping:
            return IngestResult()
        self.dumping = False
        return IngestResult(dump_end=True, events=[_event("dump", f"傾倒結束（{why}）", "warn", self.last_ms)])

    @property
    def latched(self) -> bool:
        return self.sys_state == "LATCHED"

    def snapshot(self) -> dict:
        return {
            "uptime_ms": self.last_ms,
            "boot_count": self.boot_count,
            "dry": self.dry,
            "sys_state": self.sys_state,
            "reason": self.reason,
            "latched": self.latched,
            "latched_reason": self.latched_reason,
            "latched_detail": self.latched_detail,
            "rx": dict(self.rx),
            "control": dict(self.control),
            "drive": {k: dict(v) for k, v in self.drive.items()},
            "dumping": self.dumping,
            "dump_lines": self.dump_lines,
            "chart_epoch": self.chart_epoch,
        }

    # ---- 傾倒視窗 ----
    def _ingest_dump(self, text: str) -> IngestResult:
        self.dump_lines += 1
        if text in DUMP_END_MARKERS:
            self.dumping = False
            level = "info" if text.endswith("dump end") else "warn"
            label = text.removeprefix("# ONBOARD_LOG ")
            return IngestResult(in_dump=True, dump_end=True,
                                events=[_event("dump", f"傾倒結束：{label}（{self.dump_lines} 行）", level, self.last_ms)])
        if text.startswith(ROM_BOOT_PREFIXES):
            # 傾倒途中重開機：結束視窗，這行改當即時行處理。
            self.dumping = False
            result = self._ingest_live(text)
            result.dump_end = True
            result.events.insert(0, _event("dump", "傾倒中斷：板子重開機", "warn", self.last_ms))
            return result
        return IngestResult(in_dump=True, events=self._dump_events(text))

    def _dump_events(self, text: str) -> list[dict]:
        """傾倒內容只產生事件（標 dump=True），不動即時狀態、不計開機次數。"""
        events: list[dict] = []
        m = _MS_LINE.match(text)
        ms = _to_int(m.group(2)) if m else None
        if m and m.group(1) == "S":
            events.append(_event("state", f"狀態 → {text.split(',')[2]}", "info", ms, True))
        elif m and m.group(1) == "T":
            events.extend(self._timeout_event(text, ms, True))
        elif m and m.group(1) == "D":
            parts = text.split(",")
            if len(parts) >= 4 and parts[2].startswith("error_code_"):
                value = _to_float(parts[3])
                if value:
                    events.append(_event("error", f"{parts[2]} = 0x{int(value):04X}", "error", ms, True))
        elif text.startswith("#"):
            ev = self._hash_event(text, dump=True)
            if ev is not None:
                events.append(ev)
        return events

    # ---- 即時行 ----
    def _ingest_live(self, text: str) -> IngestResult:
        result = IngestResult()
        if not text:
            return result
        m = _MS_LINE.match(text)
        if m:
            ms = int(m.group(2))
            self._track_ms(ms, result)
            handler = {"R": self._on_r, "C": self._on_c, "D": self._on_d, "S": self._on_s, "T": self._on_t}[m.group(1)]
            handler(text, ms, result)
        elif text.startswith("#"):
            self._on_hash(text, result)
        elif text.startswith("rst:"):
            result.events.append(_event("rom", f"ROM：{text}", "warn", self.last_ms))
        return result

    def _track_ms(self, ms: int, result: IngestResult) -> None:
        if self.last_ms is not None and ms + REBOOT_TOLERANCE_MS < self.last_ms:
            previous = self.last_ms
            self.boot_count += 1
            self.chart_epoch += 1
            self._reset_after_reboot()
            result.reboot = True
            result.events.append(_event("reboot", f"偵測到重開機（ms {previous} → {ms}），第 {self.boot_count} 次",
                                        "warn", ms))
        self.last_ms = ms

    def _reset_after_reboot(self) -> None:
        self.dry = False
        self.sys_state = None
        self.reason = None
        self.latched_reason = None
        self.latched_detail = None
        self.control = {}
        self.drive = {}

    def _on_r(self, text: str, ms: int, result: IngestResult) -> None:
        parts = text.split(",")
        if len(parts) < 7:
            return
        values = [_to_int(p) for p in parts[3:]]
        if None in values:
            return
        self.rx = {
            "ms": ms,
            "status": parts[2],
            "frame_age_ms": values[0],
            "values_unchanged_ms": values[1],
            "ch2": values[2],
            "ch7": values[3],
            "ch4": values[4] if len(values) > 4 else None,
        }
        result.sample = self._sample(ms)

    def _on_c(self, text: str, ms: int, result: IngestResult) -> None:
        parts = text.split(",")
        nums = [_to_float(p) for p in parts[2:]]
        if len(parts) not in (6, 9) or None in nums:
            return
        control = {"ms": ms, "ch2raw": int(nums[0]), "ramped": nums[1], "wireL": int(nums[2]), "wireR": int(nums[3])}
        if len(parts) == 9:
            control.update(steer=nums[4], tgtL=nums[5], tgtR=nums[6])
        else:  # 沒有轉向的韌體：左右目標都等於 rampedBase
            control.update(steer=None, tgtL=nums[1], tgtR=nums[1])
        self.control = control
        result.sample = self._sample(ms)

    def _on_d(self, text: str, ms: int, result: IngestResult) -> None:
        parts = text.split(",")
        if len(parts) < 4 or parts[2] not in DRIVE_KEYS:
            return
        name, value = parts[2], _to_float(parts[3])
        if value is None:
            return
        old = self.drive.get(name, {}).get("value")
        self.drive = {**self.drive, name: {"value": value, "ms": ms, "unit": parts[4] if len(parts) > 4 else ""}}
        if name.startswith("error_code_") and value != old:
            if value:
                result.events.append(_event("error", f"{name} = 0x{int(value):04X}", "error", ms))
            elif old:
                result.events.append(_event("error", f"{name} 回到 0", "info", ms))

    def _on_s(self, text: str, ms: int, result: IngestResult) -> None:
        parts = text.split(",")
        if len(parts) < 3:
            return
        state = parts[2]
        if state != self.sys_state:
            level = "error" if state in ("FAULT_STOP", "LATCHED") else "info"
            result.events.append(_event("state", f"狀態 {self.sys_state or '?'} → {state}", level, ms))
            self.reason = None
        self._set_state(state)

    def _on_t(self, text: str, ms: int, result: IngestResult) -> None:
        result.events.extend(self._timeout_event(text, ms, False))

    @staticmethod
    def _timeout_event(text: str, ms: int | None, dump: bool) -> list[dict]:
        parts = text.split(",")
        if len(parts) >= 7 and parts[6] != "OK":
            attempts = parts[7] if len(parts) > 7 else "?"
            return [_event("timeout", f"Modbus {parts[6]} fc={parts[3]} addr={parts[4]}h attempts={attempts}",
                           "warn", ms, dump)]
        return []

    def _set_state(self, state: str) -> None:
        if state != "LATCHED":
            self.latched_reason = None
            self.latched_detail = None
        if state in NO_TARGET_STATES:
            self.control = {}
        self.sys_state = state

    def _on_hash(self, text: str, result: IngestResult) -> None:
        if text.startswith("# DRY W"):
            self.dry = True
            return
        m = _STATE_LINE.match(text)
        if m:
            state, reason = m.group(1), m.group(2)
            if state != self.sys_state or reason != self.reason:
                result.events.append(_event("state", f"狀態 {state} reason={reason}",
                                            "error" if state == "LATCHED" else "info", self.last_ms))
            self._set_state(state)
            self.reason = reason
            if state == "LATCHED" and self.latched_reason is None:
                self.latched_reason = reason
            return
        m = _LATCHED_LINE.match(text)
        if m:
            self._set_state("LATCHED")
            self.latched_reason, self.latched_detail = m.group(1), m.group(2)
            self.reason = m.group(1)
        ev = self._hash_event(text, dump=False)
        if ev is not None:
            ev["ms"] = self.last_ms
            result.events.append(ev)

    @staticmethod
    def _hash_event(text: str, dump: bool) -> dict | None:
        """# 開頭行中屬於事件的那些；即時與傾倒共用。"""
        m = _STOP_LINE.match(text)
        if m:
            return _event("stop", text[2:], "warn" if "not_reached" in m.group(2) else "info", dump=dump)
        if text.startswith("# ARM blocked"):
            return _event("arm_blocked", text[2:], "warn", dump=dump)
        if _LATCHED_LINE.match(text):
            return _event("latched", text[2:], "error", dump=dump)
        m = _BOOT_LINE.match(text)
        if m:
            return _event("boot", f"BOOT seq={m.group(1)}", "info", dump=dump)
        if text.startswith("# T2 STALE"):
            return _event("rx", text[2:], "warn", dump=dump)
        if text.startswith("# ONBOARD_LOG"):
            return _event("onboard", text[2:], "info", dump=dump)
        return None

    def _sample(self, ms: int) -> dict:
        def drive_value(key: str) -> float | None:
            entry = self.drive.get(key)
            return entry["value"] if entry else None

        return {
            "epoch": self.chart_epoch,
            "ms": ms,
            "tgtL": self.control.get("tgtL"),
            "tgtR": self.control.get("tgtR"),
            "actL": drive_value("actual_velocity_L"),
            "actR": drive_value("actual_velocity_R"),
            "ch2": self.rx.get("ch2"),
            "ch4": self.rx.get("ch4"),
            "ch7": self.rx.get("ch7"),
            "state": self.sys_state,
        }
