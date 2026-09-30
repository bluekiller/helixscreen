# Config Migration System

How the versioned config migration system works, how to add new migrations, and how migrations are tested.

**Key files**: `include/config.h`, `src/system/config.cpp`, `tests/unit/test_config.cpp`

---

## Overview

HelixScreen stores user configuration in a JSON file (`config/settings.json`). As the application evolves, the config schema changes: keys get renamed, defaults change, new sections appear. The migration system handles upgrading existing configs automatically so users never need to hand-edit JSON.

There are two migration layers:

1. **Structural migrations** (legacy) -- Key-path moves like `display_rotate` to `/display/rotate` or `/display/calibration` to `/input/calibration`. These run unconditionally based on key presence.

2. **Versioned migrations** -- Numbered `v3->v4`, `v9->v10`, etc. Each bumps the integer `config_version` field. These run in sequence and only on configs older than the target version.

---

## How It Works

### The `config_version` Field

Every config file has an integer `config_version` at the root level:

```json
{
    "config_version": 19,
    "printer": { ... },
    "display": { ... }
}
```

The current version is defined in `config.h`:

```cpp
static constexpr int CURRENT_CONFIG_VERSION = 26;
static constexpr int MIN_MIGRATABLE_CONFIG_VERSION = 9;
```

The full migration ladder lives in `config.cpp`.

Two properties of the ladder worth knowing before you add to it:

- **Every step must be idempotent under replay.** A rollback to an older build stamps
  `config_version` back down, and the next upgrade re-runs the chain over a document
  already in the new shape. Each migration guards against re-doing its own work.
- **A config written by a newer build is left alone.** `run_versioned_migrations()`
  detects a `config_version` above `CURRENT_CONFIG_VERSION` and returns without
  stamping, so an older binary never rewrites data it does not understand.

The current head of the ladder:

- **v23 -> v24** tags every saved home layout with `layout_units: "cells_v21"` and a
  `legacy_rows` floor harvested from the old `/ui/cached_grid` node, then lifts legacy
  flat widget arrays into the page shape. The tag is all this migration writes: it runs
  at config load, before the screen size is known, so the actual coordinate conversion
  happens later, at the first square-cell grid build, in
  `port_legacy_layout()` (`include/layout_port.h` + `src/ui/layout_port.cpp`), which
  drops the tag when done. Three guards keep a replay from converting twice: a panel
  that already carries `grid`/`parked_grids` (per-grid storage postdates the
  migration), one already tagged, and one with no coordinates at all.
- **v24 -> v25** re-keys the pre-print prediction history's per-phase timing from
  phase ordinals to phase names (`"4"` -> `"QGL"`), against a frozen name table,
  because inserting a phase renumbers every ordinal after it.
- **v25 -> v26** converts `/completion_alert` from a JSON boolean to the
  Off/Notification/Alert int `AudioSettingsManager` has always read and written.
  A pre-migration fresh install wrote the boolean `true`, and `Config::get<int>()`
  converts that via nlohmann's bool-to-arithmetic rule (`true` -> 1, `false` -> 0)
  rather than the intended `CompletionAlertMode::ALERT` (2) — so every install
  that never touched Print Completion Alert silently fell back to Notification.
  `true` -> Alert, `false` -> Off; an explicit stored int is a real user choice
  and is left untouched.

All three have dedicated tests: `tests/unit/test_config_migration_v24.cpp`,
`tests/unit/test_config_migration_v25.cpp`, and
`tests/unit/test_config_migration_v26.cpp`.

### Fresh Install vs. Upgrade

| Scenario | What happens |
|----------|-------------|
| **No config file** | `get_default_config()` creates one with `config_version = CURRENT_CONFIG_VERSION`. No migrations run. |
| **Existing config, no `config_version`** | Treated as version 0: a shipped preset (`assets/config/presets/*.json`) or a tarball default. A rolling backup with a real version replaces it if one exists; otherwise it runs v3->v4 (single `/printer` to the `/printers` map) and then the chain from v9. |
| **Existing config, `config_version` 1-8** | Below the floor. Copied to `settings.json.pre-migration`, one warning logged, and replaced by `get_default_config()`, the same defaults a missing config gets. |
| **Existing config, `config_version = 9`** | Only migrations after v9 run (v9->v10, ...). |
| **Existing config, `config_version = CURRENT`** | No migrations run. |

### Execution Order in `Config::init()`

```
1. Load config JSON from disk (or create default if missing)
2. Run structural migrations:
   a. migrate_display_config()  -- root-level display_* keys -> /display/
   b. migrate_config_keys()     -- /display/calibration -> /input/calibration
3. Run versioned migrations:
   a. Read config_version (default 0 if absent)
      If 0 < version < CURRENT and the document came from disk, copy
      settings.json to settings.json.pre-migration first
   b. If 0 < version < MIN_MIGRATABLE_CONFIG_VERSION: replace with defaults, stop
   c. if (version < 4) migrate_v3_to_v4()   -- only version 0 reaches this
   d. if (version < 10) migrate_v9_to_v10() ... through the head
   e. Set config_version = CURRENT_CONFIG_VERSION
4. Ensure required sections exist with defaults (printer, display, input, etc.)
5. Save to disk if anything changed
```

The `.pre-migration` copy is the only record of the pre-upgrade document: the save in step 5 also refreshes the rolling backup (`src/system/config_backup.cpp#write_rolling_backup`) with the migrated one. It holds one generation, overwritten by the next migrating boot from a different version. A copy already at the starting version is kept, since a migration that threw can leave settings.json partly migrated under its old stamp. No restore path reads it; recovering from it is a manual copy.

