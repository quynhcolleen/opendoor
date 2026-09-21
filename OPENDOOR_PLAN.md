# OpenDoor — Standalone C TUI Implementation Plan

## Summary

Build **OpenDoor**, an independent Linux-first port discovery and conflict-resolution application written in C17.

- Executable: `opendoor`
- Separate MIT-licensed repository.
- Linux x86_64 and arm64 release tarballs.
- `ncursesw` interface with Midnight Cyan as the default theme.
- Per-project discovery and profiles.
- Interactive TUI only; no command subcommands.
- Does not replace or modify DSVN’s current Bash or Make workflow.
- Can generate a DSVN-compatible `.ports.env`.
- Never starts/stops services, kills processes, or invokes `sudo`.

## Product and UX

### Startup experience

1. Determine the project root from `--project PATH` or the current directory.
2. Initialize the terminal and render a centered, embedded five-line slanted `OPENDOOR` banner.
3. Run the real scan behind a compact animation:
   - Project files
   - Kernel sockets
   - Process ownership
   - Docker
   - Candidate reconciliation
4. Show completed stages, active spinner, elapsed time, and nonfatal warnings.
5. Use an 80 ms animation tick and a 350 ms minimum presentation time; never add a long fake delay.
6. `Esc` skips the cosmetic animation while scanning continues.
7. `reduced_motion=true` replaces animation with static progress text.
8. Transition to the main menu.

No runtime FIGlet dependency is permitted; the pre-rendered banner is compiled into the binary.

### Main menu

First run:

- Discover this project
- Open listener explorer
- Settings and appearance
- Help
- Quit

Configured project:

- Open dashboard
- Resolve conflicts
- Rescan
- Edit project profile
- Settings and appearance
- Help
- Quit

Disabled actions remain visible with an explanation. Existing profiles default to “Open dashboard”; new projects default to “Discover this project.”

### Dashboard

