"""背景記錄：原始行 log、同名 -series.csv、傾倒視窗另存的 dump-時間.log。

- 原始 log 寫入序列埠收到的位元組，一字不改（含 \\r\\n），每行立即 flush，約每秒 fsync。
- 檔名 dashboard-YYMMDD-HHMMSS.log；同一秒重複時加 -1、-2 避免覆蓋。
"""

from __future__ import annotations

import csv
import io
import os
import time
from datetime import datetime
from pathlib import Path
from typing import Callable

FSYNC_INTERVAL_S = 1.0
SERIES_COLUMNS = ("host_time", "ms", "tgtL", "tgtR", "actL", "actR", "CH2", "CH4", "CH7", "state")


def _unique_path(directory: Path, stem: str, suffix: str) -> Path:
    candidate = directory / f"{stem}{suffix}"
    index = 1
    while candidate.exists():
        candidate = directory / f"{stem}-{index}{suffix}"
        index += 1
    return candidate


def _fmt(value) -> str:
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.1f}"
    return str(value)


class _SyncedFile:
    """flush 每次寫入；距上次 fsync 超過 FSYNC_INTERVAL_S 才 fsync。"""

    def __init__(self, path: Path, clock: Callable[[], float]) -> None:
        self.path = path
        self._clock = clock
        self._fh = open(path, "xb")  # x：不覆蓋既有檔案
        self._dirty = False
        self._last_sync = clock()

    def write(self, data: bytes) -> None:
        self._fh.write(data)
        self._fh.flush()
        self._dirty = True
        self.maybe_sync()

    def maybe_sync(self, force: bool = False) -> None:
        if not self._dirty:
            return
        now = self._clock()
        if force or now - self._last_sync >= FSYNC_INTERVAL_S:
            os.fsync(self._fh.fileno())
            self._dirty = False
            self._last_sync = now

    def close(self) -> None:
        if self._fh.closed:
            return
        self.maybe_sync(force=True)
        self._fh.close()


class SessionLogger:
    def __init__(self, log_dir: Path, now: Callable[[], datetime] = datetime.now,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self.log_dir = Path(log_dir).expanduser()
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self._now = now
        self._clock = clock
        stem = "dashboard-" + now().strftime("%y%m%d-%H%M%S")
        self.raw_path = _unique_path(self.log_dir, stem, ".log")
        self._raw = _SyncedFile(self.raw_path, clock)
        self.series_path = self.raw_path.with_name(self.raw_path.stem + "-series.csv")
        self._series = _SyncedFile(self.series_path, clock)
        self._series.write(self._csv_line(SERIES_COLUMNS))
        self.lines_written = 0
        self._dump: _SyncedFile | None = None
        self.dump_path: Path | None = None

    @staticmethod
    def _csv_line(values) -> bytes:
        buf = io.StringIO()
        csv.writer(buf, lineterminator="\n").writerow(values)
        return buf.getvalue().encode("utf-8")

    def write_raw(self, data: bytes) -> None:
        self._raw.write(data)
        self.lines_written += 1

    def write_series(self, sample: dict, host_time: datetime) -> None:
        row = (
            host_time.isoformat(timespec="milliseconds"),
            sample["ms"], _fmt(sample["tgtL"]), _fmt(sample["tgtR"]), _fmt(sample["actL"]), _fmt(sample["actR"]),
            _fmt(sample["ch2"]), _fmt(sample["ch4"]), _fmt(sample["ch7"]), _fmt(sample["state"]),
        )
        self._series.write(self._csv_line(row))

    def begin_dump(self) -> Path:
        self.end_dump()
        stem = "dump-" + self._now().strftime("%y%m%d-%H%M%S")
        self.dump_path = _unique_path(self.log_dir, stem, ".log")
        self._dump = _SyncedFile(self.dump_path, self._clock)
        return self.dump_path

    def write_dump(self, data: bytes) -> None:
        if self._dump is not None:
            self._dump.write(data)

    def end_dump(self) -> None:
        if self._dump is not None:
            self._dump.close()
            self._dump = None

    def tick(self) -> None:
        """閒置時也讓尚未 fsync 的資料在約 1 秒內落地。"""
        for f in (self._raw, self._series, self._dump):
            if f is not None:
                f.maybe_sync()

    def close(self) -> None:
        self.end_dump()
        self._raw.close()
        self._series.close()
