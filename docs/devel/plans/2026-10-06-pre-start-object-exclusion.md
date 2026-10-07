# Pre-start Object Exclusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In a file's print details view the user picks objects to skip before pressing Print; once Moonraker confirms the start, each pick is sent as `EXCLUDE_OBJECT NAME=...`, so those objects never print and print status lists them as Excluded from the first layer.

**Architecture:** `PrinterExcludedObjectsState` stays the only object model. Print details owns a private instance (`init_subjects(false)`) filled from the file scan, and later from the 2D/3D parse; its excluded set holds the pending picks. The side list and map view take a state pointer plus an `on_object_tapped(name)` callback, and one `ExcludeModeController`, extracted from `PrintStatusPanel::show_exclude_map_view` / `hide_exclude_map_view`, drives exclude mode in both panels. The skip button becomes one shared XML component. Picks are captured at the Print tap and sent after Moonraker confirms the start by wrapping the existing "start confirmed" callback, so every start path gets them.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml (XML components, subjects), Catch2 (`tests/unit`), MoonrakerClientMock / MoonrakerAPIMock, `helix-screen ctl` for mock verification.

**Spec:** `docs/devel/plans/2026-10-06-pre-start-object-exclusion-design.md` (approved; read it alongside this plan).

## Global Constraints

- "Klipper clears `exclude_object` state when a print starts (`virtual_sdcard:reset_file`), so exclusions cannot be sent before the start. They are sent right after Moonraker confirms it."
- "No file rewrite, so no HelixPrint plugin dependency. Works on every printer with `[exclude_object]`, the Snapmaker U1 included."
- "DRY: print status and print details share one implementation of exclude mode. No parallel "pending objects" model, no second parser, no copied panel wiring."
- Exclusions go through the existing `api->exclude_object(name)` (15 minute silent timeout). A TIMEOUT error is advisory, the same rule `PrintExcludeObjectManager::on_exclude_rpc_error` applies.
- Skip button: "shown only when the printer has `[exclude_object]` and the file defines at least 2 objects". Hidden for 3MF and for files without definitions. Same icon (`icon_debug_step_over`), same top-left corner of the preview as print status.
- "Names are compared upper-case, as Klipper stores them."
- Queued start: toast exactly `Object picks apply only to prints started now`, and queue without picks.
- Every object picked: Print is refused with a toast, before any heating.
- Reprint (print status, history) never carries picks.
- Picks survive a failed start (Preston, 2026-10-06). Leaving details because Print was tapped does NOT clear them; they clear only once Moonraker confirms the start, or when the user leaves the file (back to the list, or another file). A start that fails before printing returns to details with the same objects still picked. This overrides the `NavigateAway` clear in Task 7 for the print-start hide, and Task 10 owns the test: Print with a pick, mock `printer.print.start` returning an error, details re-shown with the pick intact and the skip button's count unchanged; then the success case clears it.
- The object list covers the Print button in details, as it covers controls on print status; the user closes it (X) before pressing Print (Preston, 2026-10-06). No Print button inside the list.
- Repo rules: spdlog only; SPDX header on every new source file; declarative UI (appearance in XML, data in C++; `scripts/check_imperative_ui.py` and `scripts/check_hardcoded_pixels.py` counts must not rise); `observe<V>` always gets the owner's `SubjectLifetime`; no synchronous widget deletion inside queued callbacks; no exceptions, RTTI or `std::regex` in firmware-compiled code; `#include "hv/json.hpp"` for JSON; every new `src/**/*.cpp` goes into `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` and `python3 scripts/check_esp32_app_srcs.py` passes; every new user string goes through `lv_tr()` / `translation_tag`, then `make translation-sync` and `make translations`, committing `translations/*.yml` and `ui_xml/translations/*.xml`; no comment archaeology; commit bodies carry a `mutation: ...` line; `git show --stat HEAD` after every commit.

## Review Focus

1. **Object names Klipper cannot be sent.** A slicer that writes `NAME='Part 1'`, a name with non-ASCII letters, or one with quotes: the user taps it, and the app must refuse the pick on the spot with a toast naming it, not start the print and fail afterwards. Tests: Task 1 (quoted names parse whole), Task 7 (pick refused with toast).
2. **A scan window that cuts a definition mid-line, or ends before the last definitions.** A 200 KB partial read can stop inside `EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1_co`; a bogus truncated object must never be listed, and definitions past the window must still appear once the full parse lands, keeping the scan's numbering. Tests: Task 1 (unterminated tail ignored), Task 2 (past-window file scans empty), Task 7 (merge keeps scan order and appends parsed extras without case duplicates).
3. **Switching files, or a USB copy finishing, while a start is in flight.** Picks belong to the file tapped. A copy that lands after the user opened another file, or a confirmation that arrives after they did, must send the original picks and leave the new file's details clean. Test: Task 10.
4. **The start confirmation arriving after the details view or the panel is gone.** The send must hold nothing but the names, and must reach whatever API is current then (or report the failure if there is none), without touching a destroyed view. Test: Task 9.
5. **An exclusion failing because the print was cancelled first.** A user who cancels during `PRINT_START` must not then see "Could not skip ..." for objects of a print that no longer exists. Test: Task 9.

Two further traps get their own tests in their owning tasks: two side lists alive at once (print status keeps its overlay cached) must each close only themselves (Task 4), and re-opening the same file, where the cached scan answers without calling `on_scan_answered`, must still show the objects (Task 7).

---

## File Structure

| File | Responsibility |
|------|----------------|
| `include/gcode_parser.h`, `src/rendering/gcode_parser.cpp` | Shared free functions: `gcode_param_value`, `parse_exclude_object_define`, `collect_exclude_object_defines`; `GCodeParser` delegates to them |
| `include/gcode_ops_detector.h` | `ScanResult::objects` |
| `src/ui/ui_print_preparation_manager.cpp` | The details scan fills `ScanResult::objects` from the downloaded head |
| `include/printer_excluded_objects_state.h`, `src/printer/printer_excluded_objects_state.cpp` | `make_object_info()`: one rule for bbox-from-polygon |
| `include/ui_exclude_object_badges.h` | `ObjectTapFn`, `ExcludeTapMode` |
| `include/ui_exclude_object_side_list.h`, `src/ui/ui_exclude_object_side_list.cpp`, `ui_xml/components/exclude_object_row.xml` | State + tap callback + tap mode; per-instance close routing |
| `include/ui_exclude_object_map_view.h`, `src/ui/ui_exclude_object_map_view.cpp` | State + tap callback + tap mode |
| `include/ui_exclude_mode_controller.h`, `src/ui/ui_exclude_mode_controller.cpp` (new) | The one exclude-mode implementation both panels own |
| `include/ui_gcode_viewer.h`, `src/ui/ui_gcode_viewer.cpp`, `src/ui/gcode_viewer_input.cpp`, `src/ui/gcode_viewer_state.h` | Excluded badges can be made pickable (toggle mode) |
| `ui_xml/components/exclude_objects_button.xml` (new) | The shared skip button with optional count badge |
| `include/pre_start_exclude.h`, `src/ui/pre_start_exclude.cpp` (new) | Pure pick rules + the after-start send |
| `include/ui_print_select_detail_view.h`, `src/ui/ui_print_select_detail_view.cpp`, `ui_xml/print_file_detail.xml` | The private state, picks, button, exclude mode |
| `include/ui_panel_print_status.h`, `src/ui/ui_panel_print_status.cpp`, `ui_xml/components/print_status_preview_card.xml` | Uses the controller and the shared button |
| `include/ui_panel_print_select.h`, `src/ui/ui_panel_print_select.cpp`, `include/ui_print_start_controller.h`, `src/ui/ui_print_start_controller.cpp` | Start-path wiring |
| `docs/devel/EXCLUDE_OBJECTS.md` | Durable documentation of the feature |

---

### Task 0: Worktree and claims

**Files:** none (environment only)

**Interfaces:**
- Consumes: main at or after the commit that adds this plan file.
- Produces: `.worktrees/pre-start-exclude` on branch `feature/pre-start-exclude`, claimed.

- [ ] **Step 1: Check for a name collision and a live claim before creating anything**

```bash
cd /home/pbrown/Code/Printing/helixscreen
ls .worktrees/ | grep -x pre-start-exclude || echo "free"
scripts/helix-claim check worktree:pre-start-exclude
git log --oneline -3
```
Expected: `free`, `FREE`, and the plan commit in the log. If either is taken, stop and message the holder (CLAUDE.md "Sharing This Tree").

- [ ] **Step 2: Create the worktree and claim it**

```bash
cd /home/pbrown/Code/Printing/helixscreen
scripts/setup-worktree.sh feature/pre-start-exclude
scripts/helix-claim take worktree:pre-start-exclude "pre-start object exclusion"
```
Expected: the script ends with a successful build. Every later step runs inside `/home/pbrown/Code/Printing/helixscreen/.worktrees/pre-start-exclude` (use `(cd <tree> && ...)` subshells when agents run in parallel).

- [ ] **Step 3: Announce the branch**

Send one line to peers listed by `scripts/helix-claim list` that touch print status, print select or the gcode viewer: branch name `feature/pre-start-exclude`, files it will touch (the File Structure table above).

---

### Task 1: One shared EXCLUDE_OBJECT_DEFINE parser

**Files:**
- Modify: `include/gcode_parser.h` (free functions after `struct ParsedGCodeFile`, inside `namespace helix::gcode`)
- Modify: `src/rendering/gcode_parser.cpp` (`GCodeParser::parse_exclude_object_command`, `GCodeParser::extract_string_param`)
- Test: `tests/unit/test_gcode_parser.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces (namespace `helix::gcode`):
  - `std::optional<std::string> gcode_param_value(std::string_view line, std::string_view key);`
  - `std::optional<GCodeObject> parse_exclude_object_define(std::string_view line);`
  - `std::vector<GCodeObject> collect_exclude_object_defines(std::string_view content);`

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_gcode_parser.cpp`)

```cpp
TEST_CASE("parse_exclude_object_define reads a slicer DEFINE line",
          "[gcode][parser][pre_start_exclude]") {
    using helix::gcode::parse_exclude_object_define;
    auto obj = parse_exclude_object_define(
        "EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1_copy_0 CENTER=-36.362,6.5 "
        "POLYGON=[[-40,2],[-32,2],[-32,11],[-40,11]]");
    REQUIRE(obj.has_value());
    CHECK(obj->name == "Cube_id_1_copy_0");
    CHECK(obj->center.x == Approx(-36.362f));
    CHECK(obj->center.y == Approx(6.5f));
    REQUIRE(obj->polygon.size() == 4);
    CHECK(obj->polygon[2] == glm::vec2(-32.0f, 11.0f));

    CHECK_FALSE(parse_exclude_object_define("EXCLUDE_OBJECT_START NAME=Cube").has_value());
    CHECK_FALSE(parse_exclude_object_define("EXCLUDE_OBJECT_DEFINE CENTER=1,2").has_value());
    CHECK_FALSE(parse_exclude_object_define("G1 X10 Y10").has_value());
}

TEST_CASE("parse_exclude_object_define keeps a quoted name whole",
          "[gcode][parser][pre_start_exclude]") {
    using helix::gcode::parse_exclude_object_define;
    auto dq = parse_exclude_object_define("EXCLUDE_OBJECT_DEFINE NAME=\"Part 1\" CENTER=5,6");
    REQUIRE(dq.has_value());
    CHECK(dq->name == "Part 1");
    CHECK(dq->center == glm::vec2(5.0f, 6.0f));

    auto sq = parse_exclude_object_define("EXCLUDE_OBJECT_DEFINE NAME='Part 2' CENTER=7,8");
    REQUIRE(sq.has_value());
    CHECK(sq->name == "Part 2");
    CHECK(sq->center == glm::vec2(7.0f, 8.0f));
}

TEST_CASE("collect_exclude_object_defines keeps file order and ignores a cut final line",
          "[gcode][parser][pre_start_exclude]") {
    const std::string content = "; header\n"
                                "EXCLUDE_OBJECT_DEFINE NAME=Zed CENTER=30,30\n"
                                "EXCLUDE_OBJECT_DEFINE NAME=Alpha CENTER=90,30 ; slicer note\n"
                                "G28\n"
                                "EXCLUDE_OBJECT_DEFINE NAME=Mid CENTER=150,30\n"
                                "EXCLUDE_OBJECT_DEFINE NAME=Cut_of"; // the read stopped here
    const auto objs = helix::gcode::collect_exclude_object_defines(content);
    REQUIRE(objs.size() == 3);
    CHECK(objs[0].name == "Zed");
    CHECK(objs[1].name == "Alpha");
    CHECK(objs[2].name == "Mid");
    CHECK(helix::gcode::collect_exclude_object_defines("G28\nG1 X1\n").empty());
}

TEST_CASE("collect_exclude_object_defines: a redefinition replaces the object in place",
          "[gcode][parser][pre_start_exclude]") {
    const auto objs = helix::gcode::collect_exclude_object_defines(
        "EXCLUDE_OBJECT_DEFINE NAME=Zed CENTER=1,1\n"
        "EXCLUDE_OBJECT_DEFINE NAME=Alpha CENTER=5,5\n"
        "EXCLUDE_OBJECT_DEFINE NAME=Zed CENTER=2,2\n");
    REQUIRE(objs.size() == 2);
    CHECK(objs[0].name == "Zed");
    CHECK(objs[0].center == glm::vec2(2.0f, 2.0f));
    CHECK(objs[1].name == "Alpha");
}

TEST_CASE("GCodeParser keeps a quoted object name whole across DEFINE and START",
          "[gcode][parser][pre_start_exclude]") {
    helix::gcode::GCodeParser parser;
    parser.parse_line("EXCLUDE_OBJECT_DEFINE NAME='Part 1' CENTER=10,10");
    parser.parse_line("EXCLUDE_OBJECT_START NAME='Part 1'");
    parser.parse_line("G1 X10 Y10 Z0.2 E1");
    parser.parse_line("G1 X20 Y10 E2");
    parser.parse_line("EXCLUDE_OBJECT_END NAME='Part 1'");
    auto file = parser.finalize();
    REQUIRE(file.objects.count("Part 1") == 1);
    REQUIRE_FALSE(file.layers.empty());
    REQUIRE_FALSE(file.layers[0].segments.empty());
    CHECK(file.get_object_name(file.layers[0].segments[0].object_name_index) == "Part 1");
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make t F='[pre_start_exclude]'`
Expected: FAIL to compile (`parse_exclude_object_define` / `collect_exclude_object_defines` not declared).

- [ ] **Step 3: Declare the free functions** (`include/gcode_parser.h`, inside `namespace gcode`, after `struct ParsedGCodeFile`; add `#include <string_view>`)

```cpp
/// Value of KEY=value in one G-code command line. A quoted value ('a b' or
/// "a b") comes back without its quotes, the way Klipper's parameter parser
/// reads it. nullopt when the key is absent or has no value.
std::optional<std::string> gcode_param_value(std::string_view line, std::string_view key);

/// One EXCLUDE_OBJECT_DEFINE line as Klipper reads it (comment ignored);
/// nullopt for any other line or a DEFINE without NAME.
std::optional<GCodeObject> parse_exclude_object_define(std::string_view line);

/// Every complete EXCLUDE_OBJECT_DEFINE in @p content, in file order. A line
/// with no terminating newline is ignored: a partial read can stop mid-name.
/// A name defined twice keeps its first position and its last geometry.
std::vector<GCodeObject> collect_exclude_object_defines(std::string_view content);
```

- [ ] **Step 4: Implement them and route `GCodeParser` through them** (`src/rendering/gcode_parser.cpp`; add `#include "text_io.h"` if absent)

Add inside `namespace helix::gcode`, before `GCodeParser::parse_exclude_object_command`, moving the CENTER/POLYGON parsing body out of the method unchanged:

```cpp
std::optional<std::string> gcode_param_value(std::string_view line, std::string_view key) {
    std::string needle(key);
    needle += '=';
    const size_t pos = line.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t start = pos + needle.size();
    if (start >= line.size()) {
        return std::nullopt;
    }
    const char quote = line[start];
    if (quote == '"' || quote == '\'') {
        const size_t close = line.find(quote, start + 1);
        if (close == std::string_view::npos || close == start + 1) {
            return std::nullopt;
        }
        return std::string(line.substr(start + 1, close - start - 1));
    }
    const size_t end = line.find_first_of(" \t", start);
    return std::string(line.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                         : end - start));
}

std::optional<GCodeObject> parse_exclude_object_define(std::string_view line) {
    line = helix::text_io::trim(line.substr(0, line.find(';')));
    constexpr std::string_view kDefine = "EXCLUDE_OBJECT_DEFINE";
    if (line.substr(0, kDefine.size()) != kDefine) {
        return std::nullopt;
    }
    auto name = gcode_param_value(line, "NAME");
    if (!name) {
        return std::nullopt;
    }

    GCodeObject obj;
    obj.name = std::move(*name);

    if (auto center_str = gcode_param_value(line, "CENTER")) {
        const size_t comma = center_str->find(',');
        if (comma != std::string::npos) {
            auto [px, ecx] = parse_gcode_decimal(center_str->data(), center_str->data() + comma,
                                                 obj.center.x);
            auto [py, ecy] =
                parse_gcode_decimal(center_str->data() + comma + 1,
                                    center_str->data() + center_str->size(), obj.center.y);
            if (ecx != std::errc{} || ecy != std::errc{}) {
                spdlog::debug("[GCode Parser] Failed to parse CENTER for object: {}", obj.name);
            }
        }
    }

    if (auto polygon = gcode_param_value(line, "POLYGON")) {
        std::string polygon_str = std::move(*polygon);
        polygon_str.erase(std::remove_if(polygon_str.begin(), polygon_str.end(), ::isspace),
                          polygon_str.end());
        size_t pos = (!polygon_str.empty() && polygon_str[0] == '[') ? 1 : 0;
        while (pos < polygon_str.length()) {
            if (polygon_str[pos] != '[') {
                pos++;
                continue;
            }
            pos++;
            const size_t comma = polygon_str.find(',', pos);
            if (comma == std::string::npos) {
                break;
            }
            float x = 0, y = 0;
            auto [px, ecx] =
                parse_gcode_decimal(polygon_str.data() + pos, polygon_str.data() + comma, x);
            if (ecx != std::errc{}) {
                break;
            }
            pos = comma + 1;
            const size_t close = polygon_str.find(']', pos);
            if (close == std::string::npos) {
                break;
            }
            auto [py, ecy] =
                parse_gcode_decimal(polygon_str.data() + pos, polygon_str.data() + close, y);
            if (ecy != std::errc{}) {
                break;
            }
            obj.polygon.push_back(glm::vec2(x, y));
            pos = close + 1;
        }
    }
    return obj;
}

std::vector<GCodeObject> collect_exclude_object_defines(std::string_view content) {
    const size_t last_newline = content.rfind('\n');
    content = last_newline == std::string_view::npos ? std::string_view{}
                                                     : content.substr(0, last_newline + 1);
    std::vector<GCodeObject> out;
    for (std::string_view line : helix::text_io::lines(content)) {
        auto obj = parse_exclude_object_define(line);
        if (!obj) {
            continue;
        }
        auto same = std::find_if(out.begin(), out.end(),
                                 [&](const GCodeObject& o) { return o.name == obj->name; });
        if (same != out.end()) {
            *same = std::move(*obj);
        } else {
            out.push_back(std::move(*obj));
        }
    }
    return out;
}
```

