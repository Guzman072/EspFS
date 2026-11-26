"""本機 HTTP 伺服器：靜態頁面、SSE 推送（/events）、dump／erase 指令（/api/command）。

只綁 127.0.0.1。指令端點要求 JSON 與自訂標頭（跨站網頁無法不經預檢送出），並檢查 Host／Origin，
避免其他網頁或 DNS rebinding 觸發 erase。
"""

from __future__ import annotations

import json
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

STATIC_DIR = Path(__file__).resolve().parent / "static"
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
}
SSE_INTERVAL_S = 0.1
COMMAND_HEADER = "X-EspFS-Dashboard"
MAX_BODY_BYTES = 256


def make_handler(session, http_port: int, stop: threading.Event):
    allowed_hosts = {f"127.0.0.1:{http_port}", f"localhost:{http_port}"}
    allowed_origins = {f"http://{h}" for h in allowed_hosts}

    class Handler(BaseHTTPRequestHandler):
        server_version = "EspFSDashboard/1"

        def log_message(self, fmt, *args):  # 不把每個請求印到終端
            pass

        def _host_ok(self) -> bool:
            return self.headers.get("Host", "") in allowed_hosts

        def _send(self, status: int, body: bytes, ctype: str) -> None:
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, status: int, obj: dict) -> None:
            self._send(status, json.dumps(obj, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

        def do_GET(self):  # noqa: N802
            if not self._host_ok():
                return self._json(HTTPStatus.FORBIDDEN, {"ok": False, "message": "bad host"})
            path = self.path.split("?", 1)[0]
            if path == "/events":
                return self._stream()
            if path == "/api/state":
                payload, _ = session.updates_since(None)
                return self._json(HTTPStatus.OK, payload["state"])
            entry = STATIC_FILES.get(path)
            if entry is None:
                return self._json(HTTPStatus.NOT_FOUND, {"ok": False, "message": "not found"})
            name, ctype = entry
            self._send(HTTPStatus.OK, (STATIC_DIR / name).read_bytes(), ctype)

        def do_POST(self):  # noqa: N802
            if self.path != "/api/command":
                return self._json(HTTPStatus.NOT_FOUND, {"ok": False, "message": "not found"})
            origin = self.headers.get("Origin")
            if (not self._host_ok() or (origin is not None and origin not in allowed_origins)
                    or self.headers.get(COMMAND_HEADER) != "1"
                    or not self.headers.get("Content-Type", "").startswith("application/json")):
                return self._json(HTTPStatus.FORBIDDEN, {"ok": False, "message": "forbidden"})
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = -1
            if not 0 < length <= MAX_BODY_BYTES:
                return self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "message": "bad body"})
            try:
                body = json.loads(self.rfile.read(length))
            except (ValueError, UnicodeDecodeError):
                return self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "message": "bad json"})
            if not isinstance(body, dict) or not isinstance(body.get("cmd"), str):
                return self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "message": "bad json"})
            confirm = body.get("confirm") if isinstance(body.get("confirm"), str) else None
            ok, message = session.send_command(body["cmd"], confirm)
            self._json(HTTPStatus.OK if ok else HTTPStatus.CONFLICT, {"ok": ok, "message": message})

        def _stream(self) -> None:
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            cursor = None
            try:
                # 每 0.1 秒送一次（狀態含「距上一行幾秒」，每次都會變），同時兼作 keepalive。
                while not stop.is_set():
                    payload, cursor = session.updates_since(cursor)
                    data = json.dumps(payload, ensure_ascii=False)
                    self.wfile.write(f"data: {data}\n\n".encode())
                    self.wfile.flush()
                    stop.wait(SSE_INTERVAL_S)
            except (BrokenPipeError, ConnectionResetError):
                return

    return Handler


def start_server(session, http_port: int, stop: threading.Event) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("127.0.0.1", http_port), make_handler(session, http_port, stop))
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, name="http", daemon=True).start()
    return server
