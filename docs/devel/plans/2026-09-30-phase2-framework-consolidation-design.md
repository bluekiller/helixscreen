# Phase 2: UI framework consolidation - design

Status: draft for maintainer review, 2026-09-30, against main @ 18229e5d8.
Scope: audit findings FW-1..FW-8, FW-12 (step 1), FW-13, FW-14, plus the framework items the
audit left under 04d (D3, D11, D12). Sources: `docs/superpowers/architecture-audit/00-SUMMARY.md`,
`04-ui-core.md` (04a-04d), `05-ui-tail.md`, `06-part-core.md`, `08-crosscutting.md` (F8, F16, F22)
and the FW-* entries on `docs/devel/plans/2026-09-30-architecture-audit.html`.
Lua plugins phase 2 (`feature/lua-plugins-phase2` @ 2b0321c42) lands on main first; every
branch here starts after it and converts the plugin classes in the same pass as their
first-party twins (inventory in section 8). Nothing here was built. Counts marked "now" were re-measured on main today by grep; the
observer byte figure was re-measured with `nm -C -S` on the current native `build/bin/helix-screen`.

The one idea behind all seven workstreams: every overlay, widget and settings manager today
re-types its lifecycle, so each new one copies the last and the copies drift. Each workstream
below gives that lifecycle one home, migrates the copies onto it, and leaves a gate so the
copy-paste path stops compiling or stops passing QC.

## 0. Shared decisions

**Visit each overlay file once.** WS1 (lifecycle), WS2 (trampolines), WS3 (lookups), FW-4
(init guards) and FW-8 (setting rows) all edit the same ~70 overlay `.cpp`/`.h` pairs. Three
passes over the same files means three rounds of conflicts with every peer branch and three
reviews of the same hunks. The plan is: land the helpers first in one small additive branch
(no mass migration), then sweep overlay files in batches, where one commit converts a batch of
files completely (all five changes). Each batch commit builds and passes `make t F='[overlay]'`
plus the tags of the files it touched.

**Helpers are additive before they are mandatory.** Every new API lands next to the old one.
The old one is deleted in the commit that migrates its last caller, and that commit adds the
ratchet (a baseline file that only shrinks, same shape as `scripts/check_orphan_subjects.py`).

**No behaviour change is the default; the exceptions are listed per workstream.** Where a
migration does change behaviour (portrait back chevron in WS1, dropped callbacks in WS5,
test-time asserts in WS3), the section says so and the commit body says so.

**Proof per commit, not per branch.** `make t F=` on the touched tags per commit; `make
full-test-run` once per branch before merge; `scripts/zeus-run.sh asan` once for WS4 and WS5
(the lifetime-heavy ones); `make mutate-diff` once per branch on the new helpers only (the
migrations are deletions and have nothing to mutate).

---

## 1. WS1 OverlayBase lifecycle (FW-1, FW-4, FW-12 step 1, B7)

### Problem, now

- 70 classes derive from `OverlayBase`; 33 declare their own `void show(lv_obj_t*)`; 82 sites
  carry the "create() called but overlay already exists" / "Failed to create overlay from XML"
  strings; only 13 call `OverlayBase::create_overlay_from_xml`.
- 80 `static std::unique_ptr<X> g_x` accessors in `src/ui`, 95 `register_destroy` calls, and
  two headers that both `#define DEFINE_GLOBAL_PANEL` with different arity
  (`include/ui_panel_singleton_macros.h`, `include/ui_global_panel_helper.h`).
- `include/ui/ui_lazy_panel_helper.h#lazy_create_and_push_overlay` already contains the right
  `show()` body, including `destroy_on_close`, but it keeps the root in a CALLER-owned
  `lv_obj_t*&`, so 8 overlays are cached twice (04c B7) and the helper needs a stale-cache
  branch plus a per-instantiation `static WidgetRef created_root` to reconcile them. It has 23
  callers; the other overlays hand-roll `show()`.
- Hand-rolled `create()`s skip `ui_overlay_panel_setup_standard`, so those overlays miss the
  back-button pressed style and the portrait `chevron_up` swap
  (`src/ui/ui_panel_common.cpp#ui_overlay_panel_setup_standard`). That is a latent visual bug
  the migration fixes; see Risks.

### Proposed API

```cpp
// include/overlay_base.h
class OverlayBase : public ViewLifecycleBase {
  public:
    // Lazy init + create + register + push. Non-virtual: the sequence is the contract.
    // Returns false (and toasts) when the root could not be created.
    bool show(lv_obj_t* parent_screen);

    // Default: nothing to init. Subclasses with subjects override; show() calls it once.
    virtual void init_subjects() {}

    // Default: create_overlay_from_xml(parent, xml_component()). Overlays that pass attrs
    // to lv_xml_create or build rows keep an override.
    virtual lv_obj_t* create(lv_obj_t* parent);

    // XML component name for the default create(). nullptr = subclass overrides create().
    virtual const char* xml_component() const { return nullptr; }

    // Free the widget tree when the overlay is popped (#1329, #1246). Default keeps today's
    // behaviour: built once, retained for the session.
    virtual bool destroy_on_close() const { return false; }

  protected:
    // Per-show work that needs the root (populate, seed state). Runs after create, before push.
    virtual void before_show() {}
    ...
};

// include/static_panel_registry.h
// One lazily-constructed instance per T, destroyed by StaticPanelRegistry::destroy_all().
template <typename T, typename... Args> T& lazy_global(const char* name, Args&&... args) {
    static std::unique_ptr<T> instance;
    if (!instance) {
        instance = std::make_unique<T>(std::forward<Args>(args)...);
        StaticPanelRegistry::instance().register_destroy(name, [] { instance.reset(); });
    }
    return *instance;
}
```

`OverlayBase::show()` body (in `src/ui/overlay_base.cpp`), which is the lazy helper's body
moved onto the object:

```cpp
bool OverlayBase::show(lv_obj_t* parent_screen) {
    parent_screen_ = parent_screen;
    if (!subjects_initialized_) {
        init_subjects();
        subjects_initialized_ = true;
    }
    if (!overlay_root_) {
        register_callbacks(); // every create: XML callback slots are last-write-wins
        if (!create(parent_screen_)) {
            spdlog::error("[{}] Failed to create overlay", get_name());
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to open"), 2000);
            return false;
        }
        if (destroy_on_close()) {
            NavigationManager::instance().register_overlay_close_callback(
                overlay_root_, [this, tok = object_lifetime_.token()] {
                    if (!tok.expired())
                        destroy_overlay_ui();
                });
        }
    }
    before_show();
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
    return true;
}
```

`destroy_overlay_ui(lv_obj_t*& cached_panel)` loses its parameter once no caller holds a cache
(it nulls `overlay_root_` only). `init_subjects_guarded` and the 87 internal `if
(subjects_initialized_) return;` guards go: `show()` is the one guard, and the 12 overlays that
`SubjectInitializer` inits at boot set the flag through the same path.

**Owner-held overlays keep an owner-driven push.** `show()` is for overlays that own their
root for the session (the lazily-constructed singletons). An overlay whose OBJECT is owned by
someone else and dies with its screen, `PluginSettingsOverlay` (one per plugin, created by
`PluginHost::open_settings` and pushed through `PluginOverlayHost::push`), keeps calling
`create()` and its owner's push, because NavigationManager holds ONE close callback per root
and the owner's callback is what deletes the object. `show()` therefore asserts that no close
callback is already registered for the root it pushes. The owner-driven path is the shape of
the later `push_overlay(root, lifecycle, on_closed)` (04c C3): `PluginOverlayHost::push` is
that function already, minus the name, and item 9 in section 8 promotes it.