Replace the DEFINE branch of `GCodeParser::parse_exclude_object_command`:

```cpp
    if (line.find("EXCLUDE_OBJECT_DEFINE") == 0) {
        auto obj = parse_exclude_object_define(line);
        if (!obj) {
            return false;
        }
        spdlog::trace("[GCode Parser] Defined object: {} at ({}, {})", obj->name, obj->center.x,
                      obj->center.y);
        objects_[obj->name] = std::move(*obj);
        return true;
    }
```

Replace the body of `GCodeParser::extract_string_param`:

```cpp
bool GCodeParser::extract_string_param(const std::string& line, const std::string& param,
                                       std::string& out_value) {
    auto value = gcode_param_value(line, param);
    if (!value) {
        return false;
    }
    out_value = std::move(*value);
    return true;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make t F='[gcode]'`
Expected: PASS, including every pre-existing `[gcode][parser]` case.

- [ ] **Step 6: Prove the cut-line rule can fail**

Temporarily replace the `content = ...` line in `collect_exclude_object_defines` with a no-op, run `./build/bin/helix-tests 'collect_exclude_object_defines keeps file order and ignores a cut final line'` after `make test`, expect FAIL on `objs.size() == 3`, then restore.

- [ ] **Step 7: Sweep and commit**

```bash
make unit-sweep
git add -- include/gcode_parser.h src/rendering/gcode_parser.cpp tests/unit/test_gcode_parser.cpp
git commit -m "feat(gcode): one EXCLUDE_OBJECT_DEFINE parser for the viewer and the file scan" -m "parse_exclude_object_define() and collect_exclude_object_defines() are free functions the parser and the details scan share. Quoted names keep their spaces, matching Klipper, and an unterminated last line is ignored so a partial read never lists a cut name." -m "mutation: dropped the unterminated-tail trim; the cut-final-line case went red"
git show --stat HEAD
```

---

### Task 2: The details file scan collects object definitions

**Files:**
- Modify: `include/gcode_ops_detector.h` (`struct ScanResult`; add `#include "gcode_parser.h"`)
- Modify: `src/ui/ui_print_preparation_manager.cpp` (`PrintPreparationManager::scan_file_for_operations`, inside `on_content`)
- Test: `tests/unit/test_print_preparation_manager.cpp`

**Interfaces:**
- Consumes: `helix::gcode::collect_exclude_object_defines(std::string_view)` (Task 1).
- Produces: `helix::gcode::ScanResult::objects` — `std::vector<helix::gcode::GCodeObject>`, file order, from the first `helix::PRINTER_STOP_SCAN_BYTES` of the file.

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_print_preparation_manager.cpp`; add `#include "../lvgl_test_fixture.h"`, `#include "../test_helpers/planted_gcode.h"`, `#include "../test_helpers/update_queue_test_access.h"`, `#include "print_start_checks.h"` if absent)

```cpp
namespace {
/// The details scan over the mock, whose partial download serves planted files.
struct ObjectScanFixture : public LVGLTestFixture {
    MockPrinter mock_printer;
    helix::ui::PrintPreparationManager manager;

    ObjectScanFixture() {
        manager.set_dependencies(&mock_printer.api, &mock_printer.state);
    }
    void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};
} // namespace

TEST_CASE_METHOD(ObjectScanFixture,
                 "The file scan collects EXCLUDE_OBJECT_DEFINE objects in file order",
                 "[print_preparation][pre_start_exclude]") {
    helix::PlantedGcode file(
        "scan_objects.gcode", "",
        "; HEADER\n"
        "EXCLUDE_OBJECT_DEFINE NAME=Cone_id_0 CENTER=25.5,-4.1 "
        "POLYGON=[[20,-9],[31,-9],[31,1],[20,1]]\n"
        "EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1 CENTER=-36,6\n"
        "START_PRINT\n"
        "G1 X10 Y10 E1\n");
    manager.scan_file_for_operations(file.name(), "");
    settle();

    REQUIRE(manager.has_scan_result_for(file.name()));
    const auto& objects = manager.get_scan_result()->objects;
    REQUIRE(objects.size() == 2);
    CHECK(objects[0].name == "Cone_id_0");
    CHECK(objects[0].polygon.size() == 4);
    CHECK(objects[1].name == "Cube_id_1");
}

TEST_CASE_METHOD(ObjectScanFixture, "Definitions past the scan window are not in the scan",
                 "[print_preparation][pre_start_exclude]") {
    std::string content = "; HEADER\n";
    content += std::string(helix::PRINTER_STOP_SCAN_BYTES, ';') + "\n";
    content += "EXCLUDE_OBJECT_DEFINE NAME=Late_A CENTER=1,1\n";
    content += "EXCLUDE_OBJECT_DEFINE NAME=Late_B CENTER=9,9\n";
    helix::PlantedGcode file("late_defines.gcode", "", content);
    manager.scan_file_for_operations(file.name(), "");
    settle();

    REQUIRE(manager.has_scan_result_for(file.name()));
    CHECK(manager.get_scan_result()->objects.empty());
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `make t F='[print_preparation][pre_start_exclude]'`
Expected: FAIL to compile (`ScanResult` has no member `objects`).

- [ ] **Step 3: Add the field** (`include/gcode_ops_detector.h`, `struct ScanResult`, after `print_start`)

```cpp
    /// EXCLUDE_OBJECT_DEFINE objects in the scanned head, in file order.
    std::vector<GCodeObject> objects;
```

- [ ] **Step 4: Fill it in the scan** (`src/ui/ui_print_preparation_manager.cpp`, `scan_file_for_operations`, in `on_content` right after `auto scan_result = detector.scan_content(content);`)

```cpp
        // The whole downloaded head, not the detector's shorter op window:
        // a large thumbnail block can push the definitions past that.
        scan_result.objects = gcode::collect_exclude_object_defines(content);
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make t F='[print_preparation]'`
Expected: PASS.

- [ ] **Step 6: Sweep and commit**

```bash
make unit-sweep
git add -- include/gcode_ops_detector.h src/ui/ui_print_preparation_manager.cpp tests/unit/test_print_preparation_manager.cpp
git commit -m "feat(print-select): the details file scan lists the file's objects" -m "ScanResult::objects carries every EXCLUDE_OBJECT_DEFINE in the 200 KB head the scan already downloads, through the shared parser, so the details view can offer object picks before the G-code preview has parsed." -m "mutation: removed the objects assignment in on_content; the file-order scan case went red"
git show --stat HEAD
```

---

### Task 3: One ObjectInfo builder

**Files:**
- Modify: `include/printer_excluded_objects_state.h` (public static in `PrinterExcludedObjectsState`)
- Modify: `src/printer/printer_excluded_objects_state.cpp` (`make_object_info`, `update_from_status`)
- Test: `tests/unit/test_exclude_object_geometry.cpp`

**Interfaces:**
- Produces: `static PrinterExcludedObjectsState::ObjectInfo PrinterExcludedObjectsState::make_object_info(std::string name, std::optional<glm::vec2> center, std::vector<glm::vec2> polygon);` — `has_center` from `center`, bbox from the polygon's points, `has_bbox` only when there is at least one point.

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_exclude_object_geometry.cpp`)

```cpp
TEST_CASE("make_object_info derives the bbox from the polygon",
          "[exclude_object][geometry][pre_start_exclude]") {
    using S = PrinterExcludedObjectsState;
    const auto info = S::make_object_info("A", glm::vec2(5.0f, 6.0f), {{1, 2}, {9, 3}, {4, 8}});
    CHECK(info.name == "A");
    CHECK(info.has_center);
    CHECK(info.center == glm::vec2(5.0f, 6.0f));
    CHECK(info.has_bbox);
    CHECK(info.bbox_min == glm::vec2(1.0f, 2.0f));
    CHECK(info.bbox_max == glm::vec2(9.0f, 8.0f));
    CHECK(info.polygon.size() == 3);

    const auto bare = S::make_object_info("B", std::nullopt, {});
    CHECK_FALSE(bare.has_center);
    CHECK_FALSE(bare.has_bbox);
}

TEST_CASE("A status polygon with no usable point gives no bbox",
          "[exclude_object][geometry][pre_start_exclude]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.update_from_status(nlohmann::json::parse(R"({"exclude_object": {
        "objects": [{"name": "A", "center": [1, 1], "polygon": [["x", "y"]]}]}})"));
    const auto geom = state.get_object_geometry("A");
    REQUIRE(geom.has_value());
    CHECK(geom->has_center);
    CHECK_FALSE(geom->has_bbox);
    state.deinit_subjects();
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `make t F='[exclude_object][geometry]'`
Expected: FAIL to compile (`make_object_info` not a member).

- [ ] **Step 3: Declare it** (`include/printer_excluded_objects_state.h`, public, after the `ObjectInfo` struct)

```cpp
    /// One object as the map and badges read it: the bbox spans the polygon's
    /// points, and an empty polygon means no bbox.
    static ObjectInfo make_object_info(std::string name, std::optional<glm::vec2> center,
                                       std::vector<glm::vec2> polygon);
```

- [ ] **Step 4: Implement it and use it in `update_from_status`** (`src/printer/printer_excluded_objects_state.cpp`; add `#include <glm/common.hpp>`)

```cpp
PrinterExcludedObjectsState::ObjectInfo
PrinterExcludedObjectsState::make_object_info(std::string name, std::optional<glm::vec2> center,
                                              std::vector<glm::vec2> polygon) {
    ObjectInfo info;
    info.name = std::move(name);
    info.has_center = center.has_value();
    if (center) {
        info.center = *center;
    }
    info.has_bbox = !polygon.empty();
    if (info.has_bbox) {
        glm::vec2 lo = polygon.front();
        glm::vec2 hi = polygon.front();
        for (const auto& p : polygon) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        info.bbox_min = lo;
        info.bbox_max = hi;
    }
    info.polygon = std::move(polygon);
    return info;
}
```

Replace the per-object body of the `objects` loop in `update_from_status`:

```cpp
        for (const auto& obj : eo["objects"]) {
            if (!obj.is_object() || !obj.contains("name") || !obj["name"].is_string())
                continue;

            std::optional<glm::vec2> center;
            if (obj.contains("center") && obj["center"].is_array() && obj["center"].size() >= 2 &&
                obj["center"][0].is_number() && obj["center"][1].is_number()) {
                center = glm::vec2(obj["center"][0].get<float>(), obj["center"][1].get<float>());
            }

            std::vector<glm::vec2> polygon;
            if (obj.contains("polygon") && obj["polygon"].is_array()) {
                for (const auto& pt : obj["polygon"]) {
                    if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number()) {
                        polygon.push_back({pt[0].get<float>(), pt[1].get<float>()});
                    }
                }
            }

            objects.push_back(
                make_object_info(obj["name"].get<std::string>(), center, std::move(polygon)));
        }
```
Remove the now-unused `#include <limits>` only if nothing else in the file uses it.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make t F='[exclude_object]'`
Expected: PASS (the existing "exclude_object status parse" case included).

- [ ] **Step 6: Sweep and commit**

```bash
make unit-sweep
git add -- include/printer_excluded_objects_state.h src/printer/printer_excluded_objects_state.cpp tests/unit/test_exclude_object_geometry.cpp
git commit -m "refactor(exclude): one rule for an object's bbox from its polygon" -m "make_object_info() builds ObjectInfo for both Klipper's status and, next, the details file scan. A polygon with no usable point no longer reports a bbox of float extremes." -m "mutation: set has_bbox from the raw array instead of the parsed points; the no-usable-point case went red"
git show --stat HEAD
```

---

### Task 4: Side list and map view take a state and a tap callback

**Files:**
- Modify: `include/ui_exclude_object_badges.h` (add `ObjectTapFn`, `ExcludeTapMode`; add `#include <functional>`)
- Modify: `include/ui_exclude_object_side_list.h`, `src/ui/ui_exclude_object_side_list.cpp`
- Modify: `ui_xml/components/exclude_object_row.xml`
- Modify: `include/ui_exclude_object_map_view.h`, `src/ui/ui_exclude_object_map_view.cpp`
- Modify: `src/ui/ui_panel_print_status.cpp` (`show_exclude_map_view`: the two `create` calls only)
- Test: `tests/unit/test_exclude_object_side_list.cpp`, `tests/unit/test_exclude_object_map_view.cpp`, `tests/unit/test_exclude_object_badges.cpp` (call-site update only)

**Interfaces:**
- Produces (namespace `helix::ui`, in `ui_exclude_object_badges.h`):
  - `using ObjectTapFn = std::function<void(const std::string& name)>;`
  - `enum class ExcludeTapMode { ExcludeOnly, Toggle };` — `ExcludeOnly`: an excluded object stops taking taps (print status, Klipper cannot un-exclude). `Toggle`: excluded objects stay tappable (details picks).
  - `void ExcludeObjectSideList::create(lv_obj_t* parent, PrinterExcludedObjectsState* state, ObjectTapFn on_object_tapped, ExcludeTapMode tap_mode, SideListGeometry geom);`
  - `void ExcludeObjectMapView::create(lv_obj_t* parent, helix::PrinterExcludedObjectsState* state, float bed_w_mm, float bed_h_mm, ObjectTapFn on_object_tapped, ExcludeTapMode tap_mode, std::shared_ptr<helix::gcode::ParsedGCodeFile> parsed_file = nullptr);`
  - Row state ints bound by `exclude_object_row.xml`: 0 idle, 1 printing, 2 excluded (not tappable), 3 picked (excluded look, tappable).

- [ ] **Step 1: Update the existing side list fixture and add the failing tests** (`tests/unit/test_exclude_object_side_list.cpp`)

Remove `#include "ui_print_exclude_object_manager.h"`. Replace the fixture's constructor and members:

```cpp
class SideListFixture : public LVGLUITestFixture {
  public:
    SideListFixture() {
        objects().set_defined_objects(object_names(20));
        objects().set_current_object("obj_0");
        open(ExcludeTapMode::ExcludeOnly, exclude_side_list_geometry(false));
    }

    ~SideListFixture() override {
        list.destroy();
        objects().clear_objects();
        settle();
    }

    void open(ExcludeTapMode mode, SideListGeometry geom) {
        list.destroy();
        settle();
        list.create(
            test_screen(), &objects(), [this](const std::string& name) { taps.push_back(name); },
            mode, geom);
        settle();
        container = lv_obj_find_by_name(list.root(), "rows_container");
    }

    PrinterExcludedObjectsState& objects() {
        return state().excluded_objects_state();
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(400);
    }

    ExcludeObjectSideList list;
    lv_obj_t* container = nullptr;
    std::vector<std::string> taps;
};
```

In the portrait case, replace the `list.destroy(); settle(); list.create(...); settle(); container = ...;` block with `open(ExcludeTapMode::ExcludeOnly, exclude_side_list_geometry(true));`.

Append:

```cpp
TEST_CASE_METHOD(SideListFixture, "A row tap hands the object's name to the tap callback",
                 "[exclude_side_list][pre_start_exclude]") {
    REQUIRE(container != nullptr);
    lv_obj_send_event(rows_of(container)[4], LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "obj_4");
}

TEST_CASE_METHOD(SideListFixture, "In toggle mode a picked row reads Excluded and stays tappable",
                 "[exclude_side_list][pre_start_exclude]") {
    objects().set_current_object("");
    open(ExcludeTapMode::Toggle, exclude_side_list_geometry(false));
    REQUIRE(container != nullptr);

    objects().set_excluded_objects({"obj_3"});
    settle();

    const auto rows = rows_of(container);
    CHECK(lv_obj_has_flag(rows[3], LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_get_style_opa(rows[3], LV_PART_MAIN) == 150);
    CHECK(shows_text(rows[3], "Excluded"));
    CHECK_FALSE(shows_text(rows[4], "Excluded"));

    lv_obj_send_event(rows[3], LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "obj_3");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Each open side list's close button closes only that list",
                 "[exclude_side_list][pre_start_exclude]") {
    PrinterExcludedObjectsState first_state;
    PrinterExcludedObjectsState second_state;
    first_state.init_subjects(false);
    second_state.init_subjects(false);
    first_state.set_defined_objects({"a0", "a1"});
    second_state.set_defined_objects({"b0", "b1"});

    int first_closed = 0;
    int second_closed = 0;
    {
        ExcludeObjectSideList first;
        ExcludeObjectSideList second;
        first.set_close_callback([&] { ++first_closed; });
        second.set_close_callback([&] { ++second_closed; });
        first.create(test_screen(), &first_state, {}, ExcludeTapMode::ExcludeOnly,
                     exclude_side_list_geometry(false));
        second.create(test_screen(), &second_state, {}, ExcludeTapMode::ExcludeOnly,
                      exclude_side_list_geometry(false));
        UpdateQueue::instance().drain();

        lv_obj_send_event(lv_obj_find_by_name(first.root(), "close_btn"), LV_EVENT_CLICKED,
                          nullptr);
        CHECK(first_closed == 1);
        CHECK(second_closed == 0);

        lv_obj_send_event(lv_obj_find_by_name(second.root(), "close_btn"), LV_EVENT_CLICKED,
                          nullptr);
        CHECK(second_closed == 1);

        first.destroy();
        second.destroy();
        UpdateQueue::instance().drain();
        process_lvgl(50);
    }
    first_state.deinit_subjects();
    second_state.deinit_subjects();
}
```

