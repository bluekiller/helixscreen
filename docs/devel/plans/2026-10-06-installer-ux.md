# Installer UX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The installer prints a logo, a plan screen, one line per step and a summary; detail goes to a log file (and to screen with `--verbose`); `--dry-run` shows the plan and changes nothing.

**Architecture:** A new output layer in `scripts/lib/installer/common.sh` (`step`/`step_done`/`step_fail`, a quiet `log_info`, `log_note`, `run_logged`, a log-file sink) changes what ~1000 existing log calls do without editing them. `main()` is split at a confirm point: everything before it is read-only detection that fills a plan; `--dry-run` exits there. A generated `logo.sh` module carries the braille art.

**Tech Stack:** POSIX sh (must run under BusyBox ash and dash), bats, Python 3 (gate script only), chafa (logo generation, dev-time only).

**Spec:** `docs/devel/plans/2026-10-06-installer-ux-design.md`

## Global Constraints

- POSIX sh only in `scripts/lib/installer/`; it is bundled into one `install.sh` by `scripts/bundle-installer.sh` and run by `busybox ash` on K1/AD5M-class boards. No bashisms, no new runtime dependencies (no `tput`, no `stty` requirement).
- Every new module must be added to the module list in `scripts/bundle-installer.sh` (order matters: `common.sh` first, `main.sh` last).
- Every function defined in `scripts/lib/installer/*.sh` must have a caller, or `scripts/check_installer_step_reachability.py` fails the commit hook. Define a function in the same commit as its first caller.
- Logs go to **stderr** (`>&2`). Terminal and color decisions therefore test **fd 2**, not fd 1.
- No background processes for UI: the spinner advances only when the installer itself prints. Commands run in the foreground (a backgrounded `sudo` cannot prompt).
- Timestamps in the log file (`[HH:MM:SS]`), never on screen.
- Comments state the code as it is now; no history in comments (CLAUDE.md "Comments describe the code, not its past"). No em-dashes in user-facing text.
- Commit in the worktree with explicit pathspecs. Before a new file's first commit: `git add -N <path>`.
- Tests: `make t`-style inner loop is `bats tests/shell/<file>.bats`; any change under `tests/shell/` means running the whole shell suite (`make test-shell`) before the task's commit.

## Review Focus

1. **A run with no terminal at all** (KIAUH pipes the script on stdin; the in-app updater redirects stdout+stderr to `/var/log/helixscreen-install.log`): no prompt may block, no escape codes or `\r` may appear, and every step must still print exactly one line. Pinned in Task 3 and Task 9.
2. **`curl | sh` with a terminal on stderr but the script on stdin**: the confirm prompt must read `/dev/tty`, and a box with no `/dev/tty` (container, cron) must fall back to "no terminal" rather than fail. Pinned in Task 7.
3. **A failure in the middle of a step** (apt lock held, disk fills during extract): the ✗ line, the tail of the command output and the log path must all print, and the log must be finalized even though `exit` runs inside a step. Pinned in Task 4 and Task 5.
4. **`--dry-run` on a host the e2e sandbox never simulates** (K1, AD5M, mod payload): nothing before the confirm point may write. Pinned by the static check in Task 8.
5. **A non-UTF-8 locale or a `TERM=dumb` terminal**: braille and `✓` must not print as mojibake. Pinned in Task 2 and Task 6.

---

## File map

| File | Responsibility |
|------|---------------|
| `scripts/lib/installer/common.sh` | Output layer: terminal/UTF-8/color detection, verbosity, log sink, `log_*`, `log_note`, `step*`, `run_logged`, `tty_confirm`. |
| `scripts/lib/installer/logo.sh` (new, generated) | `print_logo` with embedded 256- and 16-color braille art. |
| `scripts/render-installer-logo.sh` (new) | Regenerates `logo.sh` from `assets/images/helix-icon-256.png` with chafa. |
| `scripts/lib/installer/plan.sh` (new) | Plan collection (`plan_set`), rendering (`print_plan`), the confirm point and summary. |
| `scripts/lib/installer/main.sh` | Flags (`--dry-run`, `--verbose`), confirm point in `main()`, steps around phases. |
| `scripts/lib/installer/requirements.sh` | `detect_missing_runtime_deps` split from `install_runtime_deps`; `unzip` install deferred. |
| `scripts/lib/installer/competing_uis.sh` | `detect_competing_uis` (read-only list). |
| `scripts/lib/installer/moonraker.sh`, `kiauh.sh` | `detect_moonraker_integration`, `detect_kiauh` (read-only). |
| `scripts/lib/installer/platform.sh`, `host_profile.sh` | Pre-confirm writes moved after the confirm point. |
| `scripts/lib/installer/service.sh`, `permissions.sh` | Raw command output routed through `run_logged`. |
| `scripts/bundle-installer.sh` | Adds `logo.sh` and `plan.sh` to the module list. |
| `tests/shell/helpers.bash` | `export HELIX_INSTALL_VERBOSE=1` so existing assertions on log text keep working. |
| `tests/shell/test_installer_output.bats` (new) | Output layer unit tests. |
| `tests/shell/test_installer_plan.bats` (new) | Plan, dry-run flag, confirm prompt unit tests. |
| `tests/shell/test_installer_confirm_point.bats` (new) | Static check: only read-only functions before the confirm point. |
| `tests/shell/test_install_e2e.bats` + `fixtures/install_e2e_scenario.sh` | dry-run immutability, prompt via pty, golden no-terminal transcripts. |
| `tests/shell/fixtures/install_transcripts/*.txt` (new) | Golden no-terminal transcripts. |
| `docs/devel/INSTALLER.md`, `docs/user/INSTALL.md` | Output, flags, log location. |

---

### Task 1: Correct the spec against the code

The survey that preceded this plan found the spec wrong in four places. Fix the spec first so reviewers judge against the truth.

**Files:**
- Modify: `docs/devel/plans/2026-10-06-installer-ux-design.md`

- [ ] **Step 1: Replace the "Where it runs" table** with:

```markdown
| Caller | stdin | stderr | Notes |
|--------|-------|--------|-------|
| `curl ... \| sh` over ssh | the script | terminal | prompts must read `/dev/tty` |
| KIAUH extension (`helixscreen_extension.py#_run_installer`) | the script | terminal | passes `--update` / `--uninstall`; asks its own confirm first |
| in-app updater (`update_checker.cpp`, `HELIX_SELF_UPDATE=1`) | none | file `/var/log/helixscreen-install.log` | `--local <tarball> --update`; NoNewPrivileges, so no sudo |

Moonraker's update_manager does NOT run install.sh: it unpacks the release zip itself and
`helixscreen-update.path` restarts the service via `config/refresh-service-units.sh`. That
script's own output (`qidi-3mf-thumbs-units.sh`) is the only installer-adjacent output a
Mainsail update produces; it is quieted in Task 5.
```

- [ ] **Step 2: In section 2, "Plan screen", replace** "Reuse whatever the existing `--clean` confirmation does for this." **with** "Nothing in the installer reads `/dev/tty` today (the three existing prompts read stdin behind `[ -t 0 ]`); `tty_confirm` in `common.sh` is new and the `--clean` prompt moves onto it."

- [ ] **Step 3: Replace the paragraph starting "Anything else before the confirm point that writes"** with:

```markdown
Pre-confirm code that writes or may prompt for sudo today, and what happens to it:

| Today | Change |
|-------|--------|
| `check_requirements` apt-installs `unzip` | detect only; install after confirm |
| `install_runtime_deps` apt-installs libraries | split (table above) |
| `set_install_paths` → `cleanup_ad5m_gcodes_root` deletes old archives (AD5M) | moved after confirm |
| `mod_payload_mode_block` → `record_payload_root` mkdir + tee (payload hosts) | moved after confirm |
| `detect_tmp_dir` probes writability with `$SUDO test -w` | plain `test -w` before confirm; sudo probe after |
| `check_disk_space` `dd` probe with `$SUDO` | `sudo -n` only before confirm; undetermined reads as "would check after sudo" |
```

- [ ] **Step 4: In section 2, "`--dry-run`", replace** "non-zero (distinct codes) when a check would stop it" **with** "non-zero (the failing check's existing exit code) when a check would stop it".

- [ ] **Step 5: In section 4, "Existing suite", replace** the first paragraph **with** "`tests/shell/helpers.bash` stubs the `log_*` functions, and most installer tests then source the real `common.sh`, whose `log_info` would turn quiet. `helpers.bash` exports `HELIX_INSTALL_VERBOSE=1`, so `log_info` keeps printing in every existing test; only tests about default (quiet) output unset it."

- [ ] **Step 6: Commit**

```bash
git commit -m "docs(installer): correct the UX spec's callers and pre-confirm writes" -- docs/devel/plans/2026-10-06-installer-ux-design.md
```

---

### Task 2: Terminal, color and UTF-8 detection; quiet `log_info`; log sink

**Files:**
- Modify: `scripts/lib/installer/common.sh` (replace `setup_colors` + the four `log_*` one-liners, lines ~245-273)
- Modify: `tests/shell/helpers.bash` (top of file)
- Create: `tests/shell/test_installer_output.bats`

**Interfaces:**
- Produces (all later tasks rely on these exact names):
  - `UI_TTY` (`1` when stderr is a terminal and `HELIX_INSTALL_TTY` does not say `0`), `UI_UTF8` (`1`/`0`), `UI_COLOR` (`0`, `16`, `256`)
  - `HELIX_INSTALL_VERBOSE` (`1` = print `log_info` to screen)
  - `INSTALL_LOG` (path of the current log file, empty until `log_open`), `_LOG_BUFFER` (lines logged before the file exists)
  - `log_open <path>`: creates the file, flushes `_LOG_BUFFER` into it
  - `_log_write <text>`: append one timestamped line to the log (or buffer)
  - `log_info`, `log_success`, `log_warn`, `log_error` (same signatures as today), `log_note <text>` (new)
  - `ui_detect`: (re)computes `UI_*` and the color variables; called once at source time

- [ ] **Step 1: Make existing tests keep seeing `log_info`.** Add to `tests/shell/helpers.bash` directly after the `export -f log_info ...` line:

```bash
# The installer's log_info prints to the terminal only when verbose; tests
# assert on its text, so they run verbose unless a test unsets this.
export HELIX_INSTALL_VERBOSE=1
```

- [ ] **Step 2: Write the failing tests** in `tests/shell/test_installer_output.bats`:

```bash
#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The installer's output layer in common.sh: what reaches the screen, what
# reaches the log file, and how terminal, color and UTF-8 are decided.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    unset HELIX_INSTALL_VERBOSE
    export HELIX_INSTALL_TTY=0
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
}

