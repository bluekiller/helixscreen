#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PreToolUse hook on Bash (wired in .claude/settings.json). Suggests where a
# heavy command should run; it never blocks and never decides permission. The
# answer is a context block the model reads before the command runs, or nothing.
#
# thelio is shared by several sessions at once; zeus has twice the RAM and is
# usually idle. Mutation, sanitizers and symbolizers belong on zeus however quiet
# thelio looks, and so does a sweep: `make unit-sweep` maps to zeus-run.sh's
# sweep mode and `make full-test-run` to full, each falling back to the other. An
# explicit -j above the fair share is always worth a word. Container builds and
# test loops are only worth moving when thelio is tight. "Tight" is not decided
# here: `helix-claim jobs -v` folds in claimed builds, live build trees and
# MemAvailable, so this reads its share and availGB and applies one threshold to
# each.
#
# A command is judged segment by segment (split on && || ; | and newlines), by
# each segment's own first word, with quoted text masked. So a commit message,
# a grep pattern or a heredoc that merely mentions `make full-test-run` is
# silent.
#
# Runs on EVERY Bash call: a command matching no heavy word returns before
# touching jq, /proc or helix-claim. Never ssh from here.
#
# Env:
#   HELIX_ADVISOR_HOSTS       hosts the advice is about (default: thelio); elsewhere it is silent
#   HELIX_ADVISOR_JOBS_CMD    command printing the `jobs -v` line (default: helix-claim jobs)
#   HELIX_ADVISOR_MIN_SHARE   share at or below which thelio is tight (default 8)
#   HELIX_ADVISOR_MIN_GB      availGB below which thelio is tight (default 16)
#   HELIX_ADVISOR_ZEUS_RUN    zeus-run.sh whose modes are offered (default: beside this script)

input=$(cat)

# Fast path: a plain substring test on the raw JSON, no parsing.
case "$input" in
    *addr2line*|*gdb*|*llvm-symbolizer*|*full-test-run*|*unit-sweep*|*mutate*|*asan*|*SANITIZE*|*docker*|*-j*|*helix-tests*|*bats*) ;;
    *) exit 0 ;;
esac

# The advice names thelio and zeus; a cloud session or a Mac has neither.
host=$(hostname -s 2>/dev/null)
case " ${HELIX_ADVISOR_HOSTS:-thelio} " in
    *" $host "*) ;;
    *) exit 0 ;;
esac

cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty' 2>/dev/null) || exit 0
[ -n "$cmd" ] || exit 0
# bash regex matching goes quadratic on long input; a heavy command names itself early.
cmd=${cmd:0:8192}

case "$cmd" in
    *zeus-run.sh*|*"ssh zeus"*) exit 0 ;;
esac

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
zeus_run=${HELIX_ADVISOR_ZEUS_RUN:-"$here/zeus-run.sh"}
push_then="zeus runs only pushed SHAs: push the branch (never main), then"
see_load="\`scripts/helix-claim resources\` shows what thelio and zeus are running now."

emit() {
    jq -cn --arg c "[resource-advisor] $1" \
        '{hookSpecificOutput: {hookEventName: "PreToolUse", additionalContext: $c}}'
    exit 0
}

# The zeus-run mode for a sweep, preferring the one that matches the make
# target ($1: sweep or full) and falling back to whichever this checkout has.
zeus_gate_mode() {
    local m other=sweep
    [ "$1" = sweep ] && other=full
    for m in "$1" "$other"; do
        grep -qE "^[[:space:]]*${m}\)" "$zeus_run" 2>/dev/null && { echo "$m"; return; }
    done
}

# Quoted text becomes Q, so `-j"$(helix-claim jobs)"` reads as -jQ (a computed
# -j) and a quoted mention of a target reads as nothing.
masked=$(printf '%s' "$cmd" | sed -E "s/'[^']*'/Q/g; s/\"[^\"]*\"/Q/g")
segments=$(printf '%s\n' "$masked" | sed -E 's/(&&|\|\||[;|])/\n/g')

# Patterns live in variables: `;|&(` inside a literal [[ =~ ]] break the parse.
re_word_sep='[[:space:]]'
re_sweep_target='(^|[[:space:]])(full-test-run|unit-sweep)([[:space:]]|$)'
re_asan='(^|[[:space:]])(test-asan|SANITIZE=address)([[:space:]]|$)'
re_remote='(-docker|deploy-|remote-)'
re_container_target='(^|[[:space:]])([A-Za-z0-9_.-]+-docker|docker-[A-Za-z0-9_.-]+)([[:space:]]|$)'
re_jobs='(^|[[:space:]])(-j[[:space:]]*|--jobs[=[:space:]]*)([0-9]+|\$\(nproc\)|Q|\$[A-Za-z(]|)([[:space:]]|$)'

want_symbolizer="" want_gdb="" want_mutate="" want_sweep="" want_asan=""
want_container="" want_loop="" jobs_asked="" in_loop=""

