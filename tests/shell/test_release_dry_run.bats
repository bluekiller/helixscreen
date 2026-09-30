#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# release.yml dry-run mode: a workflow_dispatch with dry_run=true must build and
# upload everything while publishing nothing. The guard lint below reads the
# workflow; the verifier tests run scripts/verify-dry-run-release.sh against a
# fixture built with the real manifest generator.

load helpers

YML="${HELIX_TEST_RELEASE_YML:-.github/workflows/release.yml}"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# ------------------------------------------------------------- guard lint

@test "RELEASE_MODE is publish only for a tag push or a non-dry-run dispatch on a tag" {
    run python3 - "$YML" <<'PY'
import re, sys, yaml
with open(sys.argv[1]) as fh:
    expr = yaml.safe_load(fh)["env"]["RELEASE_MODE"]
inner = re.fullmatch(r"\$\{\{(.*)\}\}", expr.strip()).group(1)
# Translate the GitHub expression into Python, whose and/or return operands
# the same way. An unknown term is a NameError, so a rewrite fails here
# instead of passing unexamined.
py = (inner.replace("startsWith(github.ref, 'refs/tags/v')", "is_tag")
           .replace("github.event_name == 'workflow_dispatch'", "is_dispatch")
           .replace("inputs.dry_run", "dry_run")
           .replace("&&", " and ").replace("||", " or ").replace("!", " not "))
cases = [
    # is_tag, is_dispatch, dry_run, expected
    (True,  False, None,  "publish"),
    (True,  True,  True,  "dryrun"),
    (True,  True,  False, "publish"),
    (False, True,  True,  "dryrun"),
    (False, True,  False, "dryrun"),
]
bad = []
for is_tag, is_dispatch, dry_run, want in cases:
    got = eval(py, {}, dict(is_tag=is_tag, is_dispatch=is_dispatch, dry_run=dry_run))
    if got != want:
        bad.append(f"tag={is_tag} dispatch={is_dispatch} dry_run={dry_run}: {got!r}, want {want!r}")
print("\n".join(bad))
sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

# Every step that publishes must be gated on publish, or (R2 only) write
# through the dry-run prefix. Prints one line per offender.
lint_publish_guards() {
    python3 - "$1" <<'PY'
import re, sys, yaml
with open(sys.argv[1]) as fh:
    jobs = yaml.safe_load(fh)["jobs"]

PUBLISH_ACTIONS = ("softprops/action-gh-release", "r0adkll/upload-google-play",
                   "peter-evans/repository-dispatch")
STEP_GATE = "env.RELEASE_MODE == 'publish'"
JOB_GATE = "needs.plan.outputs.mode == 'publish'"
PREFIX_ASSERT = '[[ "$R2_PREFIX" == dry-run/?*/ ]]'
PREFIX_ENV = "${{ needs.plan.outputs.r2_prefix }}"

bad = []
for jname, job in jobs.items():
    job_gated = JOB_GATE in str(job.get("if", ""))
    for step in job.get("steps", []):
        where = f"{jname} / {step.get('name', '?')}"
        uses = step.get("uses", "")
        run = step.get("run", "")
        env = step.get("env") or {}
        gated = job_gated or STEP_GATE in str(step.get("if", ""))
        if (uses.startswith(PUBLISH_ACTIONS) or "gh release" in run
                or "DISCORD_WEBHOOK_URL" in env) and not gated:
            bad.append(f"{where}: publishes without a publish gate")
        if ("s3://" in run or "aws s3" in run) and not gated:
            unprefixed = [m for m in re.findall(r"s3://[^\s\"']*", run)
                          if not m.startswith("s3://${R2_BUCKET}/${R2_PREFIX}")]
            if unprefixed:
                bad.append(f"{where}: R2 path outside R2_PREFIX: {unprefixed}")
            elif PREFIX_ASSERT not in run:
                bad.append(f"{where}: writes R2 without asserting a dry run's R2_PREFIX")
            elif (job.get("env") or {}).get("R2_PREFIX") != PREFIX_ENV:
                bad.append(f"{where}: job R2_PREFIX is not {PREFIX_ENV}")
print("\n".join(bad))
sys.exit(1 if bad else 0)
PY
}

@test "every publishing step is gated on publish or writes through the dry-run prefix" {
    run lint_publish_guards "$YML"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "the guard lint fires on an ungated release and an unprefixed R2 write" {
    local broken="$BATS_TEST_TMPDIR/release.yml"
    sed -e "/name: Create GitHub Release/{n;/RELEASE_MODE == 'publish'/d}" \
        -e 's|s3://${R2_BUCKET}/${R2_PREFIX}install.sh|s3://${R2_BUCKET}/install.sh|' \
        "$YML" > "$broken"
    run lint_publish_guards "$broken"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Create GitHub Release: publishes without a publish gate"* ]] || fail "$output"
    [[ "$output" == *"s3://\${R2_BUCKET}/install.sh"* ]] || fail "$output"
}

# ------------------------------------------------------------- verifier

VERSION=0.0.0-dryrun.42
BASE=https://cdn.example.invalid/dry-run/42/

# A consistent artifact + R2 prefix for pi and x86 on channels beta and dev.
make_fixture() {
    ART="$BATS_TEST_TMPDIR/art" R2="$BATS_TEST_TMPDIR/r2"
    REL="$R2/releases/v$VERSION"
    mkdir -p "$ART" "$REL" "$R2/beta" "$R2/dev"
    local p
    for p in pi x86; do
        echo "$p tarball" > "$ART/helixscreen-$p-v1.1.0-beta.3.tar.gz"
        echo "$p zip" > "$ART/helixscreen-$p.zip"
    done
    echo '#!/bin/sh' > "$ART/install.sh"
    cp "$ART"/*.tar.gz "$ART"/*.zip "$REL/"
    cp "$ART/install.sh" "$R2/install.sh"
    scripts/generate-manifest.sh --version "$VERSION" --tag "v$VERSION" --notes n \
        --dir "$REL" --base-url "${BASE}releases/v$VERSION" \
        --zip-exclude "" --output "$R2/beta/manifest.json" >/dev/null
    cp "$R2/beta/manifest.json" "$R2/dev/manifest.json"
}

verify() {
    run scripts/verify-dry-run-release.sh --artifact "$ART" --r2 "$R2" --base-url "$BASE" \
        --version "$VERSION" --platforms "pi x86" --channels "beta dev"
}

@test "verifier passes a consistent dry run" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    verify
    [ "$status" -eq 0 ] || fail "$output"
    [[ "$output" == *"beta -> releases/v$VERSION/helixscreen-x86.zip"* ]] || fail "$output"
}

@test "verifier fails when install.sh differs" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    echo changed >> "$R2/install.sh"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"install.sh differs"* ]] || fail "$output"
}

@test "verifier fails when a manifest points outside the dry-run prefix" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    sed -i 's|/dry-run/42/releases|/releases|' "$R2/dev/manifest.json"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"dev manifest points outside the dry-run prefix"* ]] || fail "$output"
}

@test "verifier fails when an uploaded file does not match its manifest sha256" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    echo corrupt >> "$REL/helixscreen-pi.zip"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"sha256 for ${BASE}releases/v$VERSION/helixscreen-pi.zip"* ]] || fail "$output"
}

@test "verifier fails when a platform's zip never reached R2" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    rm "$REL/helixscreen-x86.zip"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"x86 zip in $REL missing"* ]] || fail "$output"
}