- [ ] **Step 2: Add the failing map view tests** (`tests/unit/test_exclude_object_map_view.cpp`)

Update both existing `create` calls to the new signature: `view->create(test_screen(), &state().excluded_objects_state(), 235.0f, 235.0f, {}, ExcludeTapMode::ExcludeOnly, nullptr);` and `view.create(test_screen(), &state().excluded_objects_state(), 200.0f, 200.0f, {}, ExcludeTapMode::ExcludeOnly, nullptr);`. In `tests/unit/test_exclude_object_badges.cpp` change `view.create(test_screen(), &state().excluded_objects_state(), 235.0f, 235.0f, nullptr, nullptr);` to `view.create(test_screen(), &state().excluded_objects_state(), 235.0f, 235.0f, {}, helix::ui::ExcludeTapMode::ExcludeOnly, nullptr);`.

Append (global namespace, after `seed_objects`; add `#include "ui_update_queue.h"` if absent):

```cpp
class ExcludeObjectMapViewTestAccess {
  public:
    static lv_obj_t* rect_for(const ExcludeObjectMapView& view, const std::string& name) {
        for (const auto& r : view.object_rects_) {
            if (r.name == name) {
                return r.rect;
            }
        }
        return nullptr;
    }
};

TEST_CASE_METHOD(XMLTestFixture,
                 "Map view in toggle mode hands taps to the callback and keeps a picked rect tappable",
                 "[exclude_map][pre_start_exclude]") {
    REQUIRE(register_component("components/exclude_object_map"));
    auto& st = state().excluded_objects_state();
    seed_objects(st);
    std::vector<std::string> taps;

    ExcludeObjectMapView view;
    view.create(test_screen(), &st, 235.0f, 235.0f,
                [&](const std::string& name) { taps.push_back(name); }, ExcludeTapMode::Toggle,
                nullptr);
    process_lvgl(50);
    st.set_excluded_objects({"OBJ_1"});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    lv_obj_t* rect = ExcludeObjectMapViewTestAccess::rect_for(view, "OBJ_1");
    REQUIRE(rect != nullptr);
    CHECK(lv_obj_has_flag(rect, LV_OBJ_FLAG_CLICKABLE));
    lv_obj_send_event(rect, LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "OBJ_1");

    view.destroy();
    process_lvgl(20);
}

TEST_CASE_METHOD(XMLTestFixture, "Map view in exclude-only mode drops an excluded rect's tap",
                 "[exclude_map][pre_start_exclude]") {
    REQUIRE(register_component("components/exclude_object_map"));
    auto& st = state().excluded_objects_state();
    seed_objects(st);

    ExcludeObjectMapView view;
    view.create(test_screen(), &st, 235.0f, 235.0f, {}, ExcludeTapMode::ExcludeOnly, nullptr);
    process_lvgl(50);
    st.set_excluded_objects({"OBJ_1"});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    lv_obj_t* rect = ExcludeObjectMapViewTestAccess::rect_for(view, "OBJ_1");
    REQUIRE(rect != nullptr);
    CHECK_FALSE(lv_obj_has_flag(rect, LV_OBJ_FLAG_CLICKABLE));

    view.destroy();
    process_lvgl(20);
}
```

- [ ] **Step 3: Run them to verify they fail**

Run: `make t F='[pre_start_exclude]'`
Expected: FAIL to compile (`ExcludeTapMode` unknown; old `create` signatures).

- [ ] **Step 4: Add the shared types** (`include/ui_exclude_object_badges.h`, inside `namespace helix::ui`, before `struct ObjectBadge`)

```cpp
/// Hands the name of an object the user tapped (a list row, a map rect, a badge).
using ObjectTapFn = std::function<void(const std::string& name)>;

/// What a tap on an already-excluded object does. Print status excludes for
/// good, so the object stops taking taps; print details holds picks the user
/// can undo before Print, so it keeps taking them.
enum class ExcludeTapMode { ExcludeOnly, Toggle };
```

- [ ] **Step 5: Side list: state + callback + mode + per-instance close** (`include/ui_exclude_object_side_list.h`)

Replace the `create` declaration and the two dependency members; remove the `PrinterState` and `PrintExcludeObjectManager` forward declarations:

```cpp
    /// Create the panel as a floating child of `parent`. `geom` says which edge
    /// to cover and how much of it. Taps on rows reach `on_object_tapped`.
    void create(lv_obj_t* parent, PrinterExcludedObjectsState* state,
                ObjectTapFn on_object_tapped, ExcludeTapMode tap_mode, SideListGeometry geom);
```

```cpp
    PrinterExcludedObjectsState* state_{nullptr};
    ObjectTapFn on_object_tapped_;
    ExcludeTapMode tap_mode_{ExcludeTapMode::ExcludeOnly};
```
Update the `row_states_` comment to: `/// One int per row, bound by exclude_object_row.xml: 0 idle, 1 printing, 2 excluded, 3 picked.`

`src/ui/ui_exclude_object_side_list.cpp`: replace `#include "ui_print_exclude_object_manager.h"` and `#include "printer_state.h"` with `#include "printer_excluded_objects_state.h"`; add `#include <algorithm>` and `#include <vector>`. Replace the `g_active_side_list` global with a live list:

```cpp
namespace {
constexpr uint32_t SLIDE_IN_DURATION_MS = 220;

// Every open list. Print status keeps its overlay alive while the details view
// opens its own, so the XML close callback finds its list by ancestry.
std::vector<ExcludeObjectSideList*>& open_lists() {
    static std::vector<ExcludeObjectSideList*> lists;
    return lists;
}

void forget_open_list(ExcludeObjectSideList* list) {
    auto& lists = open_lists();
    lists.erase(std::remove(lists.begin(), lists.end(), list), lists.end());
}

bool is_within(lv_obj_t* obj, lv_obj_t* ancestor) {
    for (lv_obj_t* o = obj; o; o = lv_obj_get_parent(o)) {
        if (o == ancestor) {
            return true;
        }
    }
    return false;
}
} // namespace
```

Destructor: replace the `g_active_side_list` lines with `forget_open_list(this);`. In `destroy()`, replace the final `g_active_side_list` block with `forget_open_list(this);`.

`create()` head:

```cpp
void ExcludeObjectSideList::create(lv_obj_t* parent, PrinterExcludedObjectsState* state,
                                   ObjectTapFn on_object_tapped, ExcludeTapMode tap_mode,
                                   SideListGeometry geom) {
    if (root_) {
        spdlog::warn("[ExcludeObjectSideList] create() called but already active");
        return;
    }
    if (!parent || !state) {
        spdlog::error("[ExcludeObjectSideList] create() missing required pointers");
        return;
    }

    state_ = state;
    on_object_tapped_ = std::move(on_object_tapped);
    tap_mode_ = tap_mode;
```
Keep the callback registration; replace `g_active_side_list = this;` with `open_lists().push_back(this);`, and in the `lv_xml_create` failure branch replace `g_active_side_list = nullptr;` with `forget_open_list(this);`. Replace the two observers:

```cpp
    excluded_version_obs_ = observe<int>(
        state_->get_excluded_objects_version_subject(), this,
        [](ExcludeObjectSideList* self, int) {
            if (self->root_) {
                self->update_row_states();
            }
        },
        state_->get_subjects_lifetime());
    defined_version_obs_ = observe<int>(
        state_->get_defined_objects_version_subject(), this,
        [](ExcludeObjectSideList* self, int) {
            if (self->root_) {
                self->rebuild_rows();
            }
        },
        state_->get_subjects_lifetime());
```

Close routing:

```cpp
void ExcludeObjectSideList::on_close_clicked(lv_event_t* e) {
    lv_obj_t* button = lv_event_get_current_target_obj(e);
    for (ExcludeObjectSideList* list : open_lists()) {
        if (list->root_ && is_within(button, list->root_)) {
            spdlog::debug("[ExcludeObjectSideList] Close button clicked");
            // A copy: the callback usually destroys this list, and its own
            // std::function with it.
            auto close = list->close_cb_;
            if (close) {
                close();
            }
            return;
        }
    }
}
```

In `rebuild_rows()`, `update_row_states()` and `restyle_rows_if_stale()` replace every `printer_state_->excluded_objects_state()` with `*state_` (and the `!printer_state_` guards with `!state_`). In `update_row_states()`:

```cpp
        const int state = badges[i].excluded
                              ? (tap_mode_ == ExcludeTapMode::Toggle ? 3 : 2)
                              : (badges[i].current ? 1 : 0);
```

`on_row_clicked`: replace the manager guard and call:

```cpp
    if (!self || !self->on_object_tapped_ || !target) {
        return;
    }
    ...
    self->on_object_tapped_(std::string(name));
```

- [ ] **Step 6: Row XML: state 3 looks excluded and stays tappable** (`ui_xml/components/exclude_object_row.xml`)

Replace the state-related binds:

```xml
    <bind_style_if_ge name="row_excluded" subject="$state_subject" ref_value="2"/>
    <bind_flag_if_not_eq subject="$state_subject" flag="clickable" ref_value="2"/>
```
```xml
        <bind_style_if_lt name="name_idle" subject="$state_subject" ref_value="2"/>
        <bind_style_if_ge name="name_excluded" subject="$state_subject" ref_value="2"/>
```
```xml
          <bind_flag_if_lt subject="$state_subject" flag="hidden" ref_value="2"/>
```
(the last replaces `bind_flag_if_not_eq ... ref_value="2"` on `status_excluded`). Update the header comment's state list to: `state_subject names the row's int state: 0 idle, 1 printing now, 2 excluded, 3 picked before the print starts. Excluded and picked rows are dimmed and muted; an excluded row is not clickable, a picked row is, so the pick can be undone.`

- [ ] **Step 7: Map view: state + callback + mode** (`include/ui_exclude_object_map_view.h`, `src/ui/ui_exclude_object_map_view.cpp`)

Header: new `create` signature from the Interfaces block; replace `PrintExcludeObjectManager* exclude_manager_{nullptr};` with

```cpp
    ObjectTapFn on_object_tapped_;
    ExcludeTapMode tap_mode_{ExcludeTapMode::ExcludeOnly};
```
remove the `class PrintExcludeObjectManager;` forward declaration, and add, in the global namespace before `namespace helix`, `class ExcludeObjectMapViewTestAccess;` plus `friend class ::ExcludeObjectMapViewTestAccess;` in the class's private section.

Source: in `create`, replace `exclude_manager_ = exclude_manager;` with `on_object_tapped_ = std::move(on_object_tapped); tap_mode_ = tap_mode;`. In `destroy`, replace `exclude_manager_ = nullptr;` with `on_object_tapped_ = nullptr;`. In `update_visual_states`, the two `lv_obj_remove_flag(rect, LV_OBJ_FLAG_CLICKABLE)` sites for an excluded object become:

```cpp
            if (is_excluded && tap_mode_ == ExcludeTapMode::ExcludeOnly) {
                lv_obj_remove_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_add_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            }
```
(in the canvas branch), and in the `else if (is_excluded)` branch:

```cpp
            if (tap_mode_ == ExcludeTapMode::ExcludeOnly) {
                lv_obj_remove_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_add_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            }
```

`on_object_clicked`:

```cpp
void ExcludeObjectMapView::on_object_clicked(lv_event_t* e) {
    auto* self = static_cast<ExcludeObjectMapView*>(lv_event_get_user_data(e));
    if (!self || !self->on_object_tapped_)
        return;
    lv_obj_t* target = lv_event_get_target_obj(e);
    for (const auto& entry : self->object_rects_) {
        if (entry.rect == target) {
            spdlog::info("[ExcludeObjectMapView] Object rect clicked: '{}'", entry.name);
            self->on_object_tapped_(entry.name);
            return;
        }
    }
    spdlog::debug("[ExcludeObjectMapView] on_object_clicked: no matching rect found");
}
```

- [ ] **Step 8: Keep print status compiling** (`src/ui/ui_panel_print_status.cpp`, `show_exclude_map_view`)

```cpp
        auto request_exclude = [this](const std::string& name) {
            if (exclude_manager_) {
                exclude_manager_->request_exclude(name);
            }
        };
        map_view_->create(thumbnail_section, &printer_state_.excluded_objects_state(), bed_w, bed_h,
                          request_exclude, helix::ui::ExcludeTapMode::ExcludeOnly, parsed);
```
and

```cpp
    side_list_->create(
        overlay_content, &printer_state_.excluded_objects_state(),
        [this](const std::string& name) {
            if (exclude_manager_) {
                exclude_manager_->request_exclude(name);
            }
        },
        helix::ui::ExcludeTapMode::ExcludeOnly, list_geom);
```

- [ ] **Step 9: Run the tests to verify they pass**

Run: `make t F='[exclude_side_list],[exclude_map],[exclude_badges]'`
Expected: PASS, every pre-existing case included.

- [ ] **Step 10: Sweep, ratchets, commit**

```bash
make unit-sweep
python3 scripts/check_imperative_ui.py --summary
python3 scripts/check_hardcoded_pixels.py --summary
git add -- include/ui_exclude_object_badges.h include/ui_exclude_object_side_list.h src/ui/ui_exclude_object_side_list.cpp ui_xml/components/exclude_object_row.xml include/ui_exclude_object_map_view.h src/ui/ui_exclude_object_map_view.cpp src/ui/ui_panel_print_status.cpp tests/unit/test_exclude_object_side_list.cpp tests/unit/test_exclude_object_map_view.cpp tests/unit/test_exclude_object_badges.cpp
git commit -m "refactor(exclude): the object list and map take a state and a tap callback" -m "Both components now read any PrinterExcludedObjectsState and hand taps to their host, so print details can drive them with its own picks. Toggle mode keeps an excluded object tappable for undo; print status keeps exclude-only. Each list's close button now closes that list, also when two are alive." -m "mutation: restored the single active-list global; the two-lists close case went red"
git show --stat HEAD
```
Expected: the ratchet summaries do not exceed the `--max-allowed` values in `scripts/quality-checks.sh`.

---

### Task 5: One exclude-mode controller, used by print status

**Files:**
- Create: `include/ui_exclude_mode_controller.h`, `src/ui/ui_exclude_mode_controller.cpp`
- Modify: `include/ui_gcode_viewer.h`, `src/ui/ui_gcode_viewer.cpp`, `src/ui/gcode_viewer_state.h`, `src/ui/gcode_viewer_input.cpp`
- Modify: `include/ui_panel_print_status.h`, `src/ui/ui_panel_print_status.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`
- Test: `tests/unit/test_exclude_mode_controller.cpp` (new), `tests/unit/test_exclude_object_badges.cpp`; `tests/unit/test_print_status_exclude_badges.cpp` must pass unchanged

**Interfaces:**
- Consumes: Task 4 `create` signatures, `ObjectTapFn`, `ExcludeTapMode`; `helix::ui::exclude_side_list_geometry(bool, int32_t, int32_t, int32_t)`.
- Produces:
  - `void ui_gcode_viewer_set_excluded_badges_pickable(lv_obj_t* obj, bool pickable);`
  - test access (namespace `helix::test_access`): `void gcode_viewer_fire_object_tap(lv_obj_t* viewer, const char* name);`, `bool gcode_viewer_excluded_badges_pickable(lv_obj_t* viewer);`
  - `struct helix::ui::ExcludeModeTargets { lv_obj_t* card; lv_obj_t* columns; const char* controls_name; lv_obj_t* gcode_viewer; lv_subject_t* map_active; bool thumbnail_mode; float bed_w_mm; float bed_h_mm; };`
  - `class helix::ui::ExcludeModeController { void show(const ExcludeModeTargets&, PrinterExcludedObjectsState*, ExcludeTapMode, ObjectTapFn); void hide(); bool is_open() const; void refresh_render_badges(); };`

- [ ] **Step 1: Write the failing viewer test** (append to `tests/unit/test_exclude_object_badges.cpp`, next to "An excluded badge is drawn but the tap goes to the geometry")

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "With excluded badges pickable, a tap on one picks its object",
                 "[exclude_badges][gcode_viewer][pick][pre_start_exclude]") {
    BadgeViewer v;
    ui_gcode_viewer_set_excluded_badges_pickable(v.viewer, true);
    ui_gcode_viewer_set_object_badges(
        v.viewer, {make_badge(1, "Right", kLeftCenter, std::nullopt, /*excluded=*/true)});
    v.draw();
    REQUIRE(v.drawn().size() == 1);
    v.tap_local(v.drawn()[0].center);
    REQUIRE(v.taps.names.size() == 1);
    CHECK(v.taps.names[0] == "Right");
}
```

- [ ] **Step 2: Write the failing controller tests** (`tests/unit/test_exclude_mode_controller.cpp`)

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

// Exclude mode is one controller owned by both print status and print
// details: the object list over the host's columns, the top-down map over the
// card in thumbnail mode, numbered badges on the render otherwise, and every
// tap routed to the host's callback.

#include "ui_exclude_mode_controller.h"
#include "ui_gcode_viewer.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_excluded_objects_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

class ExcludeModeFixture : public LVGLUITestFixture {
  public:
    ExcludeModeFixture() {
        objects.init_subjects(false);
        objects.set_defined_objects({"A", "B", "C"});
        lv_subject_init_int(&map_active, 0);

        columns = lv_obj_create(test_screen());
        lv_obj_set_size(columns, 800, 400);
        lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
        card = lv_obj_create(columns);
        lv_obj_set_size(card, 400, 380);
        controls = lv_obj_create(columns);
        lv_obj_set_size(controls, 300, 380);
        lv_obj_set_name(controls, "controls_section");

        targets.card = card;
        targets.columns = columns;
        targets.controls_name = "controls_section";
        targets.map_active = &map_active;
        targets.thumbnail_mode = true;
        targets.bed_w_mm = 235.0f;
        targets.bed_h_mm = 235.0f;
    }

    ~ExcludeModeFixture() override {
        controller.hide();
        settle();
        objects.deinit_subjects();
        lv_subject_deinit(&map_active);
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(50);
    }

    void show(ExcludeTapMode mode) {
        controller.show(targets, &objects, mode,
                        [this](const std::string& name) { taps.push_back(name); });
        settle();
    }

