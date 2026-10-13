#!/usr/bin/env python3
"""Scan completeness through the real terminal; faults live in a test binary."""
import os
import sys
import tempfile
from pathlib import Path
from test_tui_pty import Session, HEIGHT, listening_socket, assert_emitted_before


def wait_screen(session, needle):
    import time
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        session.drain(0.1)
        if any(needle in row for row in session.screen_lines()):
            return
    raise AssertionError(f"missing {needle!r}: {session.screen_lines()!r}")


def malformed_project(binary, port=None):
    with tempfile.TemporaryDirectory(prefix="opendoor-scan-warning-") as root:
        project = Path(root)
        (project / "package.json").write_text("{ invalid json }\n")
        if port is not None:
            (project / ".env").write_text(f"PORT={port}\n")
        session = Session(binary, project)
        try:
            session.wait_for("Resolve conflicts", 0)
            session.click(50, 15)
            wait_screen(session, "Scan incomplete (")
            wait_screen(session, "./package.json:")
            rows = session.screen_lines()
            assert sum(row.count("Scan incomplete (") for row in rows) == 1, "duplicate scan summary"
            assert not any("No conflicts found" in row for row in rows)
            assert any("[info]" in row and "package.json" in row for row in rows)
            assert not any(root in row for row in rows), "absolute project path leaked"
            if port is not None:
                wait_screen(session, "[Apply]")
                wait_screen(session, "Scan was incomplete (")
                assert any(str(port) in row and "[Apply]" in row for row in session.screen_lines())
            else:
                wait_screen(session, "No conflicts detected in scanned data.")
                assert not any("PROPOSED" in row or "SOURCE" in row for row in session.screen_lines()), \
                    "empty Resolve result still renders a table"
            mark = session.mark()
            session.send(b"q")
            session.wait_for("Pre-start port check", mark)
            session.send(b"\r")
            wait_screen(session, "Scan incomplete (")
            wait_screen(session, "w Warnings")
            footer = session.screen_lines()[HEIGHT - 1]
            session.click(footer.index("w Warnings"), HEIGHT - 1)
            wait_screen(session, "./package.json:")
            wait_screen(session, "Scan warnings")
            for key in (b"w", b"q", b"\x1b"):
                session.send(key)
                wait_screen(session, "Ports in use")
                session.send(b"w")
                wait_screen(session, "Scan warnings")
            session.send(b"w")
            wait_screen(session, "Ports in use")
            mark = session.mark()
            session.send(b"q")
            session.wait_for("Pre-start port check", mark)
            session.send(b"q")
            session.finish()
            assert not (project / ".opendoor").exists()
            assert not (project / ".env.opendoor.bak").exists()
        except BaseException:
            session.abort()
            raise


def apply_with_warning(binary, mode, port):
    os.environ["OD_TEST_SCAN_WARNING"] = mode
    with tempfile.TemporaryDirectory(prefix="opendoor-apply-warning-") as root:
        project = Path(root)
        source = project / ".env"
        original = f"PORT={port}\n"
        source.write_text(original)
        session = Session(binary, project)
        try:
            session.wait_for("Resolve conflicts", 0)
            session.click(50, 15)
            if mode in ("blocking_initial", "unknown"):
                wait_screen(session, "Blocked")
                assert not any("[Apply]" in row for row in session.screen_lines())
                label = "Blocked"
            else:
                wait_screen(session, "[Apply]")
                label = "[Apply]"
            if mode == "owner":
                wait_screen(session, "Scan incomplete (1 warning)")
                wait_screen(session, "Scan was incomplete (1 warning); undiscovered declarations or unknown owners are possible.")
            rows = session.screen_lines()
            y = next(i for i, row in enumerate(rows) if label in row and str(port) in row)
            x = rows[y].index(label)
            apply_mark = session.mark()
            session.click(x, y)
            if label == "Blocked":
                session.send(b"\r")
            if mode in ("owner", "owner_prewrite"):
                wait_screen(session, "Updated 1 port(s)")
                if mode == "owner_prewrite":
                    assert_emitted_before(session, apply_mark, "Scan was incomplete (1 warning)", "Updated 1 port(s)")
                assert source.read_text() != original
                assert (project / ".env.opendoor.bak").read_text() == original
                assert (project / ".opendoor/history.log").exists()
            else:
                wait_screen(session, "Apply blocked: socket table could not be fully read. Refresh and retry.")
                wait_screen(session, "[blocks apply]")
                assert source.read_text() == original
                assert not (project / ".env.opendoor.bak").exists()
                assert not (project / ".opendoor").exists()
            mark = session.mark()
            session.send(b"q")
            session.wait_for("Pre-start port check", mark)
            session.send(b"q")
            session.finish()
        except BaseException:
            session.abort()
            raise
    del os.environ["OD_TEST_SCAN_WARNING"]


if __name__ == "__main__":
    malformed_project(sys.argv[1])
    listener, port = listening_socket()
    try:
        malformed_project(sys.argv[1], port)
        if len(sys.argv) > 2:
            for mode in ("owner", "owner_prewrite", "blocking_initial", "blocking_prewrite", "unknown"):
                apply_with_warning(sys.argv[2], mode, port)
    finally:
        listener.close()
    print("Scan reporting PTY checks passed")