`NavigationManager::close_overlay(root)` (Lua phase 2: queued, decided in queue order, pops
if on top, drops silently if buried, no-op if already gone) is kept as is. OverlayBase gets a
one-line `void close() { NavigationManager::instance().close_overlay(overlay_root_); }` so a
migrated overlay closing itself (today `go_back()` guarded by a visibility check) takes those
semantics instead of assuming it is on top.

### Before / after

`src/ui/ui_settings_connection.cpp` (whole file, 76 + 45 header lines, no behaviour):

Before: a 9-line `static std::unique_ptr` accessor with `register_destroy`, log-only ctor/dtor,
an `init_subjects` that sets the flag, an empty `register_callbacks`, a 14-line `create()` and
a 19-line `show()`. After:

```cpp
// after: the header is the whole class
class ConnectionSettingsOverlay : public OverlayBase {
    const char* get_name() const override { return "Connection"; }
    const char* xml_component() const override { return "settings_connection_overlay"; }
};
inline ConnectionSettingsOverlay& get_connection_settings_overlay() {
    return lazy_global<ConnectionSettingsOverlay>("ConnectionSettingsOverlay");
}
```

(05-ui-tail's `StaticXmlOverlay(component_name)` is this with the name as a constructor arg;
one class serves every behaviour-free page, so the four navigation-only hubs
`ui_settings_{connection,hardware,help,printing}.cpp` collapse to a line each.)

`src/ui/ui_panel_advanced.cpp#AdvancedPanel::handle_console_clicked` (a lazy-helper caller):

```cpp
// before: caller-owned cache member console_panel_ in include/ui_panel_advanced.h
helix::ui::lazy_create_and_push_overlay<ConsolePanel>(
    get_global_console_panel, console_panel_, parent_screen_, "Console", get_name(), true);
```

```cpp
// after: ConsolePanel overrides destroy_on_close() { return true; }; no cache member
get_global_console_panel().show(parent_screen_);
```

### Migration

- Infra commit (branch `refactor/fw-infra`): add the API, rewrite `lazy_create_and_push_overlay`
  as `getter().show(parent_screen)` ignoring its cache argument (so all 23 callers get the
  object-owned root at once, and the double-cache hazard is gone before any sweep), delete
  `include/ui_panel_singleton_macros.h` (1 user) and fold the 3-arg macro users onto
  `lazy_global`. Pilot 3 files: `ui_settings_connection`, `ui_settings_safety`,
  `ui_settings_sound`.
- Sweep (WS1-WS3 together, per file, by hand with a checklist; a script only lists
  candidates): delete the accessor body, log-only ctor/dtor, `create()` if it matches the
  default, `show()` if it matches the base (move extra steps to `before_show()`), the
  `init_subjects` guard. ~70 files, batches of 8-10 per commit grouped by directory
  (settings, AMS, calibration, print, misc), so each commit builds.
- Last sweep commit deletes the lazy helper, the caller cache members and
  `DEFINE_GLOBAL_OVERLAY_STORAGE`/`INIT_GLOBAL_OVERLAY` (2 uses: they become
  `lazy_global<T>(name, args...)`), and adds a ratchet on the "already exists" string and on
  `static std::unique_ptr<` in `src/ui` (baseline counts, shrink-only).
- Out of scope here, later with #1329: pushing with the lifecycle in one call
  (`push_overlay(root, IPanelLifecycle*)`, 04c C3) touches `ui_nav_manager.cpp`, which Lua
  phase 2 is rewriting; `show()` hides the pair behind one call already.

### Tests and gates

- `tests/unit/test_overlay_base.cpp` grows the contract: `show()` inits subjects exactly once
  across three shows; `create()` runs once; a second show reuses the root; `before_show()` runs
  every show; with `destroy_on_close()` true, a pop frees the tree, `get_root()` is null, the
  next `show()` re-creates, and an object destroyed before the deferred close callback runs
  does not touch freed memory (token check); `rebuild()` still works through the default
  `create()`.
- `HELIX_STRICT_OVERLAY_CHECK=1` (already on in tests) keeps catching a push without
  registration; `show()` makes that pair unforgettable for migrated overlays.
- Existing per-overlay tests (`test_overlay_*`, `test_led_settings_overlay`,
  `test_sensor_settings_overlay`, `test_printer_manager_overlay`, ...) run per batch.
- One `scripts/screenshot.sh` pass over the settings recipes at landscape AND portrait after the
  settings batch (the chevron change is the one visible difference).

### Risks

- **Portrait chevron / pressed style appears on ~40 overlays that lacked it.** It is the
  documented intent of `ui_overlay_panel_setup_standard`, but it is visible. Screenshot check.
- **`register_callbacks()` timing.** Today some overlays register once at first init, the lazy
  helper registers on every create. `show()` takes the helper's behaviour (every create), which
  keeps the 5 "re-register before push" last-write-wins workarounds working. WS2's collision
  gate removes the reason later.
- **The 198 `if (!overlay_root_) return;` guards are NOT all dead.** FW-4 calls them
  unreachable because methods run only after `show()`. With `destroy_on_close()` true, an
  observer or deferred callback can run after the tree is gone, and the guard becomes the
  thing standing between it and a freed widget. Rule for the sweep: delete a root guard only
  in methods reachable solely from events on the overlay's own widgets; keep it in observer,
  `defer`, timer and API-callback paths.
- **Lifetime of `this` in the close callback.** Captured with `object_lifetime_.token()`, which
  `OverlayBase::cleanup()` and the guard destructor invalidate, so a printer switch that
  destroys the object before the deferred callback fires is a no-op, not a UAF.
- **`lazy_global` static-local per T** is one instance per program (inline template); the
  destroy lambda needs no capture. Tests that reset globals through `StaticPanelRegistry`
  keep working; any test that reached into a named `g_x` must switch to the accessor.

### How it prepares destroy-on-close (#1329, #1246)

After WS1 the root lives in exactly one place (`overlay_root_`), there is one close path
(`show()` registers it) and one switch per overlay (`destroy_on_close()`). What #1329 still
needs, per overlay, is outside this phase: an `on_ui_destroyed()` that nulls cached widget
pointers and releases widget-bound observers, and #1246's SubjectManager deinit that
withdraws XML-scope names, before subjects can be freed too. The first flips (color_picker,
input_shaper, temp_graph_overlay, theme_editor, calibration_*, macro_buttons: ~1,300 objects,
~0.5-1.2 MB per 04c section E) then become one-line overrides plus a test each.

---

## 2. WS2 Event trampolines to table lambdas (FW-2)

### Problem, now

647 `static void on_x(lv_event_t*)` declarations in `include/`; 261 of the 574 handlers in the
UI tail are pure forwarders (get the singleton, read one bool or index, call `handle_x`),
each wrapped in `LVGL_SAFE_EVENT_CB_BEGIN(name)`, whose `name` argument is discarded
(`include/ui_event_safety.h`). `include/ui/ui_event_trampoline.h` has 2 users.
`src/ui/ui_probe_overlay.cpp#ui_probe_overlay_register_callbacks` shows the target shape
(captureless lambdas in the `register_xml_callbacks` table) but loses the exception guard.

### Proposed API