    std::vector<lv_obj_t*> rows() const {
        std::vector<lv_obj_t*> out;
        lv_obj_t* container = lv_obj_find_by_name(columns, "rows_container");
        if (!container) {
            return out;
        }
        for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
            out.push_back(lv_obj_get_child(container, static_cast<int32_t>(i)));
        }
        return out;
    }

    PrinterExcludedObjectsState objects;
    lv_subject_t map_active{};
    lv_obj_t* columns = nullptr;
    lv_obj_t* card = nullptr;
    lv_obj_t* controls = nullptr;
    ExcludeModeTargets targets;
    ExcludeModeController controller;
    std::vector<std::string> taps;
};

} // namespace

TEST_CASE_METHOD(ExcludeModeFixture,
                 "Thumbnail mode lays the map over the card and the list over the columns",
                 "[exclude_mode][pre_start_exclude]") {
    show(ExcludeTapMode::ExcludeOnly);
    CHECK(controller.is_open());
    CHECK(lv_subject_get_int(&map_active) == 1);
    CHECK(lv_obj_find_by_name(card, "plate_area") != nullptr);
    CHECK(rows().size() == 3);

    controller.hide();
    settle();
    CHECK_FALSE(controller.is_open());
    CHECK(lv_subject_get_int(&map_active) == 0);
    CHECK(lv_obj_find_by_name(columns, "rows_container") == nullptr);
    CHECK(lv_obj_find_by_name(card, "plate_area") == nullptr);
}

TEST_CASE_METHOD(ExcludeModeFixture, "In toggle mode a picked row still reaches the host",
                 "[exclude_mode][pre_start_exclude]") {
    show(ExcludeTapMode::Toggle);
    objects.set_excluded_objects({"B"});
    settle();
    const auto r = rows();
    REQUIRE(r.size() == 3);
    CHECK(lv_obj_has_flag(r[1], LV_OBJ_FLAG_CLICKABLE));
    lv_obj_send_event(r[1], LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "B");
}

TEST_CASE_METHOD(ExcludeModeFixture, "In exclude-only mode an excluded row takes no tap",
                 "[exclude_mode][pre_start_exclude]") {
    show(ExcludeTapMode::ExcludeOnly);
    objects.set_excluded_objects({"B"});
    settle();
    const auto r = rows();
    REQUIRE(r.size() == 3);
    CHECK_FALSE(lv_obj_has_flag(r[1], LV_OBJ_FLAG_CLICKABLE));
}

TEST_CASE_METHOD(ExcludeModeFixture,
                 "Render mode routes viewer taps and badges while open, and drops them on hide",
                 "[exclude_mode][pre_start_exclude]") {
    lv_obj_t* viewer = ui_gcode_viewer_create(card);
    REQUIRE(viewer != nullptr);
    targets.gcode_viewer = viewer;
    targets.thumbnail_mode = false;

    show(ExcludeTapMode::Toggle);
    CHECK(lv_obj_find_by_name(card, "plate_area") == nullptr); // no map over a render
    CHECK(helix::test_access::gcode_viewer_excluded_badges_pickable(viewer));
    CHECK(helix::test_access::gcode_viewer_object_badges(viewer).size() == 3);
    helix::test_access::gcode_viewer_fire_object_tap(viewer, "C");
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "C");

    objects.set_excluded_objects({"A"});
    settle();
    CHECK(helix::test_access::gcode_viewer_object_badges(viewer)[0].excluded);

    controller.hide();
    settle();
    CHECK_FALSE(helix::test_access::gcode_viewer_excluded_badges_pickable(viewer));
    CHECK(helix::test_access::gcode_viewer_object_badges(viewer).empty());
    helix::test_access::gcode_viewer_fire_object_tap(viewer, "A");
    CHECK(taps.size() == 1);
}
```

- [ ] **Step 3: Run them to verify they fail**

Run: `make t F='[pre_start_exclude]'`
Expected: FAIL to compile (`ui_exclude_mode_controller.h` missing, `ui_gcode_viewer_set_excluded_badges_pickable` undeclared).

- [ ] **Step 4: Viewer: excluded badges can be pickable**

`src/ui/gcode_viewer_state.h`, next to `object_badges`:

```cpp
    /// When true an excluded badge still picks its object (pre-start picks can
    /// be undone); otherwise a tap on it falls through to the geometry.
    bool excluded_badges_pickable = false;
