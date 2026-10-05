# Change history with per-entry revert

## Intent

OpenDoor adds a project-local audit trail for every automatic port change and
allows any still-applicable entry to be safely reversed. The main menu becomes
`Dashboard`, `Resolve conflicts`, `History`, and `Quit`, in that order.

Dashboard remains read-only. Resolve Conflicts keeps its single apply-all
action. One history record represents one changed declaration, not a session or
apply batch. Revert never restores a whole-file snapshot, relocates a
declaration, merges edits, or force-overwrites current content.

## Selected approach

Revert reuses the existing locate-then-patch transaction in reverse. It
rediscovers the current declaration at the recorded span, requires its current
port to equal the entry's `new_port`, and builds a fresh one-item reverse
`OdResolution`. Fresh discovery supplies the current file size and hash for the
existing pre-write TOCTOU check. Persistence retains responsibility for
backup-before-write, same-directory atomic replacement, post-write validation,
and rollback.

History is the third real menu row. `OD_MENU_HISTORY` sits between Resolve
Conflicts and Quit and `OD_MENU_COUNT` is four. History uses the existing
two-column menu style, full-row selection highlight, triangle marker, Enter
dispatch, and menu-row mouse target. Its label is `History` and its description
is `View and revert past changes`. No History footer shortcut exists.

Revertability is derived from current source state. An entry is actionable
only when exactly one current auto-writable declaration matches its relative
file, source/write kind, declaration key, exact byte offset and length, and has
`port == entry.new_port`. OpenDoor never relocates by key or line number. An
older entry can become actionable again if a later safe operation restores the
exact recorded state.

The underlying log is unbounded. It is never rotated, pruned, capped, or
automatically cleared. The screen paginates all valid entries newest-first.

## History log and model

The log is `<canonical-project-root>/.opendoor/history.log`. `.opendoor` is a
reserved discovery-excluded directory. Create it as mode `0700` and the log as
`0600`, using root-anchored `openat` operations and `O_NOFOLLOW`. Opening an
empty History screen does not create either path.

The format is UTF-8 JSON Lines, one versioned object per successful automatic
declaration change:

```json
{"v":1,"id":42,"timestamp":"2026-10-05T08:14:22.123Z","kind":"apply","reverts":null,"file":"./services/api/.env","source":"env","write":"env_literal","key":"PORT","line":3,"column":6,"offset":27,"length":4,"old_port":3000,"new_port":3001}
```

- `id` is a monotonically increasing record identity, never a batch ID.
- `timestamp` is RFC 3339 UTC with milliseconds.
- `kind` is `apply` or `revert`; `reverts` names the selected entry for a
  revert and is null for a normal apply.
- `file` is project-relative. Absolute paths are not persisted or rendered.
- `key` is the declaration key when one exists and null otherwise.
- Span metadata describes the post-write token. When one transaction changes
  several tokens in a file, offsets account for digit-width changes at lower
  offsets.
- Whole-file hashes are not persisted. Fresh discovery supplies the hash used
  for the immediate persistence preflight.

A successful revert appends an inverse entry instead of mutating the original.
Inverse entries use the same eligibility rule and can act as redo operations.

Load the complete log sequentially with a 64 KiB per-record limit. Unknown
fields are ignored. Unsupported versions, malformed records, duplicate IDs,
and oversized records are skipped with a visible warning and never become
actionable. A torn final line is retained and skipped.

## Recorded transaction

Normal apply and revert use one recorded transaction:

1. Securely open or create and exclusively lock the log, recording its original
   length and the next ID.
2. Run existing source identity/span checks and in-memory format validation.
3. Create every `<file>.opendoor.bak` before changing any source.
4. Atomically replace and post-validate every source.
5. Derive one post-write record per automatic item, append and `fsync` the log,
   and validate the appended tail.
6. If logging fails, remove only the uncommitted tail and invoke the existing
   all-source rollback.
7. Distinguish complete from incomplete source or log rollback; never claim
   nothing changed when recovery is uncertain.

Successful records remain append-only. Truncation applies only to a failed,
uncommitted tail. Cross-file crash recovery remains no stronger than the
existing multi-file transaction; this feature adds no journal.

Revert reloads the entry by ID, rediscovers and exactly matches the current
declaration, creates a fresh reverse resolution, and invokes the same recorded
transaction with `kind = revert`. It does not call the apply-specific live
socket validator or allocate another port.

## History screen and interaction

The screen title is `OPEN DOOR / History`. Entries appear newest-first in:

`WHEN | FILE / KEY | CHANGE | ACTION / STATUS`

Every entry uses exactly one content line. Ready entries show `[Revert]` using
the established ready style. Unavailable entries show exactly `Unavailable` in
`OD_ROLE_DANGER`; the reason never appears, wraps, or continues inside the
table. The danger color supplements the explicit word rather than carrying
meaning alone.

An unavailable row remains selectable. Its full project-relative path, key,
change, and complete reason appear only in the bottom status line while it is
selected. Reasons include missing or unsafe files, missing or moved
declarations, changed key/format, loss of automatic write eligibility, and a
current value different from the recorded `new_port`.

List controls are:

- Up/Down: move selection one entry.
- Mouse wheel: move selection three entries and keep it visible.
- Ordinary row click: select only.
- Enter, the footer Revert target, or the exact `[Revert]` cell: revalidate and
  open confirmation for a ready entry.
- `r`: reload the log and derived availability.
- `q`/Escape: return to the four-row menu.

Confirmation names the exact reverse operation. Only `y` or the explicit
`Confirm revert` mouse target writes. Enter, arrows, wheel, row clicks, and
refresh are ignored while confirming. The entry is revalidated before opening
confirmation and again immediately before mutation.

Uniform one-line entries make selection, pagination, and hit-testing operate
directly on entry indexes. Existing table separator lines remain uniform.

## Files and interfaces

- `history.c/.h` owns history records, JSONL loading and append preparation,
  availability classification, recorded apply orchestration, and revert by ID.
- `persistence.c/.h` exposes committed-change metadata and a post-validation
  commit hook. A hook failure enters the existing rollback path; persistence
  remains the only source-file mutator.
- `screens.c/.h` owns the four-row menu, constant-height History table,
  danger-role unavailable cells, confirmation rendering, and exact hit tests.
- `tui.c` dispatches `OD_MENU_HISTORY` and owns History selection,
  confirmation, refresh, and status orchestration.
- `discovery.c` excludes `.opendoor`; CMake registers the new module and tests;
  README documents the menu, history path, controls, staleness, and retention.

## Verification

Tests cover JSONL round trips and malformed lines; one record per automatic
change; correct post-write spans across digit-width changes; append failure and
source rollback; exact-span revert, inverse entries, and stale refusal; four
menu items and navigation; and real ncurses input behavior.

History rendering tests specifically prove ready and unavailable rows each use
one content line, the unavailable cell contains only `Unavailable` in
`OD_ROLE_DANGER`, no reason text is present in that row, and the selected
unavailable entry's full reason remains visible in the bottom status line.

The warning-enabled build and complete CTest suite must pass without regressing
Dashboard, Resolve Conflicts, backup, mouse, or rollback contracts.

## Non-goals

There is no CLI history command, manual history edit, clear-history action,
retention setting, backup rotation, automatic conflict allocation during
revert, or crash-recovery journal.
