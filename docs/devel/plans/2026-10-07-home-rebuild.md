# HANDOFF: Home rebuild speed-ups (perf/home-rebuild)

Parked 2026-10-07. Delete this file in the change that merges the branch.

## Done

1. **print_status builds its active views only during a print.** The printing block in
   `ui_xml/components/panel_widget_print_status.xml` sits in an `<if cond="print_status_view ge 3">`
   inside `print_card_printing_slot`. `PrintStatusWidget::bind_active_branch()` re-finds the
   active refs and rebinds the arc, pause markers and heater icons whenever the branch is
   rebuilt (`update_view_subject`). Tests: `[lazy_branch]` (print start/end while shown,
   library to detailed mid-print); existing print_status tests now start a real print
   (`set_wire_state`) before reading active widgets.
2. **A layout change that only moves widgets re-seats the built tiles.**
   `PanelWidgetManager::reseat_tiles` (relayout_tiles with every tile counted as moved) and
   `HomePanel::reseat_widgets`, tried by the config-rebuild callback before `populate_widgets()`.
   It requires the same page count, same visible ids, same widget generation and the same
   per-widget config (`CarouselPage::built_configs`) on every page; otherwise full rebuild.
   Tests: `[reseat]`.

## Measurements (native, thelio)

- print_status component build: 1000 us before, 450-476 us idle after; ~1087 us when it
  builds during a print (unchanged cost, paid once at print start).
- Home config change with the same widgets moved (default Home, 39 entries, one page):
  re-seat 0.27 ms vs full populate 3.3 ms.
- Not measured: a real mock multi-printer switch end to end, and anything on the K-Touch
  (f7 owns that).

## Left

- **Full gate not green yet.** `make full-test-run` failed one case,
  `test_job_queue_up_next.cpp` "home print status widget shows the up next line while
  queued": `up_next_row` is in the lazy active branch. The test now sets the view to 3
  before building the card and passes `[up_next]`; the sweep and the bats half have not
  been rerun since.
- The detailed idle view (view 2) is still built eagerly; only the active branch is lazy.
- Report the SHA and numbers to team-lead; f7 measures switch time on the K-Touch.

## Next steps

1. `make full-test-run` on the branch tip; fix anything else that reached into the active
   branch while idle.
2. Push, hand the SHA to team-lead for batching.