```

`src/ui/gcode_viewer_input.cpp`, in `ui_gcode_viewer_pick_object`, change the condition:

```cpp
        if ((st->excluded_badges_pickable ||
             !st->object_badges[static_cast<size_t>(idx)].excluded) &&
            glm::distance(st->drawn_badge_centers[i], tap) <= radius) {
```
and update the comment above it to: `// An excluded badge is drawn faded and is not a pick target unless the host made excluded badges pickable.`

`include/ui_gcode_viewer.h`, after `ui_gcode_viewer_set_object_badges`:

```cpp
/**
 * @brief Whether a tap on an excluded badge picks its object
 *
 * Print details sets this while exclude mode is open, so a pick can be undone
 * from the render. Off by default.
 */
// NAMESPACE_OK: joins this file's global ui_gcode_viewer_* API
void ui_gcode_viewer_set_excluded_badges_pickable(lv_obj_t* obj, bool pickable);
```
and in the `helix::test_access` block:

```cpp
/// Invoke the registered object tap callback as a tap on @p name would.
void gcode_viewer_fire_object_tap(lv_obj_t* viewer, const char* name);
bool gcode_viewer_excluded_badges_pickable(lv_obj_t* viewer);
```

`src/ui/ui_gcode_viewer.cpp`, viewer branch next to `ui_gcode_viewer_set_object_badges`:

```cpp
// NAMESPACE_OK: joins this file's global ui_gcode_viewer_* API
void ui_gcode_viewer_set_excluded_badges_pickable(lv_obj_t* obj, bool pickable) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return;
    }
    st->excluded_badges_pickable = pickable;
}
```
viewer-branch `helix::test_access` block:

```cpp
void gcode_viewer_fire_object_tap(lv_obj_t* viewer, const char* name) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (st && st->object_tap_callback) {
        st->object_tap_callback(viewer, name, st->object_tap_user_data);
    }
}

bool gcode_viewer_excluded_badges_pickable(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st && st->excluded_badges_pickable;
}
```
and the no-viewer (`#else // !HELIX_HAS_GCODE_VIEWER`) branch, next to the `ui_gcode_viewer_set_object_badges` stub:

```cpp
// NAMESPACE_OK: joins this file's global ui_gcode_viewer_* API
void ui_gcode_viewer_set_excluded_badges_pickable(lv_obj_t*, bool) {}
```

- [ ] **Step 5: The controller** (`include/ui_exclude_mode_controller.h`)

```cpp
#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_badges.h"
#include "ui_exclude_object_map_view.h"
#include "ui_exclude_object_side_list.h"
#include "ui_observer_guard.h"

#include <lvgl.h>
#include <memory>

namespace helix {
class PrinterExcludedObjectsState;
}

namespace helix::ui {

/// Where a host lets exclude mode draw.
struct ExcludeModeTargets {
    lv_obj_t* card = nullptr;            ///< Preview card the top-down map covers in thumbnail mode
    lv_obj_t* columns = nullptr;         ///< Row the object list floats over
    const char* controls_name = nullptr; ///< Child of columns the list covers in portrait
    lv_obj_t* gcode_viewer = nullptr;    ///< May be null
    lv_subject_t* map_active = nullptr;  ///< 1 while the map covers the card; may be null
    bool thumbnail_mode = true;          ///< Map + list; otherwise render badges + list
    float bed_w_mm = 0.0f;
    float bed_h_mm = 0.0f;
};

/// Exclude mode over a preview: the object list, plus the top-down map in
/// thumbnail mode or numbered badges on the 2D/3D render. Print status and
/// print details each own one. Every tap on an object, from the list, the map
/// or the render, reaches the host's callback.
class ExcludeModeController {
  public:
    ExcludeModeController() = default;
    ~ExcludeModeController();
    ExcludeModeController(const ExcludeModeController&) = delete;
    ExcludeModeController& operator=(const ExcludeModeController&) = delete;

    void show(const ExcludeModeTargets& targets, PrinterExcludedObjectsState* state,
              ExcludeTapMode mode, ObjectTapFn on_tap);
    void hide();
    [[nodiscard]] bool is_open() const {
        return side_list_ && side_list_->is_active();
    }
    /// Publish the render badges again, e.g. after the viewer parsed a new file.
    void refresh_render_badges();

  private:
    static void on_viewer_tap(lv_obj_t* viewer, const char* name, void* user_data);

    std::unique_ptr<ExcludeObjectMapView> map_view_;
    std::unique_ptr<ExcludeObjectSideList> side_list_;
    PrinterExcludedObjectsState* state_ = nullptr;
    lv_obj_t* viewer_ = nullptr;
    lv_subject_t* map_active_ = nullptr;
    ObjectTapFn on_tap_;
    ObserverGuard excluded_obs_;
    ObserverGuard defined_obs_;
};

} // namespace helix::ui
```

`src/ui/ui_exclude_mode_controller.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_mode_controller.h"

#include "ui_gcode_viewer.h"

#include "gcode_parser.h"
#include "layout_manager.h"
#include "observer_factory.h"
#include "print_status_layout_decision.h"
#include "printer_excluded_objects_state.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

ExcludeModeController::~ExcludeModeController() {
    // The viewer can outlive us; it must not call back into a freed controller.
    if (viewer_ && lv_is_initialized() && lv_obj_is_valid(viewer_)) {
        ui_gcode_viewer_set_object_tap_callback(viewer_, nullptr, nullptr);
    }
}

void ExcludeModeController::show(const ExcludeModeTargets& targets,
                                 PrinterExcludedObjectsState* state, ExcludeTapMode mode,
                                 ObjectTapFn on_tap) {
    if (is_open() || !state || !targets.columns) {
        return;
    }
    state_ = state;
    viewer_ = targets.gcode_viewer;
    map_active_ = targets.map_active;
    on_tap_ = std::move(on_tap);
    const ObjectTapFn forward = [this](const std::string& name) {
        if (on_tap_) {
            on_tap_(name);
        }
    };

    if (targets.thumbnail_mode && targets.card) {
        // Set before the map exists, so no frame shows it over a live thumbnail.
        if (map_active_) {
            lv_subject_set_int(map_active_, 1);
        }
        map_view_ = std::make_unique<ExcludeObjectMapView>();
        map_view_->set_close_callback([this]() { hide(); });
        std::shared_ptr<gcode::ParsedGCodeFile> parsed;
        if (viewer_) {
            if (const auto* raw = ui_gcode_viewer_get_parsed_file(viewer_)) {
                parsed = std::shared_ptr<gcode::ParsedGCodeFile>(
                    const_cast<gcode::ParsedGCodeFile*>(raw), [](gcode::ParsedGCodeFile*) {});
            }
        }
        map_view_->create(targets.card, state_, targets.bed_w_mm, targets.bed_h_mm, forward, mode,
                          parsed);
        // The list's X closes the whole mode; one dismiss control is enough.
        if (auto* map_root = map_view_->root()) {
            if (lv_obj_t* map_close = lv_obj_find_by_name(map_root, "close_btn")) {
                lv_obj_add_flag(map_close, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Landscape covers the right-hand column by ratio; portrait sizes the list
    // to the control stack it covers, so measure it.
    const bool portrait = helix::is_portrait_layout(helix::LayoutManager::instance().type());
    int32_t controls_h = 0;
    int32_t content_h = 0;
    int32_t gap = 0;
    if (portrait) {
        lv_obj_update_layout(targets.columns);
        if (targets.controls_name) {
            if (lv_obj_t* controls = lv_obj_find_by_name(targets.columns, targets.controls_name)) {
                controls_h = lv_obj_get_height(controls);
            }
        }
        content_h = lv_obj_get_content_height(targets.columns);
        gap = lv_obj_get_style_pad_row(targets.columns, LV_PART_MAIN);
    }

    side_list_ = std::make_unique<ExcludeObjectSideList>();
    side_list_->set_close_callback([this]() { hide(); });
    side_list_->set_gcode_viewer(viewer_);
    side_list_->create(targets.columns, state_, forward, mode,
                       exclude_side_list_geometry(portrait, controls_h, content_h, gap));

    // Installed whatever the mode, so switching thumbnail -> 2D/3D while open
    // still routes render taps.
    if (viewer_) {
        ui_gcode_viewer_set_object_tap_callback(viewer_, on_viewer_tap, this);
        ui_gcode_viewer_set_excluded_badges_pickable(viewer_, mode == ExcludeTapMode::Toggle);
    }

    const auto refresh = [](ExcludeModeController* self, int) { self->refresh_render_badges(); };
    excluded_obs_ = observe<int>(state_->get_excluded_objects_version_subject(), this, refresh,
                                 state_->get_subjects_lifetime());
    defined_obs_ = observe<int>(state_->get_defined_objects_version_subject(), this, refresh,
                                state_->get_subjects_lifetime());
    refresh_render_badges();
    spdlog::debug("[ExcludeMode] Opened ({})", targets.thumbnail_mode ? "map" : "render");
}

void ExcludeModeController::hide() {
    excluded_obs_.reset();
    defined_obs_.reset();
    if (viewer_) {
        ui_gcode_viewer_set_object_tap_callback(viewer_, nullptr, nullptr);
        ui_gcode_viewer_set_excluded_badges_pickable(viewer_, false);
        ui_gcode_viewer_set_highlighted_objects(viewer_, {});
        ui_gcode_viewer_set_object_badges(viewer_, {});
    }
    if (side_list_) {
        side_list_->destroy();
        side_list_.reset();
    }
    if (map_view_) {
        map_view_->destroy();
        map_view_.reset();
    }
    if (map_active_) {
        lv_subject_set_int(map_active_, 0);
    }
    viewer_ = nullptr;
    map_active_ = nullptr;
    state_ = nullptr;
    on_tap_ = nullptr;
}

void ExcludeModeController::refresh_render_badges() {
    if (!viewer_ || !state_ || !is_open()) {
        return;
    }
    ui_gcode_viewer_set_object_badges(
        viewer_, compute_object_badges(*state_, ui_gcode_viewer_get_parsed_file(viewer_)));
}

void ExcludeModeController::on_viewer_tap(lv_obj_t* /*viewer*/, const char* name,
                                          void* user_data) {
    auto* self = static_cast<ExcludeModeController*>(user_data);
    if (!self || !name || name[0] == '\0' || !self->on_tap_) {
        return;
    }
    spdlog::info("[ExcludeMode] Viewer tap on object: '{}'", name);
    self->on_tap_(std::string(name));
}

} // namespace helix::ui
```

- [ ] **Step 6: Print status uses it**

`include/ui_panel_print_status.h`: replace `#include "ui_exclude_object_map_view.h"` and `#include "ui_exclude_object_side_list.h"` with `#include "ui_exclude_mode_controller.h"`; delete the `refresh_render_badges()` declaration; replace the `map_view_` and `side_list_` members with:

```cpp
    /// Exclude mode over the preview card (list + map or render badges).
    helix::ui::ExcludeModeController exclude_mode_;
```

`src/ui/ui_panel_print_status.cpp`:
- Constructor `preview_` callback: `[this]() { recompute_scoped_runout(); exclude_mode_.refresh_render_badges(); }`.
- `exclude_objects_observer_` and `excluded_objects_version_observer_` lambdas: delete their `self->refresh_render_badges();` lines (the controller observes the state itself while open).
- `on_ui_destroyed`: replace the `side_list_.reset(); if (map_view_) {...}` block with `exclude_mode_.hide();`.
- Replace `show_exclude_map_view`, `refresh_render_badges`, `hide_exclude_map_view` with:

```cpp
void PrintStatusPanel::show_exclude_map_view() {
    if (!exclude_manager_) {
        return;
    }
    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (!overlay_content) {
        spdlog::warn("[{}] Cannot show exclude panel: overlay_content not found", get_name());
        return;
    }
    helix::ui::ExcludeModeTargets targets;
    targets.card = lv_obj_find_by_name(overlay_content, "thumbnail_section");
    targets.columns = overlay_content;
    targets.controls_name = "controls_section";
    targets.gcode_viewer = gcode_viewer_;
    targets.map_active = &exclude_map_active_subject_;
    targets.thumbnail_mode = lv_subject_get_int(&gcode_viewer_mode_subject_) == 0;
    const auto bed = helix::bed_dimensions(api_, &printer_state_);
    targets.bed_w_mm = bed.w_mm;
    targets.bed_h_mm = bed.h_mm;
    exclude_mode_.show(targets, &printer_state_.excluded_objects_state(),
                       helix::ui::ExcludeTapMode::ExcludeOnly, [this](const std::string& name) {
                           if (exclude_manager_) {
                               exclude_manager_->request_exclude(name);
                           }
                       });
}

void PrintStatusPanel::hide_exclude_map_view() {
    exclude_mode_.hide();
}
```
- `handle_objects_toggle`:

```cpp
void PrintStatusPanel::handle_objects_toggle() {
    if (exclude_mode_.is_open()) {
        hide_exclude_map_view();
    } else {
        show_exclude_map_view();
    }
}
```
Remove includes that no longer have users in the file (`ui_exclude_object_map_view.h`; keep `ui_exclude_object_badges.h` only if still referenced).

`firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`: insert `src/ui/ui_exclude_mode_controller.cpp` directly before `src/ui/ui_exclude_object_badges.cpp`.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `make t F='[exclude_mode],[exclude_badges],[print_status]'`
Expected: PASS, with `test_print_status_exclude_badges.cpp` untouched and green.

- [ ] **Step 8: Prove the controller's observers carry the badges**

Delete the `excluded_obs_ = observe<int>(...)` statement, `make test`, run `./build/bin/helix-tests 'Render badges follow the exclude side list open, update and close'`; expect FAIL at `CHECK(badges[1].excluded)`. Restore.

- [ ] **Step 9: Sweep, gates, commit**

```bash
make unit-sweep
python3 scripts/check_esp32_app_srcs.py
python3 scripts/check_imperative_ui.py --summary
git add -- include/ui_exclude_mode_controller.h src/ui/ui_exclude_mode_controller.cpp include/ui_gcode_viewer.h src/ui/ui_gcode_viewer.cpp src/ui/gcode_viewer_state.h src/ui/gcode_viewer_input.cpp include/ui_panel_print_status.h src/ui/ui_panel_print_status.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs.txt tests/unit/test_exclude_mode_controller.cpp tests/unit/test_exclude_object_badges.cpp
git commit -m "refactor(exclude): one exclude-mode controller, owned by print status" -m "ExcludeModeController holds what PrintStatusPanel's show/hide_exclude_map_view did: the list, the map in thumbnail mode, render badges, viewer tap routing, and the portrait measurement. Print details takes the same controller next. The viewer can make excluded badges pickable for toggle mode." -m "mutation: dropped the controller's excluded-version observer; the print status render-badge case went red"
git show --stat HEAD
```

---

### Task 6: One shared skip button

**Files:**
- Create: `ui_xml/components/exclude_objects_button.xml`
- Modify: `src/xml_registration.cpp` (register before `components/preview_stack.xml`)
- Modify: `ui_xml/components/print_status_preview_card.xml` (`btn_objects`)
- Test: `tests/unit/test_print_status_exclude_badges.cpp`

**Interfaces:**
- Produces: XML component `exclude_objects_button`, props `callback` (event callback name), `hidden_when` (bind_flag_if cond for the button), `count_text_subject` (optional string subject, no default), `count_hidden_when` (cond, default `""`). Child `objects_pick_count` (a `notification_badge`), hidden unless the host binds `count_hidden_when`.

- [ ] **Step 1: Write the failing test** (append to `tests/unit/test_print_status_exclude_badges.cpp`)

```cpp
TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "The objects button follows a multi-object print and never shows a pick count",
                 "[exclude_button][print_status][pre_start_exclude]") {
    lv_obj_t* btn = lv_obj_find_by_name(root_, "btn_objects");
    REQUIRE(btn != nullptr);
    auto& objects = state().excluded_objects_state();

    objects.set_defined_objects({"Solo"});
    UpdateQueue::instance().drain();
    CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    objects.set_defined_objects({"A", "B"});
    UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* count = lv_obj_find_by_name(btn, "objects_pick_count");
    REQUIRE(count != nullptr);
    CHECK(lv_obj_has_flag(count, LV_OBJ_FLAG_HIDDEN));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make t F='[exclude_button]'`
Expected: FAIL on `REQUIRE(count != nullptr)`.

- [ ] **Step 3: The component** (`ui_xml/components/exclude_objects_button.xml`)

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!--
  The skip-objects button over a G-code preview, shared by print status and
  print details so both show the same button in the same corner. The host
  places it (align / translate at the instance site), names the click callback
  and the condition that hides it. A host that holds pending picks also binds
  the count badge; without count_hidden_when it stays hidden.
-->
<component>
  <api>
    <prop name="callback" type="string"/>
    <prop name="hidden_when" type="string"/>
    <!-- Optional, deliberately without a default: an unset prop drops the
         bind_text attribute instead of looking up an empty subject name. -->
    <prop name="count_text_subject" type="string"/>
    <prop name="count_hidden_when" type="string" default=""/>
  </api>
  <view extends="lv_obj"
        width="#icon_button_size_lg" height="#icon_button_size_lg" style_bg_color="#card_bg" style_bg_opa="180"
        style_radius="#icon_button_radius_lg" clickable="true" style_pad_all="0" scrollable="false">
    <style name="styles.press_wash" selector="pressed"/>
    <bind_flag_if cond="$hidden_when" flag="hidden"/>
    <lv_label text="#icon_debug_step_over" style_text_font="mdi_icons_24" style_text_color="#text_muted" align="center"/>
    <notification_badge name="objects_pick_count"
                        align="top_right" hidden="true" clickable="false" event_bubble="true" style_bg_color="#primary"
                        bind_text="$count_text_subject">
      <bind_flag_if cond="$count_hidden_when" flag="hidden"/>
    </notification_badge>
    <event_cb trigger="clicked" callback="$callback"/>
  </view>
</component>
```

`src/xml_registration.cpp`, directly before `register_xml("components/preview_stack.xml");`:

```cpp
    register_xml("components/exclude_objects_button.xml");
```

- [ ] **Step 4: Print status uses it** (`ui_xml/components/print_status_preview_card.xml`, replace the whole `btn_objects` `lv_obj`)

```xml
    <!-- Skip-objects button: top-left corner, visible only for a multi-object print -->
    <exclude_objects_button name="btn_objects"
                            ignore_layout="true" align="top_left" style_translate_x="#space_md"
                            style_translate_y="#space_md" callback="on_print_status_objects"
                            hidden_when="exclude_objects_available eq 0"/>
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make t F='[print_status]'`
Expected: PASS.

- [ ] **Step 6: Sweep, ratchets, commit**

```bash
make unit-sweep
python3 scripts/check_hardcoded_pixels.py --summary
git add -- ui_xml/components/exclude_objects_button.xml src/xml_registration.cpp ui_xml/components/print_status_preview_card.xml tests/unit/test_print_status_exclude_badges.cpp
git commit -m "refactor(exclude): the skip-objects button is one shared component" -m "exclude_objects_button carries the icon, corner styling and an optional pick-count badge, so print details gets the identical button. Print status passes its existing callback and visibility rule and binds no count." -m "mutation: bound the count badge to exclude_objects_available in the card; the never-shows-a-count case went red"
git show --stat HEAD
```

---

### Task 7: Print details holds picks in a private state

**Files:**
- Create: `include/pre_start_exclude.h`, `src/ui/pre_start_exclude.cpp`
- Modify: `include/ui_print_select_detail_view.h`, `src/ui/ui_print_select_detail_view.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`
- Modify (translations): `translations/*.yml`, `ui_xml/translations/*.xml` via `make translation-sync` / `make translations`
- Test: `tests/unit/test_pre_start_exclude.cpp` (new), `tests/unit/test_print_select_detail_subjects.cpp`

**Interfaces:**
- Consumes: `ScanResult::objects` (Task 2), `PrinterExcludedObjectsState::make_object_info` (Task 3), `ExcludeModeController::refresh_render_badges()` (Task 5), `moonraker_internal::is_safe_object_name(const std::string&)` (`include/moonraker_validation.h`).
- Produces (namespace `helix::ui`, `include/pre_start_exclude.h`):
  - `bool pre_start_exclude_available(bool printer_has_exclude_object, bool is_3mf, size_t defined_count);`
  - `std::string canonical_object_name(const std::vector<std::string>& defined, const std::string& name);` — the defined spelling, compared upper-case; `""` when not defined.
  - `bool every_object_picked(const std::vector<std::string>& defined, const std::unordered_set<std::string>& picks);` — false when nothing is defined; compared upper-case.
  - `std::vector<gcode::GCodeObject> merge_defined_objects(const std::vector<gcode::GCodeObject>& scanned, const gcode::ParsedGCodeFile* parsed);` — scan order, then parsed names the scan lacks (upper-case comparison), with the parsed outline replacing an empty scanned polygon.
  - `std::vector<PrinterExcludedObjectsState::ObjectInfo> object_infos_from(const std::vector<gcode::GCodeObject>& objects);`
- Produces (`PrintSelectDetailView`, public):
  - `void refresh_exclude_objects();`
  - `void toggle_exclude_pick(const std::string& name);`
  - `[[nodiscard]] std::vector<std::string> exclude_picks() const;` (defined order)
  - `bool drop_exclude_picks();` (true when picks were cleared)
  - `[[nodiscard]] bool all_objects_picked() const;`
  - `[[nodiscard]] const helix::PrinterExcludedObjectsState& exclude_objects() const;`
  - subjects `detail_exclude_available` (int), `detail_exclude_pick_count` (int), `detail_exclude_pick_count_text` (string).

- [ ] **Step 1: Write the failing pure-rule tests** (`tests/unit/test_pre_start_exclude.cpp`)

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

// Rules behind choosing objects to skip before a print starts: when the
// option exists, how a tapped name maps to a defined object (upper-case, as
// Klipper stores names), when every object is picked, and how the file scan's
// list and the full parse's list combine.

#include "pre_start_exclude.h"

#include "gcode_parser.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;
using helix::gcode::GCodeObject;
using helix::gcode::ParsedGCodeFile;

namespace {
GCodeObject object(const std::string& name, glm::vec2 center = {0.0f, 0.0f},
                   std::vector<glm::vec2> polygon = {}) {
    GCodeObject o;
    o.name = name;
    o.center = center;
    o.polygon = std::move(polygon);
    return o;
}
} // namespace

TEST_CASE("The skip button needs [exclude_object], two objects and a G-code file",
          "[pre_start_exclude]") {
    CHECK(pre_start_exclude_available(true, false, 2));
    CHECK(pre_start_exclude_available(true, false, 7));
    CHECK_FALSE(pre_start_exclude_available(false, false, 3));
    CHECK_FALSE(pre_start_exclude_available(true, false, 1));
    CHECK_FALSE(pre_start_exclude_available(true, false, 0));
    CHECK_FALSE(pre_start_exclude_available(true, true, 3));
}

TEST_CASE("A tapped name maps to its defined spelling, compared upper-case",
          "[pre_start_exclude]") {
    const std::vector<std::string> defined = {"Cone_id_0", "Cube_id_1"};
    CHECK(canonical_object_name(defined, "Cube_id_1") == "Cube_id_1");
    CHECK(canonical_object_name(defined, "CUBE_ID_1") == "Cube_id_1");
    CHECK(canonical_object_name(defined, "cone_id_0") == "Cone_id_0");
    CHECK(canonical_object_name(defined, "Cylinder") == "");
    CHECK(canonical_object_name({}, "Cube_id_1") == "");
}

TEST_CASE("Every object picked, as Klipper would count them", "[pre_start_exclude]") {
    CHECK_FALSE(every_object_picked({}, {}));
    CHECK_FALSE(every_object_picked({"A", "B"}, {"A"}));
    CHECK(every_object_picked({"A", "B"}, {"A", "B"}));
    // Klipper upper-cases names, so "part" and "PART" are one object to it.
    CHECK(every_object_picked({"part", "PART"}, {"part"}));
}

TEST_CASE("The parsed file adds what the scan missed, keeping the scan's order",
          "[pre_start_exclude]") {
    ParsedGCodeFile parsed;
    parsed.objects["Alpha"] = object("Alpha", {1, 1}, {{0, 0}, {2, 0}, {2, 2}});
    parsed.objects["late"] = object("late", {9, 9});
    parsed.objects["zed"] = object("zed", {5, 5}); // the scan's "Zed", other case

    const auto merged = merge_defined_objects({object("Zed", {3, 3}), object("Alpha", {1, 1})},
                                              &parsed);
    REQUIRE(merged.size() == 3);
    CHECK(merged[0].name == "Zed");
    CHECK(merged[1].name == "Alpha");
    CHECK(merged[1].polygon.size() == 3); // the parsed outline fills the scan's gap
    CHECK(merged[2].name == "late");

    CHECK(merge_defined_objects({object("Zed")}, nullptr).size() == 1);
}

TEST_CASE("object_infos_from treats a zero centre as no centre", "[pre_start_exclude]") {
    const auto infos = object_infos_from({object("A", {4, 5}, {{0, 0}, {8, 10}}), object("B")});
    REQUIRE(infos.size() == 2);
    CHECK(infos[0].has_center);
    CHECK(infos[0].has_bbox);
    CHECK(infos[0].bbox_max == glm::vec2(8.0f, 10.0f));
    CHECK_FALSE(infos[1].has_center);
    CHECK_FALSE(infos[1].has_bbox);
}
```

- [ ] **Step 2: Write the failing details tests** (append to `tests/unit/test_print_select_detail_subjects.cpp`; add `#include "gcode_ops_detector.h"`, `#include "gcode_parser.h"`, `#include "printer_discovery.h"`, `#include "../ui_test_utils.h"`)

```cpp
// ============================================================================
// Pre-start object picks
// ============================================================================

namespace {

const char* kThreeParts =
    "EXCLUDE_OBJECT_DEFINE NAME=Cone_id_0 CENTER=25,-4 POLYGON=[[20,-9],[31,-9],[31,1],[20,1]]\n"
    "EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1 CENTER=-36,6\n"
    "EXCLUDE_OBJECT_DEFINE NAME=Cylinder_id_2 CENTER=-22,30\n";

/// A printer with or without [exclude_object] configured.
struct ExcludeObjectHardware {
    explicit ExcludeObjectHardware(bool configured) {
        helix::PrinterDiscovery hw;
        hw.parse_objects(configured ? nlohmann::json{"exclude_object", "extruder"}
                                    : nlohmann::json{"extruder"});
        get_printer_state().set_hardware(hw);
    }
    ~ExcludeObjectHardware() {
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
    }
};

/// A shown detail view whose scan of @p file found @p defines.
struct OpenDetail {
    CacheDirGuard cache;
    helix::ui::PrintSelectDetailView view;

    OpenDetail(lv_obj_t* screen, const std::string& file, const char* defines) {
        register_xml_callbacks({
            {"on_print_select_detail_backdrop", detail_noop_cb},
            {"on_print_select_print_button", detail_noop_cb},
            {"on_print_select_delete_button", detail_noop_cb},
            {"on_print_detail_back_clicked", detail_noop_cb},
            {"on_toggle_sliced_colors", detail_noop_cb},
            {"on_print_select_detail_objects", detail_noop_cb},
        });
        view.set_dependencies(nullptr, &get_printer_state());
        view.init_subjects();
        REQUIRE(view.create(screen) != nullptr);
        helix::gcode::ScanResult scan;
        scan.objects = helix::gcode::collect_exclude_object_defines(defines);
        view.get_prep_manager()->set_cached_scan_result(scan, file);
        view.show(file, "", "PLA");
        settle();
    }
    ~OpenDetail() {
        close();
    }
    void close() {
        view.hide();
        settle();
        lv_timer_handler(); // the close callback runs on the next tick
    }
    static void settle() {
        helix::ui::UpdateQueue::instance().drain();
    }
    static int subject_int(const char* name) {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Details lists the scanned objects in file order",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);

    const auto& defined = d.view.exclude_objects().get_defined_objects();
    REQUIRE(defined == std::vector<std::string>{"Cone_id_0", "Cube_id_1", "Cylinder_id_2"});
    const auto cone = d.view.exclude_objects().get_object_geometry("Cone_id_0");
    REQUIRE(cone.has_value());
    CHECK(cone->has_bbox);
    CHECK(OpenDetail::subject_int("detail_exclude_available") == 1);
    // The printer's live state is untouched.
    CHECK(get_printer_state().excluded_objects_state().get_defined_objects().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "The skip option needs [exclude_object], two objects and G-code",
                 "[print_select][detail_view][pre_start_exclude]") {
    SECTION("no [exclude_object]") {
        ExcludeObjectHardware hw(false);
        OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
        CHECK(OpenDetail::subject_int("detail_exclude_available") == 0);
    }
    SECTION("one object") {
        ExcludeObjectHardware hw(true);
        OpenDetail d(test_screen(), "solo.gcode", "EXCLUDE_OBJECT_DEFINE NAME=Solo CENTER=1,1\n");
        CHECK(OpenDetail::subject_int("detail_exclude_available") == 0);
    }
    // A 3MF is pinned by the pure rule's test: the scan replaces a .3mf's cached
    // result with an empty one, so a view-level case would pass on the count alone.
}

TEST_CASE_METHOD(LVGLUITestFixture, "A pick toggles in the private state, matched upper-case",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);

    d.view.toggle_exclude_pick("CUBE_ID_1");
    OpenDetail::settle();
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
    CHECK(OpenDetail::subject_int("detail_exclude_pick_count") == 1);
    CHECK(std::string(lv_subject_get_string(
              lv_xml_get_subject(nullptr, "detail_exclude_pick_count_text"))) == "1");
    CHECK(get_printer_state().excluded_objects_state().get_excluded_objects().empty());

    d.view.toggle_exclude_pick("cube_id_1");
    OpenDetail::settle();
    CHECK(d.view.exclude_picks().empty());
    CHECK(OpenDetail::subject_int("detail_exclude_pick_count") == 0);

    d.view.toggle_exclude_pick("Not_an_object");
    CHECK(d.view.exclude_picks().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "A name the printer cannot be sent is refused with a toast",
                 "[print_select][detail_view][pre_start_exclude]") {
    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&](const std::string& msg) { warnings.push_back(msg); });
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "names.gcode",
                 "EXCLUDE_OBJECT_DEFINE NAME='Part 1' CENTER=1,1\n"
                 "EXCLUDE_OBJECT_DEFINE NAME=Pi\xc3\xa8" "ce CENTER=5,5\n"
                 "EXCLUDE_OBJECT_DEFINE NAME=Plain CENTER=9,9\n");

    d.view.toggle_exclude_pick("Part 1");
    d.view.toggle_exclude_pick("Pi\xc3\xa8" "ce");
    d.view.toggle_exclude_pick("Plain");
    helix::ui::set_test_notification_warning_hook(nullptr);

    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Plain"});
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].find("Part 1") != std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Picks clear when the user leaves the file, not on a suspend",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cube_id_1");

    d.view.on_deactivating(DeactivateReason::Suspended);
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});

    d.close();
    CHECK(d.view.exclude_picks().empty());

    // Re-opening the same file lists its objects again, with no picks.
    d.view.show("parts.gcode", "", "PLA");
    OpenDetail::settle();
    CHECK(d.view.exclude_objects().get_defined_objects().size() == 3);
    CHECK(d.view.exclude_picks().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Every object picked is reported, and dropping picks says so",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    CHECK_FALSE(d.view.drop_exclude_picks());

    d.view.toggle_exclude_pick("Cone_id_0");
    d.view.toggle_exclude_pick("Cube_id_1");
    CHECK_FALSE(d.view.all_objects_picked());
    d.view.toggle_exclude_pick("Cylinder_id_2");
    CHECK(d.view.all_objects_picked());

    CHECK(d.view.drop_exclude_picks());
    CHECK(d.view.exclude_picks().empty());
}
```

- [ ] **Step 3: Run them to verify they fail**

Run: `make t F='[pre_start_exclude]'`
Expected: FAIL to compile (`pre_start_exclude.h` missing; `PrintSelectDetailView` lacks `exclude_objects`).

- [ ] **Step 4: The pure rules** (`include/pre_start_exclude.h`)

```cpp
#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_parser.h"
#include "printer_excluded_objects_state.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace helix::ui {

/// Whether print details offers object picks for a file.
bool pre_start_exclude_available(bool printer_has_exclude_object, bool is_3mf,
                                 size_t defined_count);

/// The defined spelling of @p name, compared upper-case as Klipper stores
/// object names; "" when no defined object matches.
std::string canonical_object_name(const std::vector<std::string>& defined,
                                  const std::string& name);

/// Whether @p picks cover every defined object, counted upper-case the way
/// Klipper would. False when nothing is defined.
bool every_object_picked(const std::vector<std::string>& defined,
                         const std::unordered_set<std::string>& picks);

/// The details object list: the scan's objects in file order, then any the
/// full parse found that the scan did not (compared upper-case). A parsed
/// outline fills a scanned object that had none.
std::vector<gcode::GCodeObject> merge_defined_objects(const std::vector<gcode::GCodeObject>& scanned,
                                                      const gcode::ParsedGCodeFile* parsed);

std::vector<PrinterExcludedObjectsState::ObjectInfo>
object_infos_from(const std::vector<gcode::GCodeObject>& objects);

} // namespace helix::ui
```

`src/ui/pre_start_exclude.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pre_start_exclude.h"

#include "text_io.h"

#include <algorithm>
#include <optional>

namespace helix::ui {

bool pre_start_exclude_available(bool printer_has_exclude_object, bool is_3mf,
                                 size_t defined_count) {
    return printer_has_exclude_object && !is_3mf && defined_count >= 2;
}

std::string canonical_object_name(const std::vector<std::string>& defined,
                                  const std::string& name) {
    const std::string wanted = helix::text_io::to_upper(name);
    for (const auto& d : defined) {
        if (helix::text_io::to_upper(d) == wanted) {
            return d;
        }
    }
    return {};
}

bool every_object_picked(const std::vector<std::string>& defined,
                         const std::unordered_set<std::string>& picks) {
    if (defined.empty()) {
        return false;
    }
    std::unordered_set<std::string> picked;
    for (const auto& p : picks) {
        picked.insert(helix::text_io::to_upper(p));
    }
    return std::all_of(defined.begin(), defined.end(), [&](const std::string& d) {
        return picked.count(helix::text_io::to_upper(d)) > 0;
    });
}

std::vector<gcode::GCodeObject> merge_defined_objects(const std::vector<gcode::GCodeObject>& scanned,
                                                      const gcode::ParsedGCodeFile* parsed) {
    std::vector<gcode::GCodeObject> out = scanned;
    if (!parsed) {
        return out;
    }
    for (const auto& [name, obj] : parsed->objects) {
        const std::string upper = helix::text_io::to_upper(name);
        auto same = std::find_if(out.begin(), out.end(), [&](const gcode::GCodeObject& o) {
            return helix::text_io::to_upper(o.name) == upper;
        });
        if (same == out.end()) {
            out.push_back(obj);
        } else if (same->polygon.empty()) {
            same->polygon = obj.polygon;
        }
    }
    return out;
}

std::vector<PrinterExcludedObjectsState::ObjectInfo>
object_infos_from(const std::vector<gcode::GCodeObject>& objects) {
    std::vector<PrinterExcludedObjectsState::ObjectInfo> out;
    out.reserve(objects.size());
    for (const auto& o : objects) {
        // A zero centre is the parser's "none", the same reading compute_object_badges() uses.
        std::optional<glm::vec2> center;
        if (o.center != glm::vec2(0.0f)) {
            center = o.center;
        }
        out.push_back(PrinterExcludedObjectsState::make_object_info(o.name, center, o.polygon));
    }
    return out;
}

} // namespace helix::ui
```

`app_srcs.txt`: insert `src/ui/pre_start_exclude.cpp` in the `src/ui/` block, directly before `src/ui/print_select_button_view.cpp`.

- [ ] **Step 5: The details private state** (`include/ui_print_select_detail_view.h`)

Add includes: `#include "ui_exclude_mode_controller.h"` and `#include "printer_excluded_objects_state.h"`. Public section, after `run_when_preflight_ready`:

```cpp
    // === Pre-start object picks ===

    /// Re-read the file's objects: the scan's list, extended by the full parse
    /// once the viewer has it. Publishes whether the skip button shows.
    void refresh_exclude_objects();

    /// Pick or un-pick a defined object (matched upper-case). A name the
    /// printer cannot be sent is refused with a toast.
    void toggle_exclude_pick(const std::string& name);

    /// Picked objects, in defined order.
    [[nodiscard]] std::vector<std::string> exclude_picks() const;

    /// Clear every pick; true when there were any.
    bool drop_exclude_picks();

    /// Whether the picks cover every object, which would print nothing.
    [[nodiscard]] bool all_objects_picked() const;

    [[nodiscard]] const helix::PrinterExcludedObjectsState& exclude_objects() const {
        return exclude_objects_;
    }
```

Private section, after `detail_mapping_ready_`:

```cpp
    /// The file's objects with the pending picks as its excluded set. Private
    /// to this view: init_subjects(false) keeps it off the XML registry.
    helix::PrinterExcludedObjectsState exclude_objects_;
    /// Exclude mode over this view's preview.
    helix::ui::ExcludeModeController exclude_mode_;
    ObserverGuard exclude_picks_observer_;
    /// 1 when the skip button shows (pre_start_exclude_available()).
    lv_subject_t detail_exclude_available_{};
    lv_subject_t detail_exclude_pick_count_{};
    lv_subject_t detail_exclude_pick_count_text_{};
    char detail_exclude_pick_count_text_buf_[8]{};
    /// Push the pick count and the viewer's faded set from the picks.
    void publish_exclude_picks();
```

- [ ] **Step 6: Implement it** (`src/ui/ui_print_select_detail_view.cpp`; add `#include "pre_start_exclude.h"`, `#include "moonraker_validation.h"`, `#include "ui_filename_utils.h"` if absent)

In `init_subjects()`, before `subjects_initialized_ = true;`:

```cpp
    // Pre-start object picks: a private model and the skip button's subjects.
    exclude_objects_.init_subjects(false);
    UI_MANAGED_SUBJECT_INT(detail_exclude_available_, 0, "detail_exclude_available", subjects_);
    UI_MANAGED_SUBJECT_INT(detail_exclude_pick_count_, 0, "detail_exclude_pick_count", subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_exclude_pick_count_text_, detail_exclude_pick_count_text_buf_,
                              "", "detail_exclude_pick_count_text", subjects_);
    exclude_picks_observer_ = observe<int>(
        exclude_objects_.get_excluded_objects_version_subject(), this,
        [](PrintSelectDetailView* self, int) { self->publish_exclude_picks(); },
        exclude_objects_.get_subjects_lifetime());
```

In `set_analysis_dependencies()`, change the scan-answered hook:

```cpp
        prep_manager_->set_on_scan_answered([this]() {
            refresh_exclude_objects();
            fire_on_preflight_ready();
        });
```

In `show()`, right after `current_gcode_end_byte_ = gcode_end_byte;`:

```cpp
    // Picks belong to one file.
    exclude_objects_.clear_objects();
```

In `on_activate()`, right after the `scan_file_for_operations(...)` block:

```cpp
    // A cached scan answers without calling back, so read it here too.
    refresh_exclude_objects();
```

Change `on_deactivating`'s signature to name the parameter and add at its top (after the debug line):

```cpp
void PrintSelectDetailView::on_deactivating(DeactivateReason reason) {
    spdlog::debug("[DetailView] on_deactivating({})", deactivate_reason_name(reason));

    // A blanked screen keeps everything; leaving the file drops its picks.
    if (reason != DeactivateReason::Suspended) {
        exclude_mode_.hide();
    }
    if (reason == DeactivateReason::NavigateAway || reason == DeactivateReason::Shutdown) {
        exclude_objects_.clear_objects();
    }
```

In `cleanup()`, before `if (subjects_initialized_) { subjects_.deinit_all(); ...}`:

```cpp
    exclude_mode_.hide();
    exclude_picks_observer_.reset();
    exclude_objects_.deinit_subjects();
```

In `on_ui_destroyed()`, first statement after the debug line:

```cpp
    // Its widgets live in the tree being torn down.
    exclude_mode_.hide();
```

In `begin_viewer_load()`'s load callback, right after `self->fire_on_preflight_ready();`:

```cpp
            // The whole file is parsed: definitions past the scan window now appear.
            self->refresh_exclude_objects();
```

New member functions (near `publish_mapping_ready`):

```cpp
void PrintSelectDetailView::refresh_exclude_objects() {
    std::vector<helix::gcode::GCodeObject> scanned;
    if (prep_manager_ && prep_manager_->has_scan_result_for(current_filename_)) {
        scanned = prep_manager_->get_scan_result()->objects;
    }
    const auto* parsed = gcode_viewer_ ? ui_gcode_viewer_get_parsed_file(gcode_viewer_) : nullptr;
    exclude_objects_.set_defined_objects_with_geometry(
        helix::ui::object_infos_from(helix::ui::merge_defined_objects(scanned, parsed)));

    const bool has_exclude_object =
        printer_state_ && printer_state_->get_discovery().has_exclude_object();
    const bool available = helix::ui::pre_start_exclude_available(
        has_exclude_object, helix::gcode::is_3mf(current_filename_),
        exclude_objects_.get_defined_objects().size());
    lv_subject_set_int(&detail_exclude_available_, available ? 1 : 0);
    publish_exclude_picks();
    exclude_mode_.refresh_render_badges();
}

void PrintSelectDetailView::toggle_exclude_pick(const std::string& name) {
    const std::string defined =
        helix::ui::canonical_object_name(exclude_objects_.get_defined_objects(), name);
    if (defined.empty()) {
        return;
    }
    auto picks = exclude_objects_.get_excluded_objects();
    if (picks.erase(defined) == 0) {
        if (!moonraker_internal::is_safe_object_name(defined)) {
            NOTIFY_WARNING(lv_tr("{} cannot be skipped: its name has characters the printer "
                                 "cannot be sent"),
                           defined);
            return;
        }
        picks.insert(defined);
    }
    spdlog::info("[DetailView] Object picks for {}: {}", current_filename_, picks.size());
    exclude_objects_.set_excluded_objects(picks);
}

std::vector<std::string> PrintSelectDetailView::exclude_picks() const {
    const auto& picks = exclude_objects_.get_excluded_objects();
    std::vector<std::string> out;
    for (const auto& name : exclude_objects_.get_defined_objects()) {
        if (picks.count(name) > 0) {
            out.push_back(name);
        }
    }
    return out;
}

bool PrintSelectDetailView::drop_exclude_picks() {
    if (exclude_objects_.get_excluded_objects().empty()) {
        return false;
    }
    exclude_objects_.set_excluded_objects({});
    return true;
}

bool PrintSelectDetailView::all_objects_picked() const {
    return helix::ui::every_object_picked(exclude_objects_.get_defined_objects(),
                                          exclude_objects_.get_excluded_objects());
}

void PrintSelectDetailView::publish_exclude_picks() {
    const auto& picks = exclude_objects_.get_excluded_objects();
    const int count = static_cast<int>(picks.size());
    lv_subject_set_int(&detail_exclude_pick_count_, count);
    std::snprintf(detail_exclude_pick_count_text_buf_, sizeof(detail_exclude_pick_count_text_buf_),
                  "%d", count);
    lv_subject_copy_string(&detail_exclude_pick_count_text_, detail_exclude_pick_count_text_buf_);
    if (gcode_viewer_) {
        ui_gcode_viewer_set_excluded_objects(gcode_viewer_, picks);
    }
}
```
If `NOTIFY_WARNING` / `deactivate_reason_name` are not yet reachable in this file, add `#include "ui_error_reporting.h"` / `#include "panel_lifecycle.h"`.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `make t F='[pre_start_exclude]'`
Expected: PASS.

- [ ] **Step 8: Translations**

```bash
make translation-sync
make translations
git status --short translations ui_xml/translations
```
Expected: the new string `{} cannot be skipped: its name has characters the printer cannot be sent` appears in `translations/en.yml` and the regenerated packs.

- [ ] **Step 9: Sweep, gates, commit**

```bash
make unit-sweep
python3 scripts/check_esp32_app_srcs.py
git add -- include/pre_start_exclude.h src/ui/pre_start_exclude.cpp include/ui_print_select_detail_view.h src/ui/ui_print_select_detail_view.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs.txt tests/unit/test_pre_start_exclude.cpp tests/unit/test_print_select_detail_subjects.cpp translations ui_xml/translations
git commit -m "feat(print-select): details holds object picks in its own exclude state" -m "A private PrinterExcludedObjectsState lists the file's objects from the scan, extended by the full parse, and its excluded set holds the picks. Names match upper-case; a name the printer cannot be sent is refused at the tap. Picks clear on leaving the file and survive a screen suspend." -m "mutation: removed refresh_exclude_objects() from on_activate; the re-open-same-file case went red"
git show --stat HEAD
```

---

### Task 8: The skip button and exclude mode in print details

**Files:**
- Modify: `ui_xml/print_file_detail.xml` (the top-left of `detail_card`)
- Modify: `include/ui_print_select_detail_view.h`, `src/ui/ui_print_select_detail_view.cpp`
- Modify: `include/ui_panel_print_select.h`, `src/ui/ui_panel_print_select.cpp` (XML callback)
- Test: `tests/unit/test_print_select_detail_subjects.cpp`

**Interfaces:**
- Consumes: `exclude_objects_button` (Task 6), `ExcludeModeController` (Task 5), Task 7 subjects and `toggle_exclude_pick`.
- Produces: `void PrintSelectDetailView::toggle_exclude_mode();`, `[[nodiscard]] bool PrintSelectDetailView::is_exclude_mode_open() const;`, `void PrintSelectPanel::toggle_detail_exclude_mode();`, XML callback `on_print_select_detail_objects`, widget `btn_detail_objects`.

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_print_select_detail_subjects.cpp`, reusing Task 7's `OpenDetail` / `ExcludeObjectHardware`)

```cpp
TEST_CASE_METHOD(LVGLUITestFixture,
                 "The details skip button sits in the preview's top-left corner with a pick count",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    lv_obj_t* root = d.view.get_widget();
    lv_obj_t* btn = lv_obj_find_by_name(root, "btn_detail_objects");
    lv_obj_t* card = lv_obj_find_by_name(root, "detail_card");
    REQUIRE(btn != nullptr);
    REQUIRE(card != nullptr);
    process_lvgl(20);
    CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    lv_obj_update_layout(root);
    lv_area_t b, c;
    lv_obj_get_coords(btn, &b);
    lv_obj_get_coords(card, &c);
    const int32_t inset = theme_manager_get_spacing("space_md");
    CHECK(b.x1 - c.x1 == inset);
    CHECK(b.y1 - c.y1 == inset);

    lv_obj_t* count = lv_obj_find_by_name(btn, "objects_pick_count");
    REQUIRE(count != nullptr);
    CHECK(lv_obj_has_flag(count, LV_OBJ_FLAG_HIDDEN));

    d.view.toggle_exclude_pick("Cube_id_1");
    d.view.toggle_exclude_pick("Cone_id_0");
    OpenDetail::settle();
    process_lvgl(20);
    CHECK_FALSE(lv_obj_has_flag(count, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(lv_obj_get_child(count, 0))) == "2");
}

TEST_CASE_METHOD(LVGLUITestFixture, "The details skip button hides for a single-object file",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "solo.gcode", "EXCLUDE_OBJECT_DEFINE NAME=Solo CENTER=1,1\n");
    process_lvgl(20);
    lv_obj_t* btn = lv_obj_find_by_name(d.view.get_widget(), "btn_detail_objects");
    REQUIRE(btn != nullptr);
    CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Exclude mode in details toggles a pick on a row tap, with no confirmation",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);

    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    process_lvgl(50);
    REQUIRE(d.view.is_exclude_mode_open());
    lv_obj_t* rows = lv_obj_find_by_name(d.view.get_widget(), "rows_container");
    REQUIRE(rows != nullptr);
    REQUIRE(lv_obj_get_child_count(rows) == 3);

    lv_obj_send_event(lv_obj_get_child(rows, 1), LV_EVENT_CLICKED, nullptr);
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
    OpenDetail::settle();
    process_lvgl(20);
    CHECK(lv_obj_has_flag(lv_obj_get_child(rows, 1), LV_OBJ_FLAG_CLICKABLE));

    lv_obj_send_event(lv_obj_get_child(rows, 1), LV_EVENT_CLICKED, nullptr);
    CHECK(d.view.exclude_picks().empty());

    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    CHECK_FALSE(d.view.is_exclude_mode_open());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Leaving details closes exclude mode",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    REQUIRE(d.view.is_exclude_mode_open());

    d.close();
    process_lvgl(50);
    CHECK_FALSE(d.view.is_exclude_mode_open());
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `make t F='[pre_start_exclude]'`
Expected: FAIL to compile (`toggle_exclude_mode` undeclared).

- [ ] **Step 3: The button in the details card** (`ui_xml/print_file_detail.xml`)

Replace the `detail_loading_overlay` element (and its leading comment stays) with a row holding the button and the pill:

```xml
          <!-- Top-left of the preview: the skip-objects button (same component
               and corner as print status) and, beside it, the loading pill. -->
          <lv_obj name="detail_top_left_row"
                  width="content" height="content" align="top_left" x="#space_md" y="#space_md" flex_flow="row"
                  style_flex_cross_place="center" style_pad_all="0" style_pad_gap="#space_sm" style_bg_opa="0"
                  style_border_width="0" scrollable="false">
            <exclude_objects_button name="btn_detail_objects"
                                    callback="on_print_select_detail_objects"
                                    hidden_when="detail_exclude_available eq 0"
                                    count_text_subject="detail_exclude_pick_count_text"
                                    count_hidden_when="detail_exclude_pick_count eq 0"/>
            <lv_obj name="detail_loading_overlay"
                    width="content" height="content" flex_flow="row" style_flex_cross_place="center"
                    style_pad_top="#space_xs" style_pad_bottom="#space_xs" style_border_width="0" scrollable="false">
              <style name="styles.metadata_strip"/>
              <bind_flag_if_eq subject="detail_gcode_loading" flag="hidden" ref_value="0"/>
              <spinner size="xs"/>
              <text_small name="detail_loading_label" bind_text="detail_gcode_progress_text"/>
            </lv_obj>
          </lv_obj>
```
(the pill keeps its children and bindings; only `align`, `x` and `y` move to the row.)

- [ ] **Step 4: Toggle exclude mode** (`include/ui_print_select_detail_view.h`, public, next to Task 7's block)

```cpp
    /// Open or close exclude mode over the preview: map + list in thumbnail
    /// mode, render badges + list in 2D/3D. Taps toggle picks.
    void toggle_exclude_mode();
    [[nodiscard]] bool is_exclude_mode_open() const {
        return exclude_mode_.is_open();
    }
```

`src/ui/ui_print_select_detail_view.cpp` (add `#include "bed_dimensions.h"`):

```cpp
void PrintSelectDetailView::toggle_exclude_mode() {
    if (exclude_mode_.is_open()) {
        exclude_mode_.hide();
        return;
    }
    if (!overlay_root_) {
        return;
    }
    helix::ui::ExcludeModeTargets targets;
    targets.card = detail_card_;
    targets.columns = lv_obj_find_by_name(overlay_root_, "content_container");
    targets.controls_name = "options_section";
    targets.gcode_viewer = gcode_viewer_;
    targets.thumbnail_mode = lv_subject_get_int(&detail_viewer_hidden_) == 1;
    const auto bed = helix::bed_dimensions(api_, printer_state_);
    targets.bed_w_mm = bed.w_mm;
    targets.bed_h_mm = bed.h_mm;
    exclude_mode_.show(targets, &exclude_objects_, helix::ui::ExcludeTapMode::Toggle,
                       [this](const std::string& name) { toggle_exclude_pick(name); });
}
```

- [ ] **Step 5: Wire the XML callback** (`include/ui_panel_print_select.h`: public `void toggle_detail_exclude_mode();`)

`src/ui/ui_panel_print_select.cpp`, in the detail view callbacks list of `init_subjects()`:

```cpp
        {"on_print_select_detail_objects",
         [](lv_event_t*) { get_global_print_select_panel().toggle_detail_exclude_mode(); }},
```
and next to `forward_sliced_colors_toggle`:

```cpp
void PrintSelectPanel::toggle_detail_exclude_mode() {
    if (detail_view_) {
        detail_view_->toggle_exclude_mode();
    }
}
```
Add `{"on_print_select_detail_objects", detail_noop_cb},` to the `register_xml_callbacks` lists in `tests/unit/test_detail_gcode_download_integrity.cpp` and every list in `tests/unit/test_print_select_detail_subjects.cpp`, so those views create without an unknown-callback warning.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `make t F='[print_select]'`
Expected: PASS (including `test_print_select_lazy_load.cpp`, which looks up `detail_no_thumbnail_icon`).

- [ ] **Step 7: Sweep, ratchets, commit**

```bash
make unit-sweep
python3 scripts/check_imperative_ui.py --summary
python3 scripts/check_hardcoded_pixels.py --summary
git add -- ui_xml/print_file_detail.xml include/ui_print_select_detail_view.h src/ui/ui_print_select_detail_view.cpp include/ui_panel_print_select.h src/ui/ui_panel_print_select.cpp tests/unit/test_print_select_detail_subjects.cpp tests/unit/test_detail_gcode_download_integrity.cpp
git commit -m "feat(print-select): pick objects to skip from the details preview" -m "Print details shows the same skip button in the same corner as print status, with a count badge once anything is picked. It opens the shared exclude mode in toggle mode: a tap on a row, map outline or render badge picks or un-picks instantly, and leaving the view closes it." -m "mutation: dropped count_hidden_when from the details instance; the corner-and-count case went red"
git show --stat HEAD
```

---

### Task 9: Send the picks after Moonraker confirms the start

**Files:**
- Modify: `include/pre_start_exclude.h`, `src/ui/pre_start_exclude.cpp`
- Modify (translations): `translations/*.yml`, `ui_xml/translations/*.xml`
- Test: `tests/unit/test_pre_start_exclude.cpp`

**Interfaces:**
- Consumes: `IMoonrakerAPI::exclude_object(const std::string&, SuccessCallback, ErrorCallback)`; `get_moonraker_api()`, `get_printer_state()` (`app_globals.h`).
- Produces (namespace `helix::ui`):
  - `void send_pre_start_exclusions(IMoonrakerAPI* api, std::vector<std::string> names);` — main thread; one `EXCLUDE_OBJECT` per name; when every answer is in, an error toast `Could not skip {names}` names the failures, unless the print already ended (CANCELLED / ERROR). TIMEOUT is not a failure.
  - `std::function<void()> with_pre_start_exclusions(std::function<void()> on_confirmed, std::vector<std::string> names);` — callable from any thread; runs `on_confirmed`, then queues `send_pre_start_exclusions(get_moonraker_api(), names)` on the main thread. Returns `on_confirmed` unchanged when `names` is empty.

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_pre_start_exclude.cpp`; add includes `"../lvgl_test_fixture.h"`, `"../test_helpers/print_select_panel_fixture.h"`, `"../test_helpers/update_queue_test_access.h"`, `"../ui_test_utils.h"`, `"app_globals.h"`, `"moonraker_api_mock.h"`, `"moonraker_client_mock.h"`, `"moonraker_error.h"`)

```cpp
namespace {

/// The mock printer behind get_moonraker_api(), with error toasts captured.
class SendFixture : private helix::PrintSelectGlobalStateReset, public LVGLTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::vector<std::string> errors;

    SendFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        set_moonraker_api(api.get());
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& m) { errors.push_back(m); });
        client.clear_gcode_script_history();
    }
    ~SendFixture() override {
        helix::ui::set_test_notification_error_hook(nullptr);
        set_moonraker_api(nullptr);
        helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::STANDBY);
        settle();
    }
    void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    std::vector<std::string> exclusions() const {
        std::vector<std::string> out;
        for (const auto& s : client.gcode_script_history()) {
            if (s.rfind("EXCLUDE_OBJECT NAME=", 0) == 0) {
                out.push_back(s);
            }
        }
        return out;
    }
};

} // namespace

TEST_CASE_METHOD(SendFixture, "Picks go out only once the start is confirmed",
                 "[pre_start_exclude][send]") {
    int confirmed = 0;
    auto on_confirmed = with_pre_start_exclusions([&] { ++confirmed; }, {"Cone_id_0", "Cube_id_1"});
    settle();
    CHECK(exclusions().empty());

    on_confirmed();
    CHECK(confirmed == 1);
    settle();
    CHECK(exclusions() ==
          std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cone_id_0", "EXCLUDE_OBJECT NAME=Cube_id_1"});
    CHECK(errors.empty());
}

TEST_CASE_METHOD(SendFixture, "No picks leave the confirmation callback as it was",
                 "[pre_start_exclude][send]") {
    int confirmed = 0;
    with_pre_start_exclusions([&] { ++confirmed; }, {})();
    settle();
    CHECK(confirmed == 1);
    CHECK(exclusions().empty());
}

TEST_CASE_METHOD(SendFixture, "A failed exclusion is named in one toast; a timeout is not a failure",
                 "[pre_start_exclude][send]") {
    client.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST, "Klippy Disconnected",
                                  "NAME=Cube_id_1");
    send_pre_start_exclusions(api.get(), {"Cone_id_0", "Cube_id_1"});
    settle();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("Cube_id_1") != std::string::npos);
    CHECK(errors[0].find("Cone_id_0") == std::string::npos);

    errors.clear();
    client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "timed out", "NAME=Cone_id_0");
    send_pre_start_exclusions(api.get(), {"Cone_id_0"});
    settle();
    CHECK(errors.empty());
}

TEST_CASE_METHOD(SendFixture, "A failure after the print was cancelled raises no toast",
                 "[pre_start_exclude][send]") {
    helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::CANCELLED);
    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "Unknown object",
                                  "NAME=Cube_id_1");
    send_pre_start_exclusions(api.get(), {"Cube_id_1"});
    settle();
    CHECK(errors.empty());
}

TEST_CASE_METHOD(SendFixture,
                 "A confirmation that lands with no printer API reports the picks it could not send",
                 "[pre_start_exclude][send]") {
    auto on_confirmed = with_pre_start_exclusions([] {}, {"Cube_id_1"});
    set_moonraker_api(nullptr); // printer switched away before the start was confirmed
    on_confirmed();
    settle();
    CHECK(exclusions().empty());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("Cube_id_1") != std::string::npos);
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `make t F='[pre_start_exclude][send]'`
Expected: FAIL to compile (`with_pre_start_exclusions` undeclared).

- [ ] **Step 3: Declare** (`include/pre_start_exclude.h`; add `#include <functional>` and, before `namespace helix::ui`, `class IMoonrakerAPI;`)

```cpp
/// Send EXCLUDE_OBJECT for each picked name. Main thread, after Moonraker
/// confirmed the start: Klipper clears exclude_object state as a print starts.
/// One error toast names whatever failed, unless the print has already ended.
void send_pre_start_exclusions(IMoonrakerAPI* api, std::vector<std::string> names);

/// @p on_confirmed, followed by the exclusions for @p names on the main
/// thread. Callable from any thread. It holds only the names and reads the
/// current API when it fires, so the view that started the print may be gone.
std::function<void()> with_pre_start_exclusions(std::function<void()> on_confirmed,
                                                std::vector<std::string> names);
```

- [ ] **Step 4: Implement** (`src/ui/pre_start_exclude.cpp`; add `#include "ui_error_reporting.h"`, `#include "ui_update_queue.h"`, `#include "app_globals.h"`, `#include "i_moonraker_api.h"`, `#include "moonraker_error.h"`, `#include "printer_state.h"`, `#include <memory>`, `#include <spdlog/spdlog.h>`)

```cpp
namespace {

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += n;
    }
    return out;
}

void report_unsent(const std::vector<std::string>& names) {
    const auto state = get_printer_state().print_state().get_print_job_state();
    if (state == helix::PrintJobState::CANCELLED || state == helix::PrintJobState::ERROR) {
        spdlog::info("[PreStartExclude] Print ended before {} could be skipped", join_names(names));
        return;
    }
    NOTIFY_ERROR(lv_tr("Could not skip {}"), join_names(names));
}

} // namespace

void send_pre_start_exclusions(IMoonrakerAPI* api, std::vector<std::string> names) {
    if (names.empty()) {
        return;
    }
    if (!api) {
        spdlog::warn("[PreStartExclude] No printer API to send {} picks", names.size());
        report_unsent(names);
        return;
    }

    // Answers come back on the HTTP thread; each one hops to the main thread,
    // where this batch is only ever touched.
    struct Batch {
        size_t pending = 0;
        std::vector<std::string> failed;
    };
    auto batch = std::make_shared<Batch>();
    batch->pending = names.size();
    auto settle = [batch](std::string failed_name) {
        if (!failed_name.empty()) {
            batch->failed.push_back(std::move(failed_name));
        }
        if (--batch->pending > 0 || batch->failed.empty()) {
            return;
        }
        report_unsent(batch->failed);
    };

    for (const auto& name : names) {
        spdlog::info("[PreStartExclude] Skipping '{}' in the print that just started", name);
        api->exclude_object(
            name,
            [settle]() {
                helix::ui::queue_update("PreStartExclude::sent", [settle]() { settle({}); });
            },
            [settle, name](const MoonrakerError& err) {
                // A timeout only means PRINT_START is still running ahead of it.
                const bool failed = err.type != MoonrakerErrorType::TIMEOUT;
                spdlog::warn("[PreStartExclude] EXCLUDE_OBJECT '{}' answered: {}", name,
                             err.message);
                helix::ui::queue_update("PreStartExclude::failed", [settle, name, failed]() {
                    settle(failed ? name : std::string{});
                });
            });
    }
}

std::function<void()> with_pre_start_exclusions(std::function<void()> on_confirmed,
                                                std::vector<std::string> names) {
    if (names.empty()) {
        return on_confirmed;
    }
    return [on_confirmed = std::move(on_confirmed), names = std::move(names)]() {
        if (on_confirmed) {
            on_confirmed();
        }
        helix::ui::queue_update("PreStartExclude::send", [names]() {
            send_pre_start_exclusions(get_moonraker_api(), names);
        });
    };
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make t F='[pre_start_exclude]'`
Expected: PASS.

- [ ] **Step 6: Translations**

```bash
make translation-sync
make translations
```
Expected: `Could not skip {}` lands in `translations/en.yml` and the packs.

- [ ] **Step 7: Sweep, gates, commit**

```bash
make unit-sweep
python3 scripts/check_esp32_app_srcs.py
git add -- include/pre_start_exclude.h src/ui/pre_start_exclude.cpp tests/unit/test_pre_start_exclude.cpp translations ui_xml/translations
git commit -m "feat(exclude): send object picks once Moonraker confirms the start" -m "with_pre_start_exclusions() wraps a start-confirmed callback so each pick goes out as EXCLUDE_OBJECT behind PRINT_START, through the existing 15 minute silent exclude_object. Real failures are named in one toast; a timeout, or a print cancelled first, is not." -m "mutation: sent the exclusions when the wrapper was built instead of when it fired; the only-once-confirmed case went red"
git show --stat HEAD
```

---

### Task 10: Every start path carries the picks

**Files:**
- Modify: `include/ui_print_start_controller.h`, `src/ui/ui_print_start_controller.cpp` (`execute_print_start`)
- Modify: `include/ui_panel_print_select.h`, `src/ui/ui_panel_print_select.cpp` (`start_print`, `dispatch_print`, `apply_remap`)
- Modify: `tests/test_helpers/print_start_controller_test_access.h`, `tests/test_helpers/print_select_panel_test_access.h`
- Modify: `docs/devel/EXCLUDE_OBJECTS.md`
- Modify (translations): `translations/*.yml`, `ui_xml/translations/*.xml`
- Test: `tests/unit/test_print_select_pre_start_exclude.cpp` (new)

**Interfaces:**
- Consumes: `with_pre_start_exclusions` (Task 9); `PrintSelectDetailView::{exclude_picks, drop_exclude_picks, all_objects_picked, refresh_exclude_objects, toggle_exclude_pick}` (Task 7).
- Produces:
  - `void PrintStartController::set_exclude_picks(std::vector<std::string> picks);` (consumed by the next `execute_print_start`)
  - `void PrintSelectPanel::dispatch_print(const std::string& filename, const std::string& dir, const std::vector<std::string>& filament_colors, const std::string& thumbnail, std::vector<std::string> exclude_picks);`
  - `bool PrintSelectPanel::refuse_start_with_every_object_picked();`
  - Test access: `PrintStartControllerTestAccess::execute(PrintStartController&)`, `PrintStartControllerTestAccess::exclude_picks(const PrintStartController&)`, `PrintSelectPanelTestAccess::detail_view(PrintSelectPanel&)`, `PrintSelectPanelTestAccess::print_controller(PrintSelectPanel&)`.

- [ ] **Step 1: Test access helpers**

`tests/test_helpers/print_start_controller_test_access.h`, inside the class:

```cpp
    // --- pre-start object picks (test_print_select_pre_start_exclude.cpp) ---

    /// Run the start pipeline past initiate()'s grace period and gates.
    static void execute(helix::ui::PrintStartController& c) {
        c.execute_print_start();
    }

    static const std::vector<std::string>& exclude_picks(const helix::ui::PrintStartController& c) {
        return c.exclude_picks_;
    }
```

`tests/test_helpers/print_select_panel_test_access.h`, inside the class:

```cpp
    static helix::ui::PrintSelectDetailView* detail_view(PrintSelectPanel& panel) {
        return panel.detail_view_.get();
    }

    static helix::ui::PrintStartController* print_controller(PrintSelectPanel& panel) {
        return panel.print_controller_.get();
    }
```

- [ ] **Step 2: Write the failing tests** (`tests/unit/test_print_select_pre_start_exclude.cpp`)

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

// Object picks made in print details reach the print: captured at the Print
// tap, sent only once Moonraker confirms the start, refused when they cover
// every object, and dropped (with a toast) when the tap queues the file.

#include "ui_print_start_controller.h"
#include "ui_update_queue.h"

#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::test::set_wire_state;

namespace {

const char* kPlate = "; HEADER\n"
                     "EXCLUDE_OBJECT_DEFINE NAME=Cone_id_0 CENTER=25,-4\n"
                     "EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1 CENTER=-36,6\n"
                     "EXCLUDE_OBJECT_DEFINE NAME=Cylinder_id_2 CENTER=-22,30\n"
                     "G28\n";

/// A real panel over the mock API with a three-object file's details open.
class PickStartFixture : private helix::PrintSelectGlobalStateReset,
                         public helix::PrintSelectPanelFixture {
  public:
    PickStartFixture()
        : helix::PrintSelectPanelFixture(helix::PrintSelectFilelistHandler::Unregistered,
                                         helix::PrintSelectVisit::Immediate,
                                         helix::PrintSelectApi::Mock) {
        set_moonraker_api(api_.get());
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json{"exclude_object", "extruder"});
        get_printer_state().set_hardware(hw);
        helix::ui::set_test_notification_info_hook(
            [this](const std::string& m) { infos.push_back(m); });
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& m) { warnings.push_back(m); });

        panel_->refresh_files(/*force=*/true);
        drain();
        REQUIRE(panel_->select_file_by_name(file_.name()));
        drain();
        detail = PrintSelectPanelTestAccess::detail_view(*panel_);
        REQUIRE(detail != nullptr);
        REQUIRE(detail->exclude_objects().get_defined_objects().size() == 3);
        mock_client_.clear_gcode_script_history();
    }

    ~PickStartFixture() override {
        helix::ui::set_test_notification_info_hook(nullptr);
        helix::ui::set_test_notification_warning_hook(nullptr);
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        ps.capabilities_state().set_job_queue_available(false);
        ps.set_hardware(helix::PrinterDiscovery{});
        set_moonraker_api(nullptr);
        drain();
    }

    std::vector<std::string> exclusions() const {
        std::vector<std::string> out;
        for (const auto& s : mock_client_.gcode_script_history()) {
            if (s.rfind("EXCLUDE_OBJECT NAME=", 0) == 0) {
                out.push_back(s);
            }
        }
        return out;
    }

    /// Hold printer.print.start's answer until the test releases it.
    void hold_print_start() {
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            mock_client_, "printer.print.start",
            [this](MoonrakerClientMock*, const json&, std::function<void(const json&)> success_cb,
                   std::function<void(const MoonrakerError&)>) -> bool {
                held_start = std::move(success_cb);
                return true;
            });
    }

    static bool contains(const std::vector<std::string>& msgs, const std::string& needle) {
        for (const auto& m : msgs) {
            if (m.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    helix::PlantedGcode file_{"pick_start.gcode", "", kPlate};
    helix::ui::PrintSelectDetailView* detail = nullptr;
    std::function<void(const json&)> held_start;
    std::vector<std::string> infos;
    std::vector<std::string> warnings;
};

} // namespace

TEST_CASE_METHOD(PickStartFixture, "Picks are sent only after Moonraker confirms the start",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cube_id_1");
    auto* controller = PrintSelectPanelTestAccess::print_controller(*panel_);
    REQUIRE(controller != nullptr);
    controller->set_navigate_to_print_status(nullptr); // no status overlay in a unit test
    controller->set_file(file_.name(), "", {}, "");
    controller->set_exclude_picks(detail->exclude_picks());
    hold_print_start();

    PrintStartControllerTestAccess::execute(*controller);
    drain();
    REQUIRE(held_start);
    CHECK(exclusions().empty()); // nothing before Moonraker answers

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
    CHECK(PrintStartControllerTestAccess::exclude_picks(*controller).empty()); // consumed
}

TEST_CASE_METHOD(PickStartFixture,
                 "Opening another file while the start is in flight keeps the started file's picks",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cone_id_0");
    auto* controller = PrintSelectPanelTestAccess::print_controller(*panel_);
    REQUIRE(controller != nullptr);
    controller->set_navigate_to_print_status(nullptr);
    controller->set_file(file_.name(), "", {}, "");
    controller->set_exclude_picks(detail->exclude_picks());
    hold_print_start();
    PrintStartControllerTestAccess::execute(*controller);
    drain();
    REQUIRE(held_start);

    PrintSelectPanelTestAccess::hide_detail_view(*panel_);
    drain();
    lv_timer_handler(); // the close callback runs on the next tick
    helix::PlantedGcode other("other_file.gcode", "", kPlate);
    panel_->refresh_files(/*force=*/true);
    drain();
    REQUIRE(panel_->select_file_by_name(other.name()));
    drain();
    CHECK(detail->exclude_picks().empty());

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cone_id_0"});
}

TEST_CASE_METHOD(PickStartFixture, "The Print tap hands the picks it saw to the start controller",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cylinder_id_2");
    // A running print makes initiate() refuse after set_file(), so the hand-off
    // is read without driving a real start.
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    panel_->start_print(/*force=*/true);
    drain();
    auto* controller = PrintSelectPanelTestAccess::print_controller(*panel_);
    REQUIRE(controller != nullptr);
    CHECK(PrintStartControllerTestAccess::exclude_picks(*controller) ==
          std::vector<std::string>{"Cylinder_id_2"});
}

TEST_CASE_METHOD(PickStartFixture, "Every object picked refuses the start before anything is sent",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cone_id_0");
    detail->toggle_exclude_pick("Cube_id_1");
    detail->toggle_exclude_pick("Cylinder_id_2");
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(contains(warnings, "Every object is set to skip"));
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first.empty());
    CHECK(exclusions().empty());
}

