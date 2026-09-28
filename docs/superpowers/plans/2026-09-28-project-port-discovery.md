# Project Port Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Discover declared ports from exactly four project file formats, show project-scoped live status, preview safe conflict changes, and apply eligible changes transactionally after one confirmation.

**Architecture:** `discovery.c` produces immutable declarations with exact source spans. `dashboard.c` merges those declarations with process-owned sockets, while `resolution.c` classifies external conflicts and adapts them to the existing allocation algorithm. `persistence.c` applies only eligible spans through one backup/atomic-write/validate/rollback transaction, and the existing ncurses layer renders the resulting Dashboard and per-file preview without adding row actions.

**Tech Stack:** C17, Linux `/proc`, ncursesw, CMake/CTest, POSIX file APIs; no new JSON/YAML library and no shell process API.

**Spec:** `docs/superpowers/specs/2026-09-28-project-port-discovery-design.md`

## Global Constraints

- Keep the fixed menu `Dashboard`, `Resolve conflicts`, `Quit`.
- Dashboard is read-only: Up/Down, `r`, `q`/Esc, wheel, refresh/back footer clicks; no Enter action.
- Resolve Conflicts has one apply-all Enter action; no row selection, edit, override, or partial apply.
- Discover only `docker-compose.yml`, `compose.yaml`, `.env`/approved `.env.*`, `package.json`, and `Makefile`.
- Exclude `.env.example`, `.env.sample`, `.env.template`, and every `.env.*` whose final suffix is `.example`, `.sample`, or `.template`.
- Do not follow symlinks or traverse `.git`, `node_modules`, `vendor`, `build`, `build-*`, or `dist`.
- Use 1024 through 65535 for replacement allocation and reserve all machine-wide occupied ports.
- Auto-write only plain `.env` integer values and clean Compose `HOST:CONTAINER` host literals.
- Use exact recorded byte spans; never parse-and-regenerate a source file.
- Every touched source gets `<file>.opendoor.bak`; source writes are same-directory temp + `fsync` + rename + parent `fsync`.
- One confirmation is all-or-nothing across all auto-write files; restore every touched file on failure.
- Preserve bounds-checked string copies and the existing `scan.c` pipe/fork/`execvp` behavior.
- Do not add `popen()` or `system()` and do not restore onboarding, Docker discovery, settings, theme, help, or jsmn modules.
- Remove `src/update.c`, `include/opendoor/update.h`, `--update`, and `opendoor_print_help`; the user did not opt to keep them.
- Do not stage or commit during plan execution unless the user explicitly asks again.

## Review Focus

- A root `/work/app` must not own a process path under `/work/app-other`; Task 4 adds the component-boundary test.
- JSON script escapes must map the decoded port token back to the exact source bytes; Task 3 adds an escaped-string span test.
- CRLF files and files without a final newline must preserve every byte outside changed tokens; Task 6 adds byte-for-byte tests.
- Multiple edits in one file whose replacement ports have different digit widths must not shift later spans; Task 6 adds descending-offset tests.
- Missing cwd must fall back to exe, while missing both must classify occupancy as `in use (other)`; Task 4 adds both cases.

---

### Task 1: Discovery model, recursive walk, and dotenv declarations

**Files:**
- Modify: `include/opendoor/discovery.h`
- Modify: `src/discovery.c`
- Modify: `tests/test_discovery.c`

**Interfaces:**
- Consumes: `OdStatus`, `OdError`, `OD_PATH_CAP`.
- Produces:
  - `OdPortSourceKind { OD_SOURCE_ENV, OD_SOURCE_COMPOSE, OD_SOURCE_PACKAGE_JSON, OD_SOURCE_MAKEFILE }`
  - `OdPortDeclarationKind { OD_DECLARATION_LITERAL, OD_DECLARATION_ENV_REFERENCE, OD_DECLARATION_UNRESOLVABLE }`
  - `OdPortWriteKind { OD_WRITE_MANUAL_ONLY, OD_WRITE_ENV_LITERAL, OD_WRITE_COMPOSE_LITERAL }`
  - `OdPortDeclaration` with `source_kind`, `declaration_kind`, `write_kind`, `port`, `fallback_port`, `line`, `column`, `byte_offset`, `byte_length`, `file_size`, `file_hash`, `definition_index`, and owned strings `absolute_path`, `relative_path`, `relative_folder`, `environment_key`, `line_text`, `manual_reason`; `definition_index == SIZE_MAX` means no direct `.env` definition.
  - `OdProjectDiscovery { OdPortDeclaration *items; size_t count; char **warnings; size_t warning_count; }`
  - `OdStatus od_discover_project_ports(const char *project_root, OdProjectDiscovery *result, OdError *error)`
  - `void od_project_discovery_free(OdProjectDiscovery *result)`
  - Preserve `od_discover_occupied_ports(...)`.

