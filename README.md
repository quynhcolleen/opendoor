# OpenDoor

OpenDoor is a focused Linux ncurses application for inspecting project ports and resolving conflicts before a stack starts. Its fixed menu has exactly four items, in order: `Dashboard`, `Resolve conflicts`, `History`, and `Quit`.

## Project discovery

OpenDoor recursively scans the selected project folder and its subfolders. It recognizes port declarations in exactly four formats:

- `docker-compose.yml` and `compose.yaml`: short `HOST:CONTAINER` mappings under `ports:`, optionally quoted and optionally followed by `/tcp` or `/udp`;
- `.env` and `.env.*`: direct `KEY=PORT` assignments;
- `package.json`: string values directly inside the top-level `scripts` object containing `--port PORT` or `-p PORT`;
- `Makefile`: direct integer assignments and recipe commands containing the same two port flags.

`.env.example`, `.env.sample`, `.env.template`, and any `.env.*` filename whose last suffix is `.example`, `.sample`, or `.template` are excluded completely. OpenDoor also skips `.git`, `.opendoor`, `node_modules`, `vendor`, `build`, `build-*`, and `dist`, and it does not follow symlinks. `.opendoor` is reserved for local History artifacts and is never scanned for port declarations.

Direct `$KEY` and `${KEY}` references, plus Compose defaults such as `${KEY:-3000}`, are resolved only through one direct sibling `.env` assignment. The exact sibling `.env` takes priority; otherwise exactly one matching `.env.*` definition is required. OpenDoor does not evaluate nested or computed variable expressions.

## Dashboard

Dashboard is read-only. It combines discovered declarations with live sockets owned by processes whose working directory is inside the selected project. If the working directory is unavailable, the executable's parent directory is used. Unrelated machine listeners are shown only when they occupy a declared port; they are not emitted as standalone rows.

The columns are:

`PORT | RELATIVE FOLDER | STATUS | PROCESS | SOURCE`

Every displayed path is relative to the selected project and begins with `./`.
Statuses are:

- `running`: at least one project-owned live endpoint uses the declared port;
- `not running`: the declared port is free;
- `in use (other)`: the declared port is occupied only by another process or by an endpoint whose ownership cannot be determined.

Every declaration sharing a project-owned live port is marked `running`. A project-owned live port that has no declaration is also shown with source `live`. `PROCESS` shows the compact kernel process name, falls back to the executable basename, and shows `-` when no owner name is available. Dashboard never asks for confirmation and never writes a file.

## Resolve conflicts

Resolve Conflicts treats a declaration as conflicting when another project—or an endpoint with unavailable ownership metadata—already occupies its port. It reserves every live machine port and every declared port, then proposes a replacement from the fixed range **1024–65535**.

The screen shows a per-file diff preview for automatic edits and a separate manual-suggestion section. Automatic edits are limited to:

- the integer token in a plain `.env` assignment;
- the host token in an unambiguous Compose short mapping.

Package scripts, Makefiles, all variable references, and ambiguous or unresolvable declarations are suggestions only and are never written.

Enter is the single confirmation: it applies every automatic proposal exactly as shown. There is no row selection, partial apply, manual port override, or second confirmation. `q` or Escape cancels without writing anything.

Before writing, OpenDoor verifies that the source files and exact token spans have not changed and rescans sockets to ensure the proposal is still valid. For every touched source it creates `<file>.opendoor.bak`, patches only the recorded byte spans, writes through a same-directory temporary file, calls `fsync`, renames atomically, and validates the result. A failure restores every file touched by that confirmation byte-for-byte; partial apply is not allowed.

## History

Every successful automatic declaration change appends one flat JSON object to the project's `.opendoor/history.log`. The file is UTF-8 JSON Lines (JSONL), with one record per changed declaration, including the time, relative file, key, exact token span, and old and new ports. History is local, append-only, and retained without a size or age limit; OpenDoor never rotates, prunes, or clears successful records. Opening an empty History screen does not create the directory or log.

