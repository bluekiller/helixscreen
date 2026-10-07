#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_parser.h"
#include "printer_excluded_objects_state.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace helix::ui {

/// Whether objects can be skipped: the printer has [exclude_object] and the
/// file is G-code defining at least two objects. Print status asks the same
/// question of a running print.
bool pre_start_exclude_available(bool printer_has_exclude_object, bool is_3mf,
                                 size_t defined_count);

/// The defined spelling of @p name, compared upper-case as Klipper stores
/// object names; "" when no defined object matches.
std::string canonical_object_name(const std::vector<std::string>& defined, const std::string& name);

/// Whether @p picks cover every defined object, counted upper-case the way
/// Klipper would. False when nothing is defined.
bool every_object_picked(const std::vector<std::string>& defined,
                         const std::unordered_set<std::string>& picks);

/// The details object list: the scan's objects in file order, then any the
/// full parse found that the scan did not (compared upper-case).
std::vector<gcode::GCodeObject>
merge_defined_objects(const std::vector<gcode::GCodeObject>& scanned,
                      const gcode::ParsedGCodeFile* parsed);

std::vector<PrinterExcludedObjectsState::ObjectInfo>
object_infos_from(const std::vector<gcode::GCodeObject>& objects);

} // namespace helix::ui
