# K-Touch multi-printer: live switch, add, remove (ESP32 firmware)

Point-in-time plan. Delete it in the change that ships the work. Desktop behaviour: `MULTI_PRINTER.md`.

Goal: one K-Touch that moves between your printers, one printer connected at a time. Today both
actions no-op: `NavigationManager::set_printer_callbacks` is called only from
`src/application/printer_session.cpp#init_ui`, which firmware excludes.

Approved design: **live switch** (sections 3-7), seconds and responsive, with `esp_restart()` as
the automatic fallback when the internal-heap check fails. Switching away from a printer that is
printing asks first (`modal_confirm` with `on_dismiss`).

## 1. How desktop does it

- **Switch** (`src/application/printer_session.cpp#switch_printer`): re-entrancy latch,
  `Config::set_active_printer` + `save`, `PrinterCacheRegistry::invalidate_all`, then the
  `Restart` hooks (teardown, rebuild, land_home) and a "Connected to X" toast.
- **Add** (`#add_printer_via_wizard`): inline loop picks an unused `printer-N`, adds
  `{wizard_completed:false}`, activates it, remembers the previous id, tears down, and `rebuild`
  runs the setup wizard. **Cancel** (`#cancel_add_printer_wizard`) removes it and restores.
- **Teardown/rebuild** (`#teardown_printer_scope`, `#rebuild`): destroys and re-creates every
  panel, subject and the XML shell, plus MoonrakerManager. Correct by construction, but heavy.
- **Remove** (`src/ui/ui_printer_list_overlay.cpp#handle_delete_printer`) and the badge menu are
  shared UI already in the firmware image; both end in `trigger_printer_switch`.

## 2. How firmware boots and connects today

`app_boot_ui()` in `firmware/helixscreen-esp32/components/helixapp/app_boot.cpp` runs once on the
UI pthread: Config -> theme/fonts/XML -> function-static `SubjectInitializer`, `MoonrakerManager`,
`JobQueueState` (never destroyed) -> `setup_discovery_callbacks_esp` -> `build_shell` ->
`app_net_start` (WiFi, then `mgr->connect` to the `df()` host). Its discovery callback is a trimmed
desktop pipeline: PrinterState hardware, fans, extruders, temp sensors, AMS backend, filament
sensors, ToolState, temp graph seed, initial status.

Firmware already reconnects to a *different* host live: Settings > Host
(`src/ui/ui_change_host_modal.cpp#show_change_host_modal`) does `disconnect` + `mgr->connect`, and
`EspMoonrakerClient::connect` stops and destroys the old WS client and starts a new one. So the
transport side of a live switch exists and is exercised. What it lacks is the per-printer reset.

## 3. What a live switch must reset (measured against the code)

Already replaced by the new printer's discovery or reconnect, nothing to add:
fans (`init_fans` rebuilds the map), extruders (`init_extruders`), temp sensors (`discover` clears),
filament sensors (`discover_sensors` clears), tools (`init_tools` clears), print-select list
(forced `refresh_files` on reconnect), job queue and print history (`observe_connection_staleness`
refetch), temp graph (`seed_from_moonraker` -> `refresh_all_from_history`).

Needs explicit work:
- **AMS**: `AmsState::init_backends_from_hardware` returns early when backends exist, so printer A's
  lanes survive. Call `AmsState::clear_backends()` before connecting. (Same leak in today's Change
  Host path, on both platforms; read from code, not reproduced.)
- **Per-printer config caches**: `PrinterCacheRegistry::invalidate_all()` (PrinterState capability
  overrides + excluded objects, PanelWidgetManager layouts).
- **Home grid**: `PanelWidgetManager::notify_config_changed("home")` when B's layout differs from A's;
  skip it when equal (the common default layout), since a home build is the slowest step on device.
- **Name**: `set_active_printer_name` from config (firmware never seeds it) and
  `PrinterNameSync::resolve` after discovery (firmware never calls it).
- **Navigation**: close overlays and land on Home (an AMS panel open on A must not show B).
- **Stale queued work**: anything A's WS task already queued (discovery completion, notifications)
  must run or be dropped *before* the reset, or it re-applies A after it.
- Residual: subjects for objects only A had keep A's last value; the capability flags set by B's
  `set_hardware` hide those widgets. Accepted, listed as a risk.

## 4. DRY decision

PrinterSession itself cannot compile into firmware (header pulls `hardware_setup_prompter`,
`gcode_response_routing`; the .cpp reaches plugins, history managers, `discovery_steps`, display
manager, wizard steps), and its rebuild-the-shell shape does not fit PSRAM (section 5). Split instead:

| Shared piece (compiled in both) | Callers |
|---|---|
| `PrinterSwitchFlow`, moved out of PrinterSession: latch, switch/add/cancel, `invalidate_all`, toast, `Restart` hooks. Uses only Config, PrinterCacheRegistry, ToastManager, `set_wizard_cancel_callback` (ui_wizard.cpp is in firmware) | PrinterSession (hooks = full teardown/rebuild), firmware (hooks = retarget below) |
| `Config::next_printer_id()` | the flow's add |
| `retarget_printer_connection(host, port)`: the section 3 reset + reconnect | firmware switch hooks, ChangeHostModal save on both platforms (fixes the AMS leak there too) |
| active-name seed helper | application.cpp, `PrinterSession::rebuild`, app_boot (replaces two desktop copies) |
| ChangeHostModal with a "new printer" target, plus an mDNS list | Settings > Host, Add printer |

Firmware-only: the hooks wiring in app_boot, the internal-heap check and the restart fallback.
Desktop keeps its full rebuild for switching; `retarget` is the lighter path it gains for Change Host.

## 5. Live switch sequence (firmware, as built)

Entry: badge menu or Printers list -> `trigger_printer_switch` -> `PrinterSwitchFlow::request_switch`
(asks first when the connected printer is printing, unless it was just deleted) -> `switch_printer`,
which saves the new active id before anything else; a failed save stays put. The flow then runs
its hooks synchronously, in whatever context the request arrived (usually an UpdateQueue batch):

1. teardown hook: stamps the start time.
2. rebuild hook: `retarget_printer_connection()`: suppress the recovery dialog, `disconnect()`
   (joins the WS task), `process_notifications()` so A's last frames apply before the reset,
   clear AMS backends, show B's name, then the connect gate (the K-Touch waits 20 ms for the idle
   task to free the old WS stack and needs a 10 KB internal block, restarting when it is not there),
   then `connect(B)`, which stores B's base URL and then advances the HTTP epoch. A connect that
   cannot start returns false and the hook restarts. The home grid rebuilds only when layouts
   differ (safe in a batch: `safe_clean_children`). A 30 s watchdog restarts a switch whose
   discovery never lands on a live connection.
3. land_home hook: `request_panel(Home, Queued)`, which hides every open overlay like a navbar tap.

There is no UpdateQueue drain. What A can still deliver after step 2, and why it is safe:
- Discovery queued from A's WS task carries A's HTTP epoch and is dropped on the UI thread.
- REST downloads that finish after the switch reach `on_error` as CONNECTION_LOST.
- JSON-RPC requests pending on A fail with connection_lost when the stop reports DISCONNECTED,
  or by timeout; replies cannot arrive once the socket is closed.
- `on_discovery_complete` fires from one place, the subscribe reply, behind a
  `connection_generation_` check; A's WS task is joined before `connect(B)` bumps that generation,
  and timeouts from `process_timeouts` deliver only error callbacks, so A's discovery cannot complete
  for B. The HTTP epoch read on the WS task drops anything already queued.