- [ ] **Step 1: Write failing recursive dotenv tests**

Add `test_recursive_env_discovery_records_exact_spans()` and `test_env_template_and_unrelated_files_are_ignored()` using a temporary project tree. Assert direct integer and quoted integer ports, `./` relative paths/folders, one-based line/column, exact byte offsets/lengths, FNV-1a file identity, `OD_WRITE_ENV_LITERAL`, deduplication by `(file, port)`, and total exclusion of the three exact template names plus suffixed variants such as `.env.local.example`. Include an unrelated `.txt`, a symlinked directory, and every skipped directory class.

- [ ] **Step 2: Run the discovery target and verify RED**

Run: `cmake --build build --target test_discovery && ./build/test_discovery`

Expected: compilation fails because the discovery types/functions do not exist.

- [ ] **Step 3: Implement the bounded recursive walk and dotenv parser**

In `src/discovery.c`, use `opendir`/`readdir`/`lstat` without following symlinks, canonicalize the root, build only contained absolute and `./` relative paths, bound regular files to 4 MiB, hash exact bytes with FNV-1a 64, and append warnings without aborting other files. Accept identifier assignments whose complete value is one optionally quoted integer plus optional trailing comment; reject ports outside 1–65535 and ambiguous/comment-contained tokens. Every string copy must check capacity before copying or allocate exact length.

- [ ] **Step 4: Run the discovery test and verify GREEN**

Run: `cmake --build build --target test_discovery && ./build/test_discovery`

Expected: all scan and dotenv discovery checks pass.

- [ ] **Step 5: Check the task diff**

Run: `git diff --check -- include/opendoor/discovery.h src/discovery.c tests/test_discovery.c`

Expected: no output. Do not stage or commit.

### Task 2: Compose short mappings and direct environment references

**Files:**
- Modify: `include/opendoor/discovery.h`
- Modify: `src/discovery.c`
- Modify: `tests/test_discovery.c`

**Interfaces:**
- Consumes: `OdProjectDiscovery` and declaration types from Task 1.
- Produces:
  - `OdStatus od_validate_discovery_text(OdPortSourceKind source_kind, const char *text, size_t length, OdError *error)`; accepts env and Compose, returns `OD_ERROR_UNSUPPORTED` for other kinds.
  - Compose declarations with exact host-token spans and resolved `definition_index` links.

- [ ] **Step 1: Write failing Compose tests**

Add `test_compose_literals_and_direct_env_references()` with quoted/unquoted clean mappings, optional `/tcp`, `${API_PORT:-3000}`, `${API_PORT}`, `$API_PORT`, sibling `.env`, and unique sibling `.env.local`. Assert only clean literal hosts receive `OD_WRITE_COMPOSE_LITERAL`; every reference is manual-only, records its key/fallback, and links to the direct env definition. Add `test_compose_rejects_unsupported_or_ambiguous_mappings()` for IP-prefixed mappings, ranges, long syntax, multiple numeric candidates, comments containing a mapping, conflicting `.env.*` definitions, indirect values, and malformed quoting.

- [ ] **Step 2: Run the discovery target and verify RED**

Run: `cmake --build build --target test_discovery && ./build/test_discovery`

Expected: new Compose assertions fail because only dotenv declarations exist.

- [ ] **Step 3: Implement the scoped Compose parser and resolver**

Track indentation and list membership under `ports:`; accept only scalar `HOST:CONTAINER` with optional protocol suffix. Preserve quote style and record only the host token. Parse the approved direct variable forms without evaluating nested expressions. Resolve the exact sibling `.env` first, otherwise require one matching non-template `.env.*` definition; set `definition_index` only for that direct result. Implement whole-text validation for balanced quotes, recognized indentation, and clean patched short mappings without serializing YAML.

