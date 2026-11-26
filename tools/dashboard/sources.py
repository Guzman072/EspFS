"""資料來源：序列埠（自動偵測、拔插重連、不讓板子重開機）與 log 重播。"""

from __future__ import annotations

import glob
import os
import re
import sys
import threading
import time
from pathlib import Path
from typing import Callable

BAUDRATE = 115200
READ_TIMEOUT_S = 0.1
RETRY_INTERVAL_S = 1.0
HOLDER_CHECK_INTERVAL_S = 2.0
PORT_PATTERNS = ("/dev/ttyACM*", "/dev/ttyUSB*")  # 綠板 JK2501 的 CH340 是 ttyUSB
REPLAY_MAX_GAP_MS = 2000

_MS_LINE = re.compile(rb"^[RCDST],(\d+),")


def detect_port(globber: Callable[[str], list[str]] = glob.glob) -> str | None:
    for pattern in PORT_PATTERNS:
        found = sorted(globber(pattern))
        if found:
            return found[0]
    return None


def find_port_holders(port: str, proc_root: str = "/proc", own_pid: int | None = None) -> list[tuple[int, str]]:
    """列出（同一使用者可見的）其他行程中開著此埠的 (pid, 指令)。"""
    own_pid = os.getpid() if own_pid is None else own_pid
    try:
        target = os.path.realpath(port)
    except OSError:
        return []
    holders: list[tuple[int, str]] = []
    for entry in os.listdir(proc_root):
        if not entry.isdigit() or int(entry) == own_pid:
            continue
        fd_dir = os.path.join(proc_root, entry, "fd")
        try:
            fds = os.listdir(fd_dir)
        except OSError:
            continue
        for fd in fds:
            try:
                if os.path.realpath(os.path.join(fd_dir, fd)) == target:
                    holders.append((int(entry), _cmdline(proc_root, entry)))
                    break
            except OSError:
                continue
    return holders


def _cmdline(proc_root: str, pid: str) -> str:
    try:
        raw = Path(proc_root, pid, "cmdline").read_bytes()
    except OSError:
        return "?"
    return raw.replace(b"\0", b" ").decode("utf-8", "replace").strip()[:120] or "?"


def busy_message(port: str, holders: list[tuple[int, str]]) -> str:
    who = "；".join(f"pid {pid}: {cmd}" for pid, cmd in holders)
    return f"{port} 已被其他程式占用（{who}）。請先關掉它（例如 pio device monitor 按 Ctrl+C）。"


def open_without_reset(port: str, serial_module=None):
    """先把 DTR、RTS 設為 False 再 open，避免 ESP32 自動重置電路把板子重開機。"""
    if serial_module is None:
        import serial as serial_module  # 延後匯入：重播與測試不需要 pyserial
    ser = serial_module.Serial()
    ser.port = port
    ser.baudrate = BAUDRATE
    ser.timeout = READ_TIMEOUT_S
    ser.dtr = False
    ser.rts = False
    ser.exclusive = True  # flock；另一個也用 exclusive 的程式會被擋下
    ser.open()
    return ser


class LineSplitter:
    """把位元組流切成完整行（保留行尾）。"""

    def __init__(self) -> None:
        self._buf = b""

    def push(self, data: bytes) -> list[bytes]:
        self._buf += data
        *lines, self._buf = self._buf.split(b"\n")
        return [line + b"\n" for line in lines]

    def flush(self) -> bytes | None:
        """連線中斷時剩下的半行；補上 \\n 後寫入，避免與重連後的第一行黏在一起。"""
        rest, self._buf = self._buf, b""
        return rest + b"\n" if rest else None


