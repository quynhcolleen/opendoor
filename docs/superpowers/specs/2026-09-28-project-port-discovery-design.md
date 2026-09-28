# Project-scoped port discovery and transactional conflict resolution

## Intent

OpenDoor remains a two-feature ncurses application with the fixed menu
`Dashboard`, `Resolve conflicts`, and `Quit`. This extension restores project
port discovery without restoring the old broad discovery/onboarding system.
The Dashboard must describe the selected project rather than every listener on
the machine, while Resolve Conflicts must safely patch only unambiguous source
tokens after one whole-plan confirmation.

The existing interaction constraints remain binding:

- Dashboard is read-only. Enter and mouse clicks on rows do nothing.
- Resolve Conflicts has one apply-all confirmation and no row selection,
  editing, override, or partial apply.
- Mouse wheel scrolling and footer targets remain supported and tested.
- Conflict checking is a pre-start workflow. A declared port occupied by an
  external or unidentifiable process is a conflict; a listener attributable to
  this project is not.
- Replacement ports are allocated from 1024 through 65535 and must be unused
  machine-wide.

## Selected approach

Implement one fresh, specialized discovery subsystem for the four approved
formats, retain the machine-wide socket scan as the live source, and introduce
a transaction model whose changes point back to immutable discovery spans.
This is the only approach that satisfies the format-preservation and
all-or-nothing requirements. Re-generating YAML/JSON, writing a new overlay
file, and reviving the prior generic discovery implementation are excluded.

The old same-directory temp-file, `fsync`, `rename`, and parent-directory sync
primitives may be restored from the repository version of `persistence.c`.
Profile serialization, onboarding state, backup rotation, and the former
multi-file managed-profile transaction must not return.

## Discovery model

`discovery.c/.h` owns recursive file enumeration and format-specific parsing.
It returns an ordered collection of declaration occurrences. Each occurrence
contains:

- effective port when directly known;
- declaration kind: literal, environment-variable reference, or unresolvable;
- format: env, Compose, package script, or Makefile;
- project-relative source path and relative containing folder, both beginning
  with `./`;
- one-based line and column plus zero-based byte offset and byte length of the
  literal port token;
- environment key for a reference, and literal fallback/default when present;
- write eligibility and a reason when it is manual-only;
- enough original line/file identity to reject stale spans before applying.

Bounds-checked copies remain mandatory. Absolute paths are internal only and
never reach rendered rows or messages.

The recursive walk does not follow symbolic links. It skips `.git`,
`node_modules`, `vendor`, `build`, `build-*`, and `dist` directory trees so
vendored dependencies and generated output are not treated as project
declarations. It recognizes only:

1. `docker-compose.yml` and `compose.yaml`;
2. `.env` and names beginning `.env.`;
3. `package.json`;
4. `Makefile`.

No other basename, extension, or source-code number scanning is allowed.

### `.env` parsing

Accept assignment lines with an identifier key and a value that reduces to one
integer port token after surrounding horizontal whitespace and optional
matching single or double quotes. Inline comments are accepted only after the
complete value. Values with interpolation, multiple numeric candidates, extra
tokens, or ports outside 1 through 65535 are not declarations.

A plain direct integer occurrence is a literal and is auto-write eligible when
the exact integer token is outside a comment. `.env.*` files are scanned the
same way as `.env`.

### Compose parsing

Track YAML indentation sufficiently to identify list entries under a `ports:`
key. Accept the short mapping form, quoted or unquoted, whose scalar is exactly
`HOST:CONTAINER`, with optional protocol suffix. Only the host token is the
declared port.

A numeric host token is a literal. It is auto-write eligible only when the
scalar has exactly one host candidate and one container candidate and the host
token is not inside a comment. Long mapping syntax, ranges, IP-prefixed
mappings, and other forms outside the approved `HOST:CONTAINER` shape are not
discovery matches.

`${KEY}`, `${KEY:-DEFAULT}`, `${KEY-DEFAULT}`, `$KEY`, and Make-style
`$(KEY)`/`${KEY}` forms are environment-variable references. Record the key and
the exact fallback token span when a numeric fallback exists. A reference is
resolved only by a unique direct assignment to that key in a `.env`-family
file in the same directory. Prefer the exact sibling `.env`; otherwise require
exactly one matching `.env.*` assignment. Missing, conflicting, indirect, or
non-integer values make the occurrence unresolvable. References are always
manual-only even when their effective value is known.