@test "log_info is silent on screen by default" {
    run log_info "detail line"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "log_info prints with HELIX_INSTALL_VERBOSE=1" {
    HELIX_INSTALL_VERBOSE=1
    run log_info "detail line"
    [[ "$output" == *"[INFO] detail line"* ]]
}

@test "log_warn and log_error always print" {
    run log_warn "careful"
    [[ "$output" == *"[WARN] careful"* ]]
    run log_error "broken"
    [[ "$output" == *"[ERROR] broken"* ]]
}

@test "log_note prints a plain indented line" {
    run log_note "edit config from Mainsail"
    [ "$output" = "    edit config from Mainsail" ]
}

@test "lines before log_open are buffered, then flushed with timestamps" {
    log_info "early one"
    log_warn "early two"
    log_open "$BATS_TEST_TMPDIR/install.log"
    log_info "after open"
    run cat "$BATS_TEST_TMPDIR/install.log"
    [[ "${lines[0]}" =~ ^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\]\ INFO\ early\ one$ ]]
    [[ "${lines[1]}" =~ ^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\]\ WARN\ early\ two$ ]]
    [[ "${lines[2]}" =~ INFO\ after\ open$ ]]
}

@test "the log file never contains color escapes" {
    HELIX_INSTALL_TTY=1 ui_detect
    log_open "$BATS_TEST_TMPDIR/install.log"
    log_warn "${BOLD}colored${NC} on screen"
    run grep -c "$(printf '\033')" "$BATS_TEST_TMPDIR/install.log"
    [ "$output" = "0" ]
}

@test "no terminal means no color" {
    HELIX_INSTALL_TTY=0 ui_detect
    [ "$UI_TTY" = 0 ]
    [ "$UI_COLOR" = 0 ]
    [ -z "$CYAN" ]
}

@test "a forced terminal with TERM=dumb gets no color" {
    TERM=dumb HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 0 ]
}

@test "NO_COLOR disables color on a terminal" {
    NO_COLOR=1 TERM=xterm-256color HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 0 ]
}

@test "256-color terminals are detected from TERM" {
    TERM=xterm-256color HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 256 ]
    TERM=vt100 HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 16 ]
}

@test "UTF-8 is read from LC_ALL, then LC_CTYPE, then LANG" {
    LC_ALL= LC_CTYPE= LANG=en_US.UTF-8 ui_detect;  [ "$UI_UTF8" = 1 ]
    LC_ALL=C LC_CTYPE= LANG=en_US.UTF-8 ui_detect; [ "$UI_UTF8" = 0 ]
    LC_ALL= LC_CTYPE=C.utf8 LANG= ui_detect;       [ "$UI_UTF8" = 1 ]
    LC_ALL= LC_CTYPE= LANG= ui_detect;             [ "$UI_UTF8" = 0 ]
}

@test "the output layer runs under busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; log_note hi; log_open "$2"; log_warn w; cat "$2"' \
        _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh" "$BATS_TEST_TMPDIR/a.log"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN w"* ]]
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `bats tests/shell/test_installer_output.bats`
Expected: FAIL (`log_info` prints, `log_note`/`log_open`/`ui_detect` undefined).

- [ ] **Step 4: Implement.** Replace `setup_colors()` and the four `log_*` lines in `common.sh` with:

```sh
# Output is decided by stderr, where every log line goes: under `curl | sh`
# stdin is the script and stdout may be a pipe while stderr is the terminal.
# HELIX_INSTALL_TTY=0|1 overrides the probe (tests, and callers that know).
ui_detect() {
    case "${HELIX_INSTALL_TTY:-}" in
        0) UI_TTY=0 ;;
        1) UI_TTY=1 ;;
        *) if [ -t 2 ]; then UI_TTY=1; else UI_TTY=0; fi ;;
    esac

    _ui_locale="${LC_ALL:-${LC_CTYPE:-${LANG:-}}}"
    case "$_ui_locale" in
        *[Uu][Tt][Ff]-8*|*[Uu][Tt][Ff]8*) UI_UTF8=1 ;;
        *) UI_UTF8=0 ;;
    esac

    UI_COLOR=0
    if [ "$UI_TTY" = 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
        case "${TERM:-}:${COLORTERM:-}" in
            *256color*|*:truecolor|*:24bit) UI_COLOR=256 ;;
            *) UI_COLOR=16 ;;
        esac
    fi

    if [ "$UI_COLOR" != 0 ]; then
        RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
        CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; NC='\033[0m'
    else
        RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; DIM=''; NC=''
    fi
}
ui_detect

# The log file. Lines logged before log_open (detection runs before the
# install has anywhere to write) are held in _LOG_BUFFER and flushed by it.
INSTALL_LOG=""
_LOG_BUFFER=""

_log_write() {
    _lw_line="[$(date +%H:%M:%S)] $1"
    if [ -n "$INSTALL_LOG" ]; then
        printf '%s\n' "$_lw_line" >> "$INSTALL_LOG" 2>/dev/null || true
    else
        _LOG_BUFFER="${_LOG_BUFFER}${_lw_line}
"
    fi
}

log_open() {
    INSTALL_LOG="$1"
    : > "$INSTALL_LOG" 2>/dev/null || { INSTALL_LOG=""; return 1; }
    printf '%s' "$_LOG_BUFFER" >> "$INSTALL_LOG"
    _LOG_BUFFER=""
}

# Strip \033[...m sequences from a message before it reaches the log. The ESC
# byte comes from printf: BusyBox sed does not understand \x1b.
_ESC=$(printf '\033')
_log_plain() { printf '%b' "$1" | sed "s/${_ESC}\\[[0-9;]*m//g"; }

# Screen output for the four levels. _ui_emit is redefined by the step layer
# (Task 3) to indent under an open step; here it prints the line as given.
_ui_emit() { printf '%b\n' "$1" >&2; }

log_info() {
    _log_write "INFO $(_log_plain "$1")"
    [ "${HELIX_INSTALL_VERBOSE:-0}" = 1 ] && _ui_emit "${CYAN}[INFO]${NC} $1"
    return 0
}
log_success() {
    _log_write "OK $(_log_plain "$1")"
    [ "${HELIX_INSTALL_VERBOSE:-0}" = 1 ] && _ui_emit "${GREEN}[OK]${NC} $1"
    return 0
}
log_warn() {
    _log_write "WARN $(_log_plain "$1")"
    _ui_emit "${YELLOW}[WARN]${NC} $1"
}
log_error() {
    _log_write "ERROR $(_log_plain "$1")"
    _ui_emit "${RED}[ERROR]${NC} $1"
}
# A detail line a regular user should see. Used sparingly.
log_note() {
    _log_write "NOTE $(_log_plain "$1")"
    _ui_emit "    $1"
}
```

Note: `log_success` becomes quiet by default along with `log_info`. Step results now come from `step_done` (Task 3); a `log_success` that must stay visible is promoted to `log_note` in Task 9.

- [ ] **Step 5: Run the new tests and the existing suite**

Run: `bats tests/shell/test_installer_output.bats` → all PASS.
Run: `make test-shell` → expect 0 failures. A failure here is an existing test that asserted on `log_success`/`log_info` without loading `helpers.bash`; give that test file `export HELIX_INSTALL_VERBOSE=1` in its `setup()` and re-run.

- [ ] **Step 6: Mutation check.** Change `log_info`'s guard to `[ 1 = 1 ]`; `bats tests/shell/test_installer_output.bats` must go red on "log_info is silent". Restore.

- [ ] **Step 7: Commit**

```bash
git add -N tests/shell/test_installer_output.bats
git commit -m "feat(installer): quiet log_info, a log file sink and stderr-based terminal detection" -- scripts/lib/installer/common.sh tests/shell/helpers.bash tests/shell/test_installer_output.bats
```

---

### Task 3: Steps

**Files:**
- Modify: `scripts/lib/installer/common.sh` (after the Task 2 block)
- Test: `tests/shell/test_installer_output.bats`

**Interfaces:**
- Consumes: `UI_TTY`, `UI_UTF8`, `UI_COLOR`, `_log_write`, `_ui_emit`
- Produces:
  - `STEP_TOTAL` (set by the plan in Task 7; `0` means "do not number")
  - `step <title>`: opens a step; on a terminal prints `  <spinner> <title>…` without a newline; logs `STEP <title>`
  - `step_done [detail]`: resolves to ✓; no terminal prints `[n/N] <title> ... ok (<detail>)`
  - `step_fail [reason]`: resolves to ✗; prints the same shape with `FAILED`
  - `step_skip`: closes an open step without printing anything (terminal: erases the spinner line)
  - `STEP_OPEN` (`1` while a step is open), `STEP_TITLE`, `STEP_NUM`

