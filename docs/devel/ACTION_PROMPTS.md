# Action Prompts (Developer Guide)

How HelixScreen turns Klipper's `// action:prompt_*` console lines into an interactive modal, what
the parser accepts, and how to extend it.

Klipper macros raise a prompt by echoing protocol lines through `RESPOND`:

```
RESPOND TYPE=command MSG="action:prompt_begin Filament Change"
RESPOND TYPE=command MSG="action:prompt_text Please load filament"
RESPOND TYPE=command MSG="action:prompt_button Continue|RESUME|primary"
RESPOND TYPE=command MSG="action:prompt_show"
```

Moonraker forwards each echoed line to clients as `notify_gcode_response`. HelixScreen parses
them, shows an `ActionPromptModal`, and sends the tapped button's gcode back to Klipper. The
protocol itself is Klipper's: [G-Codes: action commands](https://www.klipper3d.org/G-Codes.html#action-commands).

---

## Key Files

| File | Purpose |
|------|---------|
| `include/action_prompt_manager.h` | `PromptButton`, `PromptData`, `ActionPromptManager` (parser + state machine, static cross-TU accessors) |
| `src/ui/action_prompt_manager.cpp` | Line parser, button-spec parser, command handlers, test prompt helpers |
| `include/action_prompt_modal.h` | `helix::ui::ActionPromptModal` (a `Modal` subclass) and `report_action_prompt_gcode_failure()` |
| `src/ui/action_prompt_modal.cpp` | Builds text labels and buttons from `PromptData`, button layout, color mapping, click handling |
| `ui_xml/action_prompt_modal.xml` | Dialog shell: title, scrollable text area, AFC fault diagram, button row, footer row |
| `src/application/application.cpp#init_action_prompt` | Wiring: creates manager and modal, registers the `notify_gcode_response` handler, sends button gcode |
| `src/application/application_sdl_shortcuts.cpp` | `A` and `N` keys raise a test prompt and a test notification in `--test` |
| `src/application/demo_overlays.cpp` | `ctl demo action-prompt-worst` and `action-prompt-many` |
| `src/printer/ams_backend_mock.cpp#execute_device_action` | Mock AFC calibration wizard that injects a full prompt sequence |
| `tests/unit/test_action_prompt.cpp` | Parser, button spec, state machine, callbacks, static accessors |
| `tests/unit/test_action_prompt_dismiss.cpp` | Empty-gcode button closes without sending |
| `tests/unit/test_action_prompt_modal_layout.cpp` | One-row vs wrapping button layout |
| `tests/unit/test_action_prompt_modal_stress.cpp` | Rapid show/hide, reuse across prompt shapes (hidden tag) |
| `tests/unit/test_action_prompt_gcode_failure.cpp` | Failed button gcode raises a toast |

The modal mechanics (backdrop, `ModalStack`, `on_show`/`on_hide`, the chrome height budget) are
the shared ones in [MODAL_SYSTEM.md](MODAL_SYSTEM.md) and are not repeated here.

---

## Architecture

```
Klipper macro: RESPOND TYPE=command MSG="action:prompt_..."
  |
  v
Moonraker  notify_gcode_response  ["// action:prompt_begin Filament Change", ...]
  |
  v  (WebSocket thread)
Application::init_action_prompt handler "action_prompt_manager"
  |  one process_line() per string in params
  v
ActionPromptManager
  |  parse_action_line()  -> {command, payload}
  |  parse_button_spec()  -> PromptButton
  |  IDLE -> BUILDING -> SHOWING -> IDLE, accumulates PromptData
  |
  +-- on_show(PromptData) --defer--> ActionPromptModal::show_prompt(lv_screen_active(), data)
  +-- on_close()          --defer--> ActionPromptModal::hide()
  +-- on_notify(text)     --queue--> ToastManager INFO toast, 5s
                                         |
                                         v  (main thread, button tap)
                          ActionPromptModal::handle_button_click(gcode)
                                         |  gcode callback, then hide()
                                         v
                          IMoonrakerAPI::execute_gcode(gcode, ..., MACRO_TIMEOUT_MS)
                                         |  on error
                                         v
                          report_action_prompt_gcode_failure() -> "Macro failed: ..." toast
```

### Protocol lines to the manager

The handler accepts both shapes Moonraker's `params` can take, an array of strings or an array
whose first element is the array of strings, and feeds every string to `process_line()`. Lines
that are not action lines are dropped by the parser, so the whole console stream goes through it.

