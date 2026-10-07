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

std::vector<gcode::GCodeObject>
merge_defined_objects(const std::vector<gcode::GCodeObject>& scanned,
                      const gcode::ParsedGCodeFile* parsed) {
    std::vector<gcode::GCodeObject> out = scanned;
    if (!parsed) {
        return out;
    }
    for (const auto& [name, obj] : parsed->objects) {
        const std::string upper = helix::text_io::to_upper(name);
        const bool known = std::any_of(out.begin(), out.end(), [&](const gcode::GCodeObject& o) {
            return helix::text_io::to_upper(o.name) == upper;
        });
        if (!known) {
            out.push_back(obj);
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