- The 30 s watchdog stands down while Change Host's Test has lent the client to another host
  (the client's URL is not `active_printer_ws_url()`).
- Success callbacks A queued before the disconnect (a file list, say) may still run once; B's
  forced refresh on connect replaces them. Names that resolve late go to the printer they were
  asked for.

## 6. Add printer on a 480x272 panel

One modal, reached from the badge menu's "Add printer" and from Settings > Connection > Printers:
- Top: "Found on your network", a tappable list of Moonraker instances (mDNS). One tap fills host
  and port and runs the connection test.
- Below: host and port fields (keyboard), for printers mDNS cannot see. `.local` names already
  resolve on firmware (`CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y`).
- Save becomes active after a passing test, then the flow's add creates `{moonraker_host, port}`
  under `next_printer_id()` and switches to it live. Nothing is written before Save, so Cancel
  undoes nothing but reconnecting the live client to the current printer (the test borrows it;
  today Cancel-after-Test leaves it on the tested host, a shared bug fixed here).
- The name fills itself from Mainsail/Fluidd/hostname (`PrinterNameSync`); rename stays in the
  existing printer manager overlay. No setup wizard: the K-Touch takes fans and heaters from discovery.

**mDNS on firmware: deferred.** The future route is the ESP-IDF `mdns` component as a second implementation behind `IMdnsDiscovery`. The shared browser (`src/network/mdns_discovery.cpp`
over the vendored `lib/mdns/mdns.h`) does not compile for the ESP32: the header uses
`sockaddr_in6`, `IPPROTO_IPV6` and `IPV6_JOIN_GROUP` throughout, AAAA parsing included, and the
firmware's lwIP has no IPv6. Options: enable `CONFIG_LWIP_IPV6` (internal-RAM cost, unmeasured),
guard IPv6 out of the vendored header (invasive), or that component. Meanwhile `.local` names typed into the modal resolve.

## 7. Memory plan

- **Internal SRAM (the binding constraint)**: a switch frees and re-takes exactly one 8 KB WS stack;
  WS buffers live in PSRAM (`SPIRAM_MALLOC_ALWAYSINTERNAL=0`). Evidence the block comes back: 44
  forced-reconnect cycles stable (`esp_moonraker_client.cpp`), and the live Change Host path. Pending:
  the ~04:20 Moonraker-restart test. Step 5's check makes the switch safe whichever way it lands.
- **If the block does not come back**, in order: (a) longer reap wait, then re-check; (b) patch
  `esp_websocket_client` to create its task with a PSRAM stack (`xTaskCreatePinnedToCoreWithCaps`);
  the WS task never touches flash, which is the rule for PSRAM stacks (unverified on this board);
  (c) reboot-to-switch as the shipping behaviour: about 20 s (reset to home 12.3 s, IP at ~17 s).
- **mDNS**: one PSRAM-stack thread and a 2 KB receive buffer, only while Add is open.
- **PSRAM**: AMS backends and WS buffers are freed and re-created; no shell rebuild, so no ~1.4 MB
  overlap against the 1.44 MB largest block. A home rebuild must delete before it builds (verify).
- App slot: a few tens of KB at most (flow move, mdns.h) against 1.47 MB free.

Measured on the device (f7, `systemctl restart moonraker` mid-run, 100 ms heap logger): a WS
teardown returns its block (~8.7 KB freed, largest 22.5 KB), and the reconnect lands at 38.6 KB
free / 15.4 KB largest with full discovery and a 98-object subscription; the WS stack peaked with
1.3 KB of 8 KB spare. At BOOT, right at "connected", the connect and discovery burst briefly drops
to 14.7 KB free / 7,680 B largest for ~340 ms; mid-run reconnects did not show it. So the 10 KB gate
passes in steady state, a transport that cannot start makes `retarget_printer_connection` return
false (restart fallback), and a switch whose discovery has not landed 30 s after a live connection
restarts into the printer; an unreachable printer stays on the normal reconnect loop.

Add after Test runs two 8 KB stack cycles: the modal returns the client to the current printer,
then the switch stops it again. A later optimisation can skip that reconnect when the add is about
to switch anyway; it is correct as is, and the connect gate covers both cycles.

## 8. Risks, tests, commits

Risks: R1 stale subjects for A-only objects (section 3). R2 (resolved) thumbnails are keyed by
printer. R3 (resolved) leaving a printing printer asks first. R4 (resolved) a REST reply from A that
finishes after the switch reaches its caller as CONNECTION_LOST (`http_request_epoch.h`). R5 home
rebuild time on device is the unknown that decides "seconds"; running it inside an UpdateQueue
batch (the add path) is safe, because `HomePanel` clears its grid with `safe_clean_children`. R6 a printer whose discovery crashes
the firmware bricks the panel until reflash: the firmware has no crash-restart tracking to build
on, so a boot crash counter (N crashes right after connecting to a just-switched-to printer: boot
the previous printer instead) is planned, not built.

Desktop unit tests: `PrinterSwitchFlow` with fake hooks (latch, unknown id, save before teardown,
add/cancel bookkeeping, hook order); `next_printer_id` (gaps, collisions);
`retarget_printer_connection` clears AMS backends and invalidates caches (mock client: A has AFC,
B none, after retarget no lanes); ChangeHostModal new-printer target writes no `df()` keys and Cancel
reconnects; existing `tests/unit/application/test_application_printer_switch.cpp` stays green.

Device checklist (f7): A to B to A five times, logging tap-to-connected time and both heap numbers
per switch against the cold-boot values; AMS lanes and fans match each printer; add via mDNS and
via typed IP; Cancel mid-add keeps A connected; remove the active printer; force the fallback by
lowering the margin and confirm the restart lands on B.

Landed on `feature/ktouch-multi-printer`: v1 restart switch, `next_printer_id`, shared name lookup,
Change Host fixes (Cancel-after-Test reconnect, AMS clear), add-printer modal mode,
`PrinterSwitchFlow` (request_switch with the printing confirm, the connected-printer id, save-failure
handling, add without duplicates), `retarget_printer_connection`, the firmware live hooks with the
10 KB internal-block check and the counted restart fallback, the HTTP reply epoch, and per-printer
thumbnail keys. Open: mDNS (deferred, section 6), the WS task PSRAM-stack patch (only if device
testing shows the block does not come back), the boot crash counter (R6).
