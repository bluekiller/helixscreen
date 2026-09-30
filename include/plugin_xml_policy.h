// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace helix::plugin {

/// Empty when `xml`, a component file of plugin `id`, references only what a plugin may;
/// otherwise the first violation, as a sentence. The rules:
///  - an element name, after stripping a leading `lv_obj-`, must be an `lv_*` builtin
///    widget, one of helix-xml's `lv_obj` children (`event_cb`, `bind_*`, `style`,
///    `remove_style*`, `subject_*_event`, `play_timeline_event`), a component owned by
///    `id`, or an allowlisted app component (`overlay_panel`); screen load and create
///    events are never available, nor are the `subjects`, `images` and `fonts` sections;
///  - a `<view extends>` value obeys the same element rule;
///  - an attribute named `callback` or `event_cb`, or ending `_callback` or `_cb`, has
///    the value `plugin_event`;
///  - `user_data` on an `event_cb` is owned by `id` (optionally followed by `:<arg>`);
///  - an attribute named `subject`, ending `_subject`, or starting `bind_` names a
///    subject owned by `id`;
///  - none of those attributes takes a `$prop` value, since a prop default cannot be
///    checked against the attribute it feeds.
std::string check_plugin_xml(std::string_view id, const std::string& xml);

} // namespace helix::plugin