The screen paginates all valid records newest-first in `WHEN | FILE / KEY | CHANGE | ACTION / STATUS`. Each entry occupies one content line. An entry shows `[Revert]` only when exactly one currently writable declaration matches its recorded file, key, format, byte offset, and token length, and its current port equals the recorded new port. Missing, moved, changed, or unsafe declarations show the explicit danger-colored label `Unavailable`. Unavailable rows remain selectable; the selected row's full relative path, key, change, and reason appear in the bottom status line, with the reason kept out of the table.

Revert changes only that exact declaration from its recorded new port back to its old port. OpenDoor checks availability before opening confirmation and again immediately before writing. Only lowercase `y` or a click on the explicit `Confirm revert` target confirms; Enter does not write. There is no force option, guessed location, declaration relocation, merge, or whole-file snapshot restoration. External edits can make an entry stale and unavailable, while restoring its exact recorded state can make it available again.

A successful revert appends an inverse record naming the entry it reverses and leaves the original record intact. The inverse can itself be reverted as a redo when its exact recorded state still matches. Revert does not choose a different port or check whether the old port is currently occupied.

Apply and revert share the same backup, atomic replacement, validation, and rollback transaction. Every `<file>.opendoor.bak` is created before any source changes. The locked history log is appended and synced only after source validation; a history commit failure removes its uncommitted tail and rolls back the source changes. Incomplete recovery is reported explicitly. These operations do not provide a multi-file crash-recovery journal. Malformed, unsupported, duplicate, oversized, and torn records are skipped with a visible warning and cannot be reverted.

## Pre-start requirement and limitations

Run Resolve Conflicts **before starting the project stack**. If the stack is already running, its ports can look externally occupied. This is especially common for Docker-published ports held by `docker-proxy`, whose process ownership is outside the selected project path. OpenDoor will treat those ports as conflicts because this feature is intentionally a pre-start check.

Additional limitations:

- IP-prefixed Compose mappings such as `127.0.0.1:3000:80` are unsupported.
- Generated `.opendoor.bak` files and the local `.opendoor/` History directory may need to be added to the project's `.gitignore`.
- Changing a host port does not update other references to the old port.
- OpenDoor never starts or stops services, kills processes, invokes `sudo`, or evaluates arbitrary project code.

## Controls

Main menu:

- Up/Down or mouse wheel: navigate;
- Enter or a menu-row click: open;
- `q`: quit.

Dashboard:

- Up/Down or mouse wheel: scroll;
- `r` or the footer refresh target: rescan;
- `q`, Escape, or the footer back target: return to the menu.

Resolve Conflicts:

- Up/Down or mouse wheel: scroll the preview;
- Enter or the footer apply-all target: apply all automatic changes once;
- `q`, Escape, or the footer cancel target: return without writing.

History:

- Up/Down: select one entry;
- mouse wheel: move selection three entries and keep it visible across pages;
- ordinary row click: select only;
- Enter, the footer Revert target, or the exact `[Revert]` cell: revalidate a ready entry and open confirmation;
- `r` or the footer refresh target: reload the log and current availability;
- `q`, Escape, or the footer back target: return to the menu.

History confirmation:

- lowercase `y` or the explicit `Confirm revert` mouse target: revalidate and perform the exact revert;
- `n`, `q`, Escape, or the footer cancel target: cancel without writing;
- Enter, arrows, mouse wheel, row clicks, and refresh: do nothing.

Clicks inside Dashboard rows or Resolve preview rows have no action.

## Build and run

OpenDoor requires Linux, a C17 compiler, CMake 3.20 or newer, and the wide-character ncurses development library.

```bash
sudo apt install build-essential cmake libncurses-dev
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run from a project directory or select another root:

```bash
opendoor
opendoor --project PATH
opendoor --ascii
opendoor --update
opendoor --version
```

The TUI requires an interactive terminal. `--update` and `--version` do not.

### Update from the local checkout

`opendoor --update` rebuilds the OpenDoor source currently on disk and atomically installs the resulting executable to `~/.local/bin/opendoor`. It does not fetch or modify Git history. The updater checks the current directory, then `OPENDOOR_SOURCE_DIR`, then `~/opendoor` for the checkout. The option must be used alone.

OpenDoor is MIT licensed; vendored source notices are in [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
