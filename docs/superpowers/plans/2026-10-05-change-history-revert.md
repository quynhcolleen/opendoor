# Change History and Per-Entry Revert Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add an append-only project history and a safe, individually confirmed revert action for each still-applicable port change.

**Architecture:** Persistence remains the only source-file mutator. A generic post-validation commit hook records one JSONL entry per automatic edit; hook failure rejoins the existing rollback path. History reloads and rediscovers the project to classify exact recorded spans, then builds a fresh one-item reverse resolution for the same transaction. The TUI adds History as the third of four main-menu rows and renders a uniformly one-line, newest-first table.

**Tech Stack:** C17, POSIX/Linux file APIs, ncursesw, CMake/CTest, Python PTY integration tests.

**Spec:** `docs/superpowers/specs/2026-10-05-change-history-revert-design.md`

## Global constraints

- Preserve the approved one-record-per-declaration JSONL schema and append-only semantics.
- Never relocate or guess a declaration during revert. Path, source kind, write kind, key, offset, length, and current port must all match fresh discovery.
- All source writes continue through persistence's backup, atomic replace, validate, and rollback transaction.
- History append failure rolls source files back and truncates only the uncommitted log tail.
- The History table has constant one-line rows. `Unavailable` is the only table-cell text for a stale row and uses `OD_ROLE_DANGER`; its reason appears only in the selected-row status line.
- Use strict TDD for every behavior change: add a focused failing test, observe the intended failure, implement minimally, and rerun the focused test before the wider suite.

## Task 1: Restore and extend the menu contract

**Files:** `tests/test_ui.c`, `tests/test_tui_pty.py`, `include/opendoor/screens.h`, `src/screens.c`, `src/tui.c`

1. Add failing unit assertions for four menu items, ordering, History label/description, History and Quit hit targets, and the new frame coordinates.
2. Add a PTY assertion that Down from Resolve selects History and another Down selects Quit; update mouse coordinates for History and Quit.
3. Add `OD_MENU_HISTORY`, derive frame height from `OD_MENU_COUNT`, and dispatch History through a temporary screen entry point without adding a footer shortcut.
4. Run `menu_contracts` and the PTY test.

## Task 2: Add the bounded JSONL history model

**Files:** `include/opendoor/history.h`, `src/history.c`, `tests/test_history.c`, `CMakeLists.txt`, `src/discovery.c`

1. Add failing tests for record round trips, optional keys, escaping, invalid/unsupported/duplicate/oversized/torn records, missing log, newest-first loading, and `.opendoor` discovery exclusion.
2. Define versioned record/list types with explicit ownership and availability/reason fields.
3. Implement a bounded schema-specific JSON reader/writer and secure root-relative `.opendoor/history.log` loading. Missing storage is an empty history and does not create it.
4. Skip invalid records with a warning count/message while retaining later valid records. Reject duplicate IDs. Keep every valid record in memory.
5. Run `history_contracts` and discovery tests.

## Task 3: Add the recorded persistence transaction

**Files:** `include/opendoor/persistence.h`, `src/persistence.c`, `include/opendoor/history.h`, `src/history.c`, `tests/test_persistence.c`, `tests/test_history.c`

1. Add failing tests for committed edit metadata, final offsets after same-file digit-width changes, one log record per automatic edit, log/directory modes, and append failure rolling every source back without retaining a partial tail.
2. Add a generic commit callback to persistence, invoked only after all source post-write validations while rollback state is still live. Existing public apply APIs remain wrappers.
3. Compute final token offsets/lengths and expose the original item plus committed file metadata to the callback.
4. Implement a root-anchored, no-follow history transaction: lock, determine monotonic next ID, serialize all per-edit records, append/fsync/validate, and restore the starting length on failure.
5. Route normal confirmed apply through the recorded wrapper. Manual-only rows never create records.
6. Run persistence and history contracts.

## Task 4: Implement exact eligibility and reverse transactions

**Files:** `include/opendoor/history.h`, `src/history.c`, `tests/test_history.c`

1. Add failing tests for ready, current-port mismatch, moved span, changed key/kind, missing/unsafe file, ambiguous match, older-entry supersession, re-enabled exact state, successful revert, stale refusal, and inverse record/redo.
2. Rediscover current declarations and classify a record ready only on one exact path/source/write/key/span/length/port match that remains auto-writable.
3. Build a fresh one-item reverse `OdResolution` from that discovery result; do not call the apply-only live-socket validator.
4. Revalidate the selected ID and declaration immediately before using the recorded transaction. Append a `kind=revert` inverse record linked by `reverts` to the chosen ID.
5. Run history, persistence, and discovery contracts.

## Task 5: Build the History table and confirmation UI

**Files:** `include/opendoor/screens.h`, `src/screens.c`, `src/tui.c`, `tests/test_ui.c`

1. Add failing render/hit-test assertions for title/columns, newest-first uniform pagination, equal one-line row heights, `[Revert]`, exact `Unavailable`, danger role, absence of inline reasons, selected status reason, and exact action targets.
2. Add History row/revert/confirm mouse actions and screen renderers. Draw the row selection first, then overlay action text so unavailable text retains `OD_ROLE_DANGER`.
3. Add TUI state for load/refresh, selection, scrolling, status, and a frozen pending record ID.
4. Revalidate before confirmation and before mutation. Only `y` and the explicit confirm target write; Enter/navigation/wheel/refresh are inert while confirming. Cancel returns without writes.
5. Flush queued input when returning to the menu and disable write targets below the existing minimum dimensions.
6. Run menu and history UI contracts.

## Task 6: Exercise the end-to-end workflow and document it

**Files:** `tests/test_tui_pty.py`, `README.md`

1. Add PTY coverage for four-row keyboard navigation, apply creating history, History pagination/selection, Enter-not-writing confirmation, cancel, confirmed revert, and stale refusal.
2. Document the four menu rows, `.opendoor/history.log`, exact-state availability, confirmation controls, append-only retention, and backups.
3. Run a warning-enabled build and the complete CTest suite. Run sanitizer-backed tests if the repository supports them.

## Review focus

The final reviewer must explicitly check:

1. no write path bypasses persistence's backup/atomic/validation/rollback transaction;
2. a failed history commit cannot report success or leave a committed-looking tail;
3. revert cannot relocate, guess, or overwrite a stale declaration;
4. unavailable rows remain exactly one line with danger-role `Unavailable` and status-only reasons;
5. all fixed-three menu assumptions and PTY coordinates were updated to four.