After an auto-write, Compose validation reparses the complete document with
the scoped YAML structure parser, verifies balanced quoting and indentation for
the recognized `ports:` sections, and confirms every patched scalar remains a
valid clean short mapping. It does not parse and regenerate YAML.

### `package.json` parsing

Use a fresh, local JSON tokenizer/parser sufficient to parse valid JSON and
select string values directly within the top-level `scripts` object. Decode
JSON string escapes for command matching while retaining a decoded-to-source
offset map for token spans. Recognize only `--port N` and `-p N`, with a port
from 1 through 65535. Every package-script occurrence is manual-only.

Invalid JSON yields a discovery warning for that file and no declarations from
it; it must not terminate discovery of other files.

### Makefile parsing

Recognize direct identifier assignments with an integer value and recipe or
assignment command text containing the same two port flag forms accepted for
package scripts. Recognize direct environment-variable forms, but do not
evaluate Make functions, nested variables, shell expansions, or computed
values. All Makefile occurrences are manual-only.

### Ambiguity and comments

An occurrence with multiple plausible port tokens on its line, a token inside
a comment, indirect variable construction, or a span that cannot be mapped
exactly is manual-only or unresolvable. The scanner never guesses. Parse errors
and unresolvable declarations are collected as warnings/manual items so one
bad file does not hide valid declarations elsewhere.

## Dashboard data flow

1. Discover declarations beneath the canonical selected project root.
2. Run `od_scan_host` to collect sockets and process ownership.
3. Classify a live endpoint as project-owned when its canonical process cwd is
   under the root. If cwd is unavailable, use the parent directory of its
   canonical executable path. A component-boundary containment check prevents
   `/project-other` from matching `/project`.
4. Discard unrelated machine listeners from Dashboard presentation. Keep them
   available to Resolve Conflicts.
5. Deduplicate live occurrences by `(pid, port)` and declarations by
   `(source file, port)`.
6. For each declaration, mark it running when a project-owned live endpoint
   has the same port and the same relative containing folder. All declaration
   rows that meet that condition are marked running, and no extra live row is
   emitted for the endpoint.
7. Emit unmatched project-owned live endpoints as undeclared rows with source
   `live`.
8. Sort by relative folder path, then port number, then source path for a stable
   tie-break.

Dashboard columns are:

`PORT | RELATIVE FOLDER | STATUS | SOURCE`

Status is exactly `running` or `not running`. Every displayed folder and
source path begins `./`; no absolute path is rendered. The existing table
grid, banner, colors, scrolling, refresh key, back keys, and mouse behavior are
preserved. The screen contains no confirm or write path.

## Conflict and proposal model

A declaration conflicts when its effective port is occupied by at least one
live endpoint that is either attributable outside the selected project root or
has no ownership metadata. Project-owned listeners do not cause conflicts.

The allocation input reserves every machine-wide occupied port and every
non-conflicting declared port. Each independent conflicting declaration gets a
replacement in 1024 through 65535. Occurrences that resolve to the same direct
environment definition are grouped so they receive one replacement rather than
contradictory proposals. The plain `.env` definition is the only automatic
edit in that group. Referencing Compose/Makefile occurrences remain listed as
manual-only with the same target port and are never modified.

Each proposal records old/new ports, source line/span, auto/manual eligibility,
and preview text. Automatic changes are limited to:

- a plain `.env` integer assignment;
- the host token in an unambiguous Compose short mapping.

Package scripts, Makefiles, all variable-reference occurrences, ambiguous
matches, and unresolvable entries are manual suggestions. A manual suggestion
is displayed as `change line N in ./path to PORT` with the reason it is not
automatically applied.

## Resolve Conflicts screen

Before confirmation, group auto-write changes by relative file path and show
the original and patched complete source line for each change:

```text
./service/compose.yaml:12
-     - "3000:3000"
+     - "3001:3000"
```

Manual suggestions appear in a distinct `Manual changes (not applied
automatically)` section. The view scrolls as one preview document. Enter is
available only when at least one automatic change exists; it applies all
automatic changes exactly as previewed. `q`/Esc returns without writing.
Clicking preview rows does nothing. Mouse wheel scrolls; footer targets apply
all or cancel.

If only manual suggestions exist, the screen has no Enter action and writes
nothing. After success, report `Updated N port(s)` and, when relevant, that
manual suggestions remain unapplied.

## Transactional locate-then-patch

