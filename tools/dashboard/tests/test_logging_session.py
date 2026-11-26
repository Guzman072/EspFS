"""SessionLogger 與 Session：原始行一字不改、series CSV、dump 另存、指令白名單、重播既有 log。"""

import csv
import sys
import tempfile
import threading
import unittest
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))

from log_writer import SERIES_COLUMNS, SessionLogger  # noqa: E402
from session import Session  # noqa: E402
from sources import ReplaySource  # noqa: E402

REPO_LOGS = HERE.parents[3] / "phase3-drive" / "logs"
FIXTURE = HERE.parent / "fixtures" / "dump-window-excerpt.log"
FIXED_NOW = datetime(2026, 11, 25, 17, 1, 2, 345000)


class FakeSink:
    def __init__(self):
        self.sent = []

    def write_command(self, data):
        self.sent.append(data)
        return True


def replay(path: Path, log_dir: Path) -> tuple[Session, SessionLogger]:
    logger = SessionLogger(log_dir, now=lambda: FIXED_NOW)
    session = Session(logger=logger, mode="replay")
    ReplaySource(session, path, speed=0, stop=threading.Event()).run()
    logger.close()
    return session, logger


class LoggerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_file_names_and_no_overwrite(self):
        a = SessionLogger(self.dir, now=lambda: FIXED_NOW)
        b = SessionLogger(self.dir, now=lambda: FIXED_NOW)
        self.assertEqual(a.raw_path.name, "dashboard-261125-170102.log")
        self.assertEqual(a.series_path.name, "dashboard-261125-170102-series.csv")
        self.assertEqual(b.raw_path.name, "dashboard-261125-170102-1.log")
        a.close()
        b.close()

    def test_raw_bytes_written_verbatim_and_flushed(self):
        logger = SessionLogger(self.dir, now=lambda: FIXED_NOW)
        data = [b"R,1,RECEIVING,0,0,1500,1000,1500\r\n", b"\xff\xfe garbage\r\n", b"# STATE DISARMED reason=NONE\r\n"]
        for line in data:
            logger.write_raw(line)
        self.assertEqual(logger.raw_path.read_bytes(), b"".join(data))  # flush 後即可讀到，尚未 close
        self.assertEqual(logger.lines_written, 3)
        logger.close()

    def test_fsync_roughly_every_second(self):
        clock = [0.0]
        logger = SessionLogger(self.dir, now=lambda: FIXED_NOW, clock=lambda: clock[0])
        raw = logger._raw
        logger.write_raw(b"a\n")
        self.assertTrue(raw._dirty)
        clock[0] = 1.2
        logger.tick()
        self.assertFalse(raw._dirty)
        logger.close()


class SessionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.logger = SessionLogger(Path(self.tmp.name), now=lambda: FIXED_NOW)
        self.session = Session(logger=self.logger, now=lambda: FIXED_NOW)
        self.sink = FakeSink()
        self.session.set_sink(self.sink)
        self.session.set_connection("connected", "/dev/ttyUSB0")

    def tearDown(self):
        self.logger.close()
        self.tmp.cleanup()

    def feed(self, *lines):
        for line in lines:
            self.session.feed(line.encode() + b"\r\n")

    def test_series_csv_rows(self):
        self.feed("S,1,ARMED", "R,10,RECEIVING,0,0,1600,2000,1450", "D,20,actual_velocity_L,5.0,r/min",
                  "C,30,1600,5.0,5,-5,0.0,10.0,12.0")
        self.logger.close()
        rows = list(csv.reader(self.logger.series_path.read_text().splitlines()))
        self.assertEqual(tuple(rows[0]), SERIES_COLUMNS)
        self.assertEqual(rows[1], ["2026-11-25T17:01:02.345", "10", "", "", "", "", "1600", "1450", "2000", "ARMED"])
        self.assertEqual(rows[2], ["2026-11-25T17:01:02.345", "30", "10.0", "12.0", "5.0", "", "1600", "1450", "2000", "ARMED"])

    def test_dump_window_saved_separately_and_raw_log_complete(self):
        lines = ["R,288550,RECEIVING,0,0,1503,1000,1501", "# ONBOARD_LOG dump begin", "S,1563,BOOT",
                 "R,1600,RECEIVING,0,0,2000,2000,2000", "# ONBOARD_LOG dump end", "R,491359,RECEIVING,0,0,1500,1000,1501"]
        self.feed(*lines)
        self.logger.close()
        dumps = sorted(Path(self.tmp.name).glob("dump-*.log"))
        self.assertEqual([p.name for p in dumps], ["dump-261125-170102.log"])
        self.assertEqual(dumps[0].read_bytes(), "".join(ln + "\r\n" for ln in lines[1:5]).encode())
        self.assertEqual(self.logger.raw_path.read_bytes(), "".join(ln + "\r\n" for ln in lines).encode())
        series_ms = [r[1] for r in csv.reader(self.logger.series_path.read_text().splitlines())][1:]
        self.assertEqual(series_ms, ["288550", "491359"])  # 傾倒內容不進 series

    def test_disconnect_closes_dump_window(self):
        self.feed("# ONBOARD_LOG dump begin", "S,1563,BOOT")
        self.session.set_connection("disconnected", "/dev/ttyUSB0")
        self.assertFalse(self.session.monitor.dumping)
        payload, _ = self.session.updates_since(None)
        self.assertIn("傾倒結束（連線中斷）", [e["text"] for e in payload["events"]])

    def test_replay_end_closes_dump_window(self):
        self.feed("# ONBOARD_LOG dump begin", "S,1563,BOOT")
        self.session.set_connection("replay_done", "x.log")
        self.assertFalse(self.session.monitor.dumping)
        self.assertIsNone(self.logger._dump)

    def test_commands_only_in_disarmed_or_latched(self):
        self.feed("S,1,ARMED")
        self.assertEqual(self.session.send_command("dump"), (False, "只在 DISARMED 或 LATCHED 可用（目前 ARMED）"))
        self.feed("S,2,DISARMED")
        self.assertEqual(self.session.send_command("dump"), (True, "已送出"))
        self.feed("S,3,FAULT_STOP", "S,3,LATCHED")
        self.assertTrue(self.session.send_command("erase", "ERASE")[0])
        self.assertEqual(self.sink.sent, [b"dump\n", b"erase\n"])

    def test_erase_requires_confirmation_and_unknown_commands_rejected(self):
        self.feed("S,2,DISARMED")
        self.assertFalse(self.session.send_command("erase")[0])
        self.assertFalse(self.session.send_command("erase", "yes")[0])
        for bad in ("dump\n", "DUMP", "speed 10", "erase\nfoo", ""):
            self.assertEqual(self.session.send_command(bad), (False, "不允許的指令"))
        self.assertEqual(self.sink.sent, [])

    def test_commands_blocked_while_dumping_or_disconnected(self):
        self.feed("S,2,DISARMED", "# ONBOARD_LOG dump begin")
        self.assertEqual(self.session.send_command("dump"), (False, "傾倒中"))
        self.feed("# ONBOARD_LOG dump end")
        self.session.set_connection("disconnected", "/dev/ttyUSB0")
        self.assertEqual(self.session.send_command("dump"), (False, "未連線"))
        self.assertEqual(self.sink.sent, [])

    def test_reconnect_blocks_commands_until_state_seen_again(self):
        self.feed("S,2,DISARMED")
        self.session.set_connection("disconnected", "/dev/ttyUSB0")
        self.session.set_connection("connected", "/dev/ttyUSB0")
        self.assertEqual(self.session.send_command("dump"), (False, "只在 DISARMED 或 LATCHED 可用（目前 未知）"))
        self.feed("# STATE DISARMED reason=NONE")
        self.assertEqual(self.session.send_command("dump"), (True, "已送出"))
        self.assertEqual(self.sink.sent, [b"dump\n"])

    def test_parser_exception_does_not_escape_feed(self):
        def boom(text):
            raise ValueError("bug")

        self.session.monitor.ingest = boom
        self.session.feed(b"R,1,whatever\r\n")  # 不得丟例外，原始行仍要寫進 log
        self.logger.close()
        self.assertEqual(self.logger.raw_path.read_bytes(), b"R,1,whatever\r\n")
        payload, _ = self.session.updates_since(None)
        self.assertTrue(any(e["text"].startswith("解析錯誤（ValueError）") for e in payload["events"]))

    def test_updates_since_cursor(self):
        self.feed("S,1,DISARMED")
        first, cursor = self.session.updates_since(None)
        self.assertEqual(len(first["raw"]), 1)
        self.feed("R,10,RECEIVING,0,0,1500,1000,1500")
        second, _ = self.session.updates_since(cursor)
        self.assertEqual([r["text"] for r in second["raw"]], ["R,10,RECEIVING,0,0,1500,1000,1500"])
        self.assertEqual(len(second["samples"]), 1)
        self.assertEqual(second["state"]["log_lines"], 2)


class ReplayTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_replay_command_mode_is_blocked(self):
        session, _ = replay(FIXTURE, self.dir)
        self.assertEqual(session.send_command("dump"), (False, "重播模式不能送指令"))

    def test_replay_fixture_writes_dump_file(self):
        session, logger = replay(FIXTURE, self.dir)
        self.assertEqual(logger.raw_path.read_bytes(), FIXTURE.read_bytes())
        dump = self.dir / "dump-261125-170102.log"
        dump_lines = dump.read_bytes().splitlines()
        self.assertEqual(dump_lines[0], b"# ONBOARD_LOG dump begin")
        self.assertEqual(dump_lines[-1], b"# ONBOARD_LOG dump end")
        self.assertEqual(session.monitor.boot_count, 0)
        self.assertEqual(session.connection["status"], "replay_done")

    @unittest.skipUnless(REPO_LOGS.is_dir(), "repo 內沒有 phase3-drive/logs")
    def test_replay_all_repo_logs_verbatim(self):
        logs = sorted(REPO_LOGS.glob("device-monitor-*.log"))
        self.assertTrue(logs)
        for i, path in enumerate(logs):
            with self.subTest(log=path.name):
                sub = self.dir / str(i)
                session, logger = replay(path, sub)
                data = path.read_bytes()
                expected = data if data.endswith(b"\n") or not data else data + b"\n"
                self.assertEqual(logger.raw_path.read_bytes(), expected)

    @unittest.skipUnless((REPO_LOGS / "device-monitor-261123-142319.log").is_file(), "缺少測試 log")
    def test_replay_counts_reboots(self):
        session, _ = replay(REPO_LOGS / "device-monitor-261123-142319.log", self.dir)
        self.assertEqual(session.monitor.boot_count, 2)

    @unittest.skipUnless((REPO_LOGS / "device-monitor-261125-132455.log").is_file(), "缺少測試 log")
    def test_replay_comm_error_latch(self):
        session, _ = replay(REPO_LOGS / "device-monitor-261125-132455.log", self.dir)
        payload, _ = session.updates_since(None)
        texts = [e["text"] for e in payload["events"]]
        self.assertIn("LATCHED reason=COMM_ERROR detail=0x1", texts)
        self.assertTrue(any(e["kind"] == "timeout" for e in payload["events"]))
        self.assertTrue(payload["state"]["latched"])
        self.assertEqual(payload["state"]["latched_reason"], "COMM_ERROR")


if __name__ == "__main__":
    unittest.main()
