"""espfs_parser 單元測試：涵蓋 R/C/D/S/T、# 事件行、傾倒視窗與重開機偵測。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from espfs_parser import EspfsMonitor  # noqa: E402


def feed(monitor, *lines):
    return [monitor.ingest(line) for line in lines]


def kinds(results):
    return [ev["kind"] for r in results for ev in r.events]


class RLineTest(unittest.TestCase):
    def test_r_line_with_steering_channel(self):
        m = EspfsMonitor()
        (r,) = feed(m, "R,286541,RECEIVING,0,65,1503,1000,1501")
        self.assertEqual(m.rx, {"ms": 286541, "status": "RECEIVING", "frame_age_ms": 0, "values_unchanged_ms": 65,
                                "ch2": 1503, "ch7": 1000, "ch4": 1501})
        self.assertEqual(r.sample["ch2"], 1503)
        self.assertEqual(m.last_ms, 286541)

    def test_r_line_without_steering_and_waiting(self):
        m = EspfsMonitor()
        feed(m, "R,70623,WAITING,70009,70009,0,0")
        self.assertEqual(m.rx["status"], "WAITING")
        self.assertIsNone(m.rx["ch4"])

    def test_malformed_r_line_is_ignored(self):
        m = EspfsMonitor()
        (r,) = feed(m, "R,100,RECEIVING,x,1,2,3")
        self.assertEqual(m.rx, {})
        self.assertIsNone(r.sample)


class CLineTest(unittest.TestCase):
    def test_c_line_with_steering(self):
        m = EspfsMonitor()
        (r,) = feed(m, "C,12862,1679,9.0,9,-9,0.0,18.1,18.1")
        self.assertEqual(m.control["tgtL"], 18.1)
        self.assertEqual(m.control["wireR"], -9)
        self.assertEqual(r.sample["tgtR"], 18.1)

    def test_c_line_without_steering_uses_ramped_as_target(self):
        m = EspfsMonitor()
        feed(m, "C,50,1500,3.0,3,-3")
        self.assertEqual((m.control["tgtL"], m.control["tgtR"]), (3.0, 3.0))
        self.assertIsNone(m.control["steer"])


class DLineTest(unittest.TestCase):
    def test_drive_values(self):
        m = EspfsMonitor()
        feed(m, "D,34723,actual_velocity_L,12.5,r/min", "D,34723,actual_velocity_R,-3.0,r/min",
             "D,34723,actual_torque_L,0.4,A", "D,34724,motor_state_R,64,state", "D,34725,sw_version,26057,raw")
        self.assertEqual(m.drive["actual_velocity_L"]["value"], 12.5)
        self.assertEqual(m.drive["actual_velocity_R"]["value"], -3.0)
        self.assertEqual(m.drive["motor_state_R"]["value"], 64)
        self.assertNotIn("sw_version", m.drive)

    def test_nonzero_error_code_creates_event_once(self):
        m = EspfsMonitor()
        results = feed(m, "D,10,error_code_L,0,bits", "D,20,error_code_L,548,bits", "D,30,error_code_L,548,bits",
                       "D,40,error_code_L,0,bits")
        texts = [ev["text"] for r in results for ev in r.events]
        self.assertEqual(texts, ["error_code_L = 0x0224", "error_code_L 回到 0"])
        self.assertEqual(results[1].events[0]["level"], "error")

    def test_nan_and_inf_values_are_ignored(self):
        m = EspfsMonitor()
        feed(m, "D,10,error_code_L,nan,bits", "D,11,error_code_R,inf,bits", "D,12,actual_velocity_L,-inf,r/min",
             "C,13,nan,1,2,3", "# ONBOARD_LOG dump begin", "D,14,error_code_L,nan,bits")
        self.assertNotIn("error_code_L", m.drive)
        self.assertNotIn("actual_velocity_L", m.drive)
        self.assertEqual(m.control, {})


class StateLineTest(unittest.TestCase):
    def test_s_line_state_change_event(self):
        m = EspfsMonitor()
        results = feed(m, "S,1563,BOOT", "S,1667,DISARMED", "S,1667,DISARMED", "S,34174,ARMING", "S,34220,ARMED")
        self.assertEqual(kinds(results), ["state"] * 4)
        self.assertEqual(m.sys_state, "ARMED")

    def test_heartbeat_sets_reason_and_only_reports_changes(self):
        m = EspfsMonitor()
        results = feed(m, "# STATE DISARMED reason=NONE", "# STATE DISARMED reason=NONE")
        self.assertEqual(kinds(results), ["state"])
        self.assertEqual((m.sys_state, m.reason), ("DISARMED", "NONE"))

    def test_latched_with_reason(self):
        m = EspfsMonitor()
        results = feed(m, "S,614,BOOT", "T,719,01,03,20A0,1,TIMEOUT,1,-,-", "S,719,FAULT_STOP", "S,719,LATCHED",
                       "# LATCHED reason=PROBE_FAIL detail=0x1")
        self.assertTrue(m.latched)
        self.assertEqual((m.latched_reason, m.latched_detail), ("PROBE_FAIL", "0x1"))
        self.assertIn("timeout", kinds(results))
        self.assertIn("latched", kinds(results))

    def test_latched_from_heartbeat_only(self):
        m = EspfsMonitor()
        feed(m, "# STATE LATCHED reason=COMM_ERROR")
        self.assertTrue(m.latched)
        self.assertEqual(m.latched_reason, "COMM_ERROR")

    def test_targets_cleared_when_disarmed(self):
        m = EspfsMonitor()
        feed(m, "S,1,ARMED", "C,2,1600,5.0,5,-5,0.0,10.0,10.0", "S,3,DISARMING")
        self.assertEqual(m.control["tgtL"], 10.0)
        feed(m, "S,4,DISARMED")
        self.assertEqual(m.control, {})


class HashEventTest(unittest.TestCase):
    def test_event_lines(self):
        m = EspfsMonitor()
        results = feed(m, "R,100,RECEIVING,0,0,1500,1000,1500", "# STOP L ms=20", "# STOP R not_reached",
                       "# ARM blocked: SWA already on, switch it off first", "# ONBOARD_LOG busy",
                       "# T2 STALE observed after 300 ms without frames", "# T2 stale_seen=0 frame_age_ms=0 values_unchanged_ms=1",
                       "# BOOT STOP confirmed 20A2h=0000")
        self.assertEqual(kinds(results), ["stop", "stop", "arm_blocked", "onboard", "rx"])
        self.assertEqual(results[2].events[0]["level"], "warn")
        self.assertEqual(results[1].events[0]["ms"], 100)

    def test_dry_flag(self):
        m = EspfsMonitor()
        feed(m, "# DRY W 01 10 20 88 00 02 04 00 00 00 00 63 A8")
        self.assertTrue(m.dry)

    def test_ok_transaction_has_no_event(self):
        m = EspfsMonitor()
        results = feed(m, "T,1572,01,03,20A0,1,OK,1,4,20A0=65C9", "T,15100,01,10,2088,2,TIMEOUT,2,-,2088=0036;2089=FFCA")
        self.assertEqual(kinds(results), ["timeout"])
        self.assertIn("fc=10", results[1].events[0]["text"])


class RebootTest(unittest.TestCase):
    def test_ms_going_backwards_counts_boot(self):
        m = EspfsMonitor()
        results = feed(m, "# DRY W 01", "S,1,ARMED", "R,113880,RECEIVING,0,0,1500,1000,1500",
                       "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)", "S,531,BOOT")
        self.assertTrue(results[-1].reboot)
        self.assertEqual(m.boot_count, 1)
        self.assertEqual(m.chart_epoch, 1)
        self.assertFalse(m.dry)
        self.assertEqual(m.sys_state, "BOOT")
        self.assertIn("rom", kinds(results))
        self.assertIn("reboot", kinds(results))

    def test_monotonic_ms_is_not_reboot(self):
        m = EspfsMonitor()
        feed(m, "R,100,RECEIVING,0,0,1500,1000,1500", "C,150,1500,0.0,0,0,0.0,0.0,0.0", "R,1100,RECEIVING,0,0,1500,1000,1500")
        self.assertEqual(m.boot_count, 0)

    def test_small_jitter_within_tolerance_is_ignored(self):
        m = EspfsMonitor()
        feed(m, "R,5000,RECEIVING,0,0,1500,1000,1500", "C,4950,1500,0.0,0,0")
        self.assertEqual(m.boot_count, 0)


class DumpWindowTest(unittest.TestCase):
    LIVE_BEFORE = ("S,100,DISARMED", "R,288550,RECEIVING,0,0,1503,1000,1501")
    DUMP = ("# ONBOARD_LOG dump begin", "", "# BOOT seq=1", "# STATE DISARMED reason=NONE", "S,1563,BOOT",
            "R,1600,RECEIVING,0,0,2000,2000,2000", "D,1657,error_code_L,548,bits", "S,719,LATCHED",
            "# LATCHED reason=PROBE_FAIL detail=0x1")

    def test_dump_window_freezes_live_state_and_tags_events(self):
        m = EspfsMonitor()
        feed(m, *self.LIVE_BEFORE)
        frozen = m.snapshot()
        results = feed(m, *self.DUMP)
        self.assertTrue(results[0].dump_begin)
        self.assertTrue(all(r.in_dump for r in results))
        self.assertTrue(m.dumping)
        self.assertTrue(all(r.sample is None for r in results))
        snap = m.snapshot()
        for key in ("uptime_ms", "boot_count", "sys_state", "rx", "drive", "latched"):
            self.assertEqual(snap[key], frozen[key], key)
        dump_events = [ev for r in results[1:] for ev in r.events]
        self.assertTrue(dump_events and all(ev["dump"] for ev in dump_events))
        self.assertEqual({ev["kind"] for ev in dump_events}, {"boot", "state", "error", "latched"})

    def test_dump_end_resumes_without_false_reboot(self):
        m = EspfsMonitor()
        feed(m, *self.LIVE_BEFORE)
        feed(m, *self.DUMP)
        (end, live) = feed(m, "# ONBOARD_LOG dump end", "R,491359,RECEIVING,0,31534,1500,1000,1501")
        self.assertTrue(end.dump_end and end.in_dump)
        self.assertFalse(m.dumping)
        self.assertFalse(live.in_dump)
        self.assertEqual(m.boot_count, 0)
        self.assertEqual(m.rx["ms"], 491359)

    def test_dump_interrupted_and_read_failed_close_window(self):
        for marker in ("# ONBOARD_LOG dump interrupted", "# ONBOARD_LOG dump read failed"):
            m = EspfsMonitor()
            feed(m, "# ONBOARD_LOG dump begin", "S,5,BOOT")
            (r,) = feed(m, marker)
            self.assertTrue(r.dump_end)
            self.assertEqual(r.events[0]["level"], "warn")
            self.assertFalse(m.dumping)

    def test_rom_boot_inside_dump_ends_window(self):
        m = EspfsMonitor()
        feed(m, "R,90000,RECEIVING,0,0,1500,1000,1500", "# ONBOARD_LOG dump begin", "S,5,BOOT")
        (r,) = feed(m, "rst:0x1 (POWERON),boot:0x2b (SPI_FAST_FLASH_BOOT)")
        self.assertTrue(r.dump_end)
        self.assertFalse(r.in_dump)
        self.assertFalse(m.dumping)
        feed(m, "S,600,BOOT")
        self.assertEqual(m.boot_count, 1)

    def test_end_dump_externally(self):
        m = EspfsMonitor()
        feed(m, "# ONBOARD_LOG dump begin")
        r = m.end_dump("連線中斷")
        self.assertTrue(r.dump_end)
        self.assertFalse(m.dumping)
        self.assertFalse(m.end_dump("x").dump_end)

    def test_real_excerpt_fixture(self):
        m = EspfsMonitor()
        fixture = Path(__file__).parent / "fixtures" / "dump-window-excerpt.log"
        results = [m.ingest(line) for line in fixture.read_text().splitlines()]
        begins = [i for i, r in enumerate(results) if r.dump_begin]
        ends = [i for i, r in enumerate(results) if r.dump_end]
        self.assertEqual(len(begins), 1)
        self.assertEqual(len(ends), 1)
        self.assertEqual(m.boot_count, 0)
        self.assertEqual(m.rx["ms"], 501409)
        boots = [ev["text"] for r in results for ev in r.events if ev["kind"] == "boot"]
        self.assertEqual(boots, ["BOOT seq=1", "BOOT seq=2", "BOOT seq=11"])
        self.assertTrue(m.latched)  # 傾倒後的即時 # STATE LATCHED reason=PROBE_FAIL


if __name__ == "__main__":
    unittest.main()
