# GOD-6: splitting theme_manager.cpp along its statics

Audit finding GOD-6 (`2026-09-30-architecture-audit.html#GOD-6`). `src/ui/theme_manager.cpp` is
3,482 lines doing ~15 jobs over file statics. All main-thread LVGL work. Delete this file when
the tranche ships.

## Seams

Each new file owns its own statics; one struct crosses files.

| File | Jobs | Owns | ~Lines |
|---|---|---|---|
| `theme_token_scan.cpp` | expat token discovery, constant-set validator | ESP32 read cache | 370 |
| `theme_color_math.cpp` | hex parse, brightness/saturation, WCAG contrast | nothing (pure) | 190 |
| `overlay_geometry.cpp` | breakpoint nav width, `compute_overlay_*` | nothing | 170 |
| `theme_fonts.cpp` | responsive font registration, `get_font`, icon fonts | nothing | 300 |
| `theme_responsive.cpp` | ui_xml dir, tiers, responsive px tokens, spacing | nothing | 420 |
| `theme_lvgl_apply.cpp` | palette build, extra styles, `helix_theme_apply` | `helix_theme`, styles, backup | 560 |
| `theme_live_recolor.cpp` | recolor tree walk, swap maps | swap maps (`set_swap_maps`) | 580 |
| `theme_tokens_register.cpp` | XML const registration, swatch subjects | swatch subjects | 430 |
| `theme_manager.cpp` | Android bg, core state, lifecycle and broadcast | the rest | 650 |

- `src/ui/theme_manager_internal.h`: `helix::theme_detail::ThemeRuntime` (display, lvgl theme,
  dark, active theme, repeat-guard fields) behind a function-local static `runtime()`, plus the
  cross-file functions. `include/theme_manager.h` stays the only public header.
- `ThemeSubjects` with one `deinit()` replaces the four hand-written deinit blocks.
- Not in scope: folding `ThemeRuntime.dark` into `ThemeManager::dark_mode_` (behaviour change).

## Constraints

- `theme_manager_init`'s body stays in place so the `register_*` order is unchanged; XML consts
  are first-wins.
- Runtime switching (`theme_manager_apply_theme`: theme editor, appearance, toggle, preview) only
  gains the swap-map handoff across files. Hot reload never calls theme_manager.
- ESP32 compiles theme_manager.cpp (`app_srcs.txt`); every new file joins `app_srcs.txt` in the
  same commit.
- `apply_theme` clears `theme_fully_initialized` on purpose; commit 4 keeps that write.

## Commits (each behaviour-preserving)

0. Pinning tests: golden const snapshot at 480x272, 800x480, 1280x720 in dark and light;
   `theme_changed` generation bumps on apply and toggle; an inline `card_bg` recolors on switch;
   swatch/breakpoint/portrait subjects exist after init and are gone after deinit.
1. Token scan out; widen the hex-colour gate's `HEX_ALLOW` to `src/ui/theme_`.
2. Color math. 3. Overlay geometry. 4. Statics become `ThemeRuntime`/`ThemeSubjects`.
5. Fonts. 6. Responsive tokens. 7. LVGL apply. 8. Live recolor. 9. Token registration.
10. Dedup: one `build_palette_pair()`, one subject-publish helper (check
    `scoped_subject_registry` first); `make mutate-diff`.

After each move, grep that every static is defined in exactly one file. Fix the comments that
name theme_manager.cpp functions (`scripts/gen_theme_tokens.py`, `mk/tools.mk`,
`scripts/check_hardcoded_pixels.py`, the responsive-token bats, `mk/fonts.mk`).

## Visual check

`scripts/screenshot.sh` before and after on home, controls, motion, settings, appearance, theme,
print-status, lock-screen and preflight-check, dark and light, 800x480 and 480x272: a pure move
gives byte-identical PNGs (`cmp`). Then a live theme switch and dark toggle, recapturing home and
settings. Boot the ESP32 once after commit 4 and once after commit 9.
