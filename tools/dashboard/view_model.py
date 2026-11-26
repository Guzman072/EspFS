"""儀表板的顯示狀態，可在沒有 Tk 的環境測試。"""

from __future__ import annotations

from collections import deque

from session import EVENT_HISTORY, RAW_HISTORY, SAMPLE_HISTORY, Session

CHART_MS = 60_000
STALE_MS = 2_000
CONNECTION_LABELS = {
    "starting": "啟動中", "waiting": "等待連線", "connected": "已連線",
    "disconnected": "已斷線", "busy": "埠被占用", "error": "開埠失敗",
    "replay": "重播中", "replay_done": "重播完畢",
}


def swa_display(value: int | None) -> tuple[str, str]:
    if value is None:
        return "—", "muted"
    if value < 1500:
        return "關", "good"
    if value <= 1700:
        return "未達解鎖門檻", "muted"
    return "開", "warn"


class DashboardModel:
    def __init__(self, session: Session) -> None:
        self.session = session
        self.cursor = None
        self.state: dict = {}
        self.events = deque(maxlen=EVENT_HISTORY)
        self.raw = deque(maxlen=RAW_HISTORY)
        self.samples = deque(maxlen=SAMPLE_HISTORY)
        self.dumping = False
        self.connection_status = "starting"
        self.port: str | None = None
        self.was_connected = False
        self.chart_epoch = 0

    def refresh(self) -> None:
        payload, self.cursor = self.session.updates_since(self.cursor)
        self.state = payload["state"]
        self.dumping = self.state["dumping"]
        self.events.extend(payload["events"])
        self.raw.extend(payload["raw"])
        connection = self.state["connection"]
        self.port = connection["port"] or self.port
        self.connection_status = connection["status"]
        if self.connection_status in ("connected", "disconnected"):
            self.was_connected = True
        if self.connection_status == "waiting" and self.was_connected:
            self.connection_status = "disconnected"

        epoch = self.state["chart_epoch"]
        if epoch != self.chart_epoch:
            self.samples.clear()
            self.chart_epoch = epoch
        self.samples.extend(sample for sample in payload["samples"] if sample["epoch"] == epoch)
        while self.samples and self.samples[0]["ms"] < self.state["uptime_ms"] - CHART_MS:
            self.samples.popleft()

    @property
    def connection_label(self) -> str:
        return CONNECTION_LABELS[self.connection_status]

    @property
    def probe_hint(self) -> str:
        if self.state["latched"] and self.state["latched_reason"] == "PROBE_FAIL":
            return "PROBE_FAIL：請按板子上的 RESET 重新開機"
        return ""

    @property
    def reboot_notice(self) -> str:
        count = self.state["boot_count"]
        if not count:
            return ""
        event = next((e["text"] for e in reversed(self.events) if e["kind"] == "reboot"), "")
        return f"板子重開機 · {event or f'本次共偵測到 {count} 次'}"

    def feedback(self, key: str) -> tuple[float | None, bool]:
        reading = self.state["drive"].get(key)
        if reading is None:
            return None, False
        now = self.state["uptime_ms"]
        return reading["value"], now is not None and now - reading["ms"] > STALE_MS
