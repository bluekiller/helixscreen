# GOD-10: splitting DisplayManager's sleep, dim and screensaver

Audit finding GOD-10 (`2026-09-30-architecture-audit.html#GOD-10`). `src/application/display_manager.cpp`
is 2,580 lines doing seven jobs; sleep/dim/screensaver is ~795 of them. Everything here runs on
the main thread from Application's loop. Delete this file when the tranche ships.

## Seams

- `decide_idle(const IdleInputs&) -> IdleDecision`: the pure tick lifted out of
  `check_display_sleep`. States Awake/Dimmed/Sleeping; actions None/Wake/Dim/StartSaver/Sleep.
  Must keep: a wake while sleeping is honoured even when entry is inhibited; dimmed can still
  sleep; awake checks sleep before dim; dim only when `can_dim || has_screensaver`; preview
  ignores activity for 750ms; activity means <500ms inactive.
- `wake_should_auto_lock(...)` and `user_brightness()` (`clamp(settings, 10, 100)`, written four
  times today).
- `DisplaySleepController`: owns sleep/dim/saver/preview/lifecycle state, overlay, flush
  suppression, sleep callbacks; the only code that touches the backlight and display backend.
  DisplayManager keeps its 14 public methods as forwarders, so the 34 callers and the ESP32
  stubs are unchanged.
- `RotationProbe` out (~350 lines, imperative labels become XML); its unit test replaces
  `scripts/check_rotation_cache_order.py` and its bats.
- Stays: init/shutdown, backend selection, DRM to fbdev fallback, input, resize fan-out.

## Device behaviour that must survive

- AD5M: FBIOBLANK keeps the flush live; the 20s brightness override after ForgeX's reset; no
  brightness floor on /dev/disp.
- Sysfs (Pi, K2, AD5X): binary backlights are all or nothing; brightness 0 also sets bl_power;
  K2's floor and `/display/backlight_floor_percent`.
- K1 fbdev: FB_BLANK_POWERDOWN only with no hardware blank and no usable backlight; the
  `/display/hardware_blank` and `/display/panel_power_off` overrides.
- Pi DRM: flush stays suppressed while the panel is off, or a page flip relights HDMI; a failed
  power_off falls back to the overlay.
- Wake order everywhere: unblank/power on, `lv_refr_now`, `trigger_activity`.
- Android: HostSleep clears keep-screen-on; the resume counter self-wakes; never HostSleep with
  a 0 sleep timeout.

## Commits (each behaviour-preserving, pinning test first)

1. `decide_idle` with today's branching copied verbatim; ~40-row truth table.
2. `check_display_sleep` applies `decide_idle`'s output; existing sleep/wake suites stay green.
3. `user_brightness()` and `wake_should_auto_lock()` with table tests.
4. ScreenHideHold and RefreshPeriodHold to their own .cpp files (pure move).
5. `DisplaySleepController` plus forwarders, together with the DRM to fbdev fallback retargeting
   its backend pointer (a stale pointer after the swap is a use-after-free).
6. RotationProbe extraction; delete the rotation-cache gate and its bats.
7. Optional: ColorTransformHook.

DisplayManager ends near 1,100 lines.

## Risks

- `restore_display_output()` decides from the config flags, not the last sleep mechanism; keep
  that exactly.
- `m_lifecycle_suspended` shares NavigationManager's suspend latch with Application's
  background/foreground pair; a wake while backgrounded must not steal it.

## Hardware checks after commit 5

Idle into dim, screensaver, sleep, then a wake tap (absorbed, first frame not black) on: AD5M
(FBIOBLANK, the 20s override), Pi DRM/HDMI (stays dark through a Klipper invalidation) and a Pi
DSI sysfs backlight, K1 fbdev (with `panel_power_off=1` too), and one device with
`sleep_while_printing` off.
