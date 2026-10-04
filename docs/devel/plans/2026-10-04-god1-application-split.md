# GOD-1: splitting Application's discovery reactions

Audit finding GOD-1 (`2026-09-30-architecture-audit.html#GOD-1`). `src/application/application.cpp`
is 5,289 lines; `setup_discovery_callbacks` is one 778-line lambda running ~30 reactions in a
fixed order on the main thread. Delete this file when the tranche ships.

## Seams

- `src/application/discovery_steps.{h,cpp}`: `DiscoveryContext` (api, client, `api->hardware()`
  only, status snapshot, `hw_changed`, `print_active` computed once, screen) and a `constexpr`
  table of `DiscoveryStep { name, only_when_hw_changed, run }`. Order is the point, so no
  self-registering registry. Application keeps the snapshot copy, the queue hop, the shutdown
  guard, the fingerprint and the splash call, then walks the table.
- Attach/detach pairs so registration and unregister live in one file, and
  `teardown_printer_scope` calls `detach()`: `TimelapseState`, `UpdateChecker::on_connected`
  (sentinel cleanup, `machine.update.refresh`, `notify_update_response`), About print hours,
  Spoolman active-spool sync (`spoolman_active_spool_sync.cpp`).
- `HardwareSetupPrompter`: owns the four prompt flags and the pending setup steps, takes the
  reapply/deferred/type-mismatch/reidentify methods and steps 17-22. Pure
  `choose_prompt(...)` returns None / Reconfig / DeferredOffer / DeferredSilent / TypeMismatch.
- `heal_heater_roles` next to `resolve_role_from_config`; `zoffset::maybe_enable_persistence`;
  `apply_safety_limits_from_printer`.
- Pure moves: process guards, demo overlays, SDL shortcuts.

## Constraints

- Both discovery callbacks fire on the WebSocket thread; all work stays inside one
  `queue_update` (FIFO keeps `init_subsystems_from_hardware` before `on_discovery_complete`).
- Hard order: hardware copy before `set_hardware` move; `set_hardware` before temp-graph seed and
  status dispatch; auto-detect, heater heal, wizard acknowledge, validate, prompts, snapshot in
  that sequence; z-offset and reconfig read `print_active` from the snapshot; manual-probe open
  stays one tick deferred; splash stays early.
- Steps re-run on every reconnect, so every attach is idempotent. Check whether
  `register_method_callback` replaces or appends on a repeated handler name before commit 3.
- Keep the existing `disc` breadcrumb keys.

## Commits (each behaviour-preserving, pinning test first)

1. Fake client captures both discovery callbacks; attach-to-detach assertion helper.
2. Pure moves (-610).
3. Attach/detach pairs; switch test asserts every attached handler is unregistered (-70).
4. Spoolman sync out, with bypass/lane/toolchanger/no-op-fetch tests (-190).
5. `heal_heater_roles` (-20).
6. `HardwareSetupPrompter`, `choose_prompt` truth table first (-370).
7. `DiscoveryContext` and the step table; order pinned; `only_when_hw_changed` honoured
   (-550 / +200).
8. Later: `PrinterSession` (switch, teardown, `init_printer_state`) and `init_action_prompt`.

Net about -1,800 from application.cpp, +1,000 elsewhere, ending near 3,400 lines.

The printer-switch discovery reset (same-shape printers skipping the `hw_changed` work, prompt
flags surviving a switch) ships first, separately, on `fix/switch-discovery-reset`.

## Verify

Mock (`--test -vv --sim-speed 6`, pinned socket): step order and breadcrumbs unchanged; klippy
restart takes the `hw_changed=false` path; switch to a same-shape printer; Spoolman bypass and
lane changes; manual-probe auto-open. Hardware: AD5M external-update restart and fresh-install
preset detect; a toolchanger's spool auto-assign; Spoolman bypass vs lane; Klipper-down first
boot then healthy boot; reconnect mid-print shows no prompt and sends no z-offset gcode.