`AmsState::set_gcode_response_callback()` is pointed at the same `process_line()`, which is how a
mock AMS backend injects prompt lines without a Moonraker connection. Both registrations are
undone in `src/application/application.cpp#teardown_printer_scope` before the manager is destroyed.
The teardown rule for `notify_gcode_response` handlers is in
[architecture/12-system-services.md](architecture/12-system-services.md).

### Parsing

`ActionPromptManager::parse_action_line()` (`src/ui/action_prompt_manager.cpp#parse_action_line`)
skips leading spaces and tabs, requires the exact prefix `// action:` (case sensitive), takes the
command up to the next space or tab, and keeps everything after exactly one separator as the
payload.

Some firmware omits the space between command and payload. FlashForge/ZMOD sends
`// action:prompt_beginResumption interrupted!!!`. The parser recovers that by matching the token
against the known commands, longest first:

```cpp
// Every command the protocol defines, LONGEST FIRST so that a prefix match can
// never pick "prompt_button" out of "prompt_button_group_start".
constexpr std::string_view ACTION_COMMANDS[] = {
    "prompt_button_group_start",
    "prompt_button_group_end",
    "prompt_footer_button",
    "prompt_button",
    "prompt_begin",
    "prompt_text",
    "prompt_show",
    "prompt_end",
    "notify",
};
```

A glued remainder that starts with `_` is not split, since that looks like a command this build
does not know rather than a title. A recovered line logs a warning at `[ActionPrompt]`.

`parse_button_spec()` (`src/ui/action_prompt_manager.cpp#parse_button_spec`)
splits on `|`:

| Spec | label | gcode | color | hex_color |
|------|-------|-------|-------|-----------|
| `OK` | `OK` | `OK` | empty | empty |
| `Continue\|RESUME` | `Continue` | `RESUME` | empty | empty |
| `Cancel\|CANCEL_PRINT\|error` | `Cancel` | `CANCEL_PRINT` | `error` | empty |
| `Retry\|\|warning` | `Retry` | `Retry` | `warning` | empty |
| `_\|CHANGE_ZCOLOR SLOT=1 ...\|primary\|F72224` | empty | `CHANGE_ZCOLOR SLOT=1 ...` | `primary` | `F72224` |

An empty gcode field falls back to the label, which is Klipper's convention. The fourth field is
a ZMOD extension for color-grid tiles: a hex color that overrides the named color, and a label made
only of `_`, `-` and spaces is cleared so the swatch is not painted with a placeholder.

### State machine

`ActionPromptManager` holds one `PromptData` and a `State`:

| State | Meaning |
|-------|---------|
| `IDLE` | No prompt. Only `prompt_begin` and `notify` do anything |
| `BUILDING` | After `prompt_begin`. Text, buttons and groups accumulate |
| `SHOWING` | After `prompt_show`. `on_show` has fired |

Rules worth knowing before writing a macro or a test:

- `prompt_text`, `prompt_button`, `prompt_footer_button` and the group directives are ignored
  unless the state is `BUILDING`. A button sent after `prompt_show` never appears.
- `prompt_show` without a preceding `prompt_begin` is ignored.
- `prompt_begin` while `SHOWING` fires `on_close` first, then starts the new prompt. That is how a
  multi-step macro replaces one dialog with the next.
- `prompt_end` from `BUILDING` discards the half-built prompt without any callback. From `SHOWING`
  it fires `on_close`. From `IDLE` it does nothing.
- `notify` is independent of the prompt state.
- Group IDs come from a counter that is not reset between prompts, so a `group_id` is unique for
  the life of the manager, not per prompt.

### Modal UI

`on_show` and `on_close` arrive on the WebSocket thread, so `init_action_prompt` hops both to the
main thread through `m_async_lifetime.defer()`. The prompt is copied into the deferred lambda.

`ActionPromptModal::show_prompt()` stores the data and calls `Modal::show()`; `on_show()` runs
`populate_content()`, which sets the title through `kModalTitleWidgetName` (the name the
duplicate-title toast suppression looks up), shows `icon_error` only when `severity == "error"`,
adds one wrapping label per text line, and builds the buttons. One `ActionPromptModal` instance is
reused for every prompt; `on_hide()` removes the button event callbacks and frees their
`ButtonCallbackData`.

Button layout (`src/ui/action_prompt_modal.cpp#create_buttons`):

- Regular buttons go in `button_container`. With three or fewer, each is content-sized and the
  container wraps (`row_wrap`).
- With four or more, they become equal-width cells on one non-wrapping row, but only if every
  label fits its share of the measured width (`equal_width_row_fits`). Otherwise they fall back to
  wrapping, which never clips.
