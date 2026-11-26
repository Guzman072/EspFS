"""sources（不重開機的開啟方式、占用偵測、拔插重連）。

全部使用假的 serial 模組與暫存檔，不開任何真實序列埠。
"""

import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from session import Session  # noqa: E402
from sources import (LineSplitter, SerialSource, busy_message, detect_port, find_port_holders,  # noqa: E402
                     open_without_reset)


class FakeSerialException(Exception):
    pass


class FakeSerialModule:
    """每次 open 取用一段腳本：list[bytes]；讀完就丟例外（模擬拔線）。"""

    def __init__(self, scripts, stop):
        self.scripts = list(scripts)
        self.stop = stop
        self.opened = []
        self.written = []
        module = self

        class Serial:
            def __init__(self):
                self.calls = []
                self._dtr = self._rts = True
                self.is_open = False

            def __setattr__(self, name, value):
                if name in ("port", "baudrate", "timeout", "exclusive", "dtr", "rts"):
                    self.__dict__.setdefault("calls", []).append((name, value))
                object.__setattr__(self, name, value)

            def open(self):
                self.calls.append(("open", None))
                if not module.scripts:
                    module.stop.set()
                    raise FakeSerialException("no device")
                self.chunks = list(module.scripts.pop(0))
                self.is_open = True
                module.opened.append(self)

            @property
            def in_waiting(self):
                return len(self.chunks[0]) if self.chunks else 0

            def read(self, n):
                if not self.chunks:
                    raise FakeSerialException("device reports readiness to read but returned no data")
                return self.chunks.pop(0)

            def write(self, data):
                module.written.append(data)

            def flush(self):
                pass

            def close(self):
                self.is_open = False

        self.Serial = Serial


class OpenWithoutResetTest(unittest.TestCase):
    def test_open_does_not_set_dtr_or_rts(self):
        stop = threading.Event()
        mod = FakeSerialModule([[b""]], stop)
        ser = open_without_reset("/dev/ttyUSB9", mod)
        self.assertEqual(ser.calls, [("port", "/dev/ttyUSB9"), ("baudrate", 115200),
                                     ("timeout", 0.1), ("exclusive", True), ("open", None)])


class DetectAndHoldersTest(unittest.TestCase):
    def test_detect_prefers_acm_then_usb(self):
        table = {"/dev/ttyACM*": [], "/dev/ttyUSB*": ["/dev/ttyUSB1", "/dev/ttyUSB0"]}
        self.assertEqual(detect_port(lambda p: table[p]), "/dev/ttyUSB0")
        table["/dev/ttyACM*"] = ["/dev/ttyACM0"]
        self.assertEqual(detect_port(lambda p: table[p]), "/dev/ttyACM0")
        self.assertIsNone(detect_port(lambda p: []))

    def test_find_port_holders_in_fake_proc(self):
        with tempfile.TemporaryDirectory() as tmp:
            port = Path(tmp, "ttyFAKE0")
            port.write_text("")
            proc = Path(tmp, "proc")
            for pid, target, cmd in ((111, port, b"pio\0device\0monitor"), (222, Path(tmp, "other"), b"bash")):
                (proc / str(pid) / "fd").mkdir(parents=True)
                os.symlink(target, proc / str(pid) / "fd" / "3")
                (proc / str(pid) / "cmdline").write_bytes(cmd)
            (proc / "self").mkdir()
            holders = find_port_holders(str(port), proc_root=str(proc), own_pid=999)
            self.assertEqual(holders, [(111, "pio device monitor")])
            self.assertEqual(find_port_holders(str(port), proc_root=str(proc), own_pid=111), [])
            self.assertIn("請先關掉它", busy_message(str(port), holders))


class LineSplitterTest(unittest.TestCase):
    def test_split_and_flush(self):
        s = LineSplitter()
        self.assertEqual(s.push(b"R,1,RE"), [])
        self.assertEqual(s.push(b"CEIVING\r\nS,2,BOOT\r\nC,3"), [b"R,1,RECEIVING\r\n", b"S,2,BOOT\r\n"])
        self.assertEqual(s.flush(), b"C,3\n")
        self.assertIsNone(s.flush())


class ReconnectTest(unittest.TestCase):
    def test_unplug_and_replug_keeps_ms_continuous(self):
        with tempfile.TemporaryDirectory() as tmp:
            port = Path(tmp, "ttyFAKE0")
            port.write_text("")
            stop = threading.Event()
            mod = FakeSerialModule([
                [b"S,1000,DISARMED\r\nR,1000,RECEIVING,0,0,1500,1000,1500\r\n", b"R,2000,RECEI"],
                [b"R,9000,RECEIVING,0,0,1500,1000,1500\r\n"],
            ], stop)
            session = Session(logger=None)
            source = SerialSource(session, str(port), stop, serial_module=mod)
            states = []
            original = session.set_connection

            def spy(status, port=None, message=""):
                states.append(status)
                original(status, port, message)

            session.set_connection = spy
            thread = threading.Thread(target=source.run, daemon=True)
            thread.start()
            thread.join(timeout=10)
            self.assertFalse(thread.is_alive())
            self.assertEqual(len(mod.opened), 2)
            self.assertEqual(states[:4], ["connected", "disconnected", "connected", "disconnected"])
            self.assertEqual(session.monitor.boot_count, 0)  # 重連前後 ms 連續 → 沒有重開機
            self.assertEqual(session.monitor.last_ms, 9000)
            raw, _ = session.updates_since(None)
            self.assertIn("R,2000,RECEI", [r["text"] for r in raw["raw"]])  # 拔線時的半行照樣保留