- [ ] **Step 4: Run the discovery target and verify GREEN**

Run: `cmake --build build --target test_discovery && ./build/test_discovery`

Expected: scan, dotenv, Compose, reference, and unsupported-IP tests pass.

- [ ] **Step 5: Check the task diff**

Run: `git diff --check -- include/opendoor/discovery.h src/discovery.c tests/test_discovery.c`

Expected: no output. Do not stage or commit.

### Task 3: Package script and Makefile manual-only discovery

**Files:**
- Modify: `src/discovery.c`
- Modify: `tests/test_discovery.c`

**Interfaces:**
- Consumes: Task 1 declaration append/free API and Task 2 direct environment resolver.
- Produces: package and Makefile declaration occurrences that always have `OD_WRITE_MANUAL_ONLY`.

- [ ] **Step 1: Write failing package/Make tests**

Add `test_package_scripts_are_manual_only()` for valid top-level `scripts` strings containing only `--port N` and `-p N`, including an escaped JSON string whose decoded match maps back to the exact source bytes. Assert flags outside `scripts`, `--port=N`, arbitrary numbers, and invalid JSON do not become declarations; invalid JSON adds one warning. Add `test_makefile_ports_are_manual_only()` for `PORT=3000`, recipe flags, direct `$PORT`/`${PORT}` references, comments, nested Make functions, and computed/indirect values.

- [ ] **Step 2: Run the discovery target and verify RED**

Run: `cmake --build build --target test_discovery && ./build/test_discovery`

Expected: package/Make declarations are absent.

- [ ] **Step 3: Implement format-specific parsers**

Add a local JSON tokenizer that validates complete JSON, selects string values directly in the top-level `scripts` object, decodes escapes with a decoded-index-to-source-offset map, and recognizes only the two space-separated flags. Add a line-oriented Makefile parser for direct integer assignments, the two flags, and direct variable forms; never evaluate Make or shell expressions. Keep every result manual-only and append parse warnings without stopping the recursive scan.

- [ ] **Step 4: Run the discovery target and verify GREEN**

Run: `cmake --build build --target test_discovery && ./build/test_discovery`

Expected: all four-format discovery checks pass.

- [ ] **Step 5: Prove the scanner is format-bounded**

Run: `rg -n "\.py|\.js|\.ts|source.code|generic" src/discovery.c include/opendoor/discovery.h tests/test_discovery.c`

Expected: no generic source-code scanning path or extra format matcher. Do not stage or commit.

### Task 4: Project-scoped Dashboard aggregation and three statuses

**Files:**
- Modify: `include/opendoor/dashboard.h`
- Modify: `src/dashboard.c`
- Modify: `tests/test_discovery.c`

**Interfaces:**
- Consumes: `OdProjectDiscovery`, `OdScanSnapshot`, endpoint cwd/executable metadata.
- Produces:
  - `OdPortStatus { OD_PORT_NOT_RUNNING, OD_PORT_RUNNING, OD_PORT_IN_USE_OTHER }`
  - `OdPortRow { uint16_t port; OdPortStatus status; bool declared; char relative_folder[OD_PATH_CAP]; char source[OD_PATH_CAP]; }`
  - `OdStatus od_dashboard_init(OdDashboard *dashboard, const char *project_root, const OdProjectDiscovery *discovery, const OdScanSnapshot *snapshot, OdError *error)`
  - Existing page-size/scroll/free functions remain.

- [ ] **Step 1: Replace the old union test with failing project-ownership tests**

Add tests for a declaration matched by same-port project cwd in another folder; `.env` and Compose declarations of port 3000 both becoming `running` from one project endpoint with no extra `live` row; deterministic representative endpoint metadata when several endpoints share a port using exact relative folder then source path as tie-breaks; an unmatched project-owned live row; external-only and owner-unknown `in use (other)`; project-owned precedence over external occupancy; cwd fallback to exe parent; `/work/app-other` containment rejection; `(pid, port)` and `(file, port)` deduplication; `./` paths; and folder/port/source sorting.

- [ ] **Step 2: Run the discovery target and verify RED**

Run: `cmake --build build --target test_discovery && ./build/test_discovery`

Expected: compilation fails on the new status/row/API contract.

- [ ] **Step 3: Implement aggregation**