- Footer buttons go in `footer_container`, full height, flex-grow 1, with a 1px divider between
  them. The footer and its divider are hidden when there are no footer buttons.
- Empty text and empty regular-button sections hide their containers.

Named colors map to theme tokens in `src/ui/action_prompt_modal.cpp#get_button_color`:

| Klipper color | Theme token |
|---------------|-------------|
| `primary` or empty | `primary` |
| `secondary` | `success` |
| `info` | `info` |
| `warning` | `warning` |
| `error` | `danger` |
| anything else | `primary` (logged at debug) |

The label color comes from `theme_manager_get_contrast_adjusted_text()` against the final
background, so hex-colored tiles stay readable.

### Button gcode back to the printer

A tap checks the modal's `LifetimeToken` (a click queued after the modal was destroyed is
dropped), plays `button_tap`, and calls `handle_button_click()`:

```cpp
void ActionPromptModal::handle_button_click(const std::string& gcode) {
    // An empty gcode is a dismiss affordance: close, send nothing. Callers no
    // longer have to smuggle a Klipper comment ("; error-dismiss") through to
    // get a button that does nothing (#1172).
    if (gcode.empty()) {
        spdlog::info("[ActionPromptModal] Dismiss button clicked (no gcode)");
        hide();
        return;
    }

    spdlog::info("[ActionPromptModal] Button clicked, gcode: {}", gcode);

    // Call the gcode callback if set
    if (gcode_callback_) {
        gcode_callback_(gcode);
    }

    // Close the modal
    hide();
}
```

Wire prompts never reach the empty-gcode branch, because the parser already substituted the
label. Only `PromptData` built in C++ uses it.

The callback installed by `init_action_prompt` sends with `IMoonrakerAPI::MACRO_TIMEOUT_MS`
instead of the default request timeout, since prompt buttons routinely start long macros (heat,
cut, purge). Its error callback calls `report_action_prompt_gcode_failure()`, which raises
`Macro failed: <Klipper's message>`. That error callback is also what makes the request
caller-handled, so the `!!` GcodeError toast for the same rejection is suppressed; see
[RPC_ERROR_OWNERSHIP.md](RPC_ERROR_OWNERSHIP.md).

The modal closes locally on every tap and the manager is not told. It stays `SHOWING` until the
firmware sends `prompt_end` or the next `prompt_begin`. Backdrop tap and ESC (from the `Modal`
base) behave the same way. A well-formed macro ends its prompt from the button gcode, for example
`RESPOND TYPE=command MSG="action:prompt_end"` followed by the real command.

### Other code that reads prompt state

The static accessors let other translation units ask about the firmware prompt without owning the
manager. `Application` registers the instance with `set_instance()` and clears it in `teardown_printer_scope`.

| Accessor | Used by | For |
|----------|---------|-----|
| `ActionPromptManager::is_showing()` | `src/printer/ams_backend_afc.cpp`, `src/ui/recovery_modal_presenter.cpp#present` | Whether a firmware prompt is up |
| `ActionPromptManager::current_prompt_name()` | same | The title, e.g. AFC suppresses its toasts while a title containing `AFC` is showing |
| `ActionPromptManager::dismiss_active()` | `RecoveryModalPresenter::present` | Closes a firmware prompt that a backend's `duplicates_firmware_prompt()` claims, once the recovery modal covers it. Main thread only |

`ActionPromptModal` is also the renderer for HelixScreen's own recovery dialogs:
`RecoveryModalPresenter` owns a separate instance and feeds it `PromptData` from
`build_recovery_prompt()`. Those dialogs never go through the manager. The pipeline is in
[FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) and the AFC fault diagram inside this modal is in
[FILAMENT_BACKEND_AFC.md](FILAMENT_BACKEND_AFC.md).

The AD5X IFS backend reads `action:prompt_button` lines independently, from its own gcode response
listener, to pick up slot colors from ZMOD's color menu. That is a separate consumer of the same
lines; see [FILAMENT_BACKEND_AD5X_IFS.md](FILAMENT_BACKEND_AD5X_IFS.md).

---

## Supported Directives

| Directive | Payload | Effect |
|-----------|---------|--------|
| `prompt_begin` | title | Starts a prompt, closing any prompt on screen |
| `prompt_text` | one line | Adds a wrapping text line (an empty payload gives a blank line) |
| `prompt_button` | button spec | Adds a regular button |
| `prompt_footer_button` | button spec | Adds a footer button |
| `prompt_button_group_start` | none | Following buttons get a shared `group_id` |
| `prompt_button_group_end` | none | Ends the group |
| `prompt_show` | none | Shows the modal |
| `prompt_end` | none | Closes the modal |
| `notify` | message | 5 second INFO toast, independent of any prompt |