```cpp
// include/ui_callback_helpers.h
namespace helix::ui {
inline bool event_checked(lv_event_t* e) {
    return lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED);
}
inline int event_selected(lv_event_t* e) {
    return static_cast<int>(lv_dropdown_get_selected(lv_event_get_current_target_obj(e)));
}
} // namespace helix::ui

struct XmlCallbackEntry {
    const char* name;
    lv_event_cb_t callback;

    XmlCallbackEntry(const char* n, lv_event_cb_t cb) : name(n), callback(cb) {}

    // A captureless lambda gets the exception guard for free. Each lambda expression is its
    // own type, so the static copy below is per-entry; C++17 cannot default-construct it.
    template <typename F, typename = std::enable_if_t<std::is_class_v<F>>>
    XmlCallbackEntry(const char* n, F f) : name(n), callback(guarded<F>(n, f)) {}

  private:
    template <typename F> static lv_event_cb_t guarded(const char* n, F f) {
        static_assert(std::is_empty_v<F>, "XML callbacks cannot capture; use user_data");
        static const char* s_name;
        static std::optional<F> s_fn;
        s_name = n;
        s_fn.emplace(f);
        return [](lv_event_t* e) {
            HELIX_TRAMPOLINE_GUARD_BEGIN (*s_fn)(e);
            HELIX_TRAMPOLINE_GUARD_END_NAMED(s_name)
        };
    }
};
```

`HELIX_TRAMPOLINE_GUARD_*` (`include/ui/ui_event_trampoline.h`) already compile to a plain
block without exceptions (ESP32), so the firmware build pays nothing; `_END_NAMED` is a new
sibling of `_END` that takes the name at runtime. The guard now logs the real callback name, which
`LVGL_SAFE_EVENT_CB_BEGIN` never did.

### Before / after

`src/ui/ui_settings_safety.cpp` (8 trampolines, 7 lines each, plus 8 header declarations and 8
`handle_*` declarations):

```cpp
// before
void SafetySettingsOverlay::on_estop_confirm_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SafetySettingsOverlay] on_estop_confirm_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_safety_settings_overlay().handle_estop_confirm_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}
// ... x8, and {"on_estop_confirm_changed", on_estop_confirm_changed} in register_callbacks
```

```cpp
// after: the table is the whole event surface; setters that only persist call the manager
register_xml_callbacks({
    {"on_estop_confirm_changed", [](lv_event_t* e) {
         bool on = event_checked(e);
         SafetySettingsManager::instance().set_estop_require_confirmation(on);
         EmergencyStopOverlay::instance().set_require_confirmation(on);
     }},
    {"on_macro_confirm_changed", [](lv_event_t* e) {
         SafetySettingsManager::instance().set_macro_require_confirmation(event_checked(e));
     }},
    // ...
});
```

The `handle_*` layer goes where it only forwarded (the manager setter already logs the change
at info). With WS6's XML binding, the overlay keeps no per-row C++ state at all.

`src/ui/ui_panel_settings.cpp#SettingsPanel::on_led_settings_clicked` (navigation trampoline,
plus `handle_led_settings_clicked`, plus a header declaration, registered twice: in
`register_settings_panel_callbacks` and again in `init_subjects`):

```cpp
// after, registered once
{"on_led_settings_clicked",
 [](lv_event_t*) { helix::settings::get_led_settings_overlay().show(settings_screen()); }},
```

The 14 navigation rows of this shape become one table
`{callback_name, [] { return &get_x_overlay(); }}` iterated once (05-ui-tail, "Settings
sub-pages"), and the 18 SettingsPanel duplicates of sub-overlay callbacks are deleted (the
sub-overlay registration is the live one today).

### Migration

By hand inside the per-file sweep (WS1). A candidate lister (the audit's `tramp.py`,
promoted to `scripts/list_trampolines.py`) prints each forwarder with its handler so the
editor converts from a list rather than by search. ~130 files carry trampolines; the ones
outside the 70 overlays (panels, modals, widgets) form their own sweep batches.

### Tests and gates