class SerialSource:
    def __init__(self, session, port: str | None, stop: threading.Event, serial_module=None,
                 globber: Callable[[str], list[str]] = glob.glob) -> None:
        self.session = session
        self.port_arg = port
        self.stop = stop
        self._serial_module = serial_module
        self._globber = globber
        self._ser = None
        self._write_lock = threading.Lock()

    # CommandSink
    def write_command(self, data: bytes) -> bool:
        with self._write_lock:
            if self._ser is None:
                return False
            try:
                self._ser.write(data)
                self._ser.flush()
                return True
            except Exception:  # noqa: BLE001 — 任何寫入錯誤都只回報失敗
                return False

    def _resolve_port(self) -> str | None:
        if self.port_arg:
            return self.port_arg if os.path.exists(self.port_arg) else None
        return detect_port(self._globber)

    def run(self) -> None:
        self.session.set_sink(self)
        while not self.stop.is_set():
            port = self._resolve_port()
            if port is None:
                want = self.port_arg or " 或 ".join(PORT_PATTERNS)
                self.session.set_connection("waiting", None, f"等待 {want} 出現（請接上 Type-C）")
                self.stop.wait(RETRY_INTERVAL_S)
                continue
            holders = find_port_holders(port)
            if holders:
                self.session.set_connection("busy", port, busy_message(port, holders))
                self.stop.wait(RETRY_INTERVAL_S)
                continue
            try:
                ser = open_without_reset(port, self._serial_module)
            except Exception as exc:  # noqa: BLE001
                text = str(exc)
                if "lock" in text.lower() or "busy" in text.lower():
                    self.session.set_connection("busy", port, busy_message(port, [(0, text)]))
                else:
                    self.session.set_connection("error", port, f"無法開啟 {port}：{text}")
                self.stop.wait(RETRY_INTERVAL_S)
                continue
            self._read_loop(ser, port)

    def _read_loop(self, ser, port: str) -> None:
        with self._write_lock:
            self._ser = ser
        self.session.set_connection("connected", port, "")
        splitter = LineSplitter()
        last_holder_check = time.monotonic()
        try:
            while not self.stop.is_set():
                data = ser.read(ser.in_waiting or 1)
                for line in splitter.push(data):
                    self.session.feed(line)
                self.session.tick()
                if time.monotonic() - last_holder_check >= HOLDER_CHECK_INTERVAL_S:
                    last_holder_check = time.monotonic()
                    holders = find_port_holders(port)
                    message = busy_message(port, holders) + " 資料可能被搶走。" if holders else ""
                    self.session.set_connection("connected", port, message)
        except Exception as exc:  # noqa: BLE001 — 拔線時 pyserial 會丟 SerialException／OSError
            # 其他例外（例如解析錯誤）也走這裡；印出來，免得看起來只是一直斷線重連。
            print(f"序列埠讀取結束：{type(exc).__name__}: {exc}", file=sys.stderr, flush=True)
        finally:
            with self._write_lock:
                self._ser = None
            rest = splitter.flush()
            if rest is not None:
                self.session.feed(rest)
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
            if not self.stop.is_set():
                self.session.set_connection("disconnected", port, "等待重新接上")
                self.stop.wait(RETRY_INTERVAL_S)


class ReplaySource:
    """把既有 log 當輸入重播。speed=1 依 ms 間隔即時重播；speed<=0 不等待。傾倒內容不等待。"""

    def __init__(self, session, path: Path, speed: float, stop: threading.Event,
                 sleep: Callable[[float], bool] | None = None) -> None:
        self.session = session
        self.path = Path(path)
        self.speed = speed
        self.stop = stop
        self._sleep = sleep or stop.wait

    def run(self) -> None:
        self.session.set_connection("replay", str(self.path), f"重播 ×{self.speed:g}")
        last_ms: int | None = None
        in_dump = False
        with open(self.path, "rb") as fh:
            for line in fh:
                if self.stop.is_set():
                    return
                stripped = line.strip()
                if stripped == b"# ONBOARD_LOG dump begin":
                    in_dump = True
                elif stripped.startswith(b"# ONBOARD_LOG dump "):
                    in_dump = False
                m = _MS_LINE.match(stripped)
                if m and not in_dump:
                    ms = int(m.group(1))
                    if last_ms is not None and ms > last_ms and self.speed > 0:
                        self._sleep(min(ms - last_ms, REPLAY_MAX_GAP_MS) / 1000.0 / self.speed)
                    last_ms = ms
                self.session.feed(line if line.endswith(b"\n") else line + b"\n")
                self.session.tick()
        self.session.set_connection("replay_done", str(self.path), "重播完畢")
