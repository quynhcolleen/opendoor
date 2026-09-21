#!/usr/bin/env python3
import fcntl
import os
import pathlib
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time


def resize(fd: int, rows: int, columns: int) -> None:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))


def read_until(fd: int, needle: bytes, timeout: float, transcript: bytearray) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.1)
        if not readable:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        transcript.extend(chunk)
        if needle in transcript:
            return
    visible = bytes(transcript[-2000:]).decode("utf-8", "replace")
    raise AssertionError(f"did not observe {needle!r}; final PTY output:\n{visible}")


def start(binary: str, project: str, config: str) -> tuple[subprocess.Popen[bytes], int]:
    master, slave = os.openpty()
    resize(slave, 24, 80)
    environment = os.environ.copy()
    environment.update({
        "TERM": "xterm-256color",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "XDG_CONFIG_HOME": config,
    })
    process = subprocess.Popen(
        [binary, "--ascii", "--no-color", "--reduced-motion"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=environment,
        cwd=project,
        start_new_session=True,
    )
    os.close(slave)
    return process, master


def stop(process: subprocess.Popen[bytes], master: int) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
    os.close(master)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_tui_pty.py PATH_TO_OPENDOOR")
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="opendoor-pty-") as root:
        project = pathlib.Path(root, "project")
        profile_directory = project / ".opendoor"
        profile_directory.mkdir(parents=True)
        profile_directory.joinpath("project.toml").write_text(
            "schema_version = 1\n"
            "project_name = \"PTY project\"\n"
            "assignment_file = \".ports.env\"\n"
            "port_min = 20000\n"
            "port_max = 21000\n"
            "[discovery]\n"
            "compose_files = []\n"
            "env_files = []\n"
            "package_files = []\n"
            "[[service]]\n"
            "id = \"api\"\n"
            "name = \"API service\"\n"
            "group = \"backend\"\n"
            "variable = \"API_PORT\"\n"
            "preferred_port = 20555\n"
            "protocols = [\"tcp\"]\n"
            "sources = [\"manual:test\"]\n",
            encoding="utf-8",
        )
        config = str(pathlib.Path(root, "config"))

        process, master = start(binary, str(project), config)
        transcript = bytearray()
        try:
            read_until(master, b"Open dashboard", 8.0, transcript)
            time.sleep(3.0)
            transcript.clear()
            os.write(master, b"\n")
            read_until(master, b"API service", 5.0, transcript)
            transcript.clear()
            os.write(master, b"/APIx\x7f\n")
            read_until(master, b"Search applied: API", 3.0, transcript)
            transcript.clear()
            os.write(master, b"d")
            read_until(master, b"Service details", 3.0, transcript)
            transcript.clear()
            os.write(master, b"q")
            read_until(master, b"Details closed", 3.0, transcript)
            transcript.clear()
            os.write(master, b"q")
            read_until(master, b"Edit project profile", 3.0, transcript)
            transcript.clear()
            os.write(master, b"jjj\n")
            read_until(master, b"nothing is saved until you press s", 3.0, transcript)
            os.write(master, b"q")
            os.write(master, b"q")
            if process.wait(timeout=5) != 0:
                raise AssertionError("keyboard flow did not exit cleanly")
        finally:
            stop(process, master)

        process, master = start(binary, str(project), config)
        transcript = bytearray()
        try:
            read_until(master, b"Open dashboard", 8.0, transcript)
            transcript.clear()
            resize(master, 15, 50)
            os.kill(process.pid, signal.SIGWINCH)
            read_until(master, b"Terminal too small", 3.0, transcript)
            transcript.clear()
            resize(master, 24, 80)
            os.kill(process.pid, signal.SIGWINCH)
            read_until(master, b"Open dashboard", 3.0, transcript)
            os.kill(process.pid, signal.SIGTERM)
            if process.wait(timeout=5) != 0:
                raise AssertionError("SIGTERM flow did not exit cleanly")
        finally:
            stop(process, master)

    print("PTY interaction checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
