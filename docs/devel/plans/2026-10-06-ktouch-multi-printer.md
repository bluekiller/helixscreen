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

## 5. Live switch sequence (firmware)

Entry: badge menu or Printers list -> `trigger_printer_switch` -> `PrinterSwitchFlow::switch_printer`,
which saves the new active id first, so any later failure can fall back to a restart safely.
The hooks run from a one-shot `lv_timer`, outside any UpdateQueue batch, because both callers arrive
inside `queue_update` and `UpdateQueue::drain()` must not run inside a batch.

1. UI thread: show a non-blocking "Switching to <name>..." status (badge dot amber, home not-ready
   state, as on any reconnect). `EmergencyStopOverlay::suppress_recovery_dialog`. Land on Home.
2. `client->disconnect()`: `esp_websocket_client_stop` waits for the WS task's STOPPED bit, so after
   it returns that task enqueues nothing more. Bounded by `UI_STALL_BUDGET_MS` (3 s) plus DNS time.
3. `UpdateQueue::drain()` + `mgr->process_notifications()`: A's last queued work runs now, before
   the reset, instead of landing on B.
4. Reset (section 3): `clear_backends`, `invalidate_all`, name seed, home rebuild only if needed.
5. `vTaskDelay(pdMS_TO_TICKS(20))`: the WS task deleted itself, and its 8 KB stack is freed later by
   the idle task, which the UI thread outranks. Then check
   `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 8 KB + 2 KB margin`.
6. Pass: `mgr->connect(B)`; discovery on the WS task then queues the usual subject updates. Fail:
   log both heap numbers and `esp_restart()`; boot comes up on B because the entry step already saved it.

Expected time: steps 1-5 well under a second; connect to discovery about 2 s (measured at boot,
2026-09-27); plus a home rebuild only when layouts differ. So **about 2-3 s typical**, unmeasured.
The UI stays responsive throughout except during a home rebuild.

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

**mDNS on firmware: yes, by reusing the shared browser.** `src/network/mdns_discovery.cpp` (mjansson
`mdns.h` over BSD sockets, `IMdnsDiscovery` interface) runs on lwIP sockets in principle. Blockers to
clear: it spawns via `helix::make_thread` (sigaltstack, Linux-only) inside try/catch (firmware is
`-fno-exceptions`), so it needs a thread seam (pthread with a PSRAM stack, since
`SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`) and `exception_policy.h`; one more socket of
`LWIP_MAX_SOCKETS=10`; multicast join needs lwIP IGMP (default on, verify). Run it only while the
Add modal is open. Rejected: the ESP-IDF `mdns` component, a second browser plus a resident task.

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

## 8. Risks, tests, commits

Risks: R1 stale subjects for A-only objects (section 3). R2 thumbnail cache is keyed by relative path
only (`ThumbnailCache::compute_hash`), so same-named files on two printers share a thumbnail, on
both platforms. R3 a switch mid-print on A leaves A printing, which is correct but needs a confirm
("A is printing, switch anyway?"). R4 pending A requests time out after the switch and may toast.
R5 home rebuild time on device is the unknown that decides "seconds".

Desktop unit tests: `PrinterSwitchFlow` with fake hooks (latch, unknown id, save before teardown,
add/cancel bookkeeping, hook order); `next_printer_id` (gaps, collisions);
`retarget_printer_connection` clears AMS backends and invalidates caches (mock client: A has AFC,
B none, after retarget no lanes); ChangeHostModal new-printer target writes no `df()` keys and Cancel
reconnects; existing `tests/unit/application/test_application_printer_switch.cpp` stays green.

Device checklist (f7): A to B to A five times, logging tap-to-connected time and both heap numbers
per switch against the cold-boot values; AMS lanes and fans match each printer; add via mDNS and
via typed IP; Cancel mid-add keeps A connected; remove the active printer; force the fallback by
lowering the margin and confirm the restart lands on B.

Commits, in order (status as of this revision):
1. `Config::next_printer_id()` (landed) + move switch/add/cancel into `PrinterSwitchFlow`.
2. `retarget_printer_connection`; ChangeHostModal uses it. Landed already: Cancel-after-Test
   reconnects; a new host clears AMS backends (reproduced on desktop, `HELIX_MOCK_AMS=medusahc`).
3. Shared active-name seed and `PrinterNameSync::resolve` in firmware discovery (landed).
4. Firmware: `set_printer_callbacks` with retarget hooks, heap check, restart fallback, timing
   logs, printing confirm. A restart-only wiring is on the branch now and gets replaced.
5. ChangeHostModal add-printer mode + Add flow (landed).
6. mDNS on firmware (thread seam + exception policy) and the found-printers list.
7. WS task PSRAM-stack patch, only if device testing shows the 8 KB block does not come back.