- [ ] **Step 1: Write the failing tests** (append to `test_installer_output.bats`):

```bash
@test "no terminal: a step prints one numbered line when done" {
    STEP_TOTAL=8
    step "Downloaded"
    run step_done "100 MB, SHA256 verified"
    [ "$output" = "[1/8] Downloaded ... ok (100 MB, SHA256 verified)" ]
}

@test "no terminal: nothing prints until the step resolves" {
    STEP_TOTAL=8
    run step "Downloaded"
    [ -z "$output" ]
}

@test "no terminal: steps without a total are not numbered" {
    STEP_TOTAL=0
    step "Checked system"
    run step_done
    [ "$output" = "Checked system ... ok" ]
}

@test "no terminal: a failed step says FAILED" {
    STEP_TOTAL=3
    step "Installing libraries"
    run step_fail
    [ "$output" = "[1/3] Installing libraries ... FAILED" ]
}

@test "step_skip prints nothing and does not consume a number" {
    STEP_TOTAL=2
    step "Installing libraries"; step_skip
    step "Downloaded"
    run step_done
    [ "$output" = "[1/2] Downloaded ... ok" ]
}

@test "warnings inside a step are indented under it" {
    STEP_TOTAL=2
    step "Downloaded"
    run log_warn "plain-HTTP mirror"
    [[ "$output" == "      [WARN] plain-HTTP mirror" ]]
}

@test "terminal + UTF-8: done line uses a check mark and erases the spinner" {
    HELIX_INSTALL_TTY=1 LANG=en_US.UTF-8 TERM=vt100 ui_detect
    step "Downloaded"
    run step_done "100 MB"
    [[ "$output" == *$'\r'*"✓ Downloaded"*"100 MB"* ]]
}

@test "terminal without UTF-8 falls back to ASCII marks" {
    HELIX_INSTALL_TTY=1 LANG=C TERM=vt100 ui_detect
    step "Downloaded"
    run step_done
    [[ "$output" == *"ok Downloaded"* ]]
    [[ "$output" != *"✓"* ]]
}

@test "steps are recorded in the log" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    step "Downloaded"; step_done "100 MB" >/dev/null 2>&1
    run grep -E 'STEP Downloaded|DONE Downloaded \(100 MB\)' "$BATS_TEST_TMPDIR/install.log"
    [ "${#lines[@]}" -eq 2 ]
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `bats tests/shell/test_installer_output.bats`
Expected: the new tests FAIL (`step` undefined).

- [ ] **Step 3: Implement** (append to `common.sh` after the Task 2 block):

```sh
STEP_TOTAL=0
STEP_NUM=0
STEP_OPEN=0
STEP_TITLE=""
_SPIN_I=0

_ui_marks() {
    if [ "$UI_UTF8" = 1 ]; then
        MARK_OK="✓"; MARK_FAIL="✗"; ELLIPSIS="…"
    else
        MARK_OK="ok"; MARK_FAIL="FAIL"; ELLIPSIS="..."
    fi
}