The apply operation groups changes per target file and performs this sequence:

1. Re-read every target before any write. Verify that its size/content identity
   and every original token at the recorded span still match discovery. Reject
   a stale proposal and request refresh rather than relocating or guessing.
2. Build each patched buffer in memory. Apply spans from highest byte offset to
   lowest so changes in digit width cannot invalidate later locations.
3. Validate every patched buffer in memory with the matching format parser
   before touching disk. Persistence receives the format validator as a
   callback so it remains independent from discovery syntax.
4. Create or replace `<file>.opendoor.bak` with the exact current bytes for
   every target. Backups use same-directory atomic replacement and preserve
   original mode. If any backup fails, no source file is written.
5. Atomically replace each source with its patched bytes using a temporary file
   in the same directory, file `fsync`, `rename`, and parent-directory `fsync`.
6. Re-read and re-validate each written file. `.env` uses the assignment
   scanner; Compose uses the scoped YAML/Compose parser and confirms the
   expected new tokens.
7. On any write or post-write validation failure, atomically restore every
   source file already touched from its backup, including the failing file.
   Verify restored bytes against the preflight originals. The operation reports
   failure and states that none of the listed automatic conflicts were applied.

Backups remain after a successful operation as the immediately previous
version. No backup rotation is introduced. A rollback test uses the validator
callback as a deterministic failure seam: after the second source file has
been atomically replaced, the test validator deliberately corrupts that file
and returns a validation failure. The test then asserts every source is
restored byte-for-byte and no partial proposal remains. Production validators
are read-only.

## Errors and safety

- Discovery warnings are surfaced in the screen status without discarding
  valid rows.
- Files are bounded regular files and symlink targets are rejected before
  reading or writing.
- Allocation failure leaves every file untouched.
- A rescan occurs when Resolve Conflicts is opened. Apply rechecks live sockets
  and proposal targets before writing; if the environment changed, it aborts
  and asks for refresh.
- No `popen()` or `system()` is added. The existing scan process execution
  pattern is unchanged.
- The process never writes package.json or Makefile and never rewrites a full
  source file through format serialization.

## Files and interfaces

- `src/include opendoor/discovery.*`: discovery occurrences, recursive walk,
  the four parsers, environment reference resolution, relative paths.
- `src/include opendoor/dashboard.*`: project ownership filtering, merge,
  deduplication, and sorting.
- `src/include opendoor/resolution.*`: conflict/proposal construction and
  preview/manual classification.
- `src/include opendoor/persistence.*`: stale-span preflight, in-memory span
  patching, backup, atomic replace, validation, rollback.
- `src/tui.c`: refresh/prepare/apply orchestration and unchanged keyboard/mouse
  interaction contract.
- `src/include opendoor/screens.*`: four-column Dashboard and grouped diff/manual
  preview rendering.
- `tests/test_discovery.c`: recursion and all four parser contracts, literal vs
  reference classification, path filtering, ownership merge/dedup/sort.
- `tests/test_persistence.c`: eligible vs manual changes, exact span changes,
  formatting preservation, backup creation, multi-file rollback byte equality.
- `tests/test_core.c`: external-owner conflict selection and allocation range.
- `tests/test_ui.c`: fixed menu, Dashboard read-only/mouse behavior, grouped
  preview, single apply-all action, and manual-only no-Enter state.
- `CMakeLists.txt`, `README.md`, and `THIRD_PARTY_LICENSES.md`: build/test and
  user-facing scope documentation. No JSON or YAML vendor dependency is added.

## Verification

Completion requires:

1. `cmake -S . -B build && cmake --build build` succeeds with existing warning
   flags.
2. `ctest --test-dir build --output-on-failure` passes.
3. Discovery tests cover only the four approved formats and reject unrelated
   files.
4. Dashboard tests cover declared-running, declared-not-running,
   project-owned undeclared live, unrelated-live filtering, deduplication,
   relative paths, and folder/port sorting.
5. Persistence tests prove only clean `.env` and Compose literals are written,
   manual sources remain byte-identical, comments/formatting survive, and a
   mid-transaction validation failure restores all targets byte-for-byte.
6. UI tests prove no Dashboard confirm action, one whole-plan Resolve confirm,
   diff preview and manual sections, inactive row clicks, working wheel/footer
   targets, and no per-row edit/override.
7. Grep confirms there is no generic file scanner, no `popen`/`system`, and no
   resurrected onboarding/profile persistence.