Canonicalize project and owner paths and compare at component boundaries. Prefer cwd; use `dirname(executable)` only when cwd is empty. Build declaration rows first, deduplicate live endpoints by `(pid, port)`, mark every same-port declaration running when any project endpoint exists, use folder/source tie-breaks only to select representative endpoint metadata, apply project-owned precedence, classify remaining declared occupancy, append only truly undeclared project live ports, then sort. Reject any absolute display path and retain bounds checks before fixed-array copies.

- [ ] **Step 4: Run the discovery target and verify GREEN**

Run: `cmake --build build --target test_discovery && ./build/test_discovery`

Expected: all Dashboard ownership, status, deduplication, containment, and sorting checks pass.

- [ ] **Step 5: Check the task diff**

Run: `git diff --check -- include/opendoor/dashboard.h src/dashboard.c tests/test_discovery.c`

Expected: no output. Do not stage or commit.

### Task 5: External-conflict selection and source-aware proposals

**Files:**
- Modify: `include/opendoor/allocation.h`
- Modify: `src/allocation.c`
- Modify: `include/opendoor/resolution.h`
- Modify: `src/resolution.c`
- Modify: `tests/test_core.c`

**Interfaces:**
- Consumes: Task 1 declarations, Task 4 ownership containment, existing `od_allocate` replacement search.
- Produces:
  - `OdStatus od_allocate_selected(const OdAssignments *wanted, const uint16_t *occupied, size_t occupied_count, const bool *must_reassign, size_t must_reassign_count, OdAllocationPlan *plan, OdError *error)`; `od_allocate` remains as a compatibility wrapper.
  - `OdResolutionItem` containing `declaration_index`, `old_port`, `new_port`, `automatic`, `source_kind`, `write_kind`, exact span/file identity fields, and owned `absolute_path`, `relative_path`, `line_before`, `line_after`, `manual_reason`.
  - `OdResolution` adds `automatic_count` and `manual_count`.
  - `OdStatus od_resolution_build(const char *project_root, const OdProjectDiscovery *discovery, const OdScanSnapshot *snapshot, OdResolution *resolution, OdError *error)`
  - `OdStatus od_resolution_validate_snapshot(const char *project_root, const OdResolution *resolution, const OdScanSnapshot *snapshot, OdError *error)`

- [ ] **Step 1: Write failing selective-allocation and proposal tests**

Test that project-owned occupied ports remain unchanged, external and owner-unknown ports are selected, every machine port is reserved for replacement, allocation starts/wraps at 1024/65535, linked env definition/reference occurrences share one new port, only the env definition is automatic, clean Compose literal is automatic, package/Make/reference entries are manual, multiple independent literals get distinct ports, no-conflict is empty, and a fresh snapshot occupying a proposed new port returns `OD_ERROR_CHANGED`.

- [ ] **Step 2: Run the core target and verify RED**

Run: `cmake --build build --target test_core && ./build/test_core`

Expected: compilation fails on `od_allocate_selected` and `od_resolution_build`.

- [ ] **Step 3: Implement selective allocation and proposal construction**

Make `must_reassign` decide old-port reassignment independently from the occupied bitmap used to reject candidates. Build synthetic unique assignment names per logical declaration group, call `od_allocate_selected`, and expand grouped allocations back into automatic/manual resolution items with complete `-`/`+` line previews. Conflict means at least one external or owner-unknown endpoint at the declared port; project-owned occupancy alone never conflicts. Snapshot validation must verify every proposed new port remains free and every original conflict still qualifies without recomputing different replacements.

- [ ] **Step 4: Run core and discovery tests and verify GREEN**

Run: `cmake --build build --target test_core test_discovery && ./build/test_core && ./build/test_discovery`

Expected: allocation/resolution and discovery/Dashboard tests pass.

- [ ] **Step 5: Check the task diff**

Run: `git diff --check -- include/opendoor/allocation.h src/allocation.c include/opendoor/resolution.h src/resolution.c tests/test_core.c`

Expected: no output. Do not stage or commit.

### Task 6: Atomic locate-then-patch transaction and rollback

**Files:**
- Modify: `include/opendoor/persistence.h`
- Modify: `src/persistence.c`
- Modify: `tests/test_persistence.c`

