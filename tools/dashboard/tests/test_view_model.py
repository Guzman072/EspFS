from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from instance_lock import AlreadyRunning, instance_lock  # noqa: E402
from session import Session  # noqa: E402
from view_model import DashboardModel, swa_display  # noqa: E402


class DashboardModelTest(unittest.TestCase):
    def test_replay_fixture_toggles_dump_and_freezes_live_panel(self):
        session = Session(mode='replay')
        model = DashboardModel(session)
        fixture = Path(__file__).with_name('fixtures') / 'dump-window-excerpt.log'
        before = None
        started = ended = False
        for line in fixture.read_bytes().splitlines(keepends=True):
            session.feed(line)
            model.refresh()
            if line.strip() == b'# ONBOARD_LOG dump begin':
                started = True
                self.assertTrue(model.dumping)
                before = model.state['uptime_ms']
            elif line.strip() == b'# ONBOARD_LOG dump end':
                ended = True
                self.assertFalse(model.dumping)
                self.assertEqual(model.state['uptime_ms'], before)
            elif started and not ended:
                self.assertTrue(model.dumping)
                self.assertEqual(model.state['uptime_ms'], before)
        self.assertTrue(started and ended)
        self.assertTrue(any(event['dump'] for event in model.events))

    def test_swa_boundaries(self):
        self.assertEqual(swa_display(None), ('—', 'muted'))
        self.assertEqual(swa_display(1499), ('關', 'good'))
        self.assertEqual(swa_display(1500), ('未達解鎖門檻', 'muted'))
        self.assertEqual(swa_display(1700), ('未達解鎖門檻', 'muted'))
        self.assertEqual(swa_display(1701), ('開', 'warn'))

    def test_reboot_notice_resets_chart(self):
        session = Session()
        model = DashboardModel(session)
        session.feed(b'R,65000,RECEIVING,0,0,1500,1000,1500\n')
        model.refresh()
        self.assertEqual(len(model.samples), 1)
        session.feed(b'R,700,RECEIVING,0,0,1500,1000,1500\n')
        model.refresh()
        self.assertEqual(model.state['boot_count'], 1)
        self.assertIn('板子重開機', model.reboot_notice)
        self.assertEqual([sample['ms'] for sample in model.samples], [700])

    def test_chart_and_raw_only_keep_the_requested_window(self):
        session = Session()
        model = DashboardModel(session)
        for ms in range(1_000, 206_000, 1_000):
            session.feed(f'R,{ms},RECEIVING,0,0,1500,1000,1500\n'.encode())
        model.refresh()
        self.assertEqual(len(model.raw), 200)
        self.assertEqual([model.samples[0]['ms'], model.samples[-1]['ms']], [145_000, 205_000])

    def test_disconnect_stays_visible_during_retry(self):
        session = Session()
        model = DashboardModel(session)
        session.set_connection('connected', '/dev/ttyUSB0')
        model.refresh()
        session.set_connection('disconnected', '/dev/ttyUSB0')
        session.set_connection('waiting')
        model.refresh()
        self.assertEqual((model.connection_label, model.port), ('已斷線', '/dev/ttyUSB0'))
        session.set_connection('connected', '/dev/ttyUSB0')
        model.refresh()
        self.assertEqual(model.connection_label, '已連線')

    def test_probe_hint_and_feedback_age(self):
        session = Session()
        model = DashboardModel(session)
        session.feed(b'# STATE LATCHED reason=PROBE_FAIL\n')
        session.feed(b'D,1000,actual_velocity_L,12.0,r/min\n')
        session.feed(b'R,3001,RECEIVING,0,0,1500,1000,1500\n')
        model.refresh()
        self.assertIn('請按板子上的 RESET', model.probe_hint)
        self.assertEqual(model.feedback('actual_velocity_L'), (12.0, True))


class InstanceLockTest(unittest.TestCase):
    def test_second_instance_is_rejected_until_the_first_exits(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'dashboard.lock'
            with instance_lock(path):
                with self.assertRaises(AlreadyRunning):
                    with instance_lock(path):
                        self.fail('second instance obtained the lock')
            with instance_lock(path):
                self.assertTrue(path.is_file())