Not supported:

- **Button group layout.** Groups are parsed and stored in `PromptButton::group_id`, but the modal
  does not read it. Grouped buttons flow in the same container as ungrouped ones.
- **Any other `action:` command**, including OctoPrint-style `action:pause`, `action:resume` and
  `action:cancel`, and any unrecognised `prompt_*`. These log `unknown command` at debug and are
  dropped.
- **Severity from the wire.** `PromptData::severity` (the red error icon) is only set by C++
  callers such as the recovery presenter. No directive sets it.
- **Telling the firmware the user closed the dialog.** A backdrop tap, ESC or button tap closes the
  modal without sending `prompt_end` or anything else.

---

## Testing and Mock Knobs

```bash
make t F='[action_prompt]'                         # parser, state machine, layout, dismiss, failure toast
./build/bin/helix-tests '[action_prompt][stress]'  # stress cases are hidden behind [.ui_integration]
```

The cases in `tests/unit/test_action_prompt.cpp` whose names start with `ActionPromptModal:` check
`PromptData` and the manager, not the modal. Modal behavior is covered by the layout, dismiss and
stress files.

In a running mock (`--test`):

| How | What you get |
|-----|--------------|
| `A` key (SDL window, test mode) | `trigger_test_prompt()`: all five colors, a Yes/No group and a Cancel footer button. Each button sends `RESPOND msg="..."` |
| `N` key (SDL window, test mode) | `trigger_test_notify()`: an `action:notify` toast |
| `HELIX_MOCK_AMS=afc`, then the AFC device action "Run Calibration Wizard" | The mock backend replays an `AFC Calibration` prompt through `AmsState`'s gcode response callback, with four grouped lane buttons, a "Calibrate All" button and a Cancel footer |
| `helix-screen ctl -s "$HELIX_SOCK" demo action-prompt-worst` | Tallest shape: AFC diagram, three wrapping text lines, three buttons and a footer, error severity. Used for the chrome budget in `ui_xml/action_prompt_modal.xml` |
| `helix-screen ctl -s "$HELIX_SOCK" demo action-prompt-many` | Seven long-label buttons that must wrap |

The `ctl demo` screens construct `PromptData` directly and skip the manager. `HELIX_MOCK_AMS` is
documented in [MOCK_ENVIRONMENT_VARIABLES.md](MOCK_ENVIRONMENT_VARIABLES.md); `ctl` in
[HELIXCTL.md](HELIXCTL.md).

Logs to grep with `-vv`: `[ActionPrompt]` (wiring, sends, failures), `[ActionPromptModal]`
(show, buttons, clicks), `ActionPromptManager: command=` (every parsed line, debug).

---

## Extending

### Adding a directive

1. Add the command to `ACTION_COMMANDS` in `src/ui/action_prompt_manager.cpp`, keeping the list
   longest first, or glued-command recovery can match the shorter prefix.
2. Add a branch in `ActionPromptManager::process_line()` and a private `handle_*` method. Decide
   which states it is valid in and ignore it elsewhere, like the existing handlers.
3. If it carries data the modal needs, add a field to `PromptData` or `PromptButton` and read it in
   `ActionPromptModal::populate_content()` or `create_button()`. `PromptButton` is built with
   positional aggregate initialisation in tests and `demo_overlays.cpp`, so add new fields at the
   end.
4. Test the parse and the state rules in `tests/unit/test_action_prompt.cpp`, and anything visible
   in `test_action_prompt_modal_layout.cpp`, which builds a real modal.

### Adding a button color

Add the name to `ActionPromptModal::get_button_color()` and map it to an existing theme token. Do
not hardcode a color; the hex field already covers firmware-specified colors.

### Showing a prompt from C++

Do not feed fake `// action:` lines into the shared manager. That puts a HelixScreen dialog into
the firmware's prompt state, where `prompt_end` from Klipper would close it. Own an
`ActionPromptModal` and call `show_prompt()` with your own `PromptData`, as
`RecoveryModalPresenter` does. Give it a gcode callback, use an empty gcode for a pure dismiss
button, and pass `severity = "error"` for the error icon.

### Reacting to a firmware prompt

Use `ActionPromptManager::is_showing()` and `current_prompt_name()`. They are safe to call from
any thread but read without a lock, so treat them as hints. If your feature replaces a backend's
own firmware dialog, implement `AmsBackend::duplicates_firmware_prompt()` rather than calling
`dismiss_active()` yourself.
