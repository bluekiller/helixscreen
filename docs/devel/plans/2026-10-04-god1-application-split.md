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

## Tranche 2: PrinterSession (designed 2026-10-04, awaiting approval)

Tranche 1 shipped at `709ec4e11` (application.cpp 5,289 -> 3,855).

- `PrinterSession` owns everything a printer switch destroys and rebuilds: Moonraker, history
  managers, job queue state, subjects, panels, app layout, overlay panels, the plugin trio, the
  G-code response routing, the prompter, the fingerprint fields, the soft-restart flag and
  `m_wizard_previous_printer_id`. Application keeps process state (display, config, args,
  screen, splash, hot reload, lock, background state) and owns the `AsyncLifetimeGuard` the
  session borrows.
- One session for the process lifetime, torn down and rebuilt in place: queued discovery
  lambdas and nav callbacks hold raw pointers to it. It holds `m_screen` as `lv_obj_t*&`
  (`init_moonraker` reassigns it).
- Boot connects before `init_ui`; a switch connects after. Both orders stay.
- Teardown body moves verbatim; the display restore becomes an `exit_display` parameter at the
  same position, and the exit tail (`HttpExecutor::stop_all`, display reset, theme deinit) stays
  in `Application::shutdown()`.
- `src/application/gcode_response_routing.{h,cpp}` takes `init_action_prompt`'s seven objects;
  detach is two calls kept at today's two teardown positions.

Commits: (1) bats gate pinning the teardown call order, proven red by swapping two lines;
(2) pure `parse_layer_line()` plus one shared response-line walker; (3) the routing bundle;
(4) the switch state machine with Restart hooks, success path tested for the first time;
(5) discovery-session state; (6) members and teardown body verbatim, gate repointed with the
golden list unchanged; (7) phase methods, discovery callbacks, plugin trio; mock switch smoke and
zeus ASAN. Gates and doc anchors naming application.cpp move in the same commit as the code.
application.cpp ends near 2,450 lines.