Versioned migrations only run on **existing** configs. A fresh install skips straight to step 4 because `get_default_config()` already sets `config_version = CURRENT_CONFIG_VERSION`.

---

## The Migration Floor

`MIN_MIGRATABLE_CONFIG_VERSION` (9, first shipped in v0.99.4) is the oldest stamp the chain still migrates. A config stamped 1-8 is not migrated: `init()` keeps it as `settings.json.pre-migration`, logs `config_version N is older than this build migrates`, and starts from defaults. Raising the floor means deleting the steps below it, except any a version-0 preset still needs.

Version 0 is not below the floor. The shipped presets carry no `config_version` and use the single `/printer` shape, so `migrate_v3_to_v4()` stays for them.

---

## Adding a New Migration

### Step 1: Bump the version constant

In `include/config.h`:

```cpp
static constexpr int CURRENT_CONFIG_VERSION = 3;  // was 2
```

### Step 2: Write the migration function

In `src/system/config.cpp`, add a new static function in the anonymous namespace alongside the existing migrations:

```cpp
/// Migration v2->v3: <description of what and why>
static void migrate_v2_to_v3(json& config) {
    // Your migration logic here.
    // The config JSON is passed by reference -- modify it in place.
    // Use spdlog::info() to log what changed.
}
```

### Step 3: Register it in `run_versioned_migrations()`

Add one line to the chain:

```cpp
static void run_versioned_migrations(json& config) {
    int version = 0;
    if (config.contains("config_version")) {
        version = config["config_version"].get<int>();
    }

    if (version < 1) migrate_v0_to_v1(config);
    if (version < 2) migrate_v1_to_v2(config);
    if (version < 3) migrate_v2_to_v3(config);  // <-- ADD THIS

    config["config_version"] = CURRENT_CONFIG_VERSION;
}
```

### Step 4: Update `get_default_config()` if needed

If your migration changes a default value or adds a new key, make sure `get_default_config()` reflects the final state. Fresh installs use this function directly and skip migrations entirely.

### Step 5: Write tests

See the testing section below.

---

## Migration Rules

1. **Migrations are append-only above the floor.** Never modify an existing migration function; old configs in the wild may still need it. Steps below `MIN_MIGRATABLE_CONFIG_VERSION` are removed when the floor rises.

2. **Migrations must be idempotent.** Check if the target state already exists before making changes. Use `config.contains()` guards.

3. **Never overwrite user data.** If a target key already exists, skip the migration for that key. The user's explicit value wins.

4. **Keep migrations simple.** Each migration should do one thing. If you need to both rename a key and change its type, that is still one logical migration.

5. **Log what you do.** Use `spdlog::info("[Config] Migration vN: <what happened>")` so upgrade issues are diagnosable.

6. **Fresh installs skip everything.** `get_default_config()` returns the current schema directly. Migrations only run on existing configs loaded from disk.

---

## Testing Migrations

Migration tests are in `tests/unit/test_config.cpp` under the `[core][config][migration][versioning]` tags.

### Pattern: Write config to temp file, run init(), verify results

The standard pattern creates a temp config file with pre-migration data, runs `Config::init()` on it, and verifies the post-migration state:

`tests/unit/test_config.cpp` "a config at the migration floor is migrated" is the shape: write a stamped document to a temp file, wrap `init()` in a `BackupGuard` so no real rolling backup is found, then assert on the loaded result.

### What to test for each migration

| Test case | What it verifies |
|-----------|-----------------|
| Oldest stamp triggers migration | A config at the version before yours gets migrated |
| Already-migrated config is left alone | Config at version N does not re-run migration N |
| Fresh config skips migrations | New install gets `CURRENT_CONFIG_VERSION` without running migration logic |
| Edge case: key absent | Migration handles configs that never had the key being migrated |
| Version stamp is set | After all migrations, `config_version == CURRENT_CONFIG_VERSION` |

### Running migration tests

```bash
# Build and run all migration tests
make test-run

# Run only versioning/migration tests
./build/bin/helix-tests "[migration][versioning]"

# Run all config tests
./build/bin/helix-tests "[config]"
```

---

## Structural Migrations (Legacy)

Before the versioned system existed, two key-path migration helpers were used:

### `migrate_display_config()`

Moves root-level `display_rotate`, `display_sleep_sec`, `display_dim_sec`, `display_dim_brightness`, `touch_calibrated`, and `touch_calibration` into the `/display/` section. Triggered by the presence of `display_rotate` at the root level.

### `migrate_config_keys()`

A generic helper that takes a vector of `{from_path, to_path}` pairs and moves values between JSON pointer paths. Used to move `/display/calibration` to `/input/calibration` and `/display/touch_device` to `/input/touch_device`.

These still run on every `init()` call for backward compatibility with very old configs. New migrations should use the versioned system instead.

---

## Debugging

Run HelixScreen with `-vv` (DEBUG) to see migration log output:

```
[Config] Loading config from config/settings.json
[Config] Migration v4: restructured /printer to /printers/default
```

If a config file is corrupt (unparseable JSON), `init()` backs it up as `settings.json.corrupt` and creates a fresh default config.

---

## Config File Rename

The config file was renamed from helixconfig.json to `settings.json`:
- The installer renames it, in the install dir and in printer_data
- `Config::init()` follows a helixconfig.json symlink into printer_data when no `settings.json` exists
- Rolling backups fall back to old names if new-named backups don't exist
- Template renamed from `helixconfig.json.template` to `settings.json.template`
