#!/usr/bin/env python3
"""EspFS 桌面儀表板：讀序列埠或重播 log，支援背景記錄。"""

from __future__ import annotations

import argparse
import signal
import sys
import threading
import tkinter as tk
from pathlib import Path

from desktop import DashboardWindow
from instance_lock import AlreadyRunning, instance_lock
from log_writer import SessionLogger
from session import Session
from sources import ReplaySource, SerialSource
from view_model import DashboardModel

DEFAULT_LOG_DIR = Path('~/projects/EspFS/phase3-drive/logs').expanduser()
DUMP_BANNER_PAUSE_S = 0.65


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description='EspFS 桌面儀表板（只讀顯示＋背景記錄；只能送 dump／erase）')
    p.add_argument('--port', help='序列埠路徑（預設自動偵測 /dev/ttyACM*，其次 /dev/ttyUSB*）')
    p.add_argument('--log-dir', type=Path, help=f'記錄目錄（預設 {DEFAULT_LOG_DIR}；重播模式預設不記錄）')
    p.add_argument('--replay', type=Path, help='把既有 log 當輸入重播，不開序列埠')
    p.add_argument('--speed', type=float, default=1.0, help='板上時間的重播倍率（0＝略過行與行之間的等待）')
    args = p.parse_args(argv)
    if args.replay is not None and args.port:
        p.error('--replay 與 --port 不能同時使用')
    if args.replay is not None and not args.replay.is_file():
        p.error(f'找不到重播檔：{args.replay}')
    return args


def run_desktop(args: argparse.Namespace) -> int:
    root = tk.Tk()
    stop = threading.Event()
    replay = args.replay is not None
    log_dir = args.log_dir if (args.log_dir is not None or replay) else DEFAULT_LOG_DIR
    logger = None
    worker = None
    previous_sigint = None
    try:
        logger = SessionLogger(log_dir) if log_dir is not None else None
        session = Session(logger=logger, mode='replay' if replay else 'serial')
        if logger is not None:
            print(f'Logging an output to {logger.raw_path}', flush=True)
            print(f'Series CSV: {logger.series_path}', flush=True)
        else:
            print('重播模式：未指定 --log-dir，不寫入記錄檔', flush=True)
        window = DashboardWindow(root, DashboardModel(session), stop)
        # Tk 只在主執行緒更新。重播開始傾倒時留出一小段時間讓橫幅可見。
        if replay:
            source = ReplaySource(session, args.replay, args.speed, stop, dump_pause_s=DUMP_BANNER_PAUSE_S)
        else:
            source = SerialSource(session, args.port, stop)
        worker = threading.Thread(target=source.run, name='source', daemon=True)
        previous_sigint = signal.signal(signal.SIGINT, lambda *_: root.after_idle(window.close))
        worker.start()
        root.mainloop()
    finally:
        stop.set()
        if previous_sigint is not None:
            signal.signal(signal.SIGINT, previous_sigint)
        if worker is not None and worker.ident is not None:
            worker.join(timeout=2)
        try:
            root.destroy()
        except tk.TclError:
            pass
        if logger is not None:
            logger.close()
            print(f'已寫入 {logger.lines_written} 行：{logger.raw_path}', flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        with instance_lock():
            return run_desktop(args)
    except AlreadyRunning:
        print('EspFS 儀表板已在執行，請使用既有視窗。', file=sys.stderr)
    except (tk.TclError, OSError) as exc:
        print(f'無法啟動 EspFS 儀表板：{exc}', file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())