while IFS= read -r seg; do
    # Leading keywords, env assignments and wrappers do not name the program.
    seg=${seg#"${seg%%[![:space:]]*}"}
    while :; do
        case "$seg" in
            do\ *|then\ *|else\ *|\{\ *|\(*) seg=${seg#*[ (]} ;;
            [A-Za-z_]*=*\ *)
                if [[ "${seg%% *}" == *=* ]]; then seg=${seg#* }; else break; fi ;;
            nice\ -n\ *) seg=${seg#nice -n } ; seg=${seg#* } ;;
            nice\ *|time\ *|command\ *|exec\ *|sudo\ *) seg=${seg#* } ;;
            timeout\ *) seg=${seg#timeout }; seg=${seg#* } ;;
            *) break ;;
        esac
        seg=${seg#"${seg%%[![:space:]]*}"}
    done
    word=${seg%%[[:space:]]*}
    base=${word##*/}
    case "$base" in
        for|while|until) in_loop=1 ;;
        make)
            if [[ "$seg" =~ $re_sweep_target ]]; then
                [ "${BASH_REMATCH[2]}" = unit-sweep ] && want_sweep=sweep || want_sweep=full
            fi
            if [[ "$seg" =~ mutate-diff ]]; then want_mutate=1; fi
            if [[ "$seg" =~ $re_asan ]] && ! [[ "$seg" =~ $re_remote ]]; then want_asan=1; fi
            if [[ "$seg" =~ $re_container_target ]]; then want_container=1; fi
            if [[ "$seg" =~ $re_jobs ]]; then
                # '$(nproc)' below is the literal text a caller typed, not an expansion.
                # shellcheck disable=SC2016
                case "${BASH_REMATCH[3]}" in
                    '$(nproc)') jobs_asked=$(nproc 2>/dev/null || echo 32) ;;
                    # A bare -j takes the Makefile's own bound; a computed one is the caller's.
                    ''|Q|\$*) ;;
                    *) jobs_asked=${BASH_REMATCH[3]} ;;
                esac
            fi
            ;;
        mutate_diff.py) want_mutate=1 ;;
        gdb)
            [[ "$seg" == *helix-tests* || "$seg" == *helix-screen* ]] && want_gdb=1 ;;
        addr2line|eu-addr2line|llvm-symbolizer|llvm-addr2line)
            [[ "$seg" == *helix-* ]] && want_symbolizer=1 ;;
        docker)
            [[ "$seg" =~ ${re_word_sep}run${re_word_sep} && "$seg" == *idf* ]] && want_container=1 ;;
        helix-tests|bats)
            [ -n "$in_loop" ] && want_loop=1 ;;
        xargs|parallel)
            [[ "$seg" == *helix-tests* || "$seg" == *bats* ]] && want_loop=1 ;;
    esac
done <<< "$segments"

# --- Always on zeus --------------------------------------------------------

if [ -n "$want_gdb" ]; then
    emit "gdb against a helix binary takes ~35 min and tens of GB on thelio. \`coredumpctl info <pid>\` symbolizes a core in seconds; anything more goes to zeus (${push_then} build there and run gdb -batch)."
fi
if [ -n "$want_symbolizer" ]; then
    emit "addr2line loads the whole DWARF of helix-tests per call (21GB+ RSS on thelio) and ignores SIGTERM. Symbolize on zeus: ${push_then} build in the helix-tsan container and use llvm-symbolizer or one \`gdb -batch -ex 'info symbol 0x..'\` there. For a function name alone, \`nm -C --defined-only\` plus a sorted lookup is seconds."
fi
if [ -n "$want_mutate" ]; then
    emit "Mutation rebuilds and reruns the suite per hunk. Run it on zeus: ${push_then} \`scripts/zeus-run.sh mutate --tests '[tag]'\`."
fi
if [ -n "$want_asan" ]; then
    emit "ASAN produces no output on thelio (ld.so.preload loads its runtime second) and exits 0. Run it on zeus: ${push_then} \`scripts/zeus-run.sh asan '[tag]'\`."
fi
if [ -n "$want_sweep" ]; then
    mode=$(zeus_gate_mode "$want_sweep")
    if [ -n "$mode" ]; then
        emit "A full sweep starts dozens of shards at once. Run it on zeus: ${push_then} \`scripts/zeus-run.sh ${mode}\`. Keep it local only when you need the verdict on uncommitted work in this exact tree. ${see_load}"
    fi
fi

# --- Needs the box ---------------------------------------------------------

# Reading the box costs a pgrep sweep; skip it when nothing below could fire.
[ -n "$jobs_asked$want_container$want_loop" ] || exit 0

# jobs sweeps /proc; a hang here would stall the command until the hook times out.
if [ -n "${HELIX_ADVISOR_JOBS_CMD:-}" ]; then
    line=$(timeout 2 "$HELIX_ADVISOR_JOBS_CMD" -v 2>&1 >/dev/null) || exit 0
else
    line=$(timeout 2 "$here/helix-claim" jobs -v 2>&1 >/dev/null) || exit 0
fi
share=$(printf '%s' "$line" | sed -n 's/.*-> -j\([0-9][0-9]*\).*/\1/p')
avail=$(printf '%s' "$line" | sed -n 's/.*availGB=\([0-9][0-9]*\).*/\1/p')
[ -n "$share" ] && [ -n "$avail" ] || exit 0

if [ -n "$jobs_asked" ] && [ "$jobs_asked" -gt "$share" ]; then
    emit "-j${jobs_asked} is above the fair share, -j${share} (${avail}GB available; \`scripts/helix-claim jobs -v\` shows who else is building). Use \`-j\$(scripts/helix-claim jobs)\`."
fi

min_share=${HELIX_ADVISOR_MIN_SHARE:-8}
min_gb=${HELIX_ADVISOR_MIN_GB:-16}
if [ "$avail" -lt "$min_gb" ]; then
    tight="thelio has ${avail}GB available"
elif [ "$share" -le "$min_share" ]; then
    tight="thelio's fair share is -j${share}"
else
    exit 0
fi

if [ -n "$want_container" ]; then
    emit "Container builds escape thelio's -j and nice, and ${tight}. Run it on zeus (it has the Docker images and twice the RAM), or wait for peers' builds to finish. ${see_load}"
fi
if [ -n "$want_loop" ]; then
    emit "A loop over the test binary multiplies its load, and ${tight}. Cut the count, or run the loop on zeus: ${push_then} use the helix-tsan container. ${see_load}"
fi

exit 0