_spin_frame() {
    if [ "$UI_UTF8" = 1 ]; then
        set -- ⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏
    else
        set -- '|' '/' '-' '\'
    fi
    _SPIN_I=$(( (_SPIN_I % $#) + 1 ))
    eval "printf '%s' \"\${$_SPIN_I}\""
}

# Redraw the open step's line (terminal only). Called by step and by every
# line printed while the step is open, so the spinner advances as work logs.
_step_redraw() {
    [ "$UI_TTY" = 1 ] && [ "$STEP_OPEN" = 1 ] || return 0
    printf '\r\033[K  %b%s%b %s%s' "$CYAN" "$(_spin_frame)" "$NC" "$STEP_TITLE" "$ELLIPSIS" >&2
}

_ui_emit() {
    if [ "$STEP_OPEN" = 1 ]; then
        [ "$UI_TTY" = 1 ] && printf '\r\033[K' >&2
        printf '%b\n' "      $1" >&2
        _step_redraw
    else
        printf '%b\n' "$1" >&2
    fi
}

step() {
    [ "$STEP_OPEN" = 1 ] && step_done
    _ui_marks
    STEP_TITLE="$1"
    STEP_OPEN=1
    _log_write "STEP $1"
    _step_redraw
}

_step_close() { # mark color word detail
    STEP_NUM=$((STEP_NUM + 1))
    if [ "$UI_TTY" = 1 ]; then
        if [ -n "$4" ]; then
            printf '\r\033[K  %b%s%b %-22s %b%s%b\n' "$2" "$1" "$NC" "$STEP_TITLE" "$DIM" "$4" "$NC" >&2
        else
            printf '\r\033[K  %b%s%b %s\n' "$2" "$1" "$NC" "$STEP_TITLE" >&2
        fi
    else
        _sc_prefix=""
        [ "$STEP_TOTAL" -gt 0 ] && _sc_prefix="[$STEP_NUM/$STEP_TOTAL] "
        if [ -n "$4" ]; then
            printf '%s%s ... %s (%s)\n' "$_sc_prefix" "$STEP_TITLE" "$3" "$4" >&2
        else
            printf '%s%s ... %s\n' "$_sc_prefix" "$STEP_TITLE" "$3" >&2
        fi
    fi
    STEP_OPEN=0
}

step_done() {
    [ "$STEP_OPEN" = 1 ] || return 0
    if [ -n "${1:-}" ]; then _log_write "DONE $STEP_TITLE ($1)"; else _log_write "DONE $STEP_TITLE"; fi
    _step_close "$MARK_OK" "$GREEN" ok "${1:-}"
}

step_fail() {
    [ "$STEP_OPEN" = 1 ] || return 0
    _log_write "FAIL $STEP_TITLE${1:+ ($1)}"
    _step_close "$MARK_FAIL" "$RED" FAILED "${1:-}"
}

step_skip() {
    [ "$STEP_OPEN" = 1 ] || return 0
    _log_write "SKIP $STEP_TITLE"
    [ "$UI_TTY" = 1 ] && printf '\r\033[K' >&2
    STEP_OPEN=0
}
```

- [ ] **Step 4: Run tests**

Run: `bats tests/shell/test_installer_output.bats` → all PASS. Also run the ash test from Task 2 again.

- [ ] **Step 5: Mutation check.** Make `step_skip` increment `STEP_NUM`; "step_skip ... does not consume a number" must go red. Restore.

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(installer): step, step_done, step_fail and step_skip" -- scripts/lib/installer/common.sh tests/shell/test_installer_output.bats
```

Note for the reachability gate: `step`, `step_done`, `step_fail`, `step_skip` have no installer caller until Task 9. Mark each with `# UNCALLED_OK: called from main() once steps land` in this commit and remove those markers in Task 9.

---

### Task 4: `run_logged` and the failure block

**Files:**
- Modify: `scripts/lib/installer/common.sh`
- Test: `tests/shell/test_installer_output.bats`

**Interfaces:**
- Consumes: `_log_write`, `INSTALL_LOG`, `_LOG_BUFFER`, `step_fail`, `STEP_OPEN`
- Produces:
  - `run_logged <cmd> [args...]`: runs the command in the foreground with stdout+stderr captured to a temp file, appended to the log; silent on success; returns the command's exit code. With `HELIX_INSTALL_VERBOSE=1`, the output is also printed (indented).
  - `RUN_LOGGED_TAIL` (default `15`): lines shown on failure
  - `print_failure <cmd-description> <rc> <output-file> [hint]`: the failure block (used by `run_logged` and by Task 9's interrupt handling)

- [ ] **Step 1: Write the failing tests:**

```bash
@test "run_logged is silent on success and returns 0" {
    run run_logged sh -c 'echo hello; echo world >&2'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "run_logged sends all output to the log" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    run_logged sh -c 'echo to-stdout; echo to-stderr >&2'
    run cat "$BATS_TEST_TMPDIR/install.log"
    [[ "$output" == *"to-stdout"* ]]
    [[ "$output" == *"to-stderr"* ]]
    [[ "$output" == *"RUN sh -c"* ]]
}

@test "run_logged keeps the command's exit code" {
    run run_logged sh -c 'exit 100'
    [ "$status" -eq 100 ]
}

@test "run_logged prints the command, exit code and the output tail on failure" {
    RUN_LOGGED_TAIL=2
    run run_logged sh -c 'echo one; echo two; echo three; exit 7'
    [ "$status" -eq 7 ]
    [[ "$output" == *"sh -c"*"failed (exit 7)"* ]]
    [[ "$output" == *"two"* ]]
    [[ "$output" == *"three"* ]]
    [[ "$output" != *"one"* ]]
}

@test "run_logged echoes output live when verbose" {
    HELIX_INSTALL_VERBOSE=1
    run run_logged sh -c 'echo visible'
    [[ "$output" == *"visible"* ]]
}

@test "run_logged exit code survives under busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; run_logged sh -c "exit 42"; echo "rc=$?"' \
        _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    [[ "$output" == *"rc=42"* ]]
}
```

- [ ] **Step 2: Run to verify it fails.** Run: `bats tests/shell/test_installer_output.bats` → new tests FAIL.

- [ ] **Step 3: Implement:**

```sh
RUN_LOGGED_TAIL=${RUN_LOGGED_TAIL:-15}

print_failure() { # description rc output-file [hint]
    _ui_emit "${RED}$1 failed (exit $2):${NC}"
    if [ -s "$3" ]; then
        tail -n "$RUN_LOGGED_TAIL" "$3" | while IFS= read -r _pf_line; do
            _ui_emit "  $_pf_line"
        done
    fi
    [ -n "${4:-}" ] && _ui_emit "$4"
    return 0
}

# The command runs in the foreground so a sudo inside it can still prompt;
# its output goes to a temp file, never a pipe, so $? is the command's own.
run_logged() {
    _rl_out=$(mktemp "${TMPDIR:-/tmp}/helix-run.XXXXXX") || { "$@"; return $?; }
    _log_write "RUN $*"
    _step_redraw
    "$@" > "$_rl_out" 2>&1
    _rl_rc=$?
    if [ -n "$INSTALL_LOG" ]; then
        cat "$_rl_out" >> "$INSTALL_LOG" 2>/dev/null || true
    else
        _LOG_BUFFER="${_LOG_BUFFER}$(cat "$_rl_out")
"
    fi
    if [ "${HELIX_INSTALL_VERBOSE:-0}" = 1 ]; then
        while IFS= read -r _rl_line; do _ui_emit "  $_rl_line"; done < "$_rl_out"
    fi
    [ "$_rl_rc" -ne 0 ] && print_failure "$*" "$_rl_rc" "$_rl_out"
    rm -f "$_rl_out"
    return "$_rl_rc"
}
```

- [ ] **Step 4: Run tests.** `bats tests/shell/test_installer_output.bats` → PASS.

- [ ] **Step 5: Mutation check.** Replace `"$@" > "$_rl_out" 2>&1` with `"$@" 2>&1 | cat > "$_rl_out"`; the exit-code tests must go red. Restore.

- [ ] **Step 6: Commit** (mark `run_logged` and `print_failure` `# UNCALLED_OK: callers land in Task 5` if Task 5 is a separate commit):

```bash
git commit -m "feat(installer): run_logged captures command output and shows its tail on failure" -- scripts/lib/installer/common.sh tests/shell/test_installer_output.bats
```

---

### Task 5: Route raw command output through `run_logged`

**Files:**
- Modify: `scripts/lib/installer/requirements.sh:134` (runtime libs apt), `:553` (libssl1.1 apt)
- Modify: `scripts/lib/installer/service.sh:460, :571` (`daemon-reload`), `:715` (`systemctl enable`), `:729` (`systemctl $action`), `:872` (`systemctl stop`)
- Modify: `scripts/lib/installer/competing_uis.sh:463-473` (`install_qidi_3mf_thumbs` call to `qidi-3mf-thumbs-units.sh`)
- Modify: `scripts/lib/installer/moonraker.sh` `restart_moonraker` (~:806)
- Modify: `config/qidi-3mf-thumbs-units.sh:32` (`qlog`)
- Test: `tests/shell/test_installer_output.bats`, plus the existing tests of each touched function

**Interfaces:**
- Consumes: `run_logged`

- [ ] **Step 1: Write the failing test** for the one behavior a user sees: the "Created symlink" line no longer reaches the screen. Append to `test_installer_output.bats`:

```bash
@test "enabling the service does not print systemctl's own output" {
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '#!/bin/sh\necho "Created symlink /etc/systemd/system/x.wants/helixscreen.service"\n' > "$BATS_TEST_TMPDIR/bin/systemctl"
    chmod +x "$BATS_TEST_TMPDIR/bin/systemctl"
    PATH="$BATS_TEST_TMPDIR/bin:$PATH" SUDO="" SERVICE_NAME=helixscreen
    run _enable_helixscreen_unit
    [[ "$output" != *"Created symlink"* ]]
}
```

`_enable_helixscreen_unit` is the name for the enable call at `service.sh:715`, extracted into its own function in this task so it is testable. Read the surrounding function first; if it already has a narrower helper, test that helper instead and keep the name.

- [ ] **Step 2: Run to verify it fails.** `bats tests/shell/test_installer_output.bats -f "Created symlink"` → FAIL.

- [ ] **Step 3: Convert each site.** Pattern, applied verbatim at every site listed above:

```sh
# before
$SUDO systemctl enable "$SERVICE_NAME"
# after
run_logged $SUDO systemctl enable "$SERVICE_NAME"
```

```sh
# before (requirements.sh:134)
if ! $SUDO apt-get install -y --no-install-recommends $missing; then
# after
if ! run_logged $SUDO apt-get install -y --no-install-recommends $missing; then
```

For `install_qidi_3mf_thumbs`: `run_logged $SUDO "$units_sh" "$KLIPPER_USER" "$KLIPPER_GROUP" || true` (keeping the `HELIX_QIDI_HOME=...` assignment as an `env` prefix: `run_logged env HELIX_QIDI_HOME="..." $SUDO ...`; check how the variable is passed today and keep it reaching the script).

In `config/qidi-3mf-thumbs-units.sh`, `qlog` stays (it also runs from `refresh-service-units.sh` under systemd, where its output belongs in the journal); its messages are now captured by `run_logged` when the installer runs it.

Keep existing `|| true` and `2>/dev/null` semantics: where a call had `2>/dev/null || true`, the converted call is `run_logged ... || true` (the redirect is now redundant; drop it).

- [ ] **Step 4: Run tests.** `bats tests/shell/test_installer_output.bats` and every bats file that tests the touched functions (`grep -l -E 'install_runtime_deps|verify_binary_deps|install_service|start_service|install_qidi_3mf_thumbs|restart_moonraker' tests/shell/*.bats`), then `make test-shell`.

- [ ] **Step 5: Mutation check.** Revert the `service.sh:715` hunk; the new test must go red.

- [ ] **Step 6: Commit**

```bash
git commit -m "fix(installer): apt, systemctl and helper output goes to the install log" -- scripts/lib/installer/requirements.sh scripts/lib/installer/service.sh scripts/lib/installer/competing_uis.sh scripts/lib/installer/moonraker.sh tests/shell/test_installer_output.bats
```

---

### Task 6: Logo module

**Files:**
- Create: `scripts/render-installer-logo.sh`
- Create: `scripts/lib/installer/logo.sh` (generated, committed)
- Modify: `scripts/bundle-installer.sh` (module list: insert `logo.sh` after `common.sh`)
- Test: `tests/shell/test_installer_output.bats`

**Interfaces:**
- Consumes: `UI_TTY`, `UI_UTF8`, `UI_COLOR`, color variables
- Produces: `print_banner <version> <channel>` (prints logo + logotype + version line, or one plain line with no terminal)

- [ ] **Step 1: Write the generator** `scripts/render-installer-logo.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Renders the installer's braille logo from the app icon into
# scripts/lib/installer/logo.sh. Needs chafa and python3 with Pillow.
# Usage: scripts/render-installer-logo.sh
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/assets/images/helix-icon-256.png"
out="$root/scripts/lib/installer/logo.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 -I -c 'import sys; from PIL import Image; im = Image.open(sys.argv[1]); im.crop(im.getbbox()).save(sys.argv[2])' "$src" "$tmp/ribbon.png"

render() { # colors
    chafa --format symbols --size 26x10 --symbols braille --colors "$1" --polite on --animate off "$tmp/ribbon.png"
}

{
    printf '#!/bin/sh\n# SPDX-License-Identifier: GPL-3.0-or-later\n'
    printf '# Generated by scripts/render-installer-logo.sh from assets/images/helix-icon-256.png.\n'
    printf '# Do not edit; re-run the script.\n\n'
    printf '_logo_art() {\n    case "$1" in\n'
    for c in 256 16; do
        printf '        %s) cat <<'"'"'HELIX_LOGO_EOF'"'"'\n' "$c"
        render "$c"
        printf '\nHELIX_LOGO_EOF\n            ;;\n'
    done
    printf '    esac\n}\n'
} > "$out"
echo "wrote $out"
```

Run it: `bash scripts/render-installer-logo.sh`. Confirm `logo.sh` contains two heredocs with ESC bytes: `grep -c "$(printf '\033')" scripts/lib/installer/logo.sh` > 0.

- [ ] **Step 2: Write the failing tests:**

```bash
@test "banner: no terminal prints one plain line" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_TTY=0 ui_detect
    run print_banner v1.1.0-beta.4 beta
    [ "$output" = "HelixScreen installer v1.1.0-beta.4 (beta)" ]
}

@test "banner: UTF-8 color terminal prints the braille logo" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_TTY=1 LANG=en_US.UTF-8 TERM=xterm-256color ui_detect
    run print_banner v1.1.0-beta.4 beta
    printf '%s' "$output" | grep -qP '[\x{2800}-\x{28FF}]'
    [[ "$output" == *"Helix"*"Screen"* ]]
    [[ "$output" == *"v1.1.0-beta.4"* ]]
}

@test "banner: no UTF-8 drops the art and keeps the logotype" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_TTY=1 LANG=C TERM=xterm-256color ui_detect
    run print_banner v1.1.0-beta.4 beta
    [[ "$output" == *"Helix"*"Screen"* ]]
    ! printf '%s' "$output" | grep -qP '[\x{2800}-\x{28FF}]'
}

@test "the generated logo module is in the bundle" {
    grep -q 'logo.sh' "$WORKTREE_ROOT/scripts/bundle-installer.sh"
}
```

- [ ] **Step 3: Run to verify it fails.** → FAIL (`print_banner` undefined).

- [ ] **Step 4: Implement `print_banner`** by appending to the generator's output section (so it survives regeneration). Add to `render-installer-logo.sh` after the `_logo_art` function is written:

```bash
cat <<'SH'

# Logotype colors match the wordmark: Helix in blue, Screen as a red to
# orange ramp. 16-color terminals get the nearest ANSI colors.
_logotype() {
    if [ "$UI_COLOR" = 256 ]; then
        printf '\033[1;38;5;33mHelix\033[38;5;197mSc\033[38;5;203mre\033[38;5;209me\033[38;5;214mn\033[0m'
    elif [ "$UI_COLOR" = 16 ]; then
        printf '\033[1;34mHelix\033[1;31mScr\033[1;33meen\033[0m'
    else
        printf 'HelixScreen'
    fi
}

print_banner() { # version channel
    if [ "$UI_TTY" != 1 ]; then
        printf 'HelixScreen installer %s (%s)\n' "$1" "$2" >&2
        return 0
    fi
    _pb_sub="$(printf '%b%s · %s channel%b' "$DIM" "$1" "$2" "$NC")"
    [ "$UI_UTF8" = 1 ] || _pb_sub="$(printf '%b%s (%s channel)%b' "$DIM" "$1" "$2" "$NC")"
    printf '\n' >&2
    if [ "$UI_UTF8" = 1 ] && [ "$UI_COLOR" != 0 ]; then
        _pb_i=0
        _logo_art "$UI_COLOR" | while IFS= read -r _pb_line; do
            _pb_i=$((_pb_i + 1))
            case $_pb_i in
                5) printf '  %s\033[0m   %s\n' "$_pb_line" "$(_logotype)" ;;
                6) printf '  %s\033[0m   %s\n' "$_pb_line" "$_pb_sub" ;;
                *) printf '  %s\033[0m\n' "$_pb_line" ;;
            esac
        done >&2
    else
        printf '  %s\n  %s\n' "$(_logotype)" "$_pb_sub" >&2
    fi
    printf '\n' >&2
}
SH
```

Re-run `bash scripts/render-installer-logo.sh`. Add `logo.sh` to the module list in `scripts/bundle-installer.sh` right after `common.sh`. The bundler's awk drops the first-lines `# ` comments of each module, which only removes the header; the heredocs pass untouched (confirm: `bash scripts/bundle-installer.sh -o /tmp/i.sh && grep -c HELIX_LOGO_EOF /tmp/i.sh` = 4).

- [ ] **Step 5: Run tests** → PASS. Then look at it: `HELIX_INSTALL_TTY=1 sh -c '. scripts/lib/installer/common.sh; . scripts/lib/installer/logo.sh; print_banner v1.1.0-beta.4 beta'`.

- [ ] **Step 6: Commit** (`print_banner` is `# UNCALLED_OK` until Task 9; put the marker in the generator's output):

```bash
git add -N scripts/render-installer-logo.sh scripts/lib/installer/logo.sh
git commit -m "feat(installer): braille logo and logotype banner" -- scripts/render-installer-logo.sh scripts/lib/installer/logo.sh scripts/bundle-installer.sh tests/shell/test_installer_output.bats
```

---

### Task 7: Read-only detection, the plan, the confirm prompt and flags

**Files:**
- Create: `scripts/lib/installer/plan.sh` (add to the bundle module list just before `main.sh`)
- Modify: `scripts/lib/installer/common.sh` (`tty_confirm`)
- Modify: `scripts/lib/installer/requirements.sh` (`detect_missing_runtime_deps`, `detect_missing_unzip`)
- Modify: `scripts/lib/installer/competing_uis.sh` (`detect_competing_uis`)
- Modify: `scripts/lib/installer/moonraker.sh`, `kiauh.sh` (`detect_moonraker_integration`, `detect_kiauh`)
- Modify: `scripts/lib/installer/main.sh` (`parse_installer_args`: `--dry-run`, `--verbose`; `usage`)
- Modify: `scripts/lib/installer/uninstall.sh:932` (`confirm_clean_install` onto `tty_confirm`)
- Create: `tests/shell/test_installer_plan.bats`

**Interfaces:**
- Consumes: output layer from Tasks 2-4, `print_banner`
- Produces:
  - `tty_confirm <question> <default y|n>`: returns 0 for yes. Reads `/dev/tty` when it can be opened, else stdin when `[ -t 0 ]`, else returns the default without asking. `ASSUME_YES=true` returns 0 without asking.
  - `DRY_RUN` (`true`/`false`), `HELIX_INSTALL_VERBOSE` set by `--verbose`
  - `MISSING_RUNTIME_DEPS` (space-separated package names, empty if none), `MISSING_UNZIP_PKG` (`unzip` when it must be installed, else empty)
  - `COMPETING_UIS_FOUND` (space-separated display names)
  - `MOONRAKER_ADDS` (space-separated: any of `update-manager`, `allowlist`), `KIAUH_DIR` (empty if none)
  - `plan_set <key> <value>`, `print_plan`, `plan_count_steps` (sets `STEP_TOTAL`)

- [ ] **Step 1: Write the failing tests** in `tests/shell/test_installer_plan.bats`:

```bash
#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The installer's read-only pass: the plan it builds, how it asks, and the
# --dry-run and --verbose flags.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LIB="$WORKTREE_ROOT/scripts/lib/installer"

setup() {
    load helpers
    export HELIX_INSTALL_TTY=0
    for m in common logo requirements competing_uis moonraker kiauh plan uninstall main; do
        . "$LIB/$m.sh"
    done
}

@test "--dry-run and --verbose are parsed" {
    parse_installer_args --dry-run --verbose
    [ "$DRY_RUN" = true ]
    [ "$HELIX_INSTALL_VERBOSE" = 1 ]
}

@test "DRY_RUN defaults to false" {
    parse_installer_args
    [ "$DRY_RUN" = false ]
}

@test "tty_confirm: ASSUME_YES answers yes without reading" {
    ASSUME_YES=true
    run tty_confirm "Continue?" n < /dev/null
    [ "$status" -eq 0 ]
}

@test "tty_confirm: no terminal and no /dev/tty returns the default" {
    HELIX_TTY_DEVICE=/nonexistent
    run tty_confirm "Continue?" y < /dev/null
    [ "$status" -eq 0 ]
    run tty_confirm "Continue?" n < /dev/null
    [ "$status" -eq 1 ]
}

@test "tty_confirm reads its answer from the tty device, not stdin" {
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    run tty_confirm "Continue?" y < /dev/null
    [ "$status" -eq 1 ]
}

@test "plan prints one aligned line per set key, in order" {
    plan_set Printer "Raspberry Pi 4 · Kalico · systemd"
    plan_set Install "v1.1.0-beta.4 (beta)"
    run print_plan
    [[ "${lines[0]}" == "  Printer    Raspberry Pi 4 · Kalico · systemd" ]]
    [[ "${lines[1]}" == "  Install    v1.1.0-beta.4 (beta)" ]]
}

@test "detect_missing_runtime_deps installs nothing" {
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '#!/bin/sh\necho "apt-get $*" >> "%s/apt.log"\n' "$BATS_TEST_TMPDIR" > "$BATS_TEST_TMPDIR/bin/apt-get"
    printf '#!/bin/sh\nexit 1\n' > "$BATS_TEST_TMPDIR/bin/dpkg-query"
    chmod +x "$BATS_TEST_TMPDIR/bin/"*
    PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    detect_missing_runtime_deps pi
    [ -n "$MISSING_RUNTIME_DEPS" ]
    [ ! -f "$BATS_TEST_TMPDIR/apt.log" ]
}

@test "detect_competing_uis lists an active KlipperScreen unit without stopping it" {
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    cat > "$BATS_TEST_TMPDIR/bin/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >> "$BATS_TEST_TMPDIR/sc.log"
case "\$*" in *is-active*KlipperScreen*) exit 0 ;; esac
exit 1
EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/systemctl"
    PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    detect_competing_uis
    [[ "$COMPETING_UIS_FOUND" == *KlipperScreen* ]]
    ! grep -E 'systemctl (stop|disable|mask)' "$BATS_TEST_TMPDIR/sc.log"
}

@test "detect_moonraker_integration reports what would be added" {
    conf="$BATS_TEST_TMPDIR/printer_data/config/moonraker.conf"
    mkdir -p "$(dirname "$conf")"
    printf '[server]\n' > "$conf"
    : > "$BATS_TEST_TMPDIR/printer_data/moonraker.asvc"
    MOONRAKER_CONF_OVERRIDE="$conf" detect_moonraker_integration
    [[ "$MOONRAKER_ADDS" == *update-manager* ]]
    [[ "$MOONRAKER_ADDS" == *allowlist* ]]
    [ "$(cat "$conf")" = "[server]" ]
}
```

Before writing `detect_moonraker_integration`, read `find_moonraker_conf` (moonraker.sh:151) and use whatever override it already honors instead of inventing `MOONRAKER_CONF_OVERRIDE`; change the test to match.

- [ ] **Step 2: Run to verify it fails.** `bats tests/shell/test_installer_plan.bats` → FAIL.

- [ ] **Step 3: Implement `tty_confirm`** in `common.sh`:

```sh
# Ask a yes/no question. Under `curl | sh` stdin is the script, so the answer
# comes from the controlling terminal; with neither, the default stands.
tty_confirm() { # question default(y|n)
    [ "${ASSUME_YES:-false}" = true ] && return 0
    _tc_dev="${HELIX_TTY_DEVICE:-/dev/tty}"
    _tc_hint="[y/N]"; [ "$2" = y ] && _tc_hint="[Y/n]"
    _tc_ans=""
    if { : < "$_tc_dev"; } 2>/dev/null; then
        printf '%s %s ' "$1" "$_tc_hint" >&2
        IFS= read -r _tc_ans < "$_tc_dev" || _tc_ans=""
    elif [ -t 0 ]; then
        printf '%s %s ' "$1" "$_tc_hint" >&2
        IFS= read -r _tc_ans || _tc_ans=""
    else
        _tc_ans="$2"
    fi
    _log_write "ASK $1 -> ${_tc_ans:-$2}"
    case "${_tc_ans:-$2}" in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}
```

Move `confirm_clean_install` (uninstall.sh:932) onto it, keeping its refusal when there is no way to ask: it must still refuse `--clean` without `--yes` when no terminal exists (`tty_confirm` would return the default `n`, so the existing `log_error ... exit 1` path stays; keep the existing test file for `--clean` green).

- [ ] **Step 4: Implement the detection functions.** Each is a read-only extraction of logic that already exists in its module; the acting function then consumes the variable instead of re-detecting:
  - `detect_missing_unzip`: the `command -v unzip` check from `check_requirements` (requirements.sh:~53), setting `MISSING_UNZIP_PKG=unzip` when unzip is absent and apt exists, else empty; `check_requirements` no longer installs, `install_missing_unzip` (new, called after the confirm point) installs `$MISSING_UNZIP_PKG` through `run_logged` with the same `_apt_update_once` + `apt-get install -y --no-install-recommends` line as today.
  - `detect_missing_runtime_deps <platform>`: requirements.sh:91-118 verbatim, setting `MISSING_RUNTIME_DEPS`; `install_runtime_deps` keeps lines 120-146 and reads `$MISSING_RUNTIME_DEPS`.
  - `detect_competing_uis`: walks the same `COMPETING_UIS` list with the existing read-only predicates (`_unit_is_competing`, the SysV `-x` globs, `pidof`) and the platform gates (`[ -f /opt/PROGRAM/ffstartup-arm ]`, mksclient path, `_host_ships_a_stock_ui`), appending display names to `COMPETING_UIS_FOUND`. It never calls `kill_process_by_name` or any `_take_down_unit`. `stop_competing_uis` stays as it is.
  - `detect_moonraker_integration`: `find_moonraker_conf`, then `has_update_manager_section` → not added, else `update-manager`; asvc file exists and lacks `^helixscreen$` → `allowlist`.
  - `detect_kiauh`: `KIAUH_DIR=$(detect_kiauh_dir)`; skipped when `skip_kiauh_registration=true`.

- [ ] **Step 5: Implement `plan.sh`:**

```sh
#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The plan the read-only pass builds, shown before anything changes.

_PLAN=""

plan_set() { # key value
    _PLAN="${_PLAN}$(printf '  %-10s %s' "$1" "$2")
"
    _log_write "PLAN $1: $2"
}

print_plan() {
    printf '%s' "$_PLAN" >&2
}

# Steps the run will show, so a no-terminal run can number them [n/N].
plan_count_steps() {
    STEP_TOTAL=6
    [ -n "${MISSING_RUNTIME_DEPS:-}${MISSING_UNZIP_PKG:-}" ] && STEP_TOTAL=$((STEP_TOTAL + 1))
    [ -n "${COMPETING_UIS_FOUND:-}" ] && STEP_TOTAL=$((STEP_TOTAL + 1))
    return 0
}
```

The base count of 6 is: Checked system, Downloaded, Installed files, Set up service, Connected to Moonraker, Started HelixScreen. Task 9 must keep the steps it opens in sync with this count; its golden transcripts catch a mismatch.

- [ ] **Step 6: Add the flags** to `parse_installer_args` (main.sh, beside `--yes`):

```sh
            --dry-run)
                DRY_RUN=true
                shift
                ;;
            --verbose|-v)
                HELIX_INSTALL_VERBOSE=1
                shift
                ;;
```

with `DRY_RUN=false` among the defaults at the top, and in `usage()`:

```sh
    echo "  --dry-run      Show what would be installed and changed, then exit."
    echo "                 Changes nothing; exits non-zero if a check would stop the install."
    echo "  --verbose, -v  Print every detail line and command output as it happens."
    echo "                 The full log is always written either way."
```

- [ ] **Step 7: Run tests.** `bats tests/shell/test_installer_plan.bats` → PASS; then the existing bats files for `check_requirements`, `install_runtime_deps`, `stop_competing_uis`, `configure_moonraker_updates`, `install_kiauh_extension`, `clean_old_installation`; then `make test-shell`.

- [ ] **Step 8: Mutation check.** Make `detect_competing_uis` call `_take_down_unit`; the "without stopping it" test must go red. Make `tty_confirm` read stdin first; the "/dev/tty, not stdin" test must go red. Restore both.

- [ ] **Step 9: Commit** (`plan_set`/`print_plan`/`plan_count_steps`/`detect_*` get `# UNCALLED_OK` until Task 9 unless their callers land here):

```bash
git add -N scripts/lib/installer/plan.sh tests/shell/test_installer_plan.bats
git commit -m "feat(installer): read-only detection, plan, tty_confirm, --dry-run and --verbose" -- scripts/lib/installer/plan.sh scripts/lib/installer/common.sh scripts/lib/installer/requirements.sh scripts/lib/installer/competing_uis.sh scripts/lib/installer/moonraker.sh scripts/lib/installer/kiauh.sh scripts/lib/installer/main.sh scripts/lib/installer/uninstall.sh scripts/bundle-installer.sh tests/shell/test_installer_plan.bats
```

---

### Task 8: The confirm point and its static check

**Files:**
- Modify: `scripts/lib/installer/main.sh` (`main()`)
- Modify: `scripts/lib/installer/platform.sh` (`set_install_paths` no longer calls `cleanup_ad5m_gcodes_root`; `detect_tmp_dir` probes without sudo)
- Modify: `scripts/lib/installer/host_profile.sh` / `main.sh` (`record_payload_root` after the confirm point)
- Modify: `scripts/lib/installer/requirements.sh` (`check_disk_space`'s `dd` probe uses `sudo -n` before the confirm point)
- Create: `tests/shell/test_installer_confirm_point.bats`

**Interfaces:**
- Consumes: everything from Task 7
- Produces: `confirm_point` (function in `plan.sh`): prints banner + plan; exits for `--dry-run`; asks via `tty_confirm` (fresh install, terminal); runs `sudo -v` once when `$SUDO` is set; opens the log file in `$TMP_DIR`. Returns only when the install should proceed.

- [ ] **Step 1: Write the failing static check** `tests/shell/test_installer_confirm_point.bats`:

```bash
#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything main() calls before confirm_point must only read: --dry-run exits
# there, and the plan screen promises nothing has changed yet. The e2e sandbox
# only simulates a Debian host, so this check covers the K1, AD5M and
# mod-payload branches it never runs.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
MAIN="$WORKTREE_ROOT/scripts/lib/installer/main.sh"

# Functions known to only read. Adding one here is a claim that it, and every
# function it calls, writes nothing and runs no sudo, apt or systemctl change.
READ_ONLY="host_profile_probe parse_installer_args mod_payload_autodetect
_refuse_uninstall_from_install_dir _refuse_if_firmware_managed detect_platform
get_download_platform print_platform_banner mod_check_chroot_context
detect_mod_flavor detect_k1_firmware set_install_paths mod_payload_mode_block
check_permissions check_requirements detect_missing_unzip
detect_missing_runtime_deps check_disk_space detect_init_system
check_klipper_ecosystem resolve_update_channel parse_tarball_version
get_latest_version match_channel_to_version detect_competing_uis
detect_moonraker_integration detect_kiauh probe_release usage uninstall
log_info log_warn log_error log_note log_success plan_set plan_count_steps
print_banner print_plan"

# Names called in main() between its opening line and confirm_point.
_pre_confirm_calls() {
    awk '/^main\(\) *\{/{on=1; next} on && /confirm_point/{exit} on' "$MAIN" \
        | sed 's/#.*//' \
        | grep -oE '(^|[;&|({ ]|\$\()[a-z_][a-z0-9_]*' \
        | sed -E 's/^[;&|({ ]|^\$\(//' \
        | sort -u
}

@test "main() has a confirm_point" {
    grep -q '^[[:space:]]*confirm_point' "$MAIN"
}

@test "every installer function main() calls before confirm_point is read-only" {
    lib="$WORKTREE_ROOT/scripts/lib/installer"
    defined=$(grep -hoE '^[a-z_][a-z0-9_]*\(\)' "$lib"/*.sh | tr -d '()' | sort -u)
    bad=""
    for fn in $(_pre_confirm_calls); do
        echo "$defined" | grep -qx "$fn" || continue
        case " $(echo $READ_ONLY) " in *" $fn "*) ;; *) bad="$bad $fn" ;; esac
    done
    [ -z "$bad" ] || { echo "not on the read-only list:$bad"; false; }
}

@test "nothing before confirm_point uses apt, systemctl changes or SUDO directly" {
    run sh -c "awk '/^main\\(\\) *\\{/{on=1; next} on && /confirm_point/{exit} on' '$MAIN' | grep -nE 'apt-get|systemctl (stop|start|enable|disable|restart|mask)|\\\$SUDO|mkdir|rm -'"
    [ "$status" -ne 0 ]
}
```

- [ ] **Step 2: Run to verify it fails.** `bats tests/shell/test_installer_confirm_point.bats` → FAIL (no `confirm_point`).

- [ ] **Step 3: Restructure `main()`.** Order after this task (only the pre-confirm half changes shape; everything after `confirm_point` is today's sequence with the moved writes added at its head):

```sh
    # ... banner/platform/paths as today, now all read-only ...
    check_permissions "$platform"
    # (uninstall branch unchanged)
    check_requirements            # no longer installs unzip
    detect_missing_unzip
    detect_missing_runtime_deps "$platform"
    check_disk_space "$platform"  # dd probe only via sudo -n here
    detect_init_system
    check_klipper_ecosystem "$platform"
    # channel + version resolution as today (resolve_update_channel, get_latest_version,
    # match_channel_to_version)
    probe_release "$version" "$download_platform"   # HEAD the archive URL, read manifest: no download
    detect_competing_uis
    detect_moonraker_integration
    detect_kiauh

    confirm_point "$platform" "$version"

    # Writes that used to happen during detection
    [ "$platform" = ad5m ] && cleanup_ad5m_gcodes_root
    record_payload_root_if_payload
    install_missing_unzip
    install_runtime_deps "$platform"
    # ... download_release / use_local_tarball and the rest as today ...
```

`probe_release` is new, in `release.sh`. It writes nothing; it sets `PROBE_SIZE_TEXT` for the plan and fails the way `download_release` would:

```sh
probe_release() { # version platform
    if [ -n "${local_tarball:-}" ]; then
        PROBE_SIZE_TEXT="$(du -h "$local_tarball" 2>/dev/null | cut -f1) · local file"
        return 0
    fi
    PROBE_SIZE_TEXT=""
    if _ensure_manifest "$1" && _manifest_covers_version "$1"; then
        PROBE_SIZE_TEXT="SHA256 available"
    else
        PROBE_SIZE_TEXT="no SHA256 published"
    fi
    # The archive must exist: the same candidate URLs download_release tries,
    # checked with a HEAD request. Read download_release for the candidate
    # list and the fetch helper that can issue a HEAD on this host
    # (curl -sfI, wget --spider, or python urllib); reuse it, do not add one.
    _probe_release_url_exists "$1" "$2" || {
        log_error "No HelixScreen $1 release for $2."
        exit 1
    }
}
```

`_probe_release_url_exists` is the one new helper; it shares the candidate-URL construction with `download_release` (extract that construction into a function both call, rather than copying it).

`confirm_point` in `plan.sh`:

```sh
confirm_point() { # platform version
    plan_set Printer "$(plan_printer_line "$1")"
    [ -n "${EXISTING_INSTALL_LINE:-}" ] && plan_set Found "$EXISTING_INSTALL_LINE"
    plan_set Install "$2 (${R2_CHANNEL:-stable})${PROBE_SIZE_TEXT:+ · $PROBE_SIZE_TEXT}"
    [ -n "${MISSING_RUNTIME_DEPS:-}${MISSING_UNZIP_PKG:-}" ] && plan_set Libraries "${MISSING_UNZIP_PKG:-}${MISSING_UNZIP_PKG:+ }${MISSING_RUNTIME_DEPS:-} (apt)"
    [ -n "${COMPETING_UIS_FOUND:-}" ] && plan_set Disable "$COMPETING_UIS_FOUND"
    _cp_add="$(plan_adds_line)"
    [ -n "$_cp_add" ] && plan_set Add "$_cp_add"
    [ -n "${SUDO:-}" ] && plan_set sudo "needed for: service, libraries, udev and polkit rules"

    print_banner "$2" "${R2_CHANNEL:-stable}"
    print_plan
    printf '\n' >&2

    if [ "${DRY_RUN:-false}" = true ]; then
        printf '%s\n' "Dry run, nothing changed." >&2
        exit 0
    fi

    if [ "${update_mode:-false}" != true ] && [ "$UI_TTY" = 1 ]; then
        tty_confirm "Continue?" y || { printf '%s\n' "Nothing changed." >&2; exit 0; }
    fi

    if [ -n "${SUDO:-}" ] && ! sudo -n true 2>/dev/null; then
        printf '%s\n' "sudo is needed to install the service and system files." >&2
        sudo -v || { log_error "sudo was not granted; nothing changed."; exit 1; }
    fi

    plan_count_steps
    mkdir -p "$TMP_DIR" 2>/dev/null || $SUDO mkdir -p "$TMP_DIR"
    log_open "$TMP_DIR/install.log" || true
}
```

`plan_printer_line` and `plan_adds_line` live in `plan.sh`. Read `print_platform_banner` (main.sh:483) for the variables that already hold the hardware model and firmware name, and substitute them for `HARDWARE_MODEL` / `FIRMWARE_NAME` below:

```sh
_plan_sep() { if [ "$UI_UTF8" = 1 ]; then printf ' · '; else printf ', '; fi; }

plan_printer_line() { # platform
    _ppl="${HARDWARE_MODEL:-$1}"
    [ -n "${FIRMWARE_NAME:-}" ] && _ppl="$_ppl$(_plan_sep)$FIRMWARE_NAME"
    [ -n "${INIT_SYSTEM:-}" ] && _ppl="$_ppl$(_plan_sep)$INIT_SYSTEM"
    [ -n "${KLIPPER_USER:-}" ] && _ppl="$_ppl$(_plan_sep)user $KLIPPER_USER"
    printf '%s' "$_ppl"
}

plan_adds_line() {
    _pal=""
    for _pa in ${MOONRAKER_ADDS:-}; do
        case "$_pa" in
            update-manager) _pa_text="Moonraker update manager" ;;
            allowlist) _pa_text="service allowlist" ;;
            *) continue ;;
        esac
        _pal="${_pal:+$_pal$(_plan_sep)}$_pa_text"
    done
    [ -n "${KIAUH_DIR:-}" ] && _pal="${_pal:+$_pal$(_plan_sep)}KIAUH extension"
    printf '%s' "$_pal"
}
``` `EXISTING_INSTALL_LINE` is set where `set_install_paths` logs "Install directory (existing install)" today.

For `--update`, `plan_set Install` reads `"$INSTALLED_VERSION → $2"` when the installed version is known (`moonraker.sh:565` already reads it with `--version`).

Dry-run exit codes: the checks before `confirm_point` already `exit` non-zero when they fail (disk, arch, no release), so `--dry-run` inherits their codes with no extra code.

- [ ] **Step 4: Move the pre-confirm writes** listed in the corrected spec (Task 1, Step 3), one at a time, running that function's existing bats file after each move.

- [ ] **Step 5: Run tests.** `bats tests/shell/test_installer_confirm_point.bats` → PASS; `make test-shell` → 0 failures.

- [ ] **Step 6: Mutation check.** Move `install_runtime_deps` back above `confirm_point`; both static tests must go red. Restore.

- [ ] **Step 7: Commit**

```bash
git add -N tests/shell/test_installer_confirm_point.bats
git commit -m "feat(installer): confirm point between read-only detection and the install" -- scripts/lib/installer/main.sh scripts/lib/installer/plan.sh scripts/lib/installer/platform.sh scripts/lib/installer/host_profile.sh scripts/lib/installer/requirements.sh scripts/lib/installer/release.sh tests/shell/test_installer_confirm_point.bats
```

---

### Task 9: Steps in `main()`, the summary, interrupts, log placement

**Files:**
- Modify: `scripts/lib/installer/main.sh` (steps around phases; summary; trap)
- Modify: `scripts/lib/installer/common.sh` (`print_post_install_commands` → `print_summary`; `finalize_install_log`)
- Modify: call sites of the hint lines that move into the summary: "You can now edit HelixScreen config from Mainsail/Fluidd", "Restart KIAUH ... Extensions menu", "You can now update HelixScreen from the Mainsail/Fluidd web interface!"
- Test: `tests/shell/test_installer_output.bats`

**Interfaces:**
- Produces: `finalize_install_log` (moves `$INSTALL_LOG` to `<printer_data>/logs/helixscreen-install.log`, else `$INSTALL_DIR/logs/helixscreen-install.log`, rotating an existing one to `.1`), `print_summary`.

- [ ] **Step 1: Write the failing tests:**

```bash
@test "finalize_install_log moves the log into printer_data/logs and keeps one old run" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home"; mkdir -p "$KLIPPER_HOME/printer_data/logs"
    echo old > "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log"
    log_open "$BATS_TEST_TMPDIR/install.log"; log_warn "new run"
    finalize_install_log
    grep -q "new run" "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log"
    [ "$(cat "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log.1")" = old ]
}

@test "finalize_install_log falls back to INSTALL_DIR/logs without printer_data" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/nohome"; INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    log_open "$BATS_TEST_TMPDIR/install.log"; log_warn "bare host"
    finalize_install_log
    grep -q "bare host" "$INSTALL_DIR/logs/helixscreen-install.log"
}

@test "an interrupt during a step resolves it as interrupted" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; STEP_TOTAL=2
        trap "step_fail interrupted; exit 130" INT
        step Downloading; kill -INT $$; sleep 1' _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    [[ "$output" == *"Downloading ... FAILED (interrupted)"* ]]
}
```

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement.**
  - Wrap each phase of `main()` after `confirm_point` with the six base steps plus the two optional ones, in this order and with these exact titles (the golden transcripts in Task 10 pin them): `Checked system` (opened before detection starts, done at `confirm_point`, detail `"$HARDWARE_MODEL · $FIRMWARE_NAME · $INIT_SYSTEM"`), `Installed libraries` (only when something is missing; detail = package list), `Downloaded` (detail `"<size> · SHA256 verified"` or `"<size>"`), `Stopped <names>` (only when `COMPETING_UIS_FOUND`), `Installed files` (detail = `$INSTALL_DIR` with `$HOME` shown as `~`), `Set up service`, `Connected to Moonraker` (detail from what was added), `Started HelixScreen`.
  - When a function inside a step `exit`s non-zero, the existing EXIT trap runs: extend the traps at main.sh:17-34 to call `step_fail` before `cleanup_on_success`, and `step_fail interrupted` for HUP/INT/TERM. Then `finalize_install_log`, then print `Full log: <path>`.
  - The closing state line ("Nothing on your printer was changed after this step." / what was rolled back): read the existing rollback paths in `error_handler` (common.sh:276) and print the line that matches what they did, set by a variable those paths already touch (`BACKUP_CONFIG`, `INSTALL_BACKUP`). Before the atomic swap no rollback variable is set, so "Nothing on your printer was changed" is exact there.
  - Replace the "Installation Complete!" banner and `print_post_install_commands` with `print_summary` (spec §1 "Summary"), and delete the three scattered hint lines (their content is in the summary). Keep `print_k2_stock_ai_notice` and the reboot note as `log_note` lines after the summary.
  - Promote to `log_note` any `log_success` whose content the summary does not cover and a user needs (the polkit/udev "Installed" lines do not qualify; review each `log_success` in `main()`'s direct callees and list the promoted ones in the commit body).
  - Remove the `# UNCALLED_OK` markers added in Tasks 3, 4, 6, 7.

- [ ] **Step 4: Run tests.** `bats tests/shell/test_installer_output.bats`, then `make test-shell`.

- [ ] **Step 5: Mutation check.** Remove `step_fail` from the EXIT trap; the e2e failed-apt transcript in Task 10 must go red (do this check in Task 10, once that transcript exists).

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(installer): steps, summary and an install log that survives failures" -- scripts/lib/installer/main.sh scripts/lib/installer/common.sh scripts/lib/installer/plan.sh scripts/lib/installer/moonraker.sh scripts/lib/installer/kiauh.sh tests/shell/test_installer_output.bats
```

---

### Task 10: End-to-end tests: dry-run, prompt, transcripts

**Files:**
- Modify: `tests/shell/fixtures/install_e2e_scenario.sh` (steps `dry-run`, `install-tty-yes`, `install-tty-no`, `fail-apt`; normalize transcript)
- Modify: `tests/shell/test_install_e2e.bats`
- Create: `tests/shell/fixtures/install_transcripts/fresh.txt`, `update.txt`, `dry-run.txt`

**Interfaces:**
- Consumes: the whole installer.

- [ ] **Step 1: Add scenario steps** to the `case` in `install_e2e_scenario.sh` (and the header list):

```sh
        dry-run)
            run_installer --dry-run || rc=$?
            ;;
        install-tty-yes)
            # A pseudo-terminal for stderr and /dev/tty; stdin stays the script's.
            script -qec "printf 'y\n' | HELIX_TTY_DEVICE=/dev/stdin busybox ash /mnt/install.sh" /dev/null || rc=$?
            ;;
        install-tty-no)
            script -qec "printf 'n\n' | HELIX_TTY_DEVICE=/dev/stdin busybox ash /mnt/install.sh" /dev/null || rc=$?
            ;;
```

Check how `run_installer` invokes the bundle (env, cwd) and give the `script` lines the same environment; use `sh` when `busybox` is absent, as `run_installer` does. Skip the two tty steps (exit 0, print `SKIP no script`) when `script` is not installed.

- [ ] **Step 2: Write the failing tests** in `test_install_e2e.bats`:

```bash
@test "dry-run changes nothing and makes no changing systemctl call" {
    run_scenario dry-run
    [ "$status" -eq 0 ]
    [[ "$output" == *"Dry run, nothing changed."* ]]
    # The snapshot taken after the step equals the seeded tree.
    run diff -r "$(snap_seed)" "$(snap 1-dry-run)"
    [ "$status" -eq 0 ]
    ! grep -E 'systemctl (stop|start|enable|disable|restart|mask|daemon-reload)' "$(snap 1-dry-run)/var/log/e2e-systemctl.log" 2>/dev/null
}

@test "dry-run exits non-zero when the release is missing" {
    E2E_RELEASE_VERSION=v9.9.9 run_scenario dry-run
    [[ "$output" == *"=== STEP 1: dry-run exit=1"* || "$output" =~ "STEP 1: dry-run exit=[1-9]" ]]
}

@test "answering n at the prompt changes nothing" {
    command -v script >/dev/null || skip "no script(1)"
    run_scenario install-tty-no
    [[ "$output" == *"Nothing changed."* ]]
    run diff -r "$(snap_seed)" "$(snap 1-install-tty-no)"
    [ "$status" -eq 0 ]
}

@test "answering y at the prompt installs" {
    command -v script >/dev/null || skip "no script(1)"
    run_scenario install-tty-yes
    [ -f "$(snap 1-install-tty-yes)$UNIT" ]
}

@test "no-terminal install output matches the golden transcript" {
    run_scenario install
    diff -u "$BATS_TEST_DIRNAME/fixtures/install_transcripts/fresh.txt" <(normalize_transcript "$output")
}

@test "no-terminal update output matches the golden transcript" {
    run_scenario install update
    diff -u "$BATS_TEST_DIRNAME/fixtures/install_transcripts/update.txt" <(normalize_transcript "$(step_output 2 "$output")")
}
```

Add helpers to the bats file: `snap_seed` (a snapshot of the seeded tree; add a `seed-snapshot` pseudo-step at scenario start if none exists), `normalize_transcript` (strip `[HH:MM:SS]`, sizes `[0-9.]+ ?[KMG]B` → `<size>`, the sandbox's temp paths → `<tmp>`), `step_output N` (the lines between `=== STEP N` markers). Read the existing `snap` helper and reuse its conventions.

- [ ] **Step 3: Generate the goldens once** by running the scenario and writing `normalize_transcript` output to the three files; read them line by line against spec §1 (one line per step, summary block, no `[INFO]`, no escape codes: `grep -c "$(printf '\033')"` = 0) before committing them.

- [ ] **Step 4: Run.** `bats tests/shell/test_install_e2e.bats` → PASS. `make test-shell` → 0 failures.

- [ ] **Step 5: Mutation checks.**
  - Move `install_runtime_deps` above `confirm_point`, rebuild the bundle: the dry-run test must go red (the confirm-point static test from Task 8 also goes red).
  - Remove `step_fail` from the EXIT trap (Task 9 Step 5): add a `fail-apt` step whose apt stub exits 100, assert its output contains `FAILED` and `Full log:`; it must go red without the trap change.

- [ ] **Step 6: Commit**

```bash
git add -N tests/shell/fixtures/install_transcripts/fresh.txt tests/shell/fixtures/install_transcripts/update.txt tests/shell/fixtures/install_transcripts/dry-run.txt
git commit -m "test(installer): dry-run immutability, the confirm prompt and golden transcripts" -- tests/shell/fixtures/install_e2e_scenario.sh tests/shell/test_install_e2e.bats tests/shell/fixtures/install_transcripts
```

---

### Task 11: Docs, device dry-runs, and retiring the plan

**Files:**
- Modify: `docs/devel/INSTALLER.md` ("## Installation Flow" :113-135 is stale: rewrite it from the new `main()`, split at the confirm point; add "## Output and logging" covering `step*`, `log_info` vs `log_note`, `run_logged`, `HELIX_INSTALL_VERBOSE`, `HELIX_INSTALL_TTY`, the log file location, and "### Test Helpers" mentioning `HELIX_INSTALL_VERBOSE=1` in `helpers.bash`)
- Modify: `docs/user/INSTALL.md` (Step 2 "Run the Installer": `--dry-run`, `--verbose`, where the log is; "Getting Help/Check Logs First" :602: the install log path)
- Modify: `scripts/CLAUDE.md` (index entry for `render-installer-logo.sh`)
- Delete: `docs/devel/plans/2026-10-06-installer-ux-design.md`, `docs/devel/plans/2026-10-06-installer-ux.md`

- [ ] **Step 1: Write the docs.** Every flag and path in them must match `usage()` and `finalize_install_log` exactly.

- [ ] **Step 2: Device dry-runs (read-only).**
  - Voron V0 (`pbrown@192.168.1.133`, Pi 4, systemd, Kalico, ~4.8" DSI): copy the bundle and run `sh install.sh --dry-run --update` and `sh install.sh --dry-run`. Expect exit 0, the plan, "Dry run, nothing changed." Confirm nothing changed: `systemctl is-active helixscreen` still active and `ls -la ~/helixscreen` mtimes unchanged.
  - One BusyBox printer (K1C at 192.168.30.182 or AD5M at 192.168.1.67): **ask Preston first**, then run `sh install.sh --dry-run` from `/tmp`. Expect the ASCII fallbacks if its locale is not UTF-8.
  - Put the two plan screens in the PR/commit body.

- [ ] **Step 3: Full gate.** `make full-test-run` → passes.

- [ ] **Step 4: Commit** (deleting the spec and plan in the same change that ships the work, per `docs/CLAUDE.md`):

```bash
git rm -q docs/devel/plans/2026-10-06-installer-ux-design.md docs/devel/plans/2026-10-06-installer-ux.md
git commit -m "docs(installer): document step output, --dry-run and the install log" -- docs/devel/INSTALLER.md docs/user/INSTALL.md scripts/CLAUDE.md docs/devel/plans/2026-10-06-installer-ux-design.md docs/devel/plans/2026-10-06-installer-ux.md
```

- [ ] **Step 5: After merge:** a real `--update` on the V0 (Preston's printer, idle): `curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update` once the release carries this; until then, the bundle from the merged tree with `--local` and a release zip.