TEST_CASE_METHOD(PickStartFixture, "A queued start drops the picks and says so",
                 "[print_select][pre_start_exclude][start][job_queue]") {
    auto& ps = get_printer_state();
    JobQueueState jqs(api_.get(), &mock_client_);
    JobQueueState* previous = get_job_queue_state();
    set_job_queue_state(&jqs);
    set_wire_state(ps, PrintJobState::PRINTING);
    ps.capabilities_state().set_job_queue_available(true);
    drain();
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "print_select_button_mode")) == 1);

    detail->toggle_exclude_pick("Cube_id_1");
    panel_->start_print();
    drain();

    CHECK(contains(infos, "Object picks apply only to prints started now"));
    CHECK(detail->exclude_picks().empty());
    CHECK(exclusions().empty());
    set_job_queue_state(previous);
}
```

- [ ] **Step 3: Run them to verify they fail**

Run: `make t F='[pre_start_exclude][start]'`
Expected: FAIL to compile (`set_exclude_picks` undeclared).

- [ ] **Step 4: The controller carries the picks** (`include/ui_print_start_controller.h`)

Public, next to `set_file`:

```cpp
    /// Objects to skip in the next start, captured at the Print tap. Consumed
    /// by that start; a reprint never reads them.
    void set_exclude_picks(std::vector<std::string> picks) {
        exclude_picks_ = std::move(picks);
    }
