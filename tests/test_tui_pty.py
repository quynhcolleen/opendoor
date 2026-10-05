#!/usr/bin/env python3
"""Exercise OpenDoor navigation and recorded changes through a real ncurses PTY."""

from __future__ import annotations

import fcntl
import json
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
SCREEN_TOKEN = re.compile(r"\x1b\[([0-?]*)([ -/]*)([@-~])|\x1b[()][0-2A-Z]|\x1b[78=>]|([^\x1b])")


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

    def screen_lines(self) -> list[str]:
        # ncurses emits deltas, so stripping CSI loses unchanged portions of a
        # row. Replay cursor/erase operations to inspect the displayed page.
        rows = [[" "] * WIDTH for _ in range(HEIGHT)]
        x = y = 0
        saved = (0, 0)
        top, bottom = 0, HEIGHT - 1
        last = " "
        for token in SCREEN_TOKEN.finditer(self.output.decode("utf-8", errors="replace")):
            parameters, _, command, character = token.groups()
            if command is not None:
                if parameters.startswith("?"):
                    continue
                values = [int(part or 0) for part in parameters.split(";")]
                amount = values[0] or 1
                if command in ("H", "f"):
                    y = amount - 1
                    x = (values[1] or 1) - 1 if len(values) > 1 else 0
                elif command == "d":
                    y = amount - 1
                elif command == "G":
                    x = amount - 1
                elif command in ("A", "B", "C", "D"):
                    y += amount * (1 if command == "B" else -1) if command in "AB" else 0
                    x += amount * (1 if command == "C" else -1) if command in "CD" else 0
                elif command == "J":
                    start = y * WIDTH + x if values[0] == 0 else 0
                    end = y * WIDTH + x + 1 if values[0] == 1 else HEIGHT * WIDTH
                    for index in range(start, end):
                        rows[index // WIDTH][index % WIDTH] = " "
                elif command in ("K", "X"):
                    start = x if values[0] == 0 or command == "X" else 0
                    end = min(WIDTH, x + amount) if command == "X" else \
                        x + 1 if values[0] == 1 else WIDTH
                    rows[y][start:end] = [" "] * (end - start)
                elif command in ("L", "M"):
                    for _ in range(min(amount, bottom - y + 1)):
                        if command == "L":
                            rows.insert(y, [" "] * WIDTH)
                            rows.pop(bottom + 1)
                        else:
                            rows.pop(y)
                            rows.insert(bottom, [" "] * WIDTH)
                elif command in ("S", "T"):
                    for _ in range(min(amount, bottom - top + 1)):
                        if command == "S":
                            rows.pop(top)
                            rows.insert(bottom, [" "] * WIDTH)
                        else:
                            rows.insert(top, [" "] * WIDTH)
                            rows.pop(bottom + 1)
                elif command == "r":
                    top = amount - 1
                    bottom = (values[1] or HEIGHT) - 1 if len(values) > 1 else HEIGHT - 1
                    x = y = 0
                elif command == "b":
                    for _ in range(amount):
                        rows[y][x] = last
                        x = min(x + 1, WIDTH - 1)
                x, y = min(max(x, 0), WIDTH - 1), min(max(y, 0), HEIGHT - 1)
            elif character is not None:
                if character == "\r":
                    x = 0
                elif character == "\n":
                    if y == bottom:
                        rows.pop(top)
                        rows.insert(bottom, [" "] * WIDTH)
                    else:
                        y = min(y + 1, HEIGHT - 1)
                elif character == "\b":
                    x = max(x - 1, 0)
                elif character == "\t":
                    x = min((x // 8 + 1) * 8, WIDTH - 1)
                elif character >= " ":
                    rows[y][x] = last = character
                    x = min(x + 1, WIDTH - 1)
            elif token.group() == "\x1b7":
                saved = (x, y)
            elif token.group() == "\x1b8":
                x, y = saved
        return ["".join(row) for row in rows]

    def wait_for_line(self, needle: str, row: int, timeout: float = 8.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.drain(0.05)
            if needle in self.screen_lines()[row]:
                return
        raise AssertionError(
            f"timed out waiting for {needle!r} on row {row}; screen={self.screen_lines()!r}"
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

    def wheel_up(self, x: int, y: int) -> None:
        self.send(f"\x1b[<64;{x + 1};{y + 1}M".encode())

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


def history_records(path: Path) -> list[dict]:
    raw = path.read_bytes()
    assert raw.endswith(b"\n"), "history has an uncommitted-looking tail"
    records = [json.loads(line) for line in raw.splitlines()]
    assert all(isinstance(record, dict) and
               all(not isinstance(value, (dict, list)) for value in record.values())
               for record in records), "history records are not flat declaration changes"
    return records


def exercise_recorded_changes(session: Session, project: Path,
                              refreshed: bytes, conflict_port: int) -> None:
    env_path = project / ".env"
    backup = Path(f"{env_path}.opendoor.bak")
    log = project / ".opendoor" / "history.log"
    apply_mark = session.mark()
    session.click(50, 15)
    session.wait_for("Automatic changes", apply_mark)
    session.send(b"\r")
    session.wait_for_line("Updated 1 port(s)", HEIGHT - 2)
    applied = env_path.read_bytes()
    assert applied != refreshed, "Resolve apply did not change the source"
    assert backup.read_bytes() == refreshed, "apply backup is not the original source"
    records = history_records(log)
    assert len(records) == 1, "one changed declaration must create one history record"
    entry = records[0]
    new_port = entry["new_port"]
    assert entry["v"] == 1 and entry["id"] == 1
    assert entry["kind"] == "apply" and entry["reverts"] is None
    assert entry["file"] == "./.env" and entry["key"] == "CONFLICT_PORT"
    assert entry["source"] == "env" and entry["write"] == "env_literal"
    assert entry["old_port"] == conflict_port and new_port != conflict_port
    assert applied == refreshed.replace(f"CONFLICT_PORT={conflict_port}\n".encode(),
                                        f"CONFLICT_PORT={new_port}\n".encode())
    assert applied[entry["offset"]:entry["offset"] + entry["length"]] == str(new_port).encode()
    first_log = log.read_bytes()

    back_mark = session.mark()
    session.send(b"q")
    session.wait_for("Pre-start port check", back_mark)
    history_mark = session.mark()
    session.click(50, 16)
    session.wait_for("OPEN DOOR / History", history_mark)
    session.wait_for_line("[Revert]", 7)
    confirm_mark = session.mark()
    session.send(b"\r")
    session.wait_for("Confirm revert #1", confirm_mark)
    assert env_path.read_bytes() == applied and log.read_bytes() == first_log, \
        "opening confirmation wrote source or history"

    # Cancel is an observable input barrier after the second Enter: ncurses
    # must consume that Enter first, and no write or append may have occurred.
    cancel_mark = session.mark()
    session.send(b"\rn")
    session.wait_for("Revert cancelled", cancel_mark)
    assert env_path.read_bytes() == applied and log.read_bytes() == first_log, \
        "Enter or confirmation cancellation wrote source or history"
    assert backup.read_bytes() == refreshed, "unconfirmed revert replaced the backup"

    confirm_mark = session.mark()
    session.send(b"\r")
    session.wait_for("Confirm revert #1", confirm_mark)
    session.send(b"y")
    session.wait_for_line("Reverted history #1; updated 1 port(s)", 2)
    assert env_path.read_bytes() == refreshed, "confirmed revert did not restore exact source bytes"
    assert backup.read_bytes() == applied, "revert did not back up the applied source"
    records = history_records(log)
    assert len(records) == 2 and log.read_bytes().startswith(first_log)
    inverse = records[1]
    assert inverse["id"] == 2 and inverse["kind"] == "revert" and inverse["reverts"] == 1
    assert inverse["old_port"] == new_port and inverse["new_port"] == conflict_port
    assert inverse["file"] == entry["file"] and inverse["key"] == entry["key"]

    # The newest inverse is a redo. Ordinary row clicks select only; the exact
    # Revert cell (x=82..89) and confirm target (x=1..16) come from test_ui.c.
    second_log = log.read_bytes()
    session.click(40, 7)
    session.wait_for_line(f"./.env | CONFLICT_PORT | {new_port} -> {conflict_port}", HEIGHT - 2)
    assert env_path.read_bytes() == refreshed and log.read_bytes() == second_log
    confirm_mark = session.mark()
    session.click(82, 7)
    session.wait_for("Confirm revert #2", confirm_mark)
    assert env_path.read_bytes() == refreshed and log.read_bytes() == second_log
    session.click(1, HEIGHT - 1)
    session.wait_for_line("Reverted history #2; updated 1 port(s)", 2)
    assert env_path.read_bytes() == applied, "confirm mouse target did not redo the exact change"
    records = history_records(log)
    assert len(records) == 3 and log.read_bytes().startswith(second_log)
    assert records[2]["id"] == 3 and records[2]["kind"] == "revert"
    assert records[2]["reverts"] == 2
    assert records[2]["old_port"] == conflict_port and records[2]["new_port"] == new_port

    # Freeze a ready entry in confirmation, then externally change the same
    # token width. Confirm must revalidate and refuse rather than overwrite it.
    session.click(40, 7)
    session.wait_for_line(f"./.env | CONFLICT_PORT | {conflict_port} -> {new_port}", HEIGHT - 2)
    confirm_mark = session.mark()
    session.send(b"\r")
    session.wait_for("Confirm revert #3", confirm_mark)
    stale_port = 60000 if len(str(new_port)) == 5 else 9000
    if stale_port == new_port:
        stale_port += 1
    external = applied.replace(f"CONFLICT_PORT={new_port}\n".encode(),
                               f"CONFLICT_PORT={stale_port}\n".encode())
    env_path.write_bytes(external)
    third_log = log.read_bytes()
    third_backup = backup.read_bytes()
    session.send(b"y")
    session.wait_for_line("Revert failed for history #3", 2)
    session.wait_for_line("Unavailable", 7)
    session.wait_for_line(f"Current port is {stale_port}; expected {new_port}", HEIGHT - 2)
    assert env_path.read_bytes() == external, "stale confirmation overwrote an external edit"
    assert log.read_bytes() == third_log, "refused revert appended a record"
    assert backup.read_bytes() == third_backup, "refused revert replaced the backup"
    assert all("Current port" not in row for row in session.screen_lines()[7:28]), \
        "unavailable reason leaked into the table"

    # Restoration here is explicit fixture setup, never a product force path.
    env_path.write_bytes(applied)
    session.send(b"r")
    session.wait_for_line("Loaded 3 change(s), newest first", 2)
    session.wait_for_line("[Revert]", 7)
    menu_mark = session.mark()
    session.send(b"q")
    session.wait_for("Pre-start port check", menu_mark)


def exercise_history_pagination(binary: str) -> None:
    # Twelve valid independent records exceed the ten-entry page at 100x32.
    # Files are already in their recorded post-change state, with hand-checked
    # literal span metadata for PORT=3000 (offset 5, length 4).
    with tempfile.TemporaryDirectory(prefix="opendoor-tui-history-") as temporary:
        project = Path(temporary)
        records = []
        for identity in range(1, 13):
            folder = project / f"row-{identity:02d}"
            folder.mkdir()
            (folder / ".env").write_text("PORT=3000\n", encoding="utf-8")
            records.append({
                "v": 1, "id": identity, "timestamp": f"2026-10-05T08:14:{identity:02d}.123Z",
                "kind": "apply", "reverts": None, "file": f"./row-{identity:02d}/.env",
                "source": "env", "write": "env_literal", "key": "PORT",
                "line": 1, "column": 6, "offset": 5, "length": 4,
                "old_port": 2000, "new_port": 3000,
            })
        state = project / ".opendoor"
        state.mkdir(mode=0o700)
        log = state / "history.log"
        log.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
        log.chmod(0o600)
        original_log = log.read_bytes()
        session = Session(binary, project)
        try:
            session.wait_for("Resolve conflicts", 0)
            history_mark = session.mark()
            session.click(50, 16)
            session.wait_for("OPEN DOOR / History", history_mark)
            session.wait_for_line("Showing 1-10 of 12 change(s)", HEIGHT - 4)
            rows = session.screen_lines()
            for visible, identity in enumerate(range(12, 2, -1)):
                assert f"./row-{identity:02d}/.env" in rows[7 + visible * 2], \
                    "history is not paginated newest first"
            session.wait_for_line("./row-12/.env | PORT | 2000 -> 3000", HEIGHT - 2)

            session.send(b"\x1bOB")
            session.wait_for_line("./row-11/.env | PORT | 2000 -> 3000", HEIGHT - 2)
            session.send(b"\x1bOA")
            session.wait_for_line("./row-12/.env | PORT | 2000 -> 3000", HEIGHT - 2)
            session.wheel_down(50, 18)
            session.wait_for_line("./row-09/.env | PORT | 2000 -> 3000", HEIGHT - 2)
            for _ in range(3):
                session.wheel_down(50, 18)
            session.wait_for_line("./row-01/.env | PORT | 2000 -> 3000", HEIGHT - 2)
            session.wait_for_line("Showing 3-12 of 12 change(s)", HEIGHT - 4)
            assert "./row-10/.env" in session.screen_lines()[7], session.screen_lines()
            assert "./row-01/.env" in session.screen_lines()[25], session.screen_lines()
            session.wheel_up(50, 18)
            session.wait_for_line("./row-04/.env | PORT | 2000 -> 3000", HEIGHT - 2)
            session.click(40, 7)
            session.wait_for_line("./row-10/.env | PORT | 2000 -> 3000", HEIGHT - 2)
            session.send(b"\x1bOA")
            session.wait_for_line("./row-11/.env | PORT | 2000 -> 3000", HEIGHT - 2)
            session.wait_for_line("Showing 2-11 of 12 change(s)", HEIGHT - 4)
            session.send(b"\x1bOA")
            session.wait_for_line("./row-12/.env | PORT | 2000 -> 3000", HEIGHT - 2)
            session.wait_for_line("Showing 1-10 of 12 change(s)", HEIGHT - 4)
            assert log.read_bytes() == original_log, "History navigation modified the log"
            assert all((project / f"row-{identity:02d}" / ".env").read_bytes() == b"PORT=3000\n"
                       for identity in range(1, 13)), "History selection modified a source"
            assert not list(project.rglob("*.opendoor.bak")), "History selection created a backup"
            menu_mark = session.mark()
            session.send(b"q")
            session.wait_for("Pre-start port check", menu_mark)
            session.click(50, 17)
            session.finish()
        except BaseException:
            session.abort()
            raise


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_tui_pty.py /path/to/opendoor")
    binary = os.path.abspath(sys.argv[1])
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

                resolve_selection = session.mark()
                session.send(b"\x1bOB")
                session.wait_for("> Resolve conflicts", resolve_selection)
                history_selection = session.mark()
                session.send(b"\x1bOB")
                session.wait_for("> History", history_selection)
                quit_selection = session.mark()
                session.send(b"\x1bOB")
                session.wait_for("> Quit", quit_selection)

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
                session.wait_for("CONFLICT", owner_mark)
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

                history_mark = session.mark()
                session.click(50, 16)
                session.wait_for("OPEN DOOR / History", history_mark)
                session.wait_for("No history yet", history_mark)
                assert env_path.read_bytes() == refreshed, "History modified the fixture"
                assert not (project / ".opendoor").exists(), \
                    "History created project state"

                history_back = session.mark()
                session.send(b"q\r")
                session.wait_for("Pre-start port check", history_back)
                session.drain(0.2)

                exercise_recorded_changes(session, project, refreshed, conflict_port)
                session.click(50, 17)
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
            session.click(50, 17)
            session.finish()
        except BaseException:
            session.abort()
            raise
    exercise_history_pagination(binary)
    print("PTY mouse and History workflow checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
