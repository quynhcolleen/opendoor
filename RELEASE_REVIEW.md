# Release readiness — 2026-10-08

Recommendation: prepare a small Linux alpha. The incomplete-scan reporting
issue below was addressed on 2026-10-09; the remaining release recommendations
still apply. The project already has a useful scope: discover project
ports, identify conflicts, apply a selected fix, and inspect/revert recorded
changes. More large features are not a prerequisite for a free, open-source
release. I would not present this checkout as a stable 1.0 yet.

This is a local source and workflow review, not verification of a published
release or other architectures. The original review accompanied the Resolve
table implementation. The 2026-10-09 follow-up addresses finding #1 only, not the
other separate release tasks.

## Findings, in priority order

1. **High — addressed 2026-10-09: incomplete discovery could be presented as a clean result.**
   Reproduced with a temporary project containing `package.json` with
   `{ invalid json }`: Resolve showed `No conflicts found` in both the status
   and table. Dashboard showed `Scan complete with 1 warning(s)` for the same
   project. Discovery recorded recoverable parse/read failures as warnings, but
   Resolve did not expose them. Both screens also discarded the socket snapshot,
   including its warnings, before composing their final status.
   See [prepare_conflicts](src/tui.c), [refresh_dashboard](src/tui.c), and
   [inspect_package_file / inspect_compose_file](src/discovery.c).

   Both screens now retain a bounded, combined scan report. Any warning marks
   the scan incomplete and suppresses the clean-result wording. Resolve lists
   warnings before proposals; Dashboard exposes the same details through the
   keyboard/mouse `w Warnings` footer. File paths are project-relative, and
   severity appears as text. Up to 200 details are retained; total and blocking
   counts include omitted warnings.

   The centralized [severity policy](src/scan_report.c) allows Apply with
   informational discovery/owner warnings and shows an incompleteness notice.
   Unreadable/partial socket data or unclassified warnings block Apply in both
   the initial scan and the fresh pre-write scan, before any backup, source, or
   History write. Detection/allocation and persistence remain unchanged.

   Real-terminal tests cover the malformed-JSON reproduction, that same file
   alongside a real `.env` conflict, Dashboard warning access/return controls,
   informational Apply, and blocking warnings first appearing at either scan.
   Unit tests cover severity defaults, combined warning lifetime, capped totals,
   partial/unreadable socket inputs, responsive scrolling, and mouse targets.

2. **Medium: first-use instructions stop before installation, and help is absent.**
   [README](README.md) builds the executable, then invokes `opendoor` without
   explaining `./build/opendoor` or installing it into PATH. `--help` currently
   returns an unknown-option error in [argument parsing](src/app.c). Add help,
   an explicit user-local installation command, runtime dependency information,
   and a short example project or terminal recording. Keep the existing
   explanation that `--update` rebuilds a local checkout; it does not download
   a newer release.

3. **Medium: release versioning and automation need tightening.**
   [build-release.sh](scripts/build-release.sh) accepts `--version` but passes it
   only to packaging. The executable version still comes from the hard-coded
   project version in [CMakeLists.txt](CMakeLists.txt). A differently named archive
   can therefore contain a binary reporting `0.1.0` (source-confirmed; a custom
   release-version build was not executed). Use one version source and assert
   that the archive version equals the executable's `--version` output.
   No repository CI workflow was found. Add native and sanitizer tests plus an
   install/package smoke test. Test arm64 and the oldest supported distribution
   before advertising those binaries. Remove tracked Python bytecode in a
   separate cleanup; retain source tests.

4. **Medium: loading feedback does not bound or cancel slow work.**
   The interstitial is flushed before work, but scans still run synchronously.
   [Socket discovery](src/scan.c) uses blocking `recv`, and
   [History transactions](src/history.c) wait on `F_SETLKW` with no deadline.
   A held history lock can keep Apply waiting indefinitely; input is not handled
   during the wait. Add a bounded lock wait with an actionable retry message,
   then cancellation/timeouts for scans. This is a follow-up robustness task,
   not evidence that the loading flush fix is broken.

## Features worth adding next

1. **Broader Compose discovery, starting with filenames.** OpenDoor recognizes
   only `compose.yaml` and `docker-compose.yml`. Docker also supports
   `compose.yml` and `docker-compose.yaml`. Support those names first; then
   consider IP-prefixed mappings and long `ports` syntax, with explicit warnings
   for unsupported declarations and automatic writes only for unambiguous
   spans. These are defined in the
   [Compose file documentation](https://docs.docker.com/compose/intro/compose-application-model/)
   and [ports specification](https://docs.docker.com/reference/compose-file/services/#ports).
2. **A read-only command for scripts**, such as `opendoor check --json`, with
   distinct exit statuses for clean, conflict, and incomplete scan. This would
   make the existing pre-start workflow usable from Makefiles and CI without
   automatically editing project files.
3. **User-controlled discovery exclusions and searchable details.** Larger
   projects need a way to skip fixture/generated directories and inspect long
   filenames or reasons without enlarging the terminal. Keep exclusions visible
   so skipped directories are not mistaken for scanned ones.

Keep process killing, service orchestration, cloud accounts, and automatic
editing of arbitrary package scripts out of the initial release scope. Gather
feedback from a few real projects before expanding into those areas.

## What was verified

- On 2026-10-09, native and AddressSanitizer/UndefinedBehaviorSanitizer suites:
  all nine CTest targets passed, including both real ncurses PTY suites.
- Resolve rows at widths 60, 78, 79, 90, 91, and 160: responsive headers, linked
  source paths, continuous selection styling, exact Apply hit targets, and
  non-actionable manual rows.
- Two conflicts in the same `.env`: applying one leaves the other unchanged,
  logs only that edit, and refreshes the remaining row so it can be applied next.
  A linked Compose reference stays unchanged and resolves through the edited
  definition. Queued Enter keys cannot confirm the next row.
- Existing persistence/History tests cover stale edits, rollback, symlink
  refusals, history locking, and malformed records. The documented absence of a
  multi-file crash-recovery journal remains a limitation.
- Temporary-prefix installation, executable version, x86_64 tarball contents,
  and SHA-256 verification passed on this machine. Runtime portability across
  distributions and arm64 execution were not verified.

The Resolve layout and controls, including exact column widths and clipping
behavior, are documented in [README](README.md#resolve-conflicts).