**Interfaces:**
- Consumes: `OdResolution`, `OdPortSourceKind`, `od_validate_discovery_text`.
- Produces:
  - `OdPatchValidationPhase { OD_PATCH_VALIDATE_MEMORY, OD_PATCH_VALIDATE_WRITTEN }`
  - `typedef OdStatus (*OdPatchValidator)(OdPortSourceKind source_kind, const char *path, const char *text, size_t length, OdPatchValidationPhase phase, void *context, OdError *error)`
  - `OdStatus od_apply_resolution_with_validator(const OdResolution *resolution, OdPatchValidator validator, void *context, size_t *updated, OdError *error)`
  - `OdStatus od_apply_resolution(const OdResolution *resolution, size_t *updated, OdError *error)` using the production discovery validator.

- [ ] **Step 1: Replace plain-overwrite tests with failing transaction tests**

Test one `.env` and one clean Compose file in a single apply: exact spans change, comments/spacing/quote style/key order and file modes survive, manual package/Make/reference files remain byte-identical, backups equal the pre-apply bytes, and `updated` counts automatic port tokens only. Add stale hash/token rejection before backups, same-file multi-edit descending offsets with digit-width changes, CRLF preservation, and no-final-newline preservation.

- [ ] **Step 2: Add the failing all-file rollback test**

Use `od_apply_resolution_with_validator` with a test validator that succeeds for in-memory validation, then deliberately corrupts the second source during `OD_PATCH_VALIDATE_WRITTEN` and returns failure. Assert both source files are restored byte-for-byte, `updated == 0`, and the error states no automatic conflicts were applied.

- [ ] **Step 3: Run the persistence target and verify RED**

Run: `cmake --build build --target test_persistence && ./build/test_persistence`

Expected: compilation fails because the transaction API is absent.

- [ ] **Step 4: Implement preflight, patching, atomic backup/write, validation, and rollback**

Restore only the repository's bounded regular-file read, `write_all`, same-directory `atomic_replace`, and parent sync primitives. Preflight every target and build/validate every patched buffer before disk writes. Create all backups before replacing any source. After each atomic replacement, re-read exact bytes and invoke post-write validation. On any failure, atomically restore every already-touched source from its saved original buffer, verify byte equality, set `updated` to zero, and preserve the original failure plus rollback outcome in the error.

- [ ] **Step 5: Run the persistence target and verify GREEN**

Run: `cmake --build build --target test_persistence && ./build/test_persistence`

Expected: eligible-write, formatting, backup, stale-span, and all-file rollback tests pass.

- [ ] **Step 6: Check the task diff**

Run: `git diff --check -- include/opendoor/persistence.h src/persistence.c tests/test_persistence.c`

Expected: no output. Do not stage or commit.

### Task 7: TUI orchestration, old-style tables, diff preview, and mouse regression

**Files:**
- Modify: `include/opendoor/screens.h`
- Modify: `src/screens.c`
- Modify: `src/tui.c`
- Modify: `tests/test_ui.c`
- Create: `tests/test_tui_pty.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: discovery, Dashboard, resolution, snapshot validation, and persistence transaction APIs from Tasks 1–6.
- Produces:
  - `void od_render_dashboard(OdCanvas *canvas, const OdDashboard *dashboard, const char *status, bool ascii)`
  - `size_t od_resolution_visual_line_count(const OdResolution *resolution)`
  - Existing `od_render_conflicts(...)` renders grouped auto diffs and a separate manual-only section; `scroll` addresses visual preview lines.
  - Existing mouse target APIs remain unchanged.

- [ ] **Step 1: Write failing canvas and hit-target tests**

Update Dashboard fixtures/assertions to exactly `PORT | RELATIVE FOLDER | STATUS | SOURCE`, all three statuses, `./` paths, Unicode/ASCII full grid lines, read-only footer, and no absolute/project process columns. Update Resolve fixtures/assertions for per-file `-`/`+` lines, manual heading/reason, Enter only with `automatic_count > 0`, no Enter for manual-only/no-conflict, one apply-all footer target, row clicks returning `OD_MOUSE_NONE`, narrow viewport pagination, and last-page scrolling.

- [ ] **Step 2: Run the UI target and verify RED**

Run: `cmake --build build --target test_ui && ./build/test_ui`

Expected: old five-column Dashboard and old-port/new-port table assertions fail.

- [ ] **Step 3: Implement screen rendering without reducing the visual chrome**

Keep the banner, roles/colors, outer panels, per-row grid rules, ASCII fallback, footer guides, and responsive minimum size. Use a four-column Dashboard with a danger role for `in use (other)`, success for `running`, and default for `not running`. Render Resolve as a scrollable preview document grouped by relative file, then manual suggestions, without selectable rows.

- [ ] **Step 4: Replace `.ports.env` orchestration with discovery/snapshot state**

In `src/tui.c`, make Dashboard refresh call `od_discover_project_ports(project_root)`, `od_scan_host`, and the new Dashboard initializer. Make Resolve preparation rescan discovery and ownership, build one `OdResolution`, and set `apply_available = automatic_count > 0`. Before Enter applies, run a fresh host scan through `od_resolution_validate_snapshot`; then call `od_apply_resolution`. Report `Updated N port(s)` and append the manual-suggestion count when nonzero. Keep cancel paths write-free and retain wheel/footer click behavior.

- [ ] **Step 5: Run the UI target and verify GREEN**

Run: `cmake --build build --target test_ui && ./build/test_ui`

Expected: fixed menu, Dashboard, preview, single-confirm, and mouse hit-target checks pass.

- [ ] **Step 6: Add and run a PTY mouse regression**

Create a standard-library Python PTY test that starts `opendoor --project <fixture>`, sends SGR mouse menu clicks, wheel events, and footer clicks, verifies Dashboard opens/scrolls/returns, verifies clicks inside Dashboard rows never alter fixture bytes, opens Resolve, and confirms cancel leaves all source files unchanged. Add it to CTest when Python 3 is available with a 30-second timeout.

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`

