#!/usr/bin/env python3
"""Exercise OpenDoor mouse behavior through a real ncurses PTY."""

from __future__ import annotations

import fcntl
import os
import re
import select
import signal
import socket
import struct
import sys
import tempfile
import termios
import time
from pathlib import Path


WIDTH = 100
HEIGHT = 32
CSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")


class Session:
    def __init__(self, binary: str, project: Path) -> None:
        pid, master = os.forkpty()
        if pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.execv(binary, [binary, "--ascii", "--project", str(project)])
        self.pid = pid
        self.master = master
        self.output = bytearray()
        fcntl.ioctl(master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", HEIGHT, WIDTH, 0, 0))

    def mark(self) -> int:
        self.drain(0.1)
        return len(self.output)

    def drain(self, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            readable, _, _ = select.select([self.master], [], [], 0.05)
            if not readable:
                continue
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                return
            if not chunk:
                return
            self.output.extend(chunk)

    def plain_since(self, mark: int) -> bytes:
        return CSI.sub(b"", bytes(self.output[mark:])).replace(b"\x1b(B", b"")

    def wait_for(self, needle: str, mark: int, timeout: float = 8.0) -> None:
        expected = needle.encode()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.drain(0.15)
            if expected in self.plain_since(mark):
                return
            finished, status = os.waitpid(self.pid, os.WNOHANG)
            if finished:
                raise AssertionError(
                    f"opendoor exited early with status {status}; output={self.plain_since(mark)!r}"
                )
        raise AssertionError(
            f"timed out waiting for {needle!r}; output={self.plain_since(mark)!r}"
        )

    def send(self, data: bytes) -> None:
        os.write(self.master, data)

    def click(self, x: int, y: int) -> None:
        # SGR coordinates are one-based. A press/release pair becomes BUTTON1_CLICKED.
        column = x + 1
        row = y + 1
        self.send(f"\x1b[<0;{column};{row}M\x1b[<0;{column};{row}m".encode())

    def wheel_down(self, x: int, y: int) -> None:
        self.send(f"\x1b[<65;{x + 1};{y + 1}M".encode())

    def finish(self) -> None:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            finished, status = os.waitpid(self.pid, os.WNOHANG)
            if finished:
                if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                    raise AssertionError(f"opendoor exited with status {status}")
                os.close(self.master)
                return
            self.drain(0.1)
        os.kill(self.pid, signal.SIGTERM)
        os.waitpid(self.pid, 0)
        os.close(self.master)
        raise AssertionError("opendoor did not exit after the Quit click")

    def abort(self) -> None:
        try:
            os.kill(self.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass
        try:
            os.close(self.master)
        except OSError:
            pass


def listening_socket() -> tuple[socket.socket, int]:
    for port in range(25000, 25101):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            listener.bind(("127.0.0.1", port))
            listener.listen(1)
            return listener, port
        except OSError:
            listener.close()
    raise AssertionError("unable to reserve a test conflict port")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_tui_pty.py /path/to/opendoor")
    binary = os.path.abspath(sys.argv[1])
    helper_process = Path("/proc/self/comm").read_text(encoding="utf-8").strip()
    listener, conflict_port = listening_socket()
    try:
        with tempfile.TemporaryDirectory(prefix="opendoor-tui-pty-") as temporary:
            project = Path(temporary)
            env_path = project / ".env"
            lines = [f"PORT_{index:02d}={20000 + index}\n" for index in range(20)]
            lines.append(f"CONFLICT_PORT={conflict_port}\n")
            env_path.write_text("".join(lines), encoding="utf-8")
            original = env_path.read_bytes()
            session = Session(binary, project)
            try:
                start = 0
                session.wait_for("Resolve conflicts", start)
                assert b"\x1b[38;5;245m" in bytes(session.output), \
                    "256-color menu did not render muted text with color 245"

                dashboard_mark = session.mark()
                session.click(50, 14)
                session.wait_for("RELATIVE FOLDER", dashboard_mark)
                session.wait_for("PROCESS", dashboard_mark)

                scroll_mark = session.mark()
                session.wheel_down(50, 18)
                session.wait_for("20010", scroll_mark)
                owner_mark = session.mark()
                for _ in range(8):
                    session.wheel_down(50, 18)
                session.wait_for(str(conflict_port), owner_mark)
                session.wait_for(helper_process, owner_mark)
                session.click(20, 10)
                session.drain(0.3)
                assert env_path.read_bytes() == original, "Dashboard row click modified the fixture"

                refreshed = original + b"REFRESH_PORT=10000\n"
                env_path.write_bytes(refreshed)
                refresh_mark = session.mark()
                session.click(20, HEIGHT - 1)
                session.wait_for("10000", refresh_mark)
                assert env_path.read_bytes() == refreshed, "Dashboard refresh modified the fixture"

                menu_mark = session.mark()
                session.click(33, HEIGHT - 1)
                session.wait_for("Pre-start port check", menu_mark)

                conflicts_mark = session.mark()
                session.click(50, 15)
                session.wait_for("Resolve conflicts", conflicts_mark)
                session.wait_for("Automatic changes", conflicts_mark)
                session.click(20, 10)
                session.wheel_down(50, 18)
                session.drain(0.3)
                assert env_path.read_bytes() == refreshed, "Resolve row click modified the fixture"

                return_mark = session.mark()
                session.click(40, HEIGHT - 1)
                session.wait_for("Pre-start port check", return_mark)
                assert env_path.read_bytes() == refreshed, "Resolve cancel modified the fixture"
                assert not Path(f"{env_path}.opendoor.bak").exists(), \
                    "Resolve cancel created a backup"

                session.click(50, 16)
                session.finish()
            except BaseException:
                session.abort()
                raise
    finally:
        listener.close()

    with tempfile.TemporaryDirectory(prefix="opendoor-tui-empty-") as temporary:
        project = Path(temporary)
        session = Session(binary, project)
        try:
            session.wait_for("Resolve conflicts", 0)
            dashboard_mark = session.mark()
            session.send(b"\r")
            session.wait_for("No ports found in this project", dashboard_mark)
            empty_screen = session.plain_since(dashboard_mark)
            assert b"Compose" in empty_screen
            assert b".env" in empty_screen
            assert b"package.json" in empty_screen
            assert b"Makefile" in empty_screen

            # A selection key already queued behind q must not cross the screen
            # boundary and immediately reopen the default Dashboard item.
            menu_mark = session.mark()
            session.send(b"q\r")
            session.wait_for("Pre-start port check", menu_mark)
            time.sleep(0.2)
            resolve_mark = session.mark()
            session.click(50, 15)
            session.wait_for("OPEN DOOR / Resolve conflicts", resolve_mark,
                             timeout=2.0)

            back_mark = session.mark()
            session.send(b"q")
            session.wait_for("Pre-start port check", back_mark)
            session.click(50, 16)
            session.finish()
        except BaseException:
            session.abort()
            raise
    print("PTY mouse checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