Use a dense widget layout inspired by `btm`’s focusable, expandable widgets, searchable tables, help overlay, and theme system, while implementing the UI independently in C. See the [bottom repository](https://github.com/ClementTsang/bottom) and [official usage guide](https://bottom.pages.dev/stable/usage/general-usage/).

- Header: project, profile state, last scan, refresh mode, permission limitations.
- Summary cards: managed, available, conflicts, reassigned, listeners, Docker mappings.
- Primary `Services` table.
- Secondary `Conflicts` widget.
- `Host listeners` and `Docker mappings` widgets.
- Bottom status and context-sensitive key-hint bar.
- `e` expands the focused widget.
- `?` opens searchable help.
- `/` searches the active table.
- Selection remains attached to a stable row ID after refresh or sorting.

Responsive layout:

- `120+` columns: full multi-widget grid.
- `80–119`: dominant services widget with one secondary widget.
- `60–79`: one full-width widget with a tab bar.
- Below `60×18`: centered resize message without corrupting terminal state.

### Tables and interaction

Tables:

- Services: `SERVICE`, `GROUP`, `VARIABLE`, `PREFERRED`, `SELECTED`, `STATUS`, `CONFLICT`
- Listeners: `PORT`, `PROTOCOL`, `BIND`, `PROCESS`, `PID`, `USER`, `SOURCE`
- Docker: `CONTAINER`, `HOST PORT`, `CONTAINER PORT`, `PROTOCOL`, `PROJECT`
- Discovery: `USE`, `CONFIDENCE`, `NAME`, `SOURCE`, `VARIABLE`, `PORT`, `GROUP`
- Changes: `SERVICE`, `OLD`, `NEW`, `REASON`

Presentation:

- Unicode borders with automatic ASCII fallback.
- Sticky, sortable headers.
- Right-aligned numeric fields.
- Selected-row highlighting and focused-widget borders.
- Full values in a detail pane when cells are truncated.
- Scroll position, empty-state copy, and permission-limited labels.
- Color is never the only status indicator.
- No horizontal border between every row; maintain `btm`-style density.

Input:

- Full keyboard support: arrows and `hjkl`, Enter, Space, PgUp/PgDn, Home/End, `e`, `/`, `?`, `r`, `q`.
- Mouse support for menus, buttons, tabs/widgets, table rows, checkboxes, scrollbars, and wheel scrolling.
- No drag resizing, double-click commands, or context menus in v1.
- Every clickable action has a keyboard equivalent.

### Theme and motion

Default Midnight Cyan roles:

- Terminal/default near-black background.
- Cyan focused borders and primary actions.
- Green available/saved states.
- Amber warnings and reassigned states.
- Red conflicts/errors.
- Neutral gray inactive borders and metadata.
- High-contrast selected rows.

Ship Midnight, Gruvbox, Nord, and monochrome built-in themes. Settings control theme, Unicode/ASCII mode, reduced motion, mouse input, auto-refresh, and refresh interval.

## Workflows

### First-run onboarding

1. Scan the current project and host.
2. Present candidates grouped as `Confirmed`, `Likely`, or `Possible`.
3. Preselect high-confidence candidates, but require review of every candidate.
4. Allow selection, deselection, editing, and manual service creation.
5. Each managed service requires:
   - Stable ID
   - Display name
   - Group
   - Environment variable
   - Preferred port
   - Protocol list
   - Discovery sources
6. Validate names, variables, ranges, duplicates, and ambiguous mappings.
7. Resolve conflicts one at a time:
   - Show owner/process/container.
   - Show recommended free port.
   - Accept, edit, skip, or cancel.
8. Display the complete assignment diff.
9. Rescan and bind-probe immediately before saving.
10. Save `.opendoor/project.toml` and `.ports.env` atomically.
11. Enter the dashboard with the saved state.

### Routine use

- Load the profile and existing assignments.
- Scan automatically.
- Classify each assignment as `AVAILABLE`, `IN USE`, `REASSIGNED`, `SAVED`, or `STALE`.
- Preserve valid saved assignments.
- Resolve only new, missing, or conflicting assignments.
- Manual refresh uses `r`.
- Auto-refresh is disabled by default and can be toggled at a five-second default interval.
- Pause auto-refresh during dialogs, editing, onboarding, and conflict resolution.

### Reset and recovery

- “Reset local assignments” removes only the generated assignment state, not the project profile.
- Managed files receive one rotating `.opendoor.bak` backup before replacement/removal.
- Refuse to overwrite a foreign assignment file without an OpenDoor or compatible `PORTS_CONFIGURED=1` marker.
- Offer import, alternate output path, or cancellation when a foreign file exists.
- Never edit `.gitignore`; warn when the local assignment file appears unignored.

## Engine and Data Contracts

### Scanner

- Query Linux `NETLINK_INET_DIAG` in batches for TCP/UDP IPv4 and IPv6.
- Fall back to `/proc/net/tcp*` and `/proc/net/udp*`.
- Collect protocol, state, addresses, port, UID, and socket inode.
- Build a target inode set, then walk `/proc/<pid>/fd` once for ownership.
- Read process name, executable, command line, PID, and user when permitted.
- Mark permission failures explicitly and continue without elevation.
- Sanitize control characters and invalid UTF-8 before displaying external data.

Docker:

- Use optional batched `docker` CLI execution through `fork`/`exec`, never a shell.
- Discover running containers, published mappings, Compose project labels, and service names.
- Degrade to host-only scanning if Docker is absent, stopped, or inaccessible.
- Apply timeouts and bounded output sizes.

Static project discovery:

- Discover standard Compose files and overrides.
- When Docker Compose is available, call `docker compose config --format json --no-interpolate`.
- Inspect dotenv files for numeric `*_PORT` values.
- Inspect package metadata/scripts for explicit port options.
- Treat Makefile matches as low-confidence suggestions only.
- Import existing compatible `.ports.env` assignments.
- Cache static discovery; routinely refresh only sockets, processes, and Docker.

Scalability:

- Scanner runs on a worker thread; only the main thread calls ncurses.
- Publish immutable scan snapshots through a synchronized queue and wake pipe.
- Render only visible table rows.
- Preserve full datasets without silently truncating them.
- Store full endpoint lists while showing compact summaries in tables.
- Cancel obsolete scan generations when a newer refresh begins.

### Allocation rules

- Default allowed range: `1024–65535`.
- Treat numeric ports as globally unique regardless of protocol or bind address.
- Prefer each service’s configured preferred port.
- When occupied, search upward.
- Skip:
  - Live host/Docker ports
  - Saved assignments belonging to another service
  - Ports assigned during the current plan
  - Other services’ preferred ports
- Preserve an existing saved assignment when it remains valid.
- Immediately before writing, rescan and attempt conservative TCP and UDP wildcard binds.
- If system state changed, invalidate the proposal and return to review instead of writing stale results.

### Project profile

Path: `.opendoor/project.toml`

```toml
schema_version = 1
project_name = "DSVN"
assignment_file = ".ports.env"
port_min = 1024
port_max = 65535

[discovery]
compose_files = ["BACKEND/docker-compose.yaml"]
env_files = []
package_files = ["FRONTEND/package.json"]

[[service]]
id = "user-service"
name = "User service"
group = "backend"
variable = "USER_HOST_PORT"
preferred_port = 3000
protocols = ["tcp"]
sources = ["compose:BACKEND/docker-compose.yaml#user-app"]
```

Use a pinned, vendored TOML parser with its license included. Reject unknown schema versions and invalid duplicate IDs/variables.

### Assignment output

Default path: `.ports.env`

```dotenv
# Generated by OpenDoor. Machine-local; do not commit.
# Profile: .opendoor/project.toml
PORTS_CONFIGURED=1
OPENDOOR_CONFIGURED=1
USER_HOST_PORT=3000
```

- Variable names must match `[A-Z_][A-Z0-9_]*`.
- Values are numeric only.
- Write a temporary file in the same directory, `fsync`, then rename.
- Preserve safe existing permissions or create as `0644` under the user’s umask.
- Reject unsafe symlink/non-regular targets.
- The compatibility marker lets existing DSVN Make targets consume the file without modification.

### Global settings

Path: `$XDG_CONFIG_HOME/opendoor/settings.toml`, falling back to `~/.config/opendoor/settings.toml`.

```toml
schema_version = 1
theme = "midnight"
unicode = "auto"
reduced_motion = false
mouse = true
auto_refresh = false
refresh_seconds = 5
```

### Public executable interface

```text
opendoor
opendoor --project PATH
opendoor --profile PATH
opendoor --ascii
opendoor --no-color
opendoor --reduced-motion
opendoor --help
opendoor --version
```

No operational subcommands in v1.

Exit codes:

- `0`: clean exit
- `2`: invalid arguments
- `3`: invalid/unreadable configuration
- `4`: unsupported or undersized terminal
- `5`: fatal scanner initialization failure

Docker and process-permission failures are nonfatal and appear inside the TUI.

## Implementation and Delivery

### Architecture

Use C17, CMake, pthreads, and dynamically linked `ncursesw`.

Subsystem boundaries:

- Application state machine and event loop
- Off-screen cell renderer and ncurses backend
- Screens, widgets, tables, dialogs, and themes
- Linux socket/process scanner
- Docker and static-project discovery providers
- Candidate reconciliation and allocation engine
- TOML profile/settings and dotenv assignment storage
- Worker queue, timers, signals, and cancellation
- Logging and diagnostics

Use `wnoutrefresh`/`doupdate` for flicker-free painting. Handle `SIGWINCH`, `SIGINT`, and `SIGTERM` through a self-pipe; signal handlers must not call ncurses or allocate memory.

Vendor pinned TOML and JSON tokenizers with license files. Do not depend on FIGlet, YAML libraries, shell execution, or DSVN source code.

### Implementation sequence

1. Initialize the separate `opendoor` MIT repository and CMake/CTest build.
2. Implement models, profile/settings parsing, dotenv output, and allocation tests.
3. Implement fixture-driven Linux, process, Docker, and project discovery providers.
4. Build the off-screen renderer, Midnight theme, banner, loading screen, and main menu.
5. Implement first-run candidate review and profile generation.
6. Implement dashboard widgets, tables, focus/expand, search, sorting, scrolling, details, and click regions.
7. Implement guided conflict resolution, final bind verification, atomic save, reset, and backups.
8. Add settings/help screens, built-in themes, responsive layouts, reduced motion, and ASCII fallbacks.
9. Harden input sanitization, error handling, cancellation, and large-dataset performance.
10. Produce Linux x86_64 and arm64 release tarballs containing the binary, license, README, and sample profile.

The existing DSVN Makefile and Bash scripts remain unchanged.

## Test Plan and Acceptance Criteria

### Automated tests

- Allocation: preferred, occupied, reserved, duplicate, range exhaustion, stale saved assignments.
- Config: valid profile, malformed TOML, duplicate IDs/variables, unsupported schema, foreign assignment files.
- Scanner: netlink/proc fixtures, IPv4/IPv6, TCP/UDP, duplicate endpoints, permission-limited processes.
- Docker: absent CLI, daemon failure, malformed output, many containers, Compose labels.
- Discovery: Compose variables/defaults, dotenv ports, package scripts, low-confidence Makefile candidates.
- Merge: source deduplication, confidence ranking, stable row IDs, saved-assignment import.
- Security: control characters, invalid UTF-8, oversized files/output, unsafe paths and symlinks.
- UI golden snapshots:
  - Main menu and banner
  - Loading stages
  - First-run discovery
  - Wide/medium/compact dashboards
  - Empty, permission-limited, error, and large-data states
  - All built-in themes
  - Unicode and ASCII modes
  - Reduced-motion mode
- PTY interaction:
  - Keyboard navigation
  - Clickable menus/widgets/rows/buttons
  - Wheel scrolling
  - Resize
  - Search, sort, expand, help, dialogs, and quit
- Memory/safety: ASan, UBSan, leak checks, warning-clean GCC and Clang builds.

### Acceptance criteria

- Main menu displays the `OPENDOOR` banner and responsive loading animation.
- No visible flicker during navigation or refresh.
- UI remains responsive while scanning thousands of sockets/processes.
- Selection remains on the same logical row after refresh.
- Every mouse action has a keyboard equivalent.
- A user can complete first-run discovery, review, resolution, and save without editing a file manually.
- Long fields never corrupt borders or adjacent columns.
- Missing Docker and restricted `/proc` access degrade clearly without terminating the app.
- Pre-save state changes cannot produce a known-conflicting assignment.
- Generated `.ports.env` works with DSVN without modifying its current workflow.
- Fresh installs build from source with documented `cmake`, compiler, pthread, and `libncurses-dev` requirements.
- Release archives are produced for Linux x86_64 and arm64.

## Assumptions

- Product branding is **OpenDoor**.
- Keyboard and targeted mouse interaction are both supported.
- Linux is the only supported OS in v1.
- OpenDoor manages ports only; it never controls project processes or containers.
- Project profiles are per-repository; there is no global project registry.
- Automatic refresh is optional and disabled by default.
- The separate repository is the source of truth; DSVN is only used to store this planning artifact and validate `.ports.env` compatibility.
