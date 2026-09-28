# OpenDoor

OpenDoor is a small Linux ncurses application with two features:

- **Dashboard** shows the union of ports requested by the project and ports
  currently occupied on the system.
- **Resolve conflicts** proposes replacement ports for every conflict and writes
  the complete proposal after one confirmation.

The main menu is always `Dashboard`, `Resolve conflicts`, and `Quit`.

## Conflict timing

Run OpenDoor **before starting the project stack**. Dashboard identifies a
listener's process and working directory when `/proc` permissions allow, but
conflict classification deliberately does not infer whether that process
belongs to the project. Consequently, a configured port that is occupied is
treated as a conflict.
Checking a stack that is already running will report its normal listeners as
conflicts.

OpenDoor never starts or stops services, kills processes, or invokes `sudo`.

## Wanted ports

Create `.ports.env` in the project root and edit it by hand. The format is one
`KEY=PORT` entry per line; blank lines and lines beginning with `#` are ignored.

```dotenv
API_PORT=3000
WEB_PORT=5173
```

Keys use uppercase letters, digits, and underscores, and each key must be
unique. Configured ports may be any value from 1 through 65535.

Replacement candidates use the fixed range **1024–65535**. OpenDoor chooses the
next available candidate, wraps to 1024 when needed, avoids occupied ports and
other configured ports, and applies every proposed replacement as-is. Saving is
a plain overwrite of `.ports.env`; there are no backups or transaction files.

## Controls

Main menu:

- Up/Down: navigate
- Enter: open
- `q`: quit
- Mouse wheel: navigate; click a menu row to open it

Dashboard:

- Up/Down: scroll
- `r`: rescan
- `q` or Escape: return to the menu
- Mouse wheel: scroll; click the footer refresh/back controls

Dashboard is read-only. Enter has no action and the screen never writes files.
Its columns are:

- `PORT`
- `STATUS`: `running` when occupied, otherwise `free`
- `CONFLICT`: `yes` when the port is both configured and occupied, or when
  multiple configured keys request the same port
- `PROCESS`: the owning process when visible through `/proc`
- `DIRECTORY`: the owning process's working directory when visible through
  `/proc`

Resolve conflicts:

- Enter: apply the entire displayed proposal once
- Up/Down: scroll
- `q` or Escape: cancel without writing
- Mouse wheel: scroll; click the footer apply-all/cancel controls

Only conflicting entries appear as `OLD PORT -> NEW PORT`. Individual rows
cannot be reviewed, skipped, or edited. If nothing conflicts, the screen shows
`No conflicts found` and offers no apply action.

## Build

OpenDoor requires Linux, a C17 compiler, CMake 3.20 or newer, and the
wide-character ncurses development library.

```bash
sudo apt install build-essential cmake libncurses-dev
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run from a project directory or pass another project root:

```bash
opendoor
opendoor --project PATH
```

`--ascii`, `--help`, and `--version` are also available. The TUI requires an
interactive terminal.

To rebuild the source checkout and install the resulting binary to
`~/.local/bin/opendoor`, run:

```bash
opendoor --update
```

`--update` must be used alone. It builds code already present in the checkout;
it does not fetch or pull source changes.

OpenDoor is MIT licensed. Vendored source notices are in
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