```
Private member next to `filament_colors_`: `std::vector<std::string> exclude_picks_;`.

`src/ui/ui_print_start_controller.cpp` (add `#include "pre_start_exclude.h"`), in `execute_print_start()` after `std::string thumbnail_path = thumbnail_path_;`:

```cpp
    std::vector<std::string> exclude_picks = std::move(exclude_picks_);
    exclude_picks_.clear();
```
add `exclude_picks` to the `start_now` capture list, and wrap the navigation callback handed to `prep_manager->start_print(...)`:

```cpp
        prep_manager->start_print(
            filename_to_print, path,
            // Called when Moonraker confirms the start, from the HTTP thread. The
            // object picks follow it: Klipper resets exclude_object as a print starts.
            helix::ui::with_pre_start_exclusions(
                [filename_to_print, path, thumbnail_path, on_started]() {
                    // ... existing body unchanged ...
                },
                exclude_picks),
            // Completion callback ... unchanged
```

- [ ] **Step 5: The panel captures, refuses and drops** (`include/ui_panel_print_select.h`: change `dispatch_print`'s declaration to the Interfaces signature; add private `bool refuse_start_with_every_object_picked();`)

`src/ui/ui_panel_print_select.cpp` (add `#include "pre_start_exclude.h"` if a helper from it is used; `NOTIFY_*` come from `ui_error_reporting.h`). In `start_print`, replace the queue branch and add the refusal right after it:

```cpp
    if (print_button_mode_ == helix::ui::PrintSelectButtonMode::Queue) {
        if (detail_view_ && detail_view_->drop_exclude_picks()) {
            NOTIFY_INFO(lv_tr("Object picks apply only to prints started now"));
        }
        add_to_queue();
        return;
    }

    if (refuse_start_with_every_object_picked()) {
        return;
    }
```
Where the tapped file is read (`const std::string filename = selected_filename_buffer_;` block), add:

```cpp
    std::vector<std::string> exclude_picks =
        detail_view_ ? detail_view_->exclude_picks() : std::vector<std::string>{};
```
and pass it on both paths:

```cpp
    if (!selected_local_path_.empty()) {
        copy_usb_file_to_printer([this, colors = std::move(colors), thumbnail = std::move(thumbnail),
                                  exclude_picks](const std::string& dest) {
            const size_t slash = dest.rfind('/');
            dispatch_print(dest.substr(slash + 1), dest.substr(0, slash), colors, thumbnail,
                           exclude_picks);
        });
        return;
    }
    dispatch_print(filename, current_path_, colors, thumbnail, std::move(exclude_picks));
```

`dispatch_print`:

```cpp
void PrintSelectPanel::dispatch_print(const std::string& filename, const std::string& dir,
                                      const std::vector<std::string>& filament_colors,
                                      const std::string& thumbnail,
                                      std::vector<std::string> exclude_picks) {
    // Pass extracted thumbnail path so USB/embedded thumbnails propagate to print status
    print_controller_->set_file(filename, dir, filament_colors, thumbnail);
    // The picks seen at the tap: a USB copy can land after another file opened.
    print_controller_->set_exclude_picks(std::move(exclude_picks));
    // ... rest unchanged ...
```

New helper:

```cpp
bool PrintSelectPanel::refuse_start_with_every_object_picked() {
    if (!detail_view_ || !detail_view_->all_objects_picked()) {
        return false;
    }
    NOTIFY_WARNING(lv_tr("Every object is set to skip, so there is nothing to print"));
    return true;
}
```

`apply_remap`, `GcodeRewrite` case (this remap prints a rewritten copy, so it is a start path too), before `prep->modify_and_print_with_remap(...)`:

```cpp
        if (refuse_start_with_every_object_picked()) {
            return;
        }
        prep->modify_and_print_with_remap(
            file_path, remap,
            helix::ui::with_pre_start_exclusions(
                [this]() { PrintStatusPanel::push_overlay(parent_screen_); },
                detail_view_ ? detail_view_->exclude_picks() : std::vector<std::string>{}));
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `make t F='[print_select]'`
Expected: PASS (`test_print_select_usb_print.cpp`, `test_print_select_add_queue.cpp` included).

- [ ] **Step 7: Translations**

```bash
make translation-sync
make translations
```
Expected: `Object picks apply only to prints started now` and `Every object is set to skip, so there is nothing to print` land in `translations/en.yml` and the packs.

- [ ] **Step 8: Document the feature** (`docs/devel/EXCLUDE_OBJECTS.md`)

- Key Files table: add rows for `include/ui_exclude_mode_controller.h` / `src/ui/ui_exclude_mode_controller.cpp` (exclude mode over a preview, shared by print status and details), `include/pre_start_exclude.h` / `src/ui/pre_start_exclude.cpp` (pick rules and the after-start send), `ui_xml/components/exclude_objects_button.xml` (the shared skip button).
- New section `## Choosing objects before a print starts`, after `## Print Objects Side List`, stating: details owns a private `PrinterExcludedObjectsState` (`init_subjects(false)`) filled by `PrintSelectDetailView::refresh_exclude_objects()` from `ScanResult::objects` (first `PRINTER_STOP_SCAN_BYTES`) merged with the viewer's parsed objects; the excluded set is the picks; taps toggle through `ExcludeTapMode::Toggle`; the button shows when `pre_start_exclude_available()`; picks are captured in `PrintSelectPanel::start_print`, carried by `PrintStartController::set_exclude_picks`, and sent by `with_pre_start_exclusions()` once Moonraker confirms the start because Klipper resets `exclude_object` at `virtual_sdcard:reset_file`; a queued start drops picks with a toast; every-object-picked refuses the start; names the printer cannot be sent are refused at the tap; a failed send is one error toast unless the print ended first. Cite code as `path#symbol`.
- `### Adding Exclude Objects to a New Panel`: replace the manager-only recipe with owning an `ExcludeModeController` and calling `show(ExcludeModeTargets, state, mode, on_tap)` / `hide()`, citing `src/ui/ui_panel_print_status.cpp#PrintStatusPanel::show_exclude_map_view` and `src/ui/ui_print_select_detail_view.cpp#PrintSelectDetailView::toggle_exclude_mode`.
- `### Test Files`: add `tests/unit/test_exclude_mode_controller.cpp`, `tests/unit/test_pre_start_exclude.cpp`, `tests/unit/test_print_select_pre_start_exclude.cpp`.
- `### Testing with Mock Mode`: add the pre-start recipe from Task 11 Step 2, and note `HELIX_MOCK_EXCLUDE_OBJECTS` replaces the file's object names at print start, so it must stay unset for this check.

Run `make check-doc-anchors` and fix any anchor it cannot resolve.

- [ ] **Step 9: Sweep and commit**

```bash
make unit-sweep
git add -- include/ui_print_start_controller.h src/ui/ui_print_start_controller.cpp include/ui_panel_print_select.h src/ui/ui_panel_print_select.cpp tests/test_helpers/print_start_controller_test_access.h tests/test_helpers/print_select_panel_test_access.h tests/unit/test_print_select_pre_start_exclude.cpp docs/devel/EXCLUDE_OBJECTS.md translations ui_xml/translations
git commit -m "feat(print-select): object picks reach every print started from details" -m "Picks are read at the Print tap with the file, carried by the start controller and sent once Moonraker confirms the start, on the direct, plugin-modify, pre-start-wait and remap-rewrite paths alike. Picking every object refuses the start; a tap that queues the file drops the picks and says so." -m "mutation: dropped set_exclude_picks() from dispatch_print; the Print-tap hand-off case went red"
git show --stat HEAD
```

---

### Task 11: Verify on the mock, then the full gates

**Files:** none changed unless a check fails (fixes go back to the owning task's files).

**Interfaces:**
- Consumes: the whole branch.
- Produces: the evidence for the final report (ctl output, six screenshots, full-test-run and mutation results).

- [ ] **Step 1: Build and start an isolated mock**

```bash
cd /home/pbrown/Code/Printing/helixscreen/.worktrees/pre-start-exclude
pgrep -x -d' ' 'make|clang++|cc1plus' || true
scripts/helix-claim check build:pre-start-exclude && scripts/helix-claim take build:pre-start-exclude "mock verification"
make
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
unset HELIX_MOCK_EXCLUDE_OBJECTS
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test --sim-speed 6 -vv --render-2d --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
./build/bin/helix-screen ctl -s "$HELIX_SOCK" ping
```
Expected: `pong`.

- [ ] **Step 2: Pick 2 of 3 objects and start**

```bash
C="./build/bin/helix-screen ctl -s $HELIX_SOCK"
$C navigate print-select
$C ls                                   # find the card whose label reads exclude_object_test.gcode
$C click <that card's path from ls>
$C state btn_detail_objects             # flags.hidden must be false
$C click btn_detail_objects
$C ls rows_container                    # three rows: Cone_id_0_copy_0, Cube_id_1_copy_0, Cylinder_id_2_copy_0
$C click <path of row 1 from ls>
$C click <path of row 3 from ls>
$C text objects_pick_count              # "2"
$C click close_btn
$C click print_button
```
Wait for print status to reach Printing (`$C text print_layer_text` changing), then:

```bash
$C text objects_count_label             # expect "1/3"
grep -n "EXCLUDE_OBJECT: '" /tmp/helix-$TREE.log
grep -n "PreStartExclude" /tmp/helix-$TREE.log
```
Expected: `1/3`; the mock log shows `EXCLUDE_OBJECT: 'Cone_id_0_copy_0'` and `'Cylinder_id_2_copy_0'` added after the `printer.print.start` line, and no `Could not skip` line. On print status, `$C click btn_objects`, then `$C ls rows_container` and confirm rows 1 and 3 read `Excluded`.

- [ ] **Step 3: Screenshots, thumbnail / 2D / 3D, landscape and portrait**

For each of: (a) thumbnail mode — start without `--render-2d`, then `$C navigate settings`, `$C click row_appearance`, `$C set_value row_gcode_mode 3`, `$C navigate print-select`; (b) `--render-2d`; (c) `--render-3d` — and for each of landscape (default size) and portrait (`--layout portrait -s 480x800`): open the file's details, pick two objects, open the skip view, then

```bash
$C screenshot /tmp/pre-start-<mode>-<orientation>.png --stable
```
Open every PNG and check: button in the preview's top-left corner with count `2`, the list covering the options column (landscape) or the bottom of the stack (portrait), picked rows dimmed and reading `Excluded`, picked objects faded on the map / render, no row reading `Printing now`. If 3D is unavailable in the dummy driver, record that and keep the other five. Stop the instance by PID resolved from the socket (CLAUDE.md "Never pkill helix-screen"), then `scripts/helix-claim release build:pre-start-exclude`.

- [ ] **Step 4: Completion gate**

```bash
make full-test-run
```
Expected: unit sweep and bats suite green.

- [ ] **Step 5: Mutation and ASAN on zeus** (push the branch, never main)

```bash
git push -u origin feature/pre-start-exclude
scripts/zeus-run.sh mutate --tests '[pre_start_exclude],[exclude_mode],[exclude_side_list],[exclude_map],[exclude_button]'
scripts/zeus-run.sh asan '[pre_start_exclude]'
```
Expected: every changed hunk killed (any survivor gets a test in its owning task before going on); ASAN clean.

---

### Task 12: Real printer, then remove the plan

**Files:**
- Delete: `docs/devel/plans/2026-10-06-pre-start-object-exclusion-design.md`, `docs/devel/plans/2026-10-06-pre-start-object-exclusion.md`

**Interfaces:**
- Consumes: Task 11 green.
- Produces: the shipping commit.

- [ ] **Step 1: Ask Preston before touching the U1**

Ask with AskUserQuestion: "Run one real print on the Snapmaker U1 with one object excluded from details, cancelled once the first layer shows it skipped?" Include the file you will use (a multi-object plate already on the U1, or the one you will upload) and what will move (homing, heating, first layer). Do nothing on the printer without a yes. Read the U1's entry in the project memory's device index before the first ssh.

- [ ] **Step 2: Run it from the desktop build against the U1**

```bash
scripts/helix-claim take device:u1 "pre-start exclude check" --note <U1 IP>
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --moonraker ws://<U1 IP>:7125 -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-u1.log 2>&1 &
```
Drive with `ctl` as in Task 11 Step 2 (one pick). Every command that starts or cancels the print is confirmed with Preston at that moment. While it runs:

```bash
ssh root@<U1 IP> "grep -n 'EXCLUDE_OBJECT' /oem/klippylogs/klippy.log | tail -20"
curl -s "http://<U1 IP>:7125/printer/objects/query?exclude_object" | python3 -m json.tool
```
Expected: the picked name in `excluded_objects` (upper-case) before the first `EXCLUDE_OBJECT_START` of that object; the first layer skips it. Cancel (with confirmation), stop the instance by PID, release `device:u1`.

- [ ] **Step 3: Delete the plan and design, and commit**

```bash
git rm docs/devel/plans/2026-10-06-pre-start-object-exclusion-design.md docs/devel/plans/2026-10-06-pre-start-object-exclusion.md
git commit -m "docs(plans): pre-start object exclusion shipped; its design lives in EXCLUDE_OBJECTS.md" -m "Verified on the mock (2 of 3 objects excluded from the first layer) and on a Snapmaker U1 print with one object skipped."
git show --stat HEAD
```

- [ ] **Step 4: Hand off**

Report the branch, the SHAs, the screenshots and the U1 evidence; merge and teardown follow `superpowers:finishing-a-development-branch` with `scripts/teardown-worktree.sh pre-start-exclude` and an independent review per the project memory.