- `scripts/check_orphan_callbacks.py` (from the audit's `cb_dead.py`, ~150 lines), baseline
  and ratchet like `check_orphan_subjects.py`: fails on a registered name no XML references, an
  XML callback no C++ registers, and a name registered from two files. Wired into
  `quality-checks.sh` and `make full-test-run`. The double-registration check is what makes
  deleting the SettingsPanel duplicates safe.
- A `tests/shell/test_code_lint.bats` ratchet on `static void on_\w+\(lv_event_t\*` in
  `include/` (baseline now 647, shrink-only).
- `tests/unit/test_callback_helpers.cpp`: a throwing lambda entry logs its name and returns; a
  function-pointer entry and a lambda entry both resolve through `lv_xml_get_event_cb`.

### Risks

Low. A lambda cannot capture, which is enforced; per-instance state still arrives through
`lv_event_get_user_data`. The 8 shared-component callbacks with two owners (wifi, touch cal,
telemetry, history row) are not converted blindly: the gate flags them, and each gets a
distinct name per owner, which also closes the latent wrong-`user_data` cast in wifi.

---

## 3. WS3 `find_required()` and the XML-names gate (FW-3, F16)

### Problem, now

1,078 `lv_obj_find_by_name` calls in `src/`; ~540 are followed by a null check within 3
lines, ~134 with a bespoke "not found" log; 382 of the UI-tail ones look up a literal that
exists in `ui_xml`. `FIND_WIDGET`/`_REQUIRED`/`_OPTIONAL` (`include/ui/ui_widget_helpers.h`)
are a fourth spelling with 30 uses. A missing name is a silent no-op in every build.

### Proposed API

```cpp
// include/ui/ui_widget_helpers.h (replaces the three FIND_WIDGET macros)
namespace helix::ui {
// A widget the component's XML must contain. Logs once per (owner, name) at error and
// returns nullptr; in --test and unit tests it aborts, so a missing name fails the run.
// A null root returns nullptr silently, so nested lookups chain without double logging.
lv_obj_t* find_required(lv_obj_t* root, const char* name, const char* owner);

// A widget that legitimately may be absent (inside <if>, or omitted by a layout variant).
inline lv_obj_t* find_optional(lv_obj_t* root, const char* name) {
    return root ? lv_obj_find_by_name(root, name) : nullptr;
}
} // namespace helix::ui
```

The abort switch reuses the existing strict pattern (`HELIX_STRICT_OVERLAY_CHECK` in
`src/ui/ui_nav_manager.cpp#overlay_registration_strict`): one atomic flag set by the test
main and by `--test`. Release builds only log. No exceptions, so it is firmware-safe.

### Before / after

`src/ui/ui_settings_sound.cpp` (volume row seeding):

```cpp
// before: two early-outs and two if-blocks, no log when a name is missing
lv_obj_t* volume_row = lv_obj_find_by_name(overlay_root_, "row_volume");
if (!volume_row)
    return;
lv_obj_t* slider = lv_obj_find_by_name(volume_row, "slider");
if (slider) { ... }
lv_obj_t* value_label = lv_obj_find_by_name(volume_row, "value_label");
if (value_label) { lv_label_set_text(value_label, volume_value_buf_); }
```

```cpp
// after: one log format, and a test-time failure if the XML drops a name
lv_obj_t* row = find_required(overlay_root_, "row_volume", get_name());
if (lv_obj_t* slider = find_required(row, "slider", get_name())) { ... }
if (lv_obj_t* label = find_required(row, "value_label", get_name()))
    lv_label_set_text(label, volume_value_buf_);
```

`src/ui/ui_settings_safety.cpp#SafetySettingsOverlay::init_estop_toggle`: the nested
`row_estop_confirm` / `toggle` double guard is not converted at all; WS6 deletes the function by
adding `subject="settings_estop_confirm"` to the row in `ui_xml/settings_safety_overlay.xml`.

### Migration

In the per-file sweep. A script can do the mechanical half (literal-name lookup followed by
`if (!x) return;` or `if (x) {...}`) and emit a diff for review, but the choice between
`find_required` and `find_optional` is per site: names under `<if>` or missing from some
layout variant stay optional, and `scripts/check_variant_content_drift.py` already knows
which names differ by variant.

### Tests and gates

- `scripts/check_required_names.py` (new, static): for each `.cpp`, the components it creates
  (`lv_xml_create` literals and `xml_component()` returns) and the literals it passes to
  `find_required`; each literal must appear as `name="..."` in that component, or in a
  component it instantiates (transitive expansion, as the audit's `xmlcount.py` does), in
  EVERY layout variant of it. This is the "test that XML-declared names exist", and it covers
  paths no unit test executes.
- Where the component is chosen at runtime (plugin setting rows pick a `setting_*_row` by
  declared type), the call site names the candidate family in a one-line annotation and the
  gate checks the name in every member.
- The runtime abort covers the dynamic names the static gate cannot see. Plugin-supplied XML
  (`plugin_xml_policy`) is data from a third party: lookups into it are always
  `find_optional`, so a plugin can never abort a test run or a `--test` session.
- The macro header shrinks; a lint ratchet on `lv_obj_find_by_name(` in `src/ui` (baseline
  now, shrink-only) keeps new code on the helpers.

### Risks

Low to medium. A layout variant that silently lacks a widget becomes a gate failure, which is
the point, but it will surface real variant drift during the sweep. Budget for it: each hit
is either a missing XML element (fix XML) or a legitimately optional name (`find_optional`).
Run the screenshot recipes once at the end.

---

## 4. WS4 One type-erased `observe<V>()` (FW-5, D3, D10)

### Problem, now

`include/observer_factory.h` has five live templates (`observe_int_sync`,
`observe_int_immediate`, `observe_int_async`, `observe_string`, `observe_string_immediate`)
whose bodies differ only in int-vs-string read and dispatch. Each call site is a distinct
lambda type, so each instantiates a context struct, the observer trampoline, the deferred
lambda's `std::function` manager+invoker and the cleanup `std::function`. Measured today on
the native x86-64 binary: **447 KB** of text in symbols carrying these template names (414 KB
`observe_int_sync`, 24.5 KB `observe_string`, 3.8 KB async, 3.0 KB immediate, 1.6 KB
language). Call sites now: 404 `observe_int_sync` in src (100 in tests), 20 immediate, 8
async, 12 string, 7 string_immediate, 13 language, 12 print-state/lifecycle adapters.
Naming trap: `observe_int_sync` is the DEFERRED one; `observe_int_immediate` is synchronous.
The header also includes `printer_state.h` (2,700+ lines) into 114 TUs for one enum.

### Proposed API

```cpp
// include/observer_factory.h
namespace helix::ui {
enum class Dispatch : uint8_t {
    Deferred,  // handler runs from the UpdateQueue after the notification (today's _sync)
    Immediate, // handler runs inside lv_subject notify (today's _immediate)
};

namespace detail {
// Defined once in src/ui/observer_factory.cpp: one trampoline, one deferred lambda and one
// cleanup per value type, instead of one per call site.
ObserverGuard observe_core(lv_subject_t* subject, std::function<void(int)> fn,
                           Dispatch dispatch, const SubjectLifetime& lifetime);
ObserverGuard observe_core(lv_subject_t* subject, std::function<void(const char*)> fn,
                           Dispatch dispatch, const SubjectLifetime& lifetime);
} // namespace detail

// V is int or const char*. The lifetime stays required: no default, so omitting it does not
// compile (threading rule 4).
template <typename V, typename Owner, typename Handler>
ObserverGuard observe(lv_subject_t* subject, Owner* owner, Handler&& handler,
                      const SubjectLifetime& lifetime, Dispatch dispatch = Dispatch::Deferred) {
    if (!subject || !owner) {
        spdlog::warn("[observe] null {} - observer not attached", subject ? "owner" : "subject");
        return ObserverGuard();
    }
    return detail::observe_core(
        subject,
        std::function<void(V)>([owner, h = std::forward<Handler>(handler)](V v) { h(owner, v); }),
        dispatch, lifetime);
}

// Typed adapters stay, 3 lines each:
template <typename Owner, typename Handler>
ObserverGuard observe_print_state(lv_subject_t*, Owner*, Handler&&, const SubjectLifetime&,
                                  Dispatch = Dispatch::Deferred);
template <typename Owner, typename Handler>
ObserverGuard observe_print_lifecycle(lv_subject_t*, Owner*, Handler&&, const SubjectLifetime&);
template <typename Owner, typename OnChange>
ObserverGuard observe_language_change(Owner*, OnChange&&); // moves to observe_language.h
} // namespace helix::ui
```

`observe_core` keeps the context layout that the LOAD-BEARING INVARIANT comment describes:
the context owns `std::shared_ptr<const std::function<void(V)>> fn` and
`std::shared_ptr<bool> alive`; the synchronous trampoline copies `fn` (a refcount bump, the
same operation the comment names) and `weak_ptr alive`, and the deferred lambda checks
`alive` before calling. Safety still rests on `ObserverGuard::reset()` removing the observer
before the cleanup deletes the context; the comment moves with the code, unchanged in
substance. The string deferred path copies to `std::string` and passes `c_str()`, as today.
`observe_int_async` (8 sites) is not a third dispatch mode: each site becomes `Immediate`
plus an explicit `lifetime_.defer`/`object_lifetime_.defer` for its update half, by hand.
`PrintJobState` is forward-declared (an opaque `enum class` is complete); the language
adapter moves to its own header so `system_settings_manager.h` leaves the 114 TUs too.

### Before / after

`src/ui/ui_panel_controls.cpp#ControlsPanel::register_observers`:

```diff
-fan_observer_ = observe_int_sync<ControlsPanel>(
+fan_observer_ = observe<int>(   // Deferred is the default, as _sync was; Owner is deduced
     printer_state_.get_fan_speed_subject(), this,
     [](ControlsPanel* self, int /* value */) {
         if (self->active_)
             self->update_fan_display();
     },
     printer_state_.get_subjects_lifetime());
```

`src/ui/ui_panel_motion.cpp` (bed-moves label, synchronous):

```cpp
// before
bed_moves_observer_ = helix::ui::observe_int_immediate<MotionPanel>(
    get_printer_state().get_printer_bed_moves_subject(), this,
    [](MotionPanel* self, int bed_moves) { ... },
    get_printer_state().get_subjects_lifetime());
// after
bed_moves_observer_ = helix::ui::observe<int>(
    get_printer_state().get_printer_bed_moves_subject(), this,
    [](MotionPanel* self, int bed_moves) { ... },
    get_printer_state().get_subjects_lifetime(), Dispatch::Immediate);
```

### Migration

Two steps, deliberately separable:

1. **Core swap, zero call-site churn** (`refactor/observer-core`). Re-implement the five old
   templates as 3-line forwarders onto `observe_core`. Every call site compiles unchanged; all
   of the binary win lands here. Measure; if the win is not there, stop and keep only the
   naming fix.
2. **Rename, scripted** (`refactor/observer-rename`, last in the phase). A sed-level script:
   `observe_int_sync<\w+>(` becomes `observe<int>(`, `observe_string<\w+>(` becomes
   `observe<const char*>(`; the 27 immediate sites get `Dispatch::Immediate` appended by hand.
   The script is committed so peers re-run it on their branches rather than hand-resolving
   ~450 hunks. Old names are deleted in the same commit, and the docs (`LVGL9_XML_GUIDE.md`,
   `THREADING.md`, `.claude/rules/threading.md` rule 4, whose "defaulted 4th parameter"
   wording no longer matches the header) are updated with it.

### Tests and gates

- Binary target, measured the same way as the baseline:
  `nm -C -S build/bin/helix-screen | awk` summing `[tTwW]` symbols matching `observe_` (baseline
  447 KB), plus the `.text` delta from `size -A`. Accept step 1 at a `.text` drop of at least
  250 KB x86-64 (audit estimate 300-400 KB; ~120-180 KB ARM); below 150 KB, drop the type
  erasure and ship only the rename. The per-site cost that remains is one `std::function`
  manager+invoker for the binding lambda (F22 counts those at ~0.5 MB tree-wide), so the
  target is net of that.
- Behaviour: `tests/unit/test_observer_factory.cpp`, `test_observer_cleanup_ordering.cpp`,
  `test_observer_guard_reinit_window.cpp`, `test_estop_observer_lifetime.cpp`,
  `test_async_callback_safety.cpp`, `test_callback_drain.cpp` unchanged and green; add: a
  deferred handler queued, then the guard reset before drain, does not run (#174 shape); the
  context is freed after reset while a queued copy is still pending, and the copy runs or skips
  without touching the context (#82 shape); `Immediate` runs inside the notify.
- `scripts/zeus-run.sh asan '[observer]'` once, and the full sweep once, before merge.

### Risks

Medium; this is the hot path in every panel. Specifically: (a) handler state now lives behind
one more indirection and one more heap object per observer, which is creation-time cost only;
(b) a handler that captured by value and mutated its copy per notification (the deferred path
copies the handler today) now shares one instance across notifications; `observe_language_change`
already relies on shared state for exactly this reason, but step 1 must grep for `mutable`
handlers and check each; (c) the rename churns every peer branch that touches an observer,
which is why it runs last and ships as a script.

---

## 5. WS5 One way to defer to the main thread (FW-6, FW-14, D11, D12)

### Problem, now

13 forms (04d census). The ones this workstream removes, counted now: 59 `helix::ui::async_call(cb,
void*)` sites (C-style, raw `this`, no lifetime check), 23 raw `lv_async_call`, 9
`helix::async::call_method*` in `src/printer/printer_state.cpp` via `include/async_helpers.h`
(raw `this`, FW-14), the `queue_update<T>(unique_ptr, std::function)` overloads that leak `T`
when the queue drops the callback (D12), and 2 `run_on_main`. The four `defer` bodies in
`include/async_lifetime_guard.h` have already drifted (the untagged guard variant does not call
`note_skipped`).

### Target: four forms, each with one job

| Form | Use |
|------|-----|
| `object_lifetime_.defer(tag, fn)` / `lifetime_.defer(tag, fn)` | main thread, work bound to an object; screen-scoped only when the work just paints |
| `guard.bg_cb(tag, fn)` | build ON the main thread, hand to a background API; it defers and guards |
| `queue_update(tag, fn)` | work bound to no object (statics, process state) |
| `run_next_tick(token, fn)` | needs LVGL's async list (after the current event dispatch), not the UpdateQueue |

One-shot `lv_timer`s (56) and untagged `queue_update` (~165) are out of scope: the first is a
delay, not a defer, and already has `LvglTimerGuard`; the second is harmless and not worth the
churn.

### API changes

```cpp
// include/async_lifetime_guard.h: one body, both overloads forward to the token
template <typename F> void AsyncLifetimeGuard::defer(const char* tag, F&& fn) {
    token().defer(tag, std::forward<F>(fn));
}
// include/ui_update_queue.h: delete async_call (both overloads) and queue_update<T>(unique_ptr,...)
// include/async_helpers.h: deleted
```

Classes that have no guard today and receive migrated callbacks (`AbortManager`,
`PrintPreparationManager`, `mdns_discovery`, and the few non-view owners in the list) get an
`AsyncLifetimeGuard lifetime_` member. PrinterState already has `async_lifetime_`.

### Before / after

`src/ui/ui_panel_filament.cpp` (extrude completion from the WebSocket thread):

```cpp
// before
api_->execute_gcode(
    gcode,
    [this]() {
        helix::ui::async_call(
            [](void* ud) {
                auto* self = static_cast<FilamentPanel*>(ud);
                self->operation_guard_.end();
                self->op_succeeded(FilamentOp::Extrude);
            },
            this);
    },
    ...
```

```cpp
// after: built on the main thread, runs on the main thread, skipped if the panel is gone
api_->execute_gcode(
    gcode,
    object_lifetime_.bg_cb("Filament::extrude_ok",
                           [this] {
                               operation_guard_.end();
                               op_succeeded(FilamentOp::Extrude);
                           }),
    ...
```

`object_lifetime_`, not `lifetime_`: `operation_guard_.end()` must run even if the user left
the Filament screen, or the next extrude is refused.

`src/printer/printer_state.cpp` (`set_klipper_version` and 8 siblings):

```cpp
// before
helix::async::call_method_ref(this, &PrinterState::set_klipper_version_internal, version);
// after
async_lifetime_.defer("PrinterState::klipper_version",
                      [this, version] { set_klipper_version_internal(version); });
```

This is the call its siblings in the same file already make from background threads
(`src/printer/printer_state.cpp#PrinterState::set_printer_connection_state` and ~20 more).
Rule 2's "main-thread only" protects owners that can die while the background thread holds
`this`; PrinterState outlives the queue in production, and the per-instance PrinterStates in
`XMLTestFixture` are where the guard earns its place. Views never take this shortcut.

### Migration

By hand, per site, because each site needs a decision: which thread calls it (background
means `bg_cb` built on the main thread, per threading rule 2), and which guard (object or
screen). ~95 sites in ~40 files, in batches by directory, each commit building. The 3
`lv_async_call` close sites in `src/ui/ui_nav_manager.cpp` are one `defer_close_callback`
after Lua phase 2; that function routes through `run_next_tick` (next LVGL tick, which the
close path needs) and `close_overlay`'s queued decision is left untouched.

### Tests and gates

- New `tests/unit/test_abort_manager.cpp` case: destroy the manager (or reset it) with a probe
  callback in flight; the callback is skipped, no UAF. Same shape for FilamentPanel and
  PrintPreparationManager, using the mock API's delayed-callback hooks.
- `test_async_lifetime_guard.cpp`: tagged and untagged `defer` both call `note_skipped` (the
  D11 drift, now one body).
- Lint (`test_code_lint.bats`): `async_call(`, `lv_async_call(` outside `include/ui_next_tick.h`,
  and `helix::async::` are banned in `src/`, zero baseline once the branch lands.
  `scripts/check_l081_anti_pattern.py` keeps policing the bg-thread TOCTOU shape.
- `scripts/zeus-run.sh asan` once over the touched tags.

### Risks

Medium, and the risk is a behaviour change by design: a callback that ran unguarded against a
dead or deactivated owner now does not run. That is the fix for the crash class, but a site
that relied on "always runs" (clear a busy flag, end an operation guard, dismiss a modal)
must use the object-scoped guard, never the screen-scoped one. Each migrated site states its
choice in the commit's site list. A background-thread `lifetime_.defer(...)` on a view that can
be destroyed would dereference `this` off-thread; `bg_cb` built on the main thread avoids it,
and the lint rule for rule 2 already flags the TOCTOU form.

---

## 6. WS6 Settings: `PersistedSetting` table and XML-bound rows (FW-7, FW-8)

### Problem, now

8 managers (`src/system/{settings,display_settings,system_settings,material_settings,
safety_settings,input_settings,audio_settings}_manager.cpp`, `label_printer_settings.cpp`),
6,079 lines with headers. Every setter is the same 6-12 lines (clamp, info log,
`lv_subject_set_int`, `Config::get_instance()`, `set<T>(path)`, `save()`, sometimes telemetry);
every getter is `lv_subject_get_int(const_cast<...>(&x_subject_))`. Only 12 of ~70 setters
report telemetry, all in `SettingsManager`. 188 KB x86 text; `SettingsManager::init_subjects`
alone is 21 KB. On the UI side, 43 of 46 `<setting_dropdown_row>` omit `bind_selected=` and
11 of 35 `<setting_toggle_row>` omit `subject=`, so 59 sites in 14 overlay files re-sync those
rows by hand in `on_activate`.

### Proposed API

```cpp
// include/persisted_setting.h
namespace helix::settings {
enum class Scope : uint8_t { Global, PerPrinter }; // PerPrinter prefixes Config::df()

struct PersistedSetting {
    const char* xml_name;      // "settings_estop_confirm": must match XML, now in one place
    const char* json_path;     // "/safety/estop_require_confirmation" or "filament/extrude_speed"
    Scope scope;
    bool is_bool;
    int def, min, max;         // bool: def 0|1, min 0, max 1
    const char* telemetry_key; // nullptr = not reported (today's set, unchanged)
};

// A fixed set of settings indexed by an enum, owning their subjects.
template <typename Key, size_t N> class PersistedSettings {
  public:
    explicit constexpr PersistedSettings(const PersistedSetting (&table)[N]);
    void init(SubjectManager& subjects);  // read config, clamp, init + register each subject
    int get(Key k) const;                 // subject_get_int_or(def) semantics before init
    bool get_bool(Key k) const { return get(k) != 0; }
    void set(Key k, int value);           // clamp, log, subject, config set + save, telemetry
    lv_subject_t* subject(Key k);
  private:
    const PersistedSetting (&table_)[N];
    std::array<lv_subject_t, N> subjects_{};
};
} // namespace helix::settings
```

This is the same shape FW-9 proposes for capability flags (enum + `std::array<lv_subject_t, N>`
+ name table); if FW-9 lands first, it should reuse this template rather than grow a twin.

Setters with side effects or non-identity storage stay hand-written, on top of the table:
dark mode, rotation, brightness preview, timezone, external spool JSON, the string settings
(macro names, scanner ids), and index-to-value settings (cancel escalation timeout stores
seconds but the subject holds an index).

### Before / after

`src/system/safety_settings_manager.cpp#SafetySettingsManager::set_macro_require_confirmation`
(and its twins for estop confirm, cancel escalation, cold extrude):

```cpp
// before: init_subjects block (3 lines) + getter (3) + setter (9), per setting
void SafetySettingsManager::set_macro_require_confirmation(bool require) {
    spdlog::info("[SafetySettingsManager] set_macro_require_confirmation({})", require);
    lv_subject_set_int(&macro_require_confirmation_subject_, require ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>("/safety/macro_require_confirmation", require);
    config->save();
    spdlog::debug("[SafetySettingsManager] Macro run confirmation {} and saved",
                  require ? "enabled" : "disabled");
}
```

```cpp
// after
enum class Safety : uint8_t { EstopConfirm, CancelEscalation, MacroConfirm, AllowColdExtrude, N };
static constexpr PersistedSetting kSafety[] = {
    {"settings_estop_confirm", "/safety/estop_require_confirmation", Scope::Global, true, 1, 0, 1, nullptr},
    {"settings_cancel_escalation_enabled", "/safety/cancel_escalation_enabled", Scope::Global, true, 0, 0, 1, nullptr},
    {"settings_macro_confirm", "/safety/macro_require_confirmation", Scope::Global, true, 1, 0, 1, nullptr},
    {"settings_allow_cold_extrude", "/safety/allow_cold_extrude", Scope::Global, true, 0, 0, 1, nullptr},
};
// header, one line each; callers unchanged
void set_macro_require_confirmation(bool v) { settings_.set(Safety::MacroConfirm, v); }
bool get_macro_require_confirmation() const { return settings_.get_bool(Safety::MacroConfirm); }
```

`src/system/settings_manager.cpp#SettingsManager::set_extrude_speed` (per-printer, clamped,
telemetry) becomes the row
`{"settings_extrude_speed", "filament/extrude_speed", Scope::PerPrinter, false, 5, 1, 50, "extrude_speed"}`
and a one-line wrapper; the old value for telemetry is read inside `set()`.

XML-bound rows, `ui_xml/settings_safety_overlay.xml`:

```xml
<!-- before: C++ init_estop_toggle() re-syncs this on every on_activate -->
<setting_toggle_row name="row_estop_confirm" ... callback="on_estop_confirm_changed"/>
<!-- after: the row shows the subject; the callback still writes through the setter -->
<setting_toggle_row name="row_estop_confirm" ... subject="settings_estop_confirm"
                    callback="on_estop_confirm_changed"/>
<setting_dropdown_row name="row_completion_alert" ... bind_selected="settings_completion_alert"
                      callback="on_completion_alert_changed"/>
```

and `SafetySettingsOverlay::on_activate`, `init_estop_toggle`, `init_completion_alert_dropdown`
are deleted. With WS1 and WS2, `ui_settings_safety.cpp` shrinks from 304 lines to the
callback table.

### Migration

- Table: per manager, by hand (each row is a reading of the old setter), smallest first to
  prove the template (safety, input, audio), then `SettingsManager` and
  `DisplaySettingsManager` (the two big ones, most of the byte win), others opportunistically.
  One commit per manager; public typed getters/setters keep their signatures, so no caller
  changes and every commit builds.
- Rows: in the settings-overlay sweep batch, one overlay per hunk: add `subject=` /
  `bind_selected=` where the subject value equals the widget state, delete the `init_*`
  re-sync. Rows whose widget index is not the stored value (the 5 option-list converters,
  06-part-core) keep C++ until they have an index subject.
- Deliberately NOT in this phase: two-way binding that removes the write callbacks. It needs
  the manager to observe its own subjects and persist on change, which changes when `save()`
  runs (including at init) and bypasses the setter's clamp for XML writes. One-way bind plus
  the existing callback gets the on_activate deletion with no persistence change.

### Tests and gates

- `tests/unit/test_persisted_setting.cpp`, table-driven over every manager's table: `set()`
  updates the subject, writes the config path under the right scope (df() prefix for
  PerPrinter), clamps out-of-range values to the same result the old setter produced, calls
  telemetry exactly for rows with a key; deinit + init round-trips the stored value; `get()`
  before init returns `def`.
- A golden list of the registered `xml_name`s per manager, generated from main before the
  change: the table must register exactly the same names (the risk the audit names).
  `check_orphan_subjects.py` still covers registered-but-unread.
- Existing `test_{audio,display,input,material}_settings_manager.cpp` unchanged.
- Binary: `nm` sum of `*SettingsManager::*` text before/after (baseline 188 KB), target
  -60 KB x86.
- Rows: `test_settings_root.cpp` / overlay tests activate each touched overlay with the
  subject set both ways and read the widget state via the test fixture.

### Risks

Low to medium. Names must stay identical (golden list). Clamp semantics must match per row;
`set_min_toast_severity` maps anything other than 1|2 to 0, which a min/max clamp would not,
so it stays hand-written. Log text changes from per-setter prose to one format; no gate greps
those lines. Coordinate with the `refactor/config-migrations` session, which owns
`config.cpp`: this workstream only calls `Config::get/set/save/df`, it does not move paths.

---

## 7. WS7 Home-widget boilerplate (FW-13)

### Problem, now

31 widgets hand-set and clear root `user_data`; 18 repeat the four `TileSizing` forwarders
(`on_size_changed`, `fits_at`, `xml_attrs`, `tile_sizing`); `HumidityWidget` and
`WidthSensorWidget` are whole classes that differ from `TileWidget`
(`src/ui/panel_widgets/tile_widget.h`) only in nouns; 10 sites hand-roll what
`panel_widget_from_event<T>` does; the icon picker cell loop exists 4 times; PowerDeviceWidget
hand-builds a 393-line picker while 9 pickers subclass `ContextMenu`.

### Proposed API

```cpp
// include/panel_widget.h
class PanelWidget {
  public:
    lv_obj_t* root() const { return root_; } // set by the manager, not by attach()
  private:
    friend class PanelWidgetManager;
    void bind_root(lv_obj_t* obj);   // root_ = obj; lv_obj_set_user_data(obj, this)
    void unbind_root();              // clears both; called after detach()
    lv_obj_t* root_ = nullptr;
};

// A widget whose sizing is a TileSizing: the four forwarders, once.
class TiledPanelWidget : public PanelWidget {
  protected:
    explicit TiledPanelWidget(TileSizing sizing) : sizing_(std::move(sizing)) {}
    void on_size_changed(int, int, int w, int h) override { sizing_.measure_and_publish(w, h); }
    bool fits_at(int w, int h) const override { return sizing_.fits(w, h); }
    const char** xml_attrs() const override { return sizing_.subject_attrs(); }
    TileSizing* tile_sizing() override { return &sizing_; }
    TileSizing sizing_;
};
// TileWidget becomes a TiledPanelWidget; humidity and width_sensor register as TileWidget.

// include/ui_icon_picker.h
void populate_icon_grid(lv_obj_t* grid, const std::vector<const char*>& icons,
                        std::string_view selected, std::function<void(const char*)> on_pick);
```

plus `ui_xml/components/icon_picker_cell.xml` (size, pressed and selected state via
`bind_style`), and `DevicePicker : ContextMenu` for PowerDeviceWidget.

### Before / after

`src/ui/panel_widgets/humidity_widget.cpp` (94 lines, plus a header):

```cpp
// before: a class whose attach/detach only manage user_data
void HumidityWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    widget_obj_ = widget_obj;
    if (widget_obj_)
        lv_obj_set_user_data(widget_obj_, this);
}
void HumidityWidget::detach() { ... }
register_widget_factory("humidity", [](const std::string&) {
    return std::make_unique<HumidityWidget>(); });
```

```cpp
// after: a kPureXmlTiles row in src/ui/panel_widgets/tile_widget.cpp#register_tile_widgets;
// humidity_widget_init_subjects and its register_widget_subjects() call stay, the class goes
{"humidity", /*widest_value*/ "100%", ...},
```

`include/favorite_macro_widget.h` (one of the 18): the four sizing forwarders and the
`attach`/`detach` user_data lines are deleted; the class derives from `TiledPanelWidget` and
keeps only its macro behaviour.

### Migration

By hand, one commit for the base change plus mechanical deletion of the 31 user_data pairs
(the manager now does it; a widget that still sets it is harmless, so the deletion can batch),
one for `TiledPanelWidget` + the 18 forwarders, one for humidity/width, one for the icon grid
(4 sites), one for the PowerDevice picker. `LuaPanelWidget` (Lua phase 2) takes the same base
change: its `root_` and `attach()` bookkeeping fold into `bind_root`.

### Tests and gates

`test_panel_widget_manager.cpp` asserts `user_data` is set before `attach()` and cleared after
`detach()` for every registered factory, so no widget depends on doing it itself;
per-widget tests (`test_panel_widget_*`, `test_print_status_widget_recycle.cpp`) per commit;
a screenshot of the icon picker and the power-device picker (GLM-safe alternative: `ctl geom`
on the picker cells).

### Risks

Low to medium. Widgets are recycled across rebuilds (CLAUDE.md "Home-panel widget"), so the
manager must bind before `attach()` on every reuse, not only at construction; the test above
pins that. Coordinate with the adaptive-sizing work (landed; micro audit queued), which edits
the same tile XML.

---

## 8. Sequencing, branches, Lua phase 2

### Lua phase 2 inventory (lands first; converted in the same pass)

Read from `git diff main...feature/lua-plugins-phase2` (2b0321c42). Each row lands in the
branch that owns the matching first-party files, never as a follow-up.

| Site | Today on the branch | Conversion | Branch |
|------|---------------------|------------|--------|
| `src/plugin/plugins_overlay.cpp#get_plugins_overlay` | hand-rolled `unique_ptr` accessor whose destroy lambda also nulls `g_plugins_panel_cache` | `lazy_global<PluginsOverlay>`; the cache global goes with the caller cache | 5 |
| `#show_plugins_overlay` | `lazy_create_and_push_overlay(..., destroy_on_close=true)` | `get_plugins_overlay().show(parent)`, `destroy_on_close()` returns true | 5 |
| `PluginsOverlay::create`, `PluginSettingsOverlay::create` | hand-rolled `lv_xml_create` + hide; settings passes attrs | Plugins: default `create()` via `xml_component()`; Settings keeps its override (attrs, rows) | 5 |
| `PluginsOverlay::init_subjects`, `PluginSettingsOverlay::init_subjects` | set the flag only | deleted (base default) | 5 |
| `PluginSettingsOverlay` lifetime | owned by `PluginHost::settings_screens_`, pushed by `PluginOverlayHost::push`, dtor deletes a still-open root | unchanged: owner-held (WS1 above); gets `close()` only if it ever closes itself | 5 |
| `src/plugin/plugin_overlay_host.cpp#push`, `#close`, `#close_all` | `register_overlay_instance` + close callback + `push_overlay`; `close_overlay` | unchanged in phase 2; becomes the `push_overlay(root, lc, on_closed)` template in item 9 | 9 |
| `src/ui/ui_nav_manager.cpp#defer_close_callback` | the three raw `lv_async_call` close sites, folded into one | routes through `run_next_tick`; `close_overlay` semantics untouched | 3 |
| `plugin_settings_overlay.cpp#register_plugin_settings_callbacks`, `plugins_overlay.cpp` row cb, `plugin_host.cpp` `plugin_event` | 4 `lv_xml_register_event_cb` with free functions dispatching through `PluginHost::live()` and row/obj `user_data` | `register_xml_callbacks` table lambdas (gains the exception guard, and the orphan-callback gate sees them); dispatch unchanged | 5 |
| `plugin_settings_overlay.cpp` (8 lookups), `plugins_overlay.cpp` (1) | `lv_obj_find_by_name` + null check on `settings_rows`, `plugins_rows`, and the `setting_*_row` parts `slider`/`value_label`/`toggle`/`dropdown`/`value_input` | `find_required`; the static gate resolves the row parts against the `setting_*_row` family (component name is runtime, so the gate checks the name exists in every family member the spec can choose) | 5 |
| `plugins_overlay.cpp`, `plugin_consent.cpp`, `lua_bind_ui.cpp` | `modal_confirm` with `owner_token` + `on_dismiss` | unchanged; already the target API | none |
| `lua_bind_ui.cpp#on_subject_change`, `lua_bind_printer.cpp` watch | raw `lv_subject_add_observer` with contexts holding Lua refs, disarmed by the runtime token | stays raw: the handler is a Lua function ref whose lifetime is the `LuaRuntime`, not an owner pointer; WS4's `observe<V>` does not fit and the row is listed so the rename script skips it | none |
| `lua_runtime.cpp`, `lua_bind_moonraker.cpp`, `plugin_host.cpp` | `token.defer(tag, ...)` | already the WS5 target | none |
| `src/plugin/lua_panel_widget.cpp` | `root_` + `install_delete_hook`, recycled, no `user_data` | `bind_root` supplies `root()`; the delete hook keeps working off it; gaining `user_data = this` is harmless (Lua reads event `user_data`, never the root's) | 7 |
| `panel_widget_registry` runtime defs + generation, `panel_widget_manager#notify_widget_defs_changed`, `panel_widget_config` `PageConfig::retained` | new | WS7 binds in the manager's attach path, so rebuilds triggered by a generation bump and retained unknown ids go through the same bind/unbind; test covers a runtime-def widget | 7 |
| `src/ui/ui_widget_catalog_overlay.cpp` | free-function overlay (`g_catalog_state`), 3 `lv_xml_register_event_cb`, 2 `queue_update`, `modal_confirm` | WS2 table lambdas, WS3 lookups; not an `OverlayBase` today, left that way in phase 2 | 6 |
| `ui_xml/setting_text_row.xml` (new), `setting_info_row.xml` (edited), Plugins row in `settings_panel.xml` / `ui_panel_settings.cpp` | new row family member; one more navigation handler | WS6 row rules apply to the whole family including `setting_text_row`; the Plugins row joins the navigation table | 5 |
| plugin settings persistence | `set_plugin_setting` into Config JSON, per-plugin, manifest-declared | out of WS6: `PersistedSetting` is a static table of first-party settings; plugin rows stay C++-applied (`apply_row_state`), which is the dynamic-list structural exception | none |

### Branches

| # | Branch | Content | Depends on | Parallel with |
|---|--------|---------|-----------|---------------|
| 1 | `refactor/fw-infra` | WS1 API + `lazy_global` + lazy-helper rewrite, WS2 `XmlCallbackEntry` lambda ctor + `event_checked/selected`, WS3 `find_required` + both gates (baselined, plugin sites in the baseline), orphan-callback gate, 3 pilot files | Lua merge | 2, 3, 4 |
| 2 | `refactor/observer-core` | WS4 step 1 + D3 + D10, measurement in the commit body | Lua merge | everything |
| 3 | `refactor/defer-lifetime` | WS5 including `defer_close_callback` | Lua merge | 1, 2, 4; rebase-trivial vs sweeps |
| 4 | `refactor/settings-table` | WS6 table (managers only) | Lua merge (coordinate config-branch) | 1, 2, 3 |
| 5 | `refactor/overlay-sweep-settings` | WS1+2+3+FW-4 for `ui_settings_*`, `ui_panel_settings`, WS6 XML rows, and every `src/plugin/` row in the inventory | 1 | 6, 7 |
| 6 | `refactor/overlay-sweep-rest` | same for the remaining overlays/panels (incl. widget catalog), in directory batches; may split into `-ams`, `-calibration`, `-print` sub-branches with disjoint file sets | 1 | 5 (disjoint files) |
| 7 | `refactor/home-widgets` | WS7 incl. `LuaPanelWidget` and runtime defs | Lua merge | 5, 6 |
| 8 | `refactor/observer-rename` | WS4 step 2 (script + docs) | 2, and after 5-7 land | nothing (touches ~150 files) |
| 9 | (later, #1329) | `push_overlay(root, lc, on_closed)` promoted from `PluginOverlayHost::push`, delete `register_overlay_instance` pairs, first `destroy_on_close()` flips | 5, 6, Lua merge | |

Branches 1-4 start as soon as Lua phase 2 is on main (hours away), in parallel. The sweeps (5, 6) are the long tail and the only ones that conflict
with each other, so their file sets are fixed up front and each is owned by one session.
Branch 8 is the last thing in the phase because it touches every observer user; running it as
a committed script means an in-flight peer branch re-runs it instead of resolving hunks.

Test decoupling (TB-1..TB-4) is the phase-2 prerequisite on the audit page. For this phase it
matters where a sweep deletes a member a `*TestAccess` shim reads (overlay `handle_*`
methods, cache members, `init_*` functions): the batch converts those tests to public inputs
in the same commit, rather than waiting for a tree-wide TestAccess pass.

### Totals (estimates, overlapping counts removed)

| Workstream | Lines | Binary (x86) |
|-----------|------:|-------------:|
| WS1 overlay lifecycle | -2,300 to -2,800 | small |
| WS2 trampolines | -2,500 | small |
| WS3 lookups | -800 | small |
| WS4 observers | -200 | -250 to -400 KB |
| WS5 defer | -530 | small |
| WS6 settings | -1,400 | -60 to -100 KB |
| WS7 widgets | -900 | small |
| **Total** | **~-8,600 to -9,100** | **~-0.3 to -0.5 MB** |

---

## 9. Decisions for the maintainer

1. **Dropped callbacks in WS5.** Moving 59 `async_call` + 9 `call_method` sites onto guarded
   defers means a callback whose owner died or deactivated is skipped instead of running on a
   stale `this`. Proposal: object-scoped guard (`object_lifetime_` / a new member) by default,
   screen-scoped only for pure painting, and `bg_cb` for every background-thread site. The
   alternative, keeping "always runs" by making those owners never-freed, hides the crash
   class instead of closing it.
2. **Observer type erasure and the rename.** Step 1 trades a per-site template body for one
   `std::function` indirection per notification and one more allocation per observer, on the
   hottest path in the UI, for an estimated 250-400 KB x86. Step 2 renames ~450 call sites
   across every panel. Proposal: land step 1 behind the old names with a hard kill criterion
   (<150 KB `.text` means revert to the naming fix only), and run the rename last as a
   committed script. Alternative: keep the old names forever as aliases and skip the churn.
3. **`find_required` fails the run.** Under `--test` and unit tests a missing required name
   aborts, and the static gate requires each name in EVERY layout variant. This will turn
   silent variant drift into red builds during the sweep. Proposal: accept that, classify each
   hit as XML fix or `find_optional`. Alternative: log-only everywhere, gate only.
4. **One sweep per file** (WS1+2+3+FW-4+FW-8 together) rather than one branch per finding:
   fewer conflict rounds, larger per-commit diffs to review.
5. **Telemetry column in WS6** keeps exactly today's 12 reported keys. Reporting the other
   ~58 settings would be a product and privacy change, not a refactor.
6. **Two overlay ownership shapes, not one.** `show()` owns singleton overlays;
   owner-held overlays (`PluginSettingsOverlay`, plugin XML overlays) keep an owner push
   because NavigationManager has one close callback per root. `show()` asserts the slot is
   free. The alternative, forcing plugin screens into `show()`, would move their deletion
   out of `PluginHost` and change the Lua unload ordering the phase-2 plan depends on.
7. **Exception guard on table lambdas** via a per-lambda static copy in `XmlCallbackEntry`
   (C++17 cannot default-construct a lambda). The alternative is dropping the try/catch for
   table callbacks, as `ui_probe_overlay.cpp` already does.

## Decisions (maintainer, 2026-09-30)

1. Deferrals: accepted. Object-scoped lifetime guards by default, `bg_cb` for background-thread sites. Sites whose callback must run even after the owner dies (e.g. `operation_guard_.end()`) get an explicit non-owner guard, reviewed per site.
2. Observer type erasure: accepted with the kill rule. Measure `.text` after `refactor/observer-core`; under 150KB saved, or any frame-time regression, reverts to the naming fix only.
3. `find_required()`: fail loudly in `--test` and unit tests, log and continue in release builds. The every-variant name gate is blocking.
4. Back button: the overlay sweep gives every overlay the standard pressed style and portrait chevron. A portrait screenshot contact sheet goes to the maintainer before merge.
5. Sequencing: branches touching overlays, settings rows or home widgets start after `feature/lua-plugins-phase2` is on main, and convert its PluginSettingsOverlay, PluginsOverlay, PluginOverlayHost and LuaPanelWidget in the same pass.
