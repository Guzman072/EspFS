"""Linux 桌面儀表板的每使用者單一實例鎖。"""

from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import tempfile


class AlreadyRunning(Exception):
    pass


@contextmanager
def instance_lock(path: Path | None = None):
    if path is None:
        directory = Path(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir())
        path = directory / f"espfs-dashboard-{os.getuid()}.lock"
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise AlreadyRunning from exc
        yield
    finally:
        # 保留路徑，避免另一個程序鎖住新建的 inode。
        os.close(fd)