Expected: all C and PTY tests pass.

- [ ] **Step 7: Check the task diff**

Run: `git diff --check -- include/opendoor/screens.h src/screens.c src/tui.c tests/test_ui.c tests/test_tui_pty.py CMakeLists.txt`

Expected: no output. Do not stage or commit.

### Task 8: User documentation and full verification

**Files:**
- Modify: `README.md`
- Modify: `CMakeLists.txt`
- Modify: `include/opendoor/app.h`
- Modify: `src/app.c`
- Delete: `include/opendoor/update.h`
- Delete: `src/update.c`
- Modify: `tests/test_ui.c`
- Modify only if dependencies changed: `THIRD_PARTY_LICENSES.md`

**Interfaces:**
- Consumes: final behavior from Tasks 1–7.
- Produces: accurate user-facing scope and safety documentation.

- [ ] **Step 1: Update README against the implemented behavior**

First add a failing CLI test that `--update` and `--help` are rejected as unknown options and that normal parsing exposes no update state; run `test_ui` to observe RED. Then remove the update source/header from CMake and disk, remove `update_requested` and `opendoor_print_help` from app interfaces/implementation, and make the CLI test GREEN.

Document recursive discovery and exactly four formats; three Dashboard statuses and project ownership; pre-start external conflict semantics; auto vs manual proposals; one apply-all confirmation; backup/atomic/rollback behavior; replacement range 1024–65535; keyboard/mouse controls; and the explicit caveats that IP-prefixed Compose mappings are unsupported, `.opendoor.bak` files may need `.gitignore`, changing a host port does not update other references to the old port, and an already-running stack—especially Docker-published ports held by `docker-proxy`—will be treated as externally occupied because Resolve Conflicts is a pre-start check.

- [ ] **Step 2: Run formatting and forbidden-reference audits**

Run: `git diff --check`

Expected: no output.

Run: `rg -n "\bpopen\s*\(|\bsystem\s*\(" src include tests`

Expected: no output.

Run: `rg -n "onboarding|docker\.h|settings_store|theme\.h|help\.h|vendor/jsmn|jsmn" src include CMakeLists.txt tests`

Expected: no removed-module references, including `src/update.c`, `include/opendoor/update.h`, `opendoor_print_help`, or an accepted `--update` path.

- [ ] **Step 3: Run a clean build and complete test suite**

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`

Expected: configure/build exit 0; every C and PTY test passes with zero failures.

- [ ] **Step 4: Inspect the final scope**

Run: `git status --short && git diff --stat && git diff --check`

Expected: only the spec, plan, approved source/header/test/build/docs files are modified; `opendoor.zip`, `dist/`, and packaging scripts are untouched. Do not stage, commit, push, or update the PR without a new explicit request.
