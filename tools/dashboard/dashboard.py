#!/usr/bin/env python3
"""EspFS 儀表板：讀序列埠（或重播 log），在瀏覽器即時顯示，並在背景記錄原始 log。

用法：
  python3 tools/dashboard/dashboard.py                       # 自動偵測 /dev/ttyACM*、/dev/ttyUSB*
  python3 tools/dashboard/dashboard.py --port /dev/ttyUSB0
  python3 tools/dashboard/dashboard.py --replay phase3-drive/logs/xxx.log --speed 5
只會送出 "dump\\n" 或 "erase\\n"；沒有任何馬達控制功能。
"""

from __future__ import annotations

import argparse
import errno
import sys
import threading
import webbrowser
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from log_writer import SessionLogger  # noqa: E402
from session import Session  # noqa: E402
from sources import ReplaySource, SerialSource  # noqa: E402
from web_server import start_server  # noqa: E402

DEFAULT_LOG_DIR = Path("~/projects/EspFS/phase3-drive/logs").expanduser()
DEFAULT_HTTP_PORT = 8765


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="EspFS 儀表板（只讀顯示＋背景記錄；只能送 dump／erase）")
    p.add_argument("--port", help="序列埠路徑（預設自動偵測 /dev/ttyACM*，其次 /dev/ttyUSB*）")
    p.add_argument("--log-dir", type=Path, help=f"記錄目錄（預設 {DEFAULT_LOG_DIR}；重播模式預設不記錄）")
    p.add_argument("--replay", type=Path, help="把既有 log 當輸入重播，不開序列埠")
    p.add_argument("--speed", type=float, default=1.0, help="重播速度倍率（0＝不等待）")
    p.add_argument("--http-port", type=int, default=DEFAULT_HTTP_PORT, help="瀏覽器埠（預設 8765）")
    p.add_argument("--no-browser", action="store_true", help="不要自動開瀏覽器")
    args = p.parse_args(argv)
    if args.replay is not None and args.port:
        p.error("--replay 與 --port 不能同時使用")
    if args.replay is not None and not args.replay.is_file():
        p.error(f"找不到重播檔：{args.replay}")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stop = threading.Event()
    replay = args.replay is not None

    log_dir = args.log_dir if (args.log_dir is not None or replay) else DEFAULT_LOG_DIR
    logger = SessionLogger(log_dir) if log_dir is not None else None
    session = Session(logger=logger, mode="replay" if replay else "serial")
    if logger is not None:
        print(f"Logging an output to {logger.raw_path}", flush=True)
        print(f"Series CSV: {logger.series_path}", flush=True)
    else:
        print("重播模式：未指定 --log-dir，不寫入記錄檔", flush=True)

    try:
        server = start_server(session, args.http_port, stop)
    except OSError as exc:
        if exc.errno == errno.EADDRINUSE:
            print(f"錯誤：127.0.0.1:{args.http_port} 已被使用（儀表板是否已在執行？可改用 --http-port）", file=sys.stderr)
            if logger is not None:
                logger.close()
            return 2
        raise
    url = f"http://127.0.0.1:{args.http_port}"
    print(f"Dashboard: {url}", flush=True)

    source = ReplaySource(session, args.replay, args.speed, stop) if replay else SerialSource(session, args.port, stop)
    worker = threading.Thread(target=source.run, name="source", daemon=True)
    worker.start()
    if not args.no_browser:
        webbrowser.open(url)

    try:
        while worker.is_alive() or replay:
            stop.wait(0.5)
    except KeyboardInterrupt:
        print("\n結束中…", flush=True)
    finally:
        stop.set()
        worker.join(timeout=2.0)
        server.shutdown()
        if logger is not None:
            logger.close()
            print(f"已寫入 {logger.lines_written} 行：{logger.raw_path}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
