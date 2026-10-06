// SPDX-License-Identifier: GPL-3.0-or-later

#include "buffer_status_modal.h"

#include "ui_clog_bar.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "clog_meter_geometry.h"
#include "observer_factory.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>

#include <cmath>
#include <memory>

// Static member definitions
bool BufferStatusModal::subjects_initialized_ = false;
lv_subject_t BufferStatusModal::type_subject_;
lv_subject_t BufferStatusModal::show_lean_subject_;
lv_subject_t BufferStatusModal::show_espooler_subject_;
lv_subject_t BufferStatusModal::show_flow_subject_;
lv_subject_t BufferStatusModal::show_distance_subject_;
lv_subject_t BufferStatusModal::description_subject_;
lv_subject_t BufferStatusModal::unsupported_subject_;
char BufferStatusModal::unsupported_buf_[128];
char BufferStatusModal::description_buf_[128]{};
lv_subject_t BufferStatusModal::pressure_subject_;
char BufferStatusModal::pressure_buf_[128]{};
lv_subject_t BufferStatusModal::espooler_value_subject_;
char BufferStatusModal::espooler_buf_[128]{};
lv_subject_t BufferStatusModal::gear_sync_value_subject_;
char BufferStatusModal::gear_sync_buf_[32]{};
lv_subject_t BufferStatusModal::flow_value_subject_;
char BufferStatusModal::flow_buf_[32]{};
lv_subject_t BufferStatusModal::afc_state_subject_;
char BufferStatusModal::afc_state_buf_[128]{};
lv_subject_t BufferStatusModal::afc_distance_subject_;
char BufferStatusModal::afc_distance_buf_[128]{};

BufferStatusModal::BufferStatusModal() {
    init_subjects();
}

BufferStatusModal::~BufferStatusModal() {
    // Before Modal::~Modal() destroys the dialog tree, so it can remove its
    // event callbacks from widgets that still exist.
    delete clog_bar_;
    // Subjects are static — never deinited (persist for the process lifetime)
}

void BufferStatusModal::init_subjects() {
    if (subjects_initialized_)
        return;

    lv_subject_init_int(&type_subject_, 0);
    lv_subject_init_int(&show_lean_subject_, 0);
    lv_subject_init_int(&show_espooler_subject_, 0);
    lv_subject_init_int(&show_flow_subject_, 0);
    lv_subject_init_int(&show_distance_subject_, 0);

    lv_subject_init_string(&description_subject_, description_buf_, nullptr,
                           sizeof(description_buf_), "");
    lv_subject_init_string(&pressure_subject_, pressure_buf_, nullptr, sizeof(pressure_buf_), "");
    lv_subject_init_string(&unsupported_subject_, unsupported_buf_, nullptr,
                           sizeof(unsupported_buf_), "");
    lv_subject_init_string(&espooler_value_subject_, espooler_buf_, nullptr, sizeof(espooler_buf_),
                           "");
    lv_subject_init_string(&gear_sync_value_subject_, gear_sync_buf_, nullptr,
                           sizeof(gear_sync_buf_), "");
    lv_subject_init_string(&flow_value_subject_, flow_buf_, nullptr, sizeof(flow_buf_), "");
    lv_subject_init_string(&afc_state_subject_, afc_state_buf_, nullptr, sizeof(afc_state_buf_),
                           "");
    lv_subject_init_string(&afc_distance_subject_, afc_distance_buf_, nullptr,
                           sizeof(afc_distance_buf_), "");

    lv_xml_register_subject(nullptr, "buf_type", &type_subject_);
    lv_xml_register_subject(nullptr, "buf_show_lean", &show_lean_subject_);
    lv_xml_register_subject(nullptr, "buf_show_espooler", &show_espooler_subject_);
    lv_xml_register_subject(nullptr, "buf_show_flow", &show_flow_subject_);
    lv_xml_register_subject(nullptr, "buf_show_distance", &show_distance_subject_);
    lv_xml_register_subject(nullptr, "buf_description", &description_subject_);
    lv_xml_register_subject(nullptr, "buf_pressure", &pressure_subject_);
    lv_xml_register_subject(nullptr, "buf_unsupported", &unsupported_subject_);
    lv_xml_register_subject(nullptr, "buf_espooler_value", &espooler_value_subject_);
    lv_xml_register_subject(nullptr, "buf_gear_sync_value", &gear_sync_value_subject_);
    lv_xml_register_subject(nullptr, "buf_flow_value", &flow_value_subject_);
    lv_xml_register_subject(nullptr, "buf_afc_state", &afc_state_subject_);
    lv_xml_register_subject(nullptr, "buf_afc_distance", &afc_distance_subject_);

    subjects_initialized_ = true;
}

namespace helix {
namespace {

/// The unit's filament pressure sensor, or nullptr when its buffer reports no
/// pressure (or it has no buffer).
const BufferHealth* pressure_sensor(const AmsSystemInfo& info, int unit) {
    if (unit < 0 || unit >= static_cast<int>(info.units.size())) {
        return nullptr;
    }
    const auto& health = info.units[static_cast<std::size_t>(unit)].buffer_health;
    return health.has_value() && health->fps_reported ? &*health : nullptr;
}

/// What a sync-feedback bias means for the filament, whichever backend
/// produced it: the lean the clog meter's TIGHT / LOOSE ends label.
const char* bias_description(float bias) {
    switch (helix::ui::buffer_lean(bias)) {
    case helix::ui::BufferLean::Tight:
        return lv_tr("Filament is pulling tight");
    case helix::ui::BufferLean::Loose:
        return lv_tr("Filament is loose");
    case helix::ui::BufferLean::Balanced:
        break;
    }
    return lv_tr("Filament tension is balanced");
}

} // namespace
} // namespace helix

void BufferStatusModal::populate(const helix::AmsSystemInfo& info, int effective_unit) {
    // Cleared up front: the modal's subjects are static, so a message left from
    // a previous open would otherwise sit under a supported backend's body.
    lv_subject_copy_string(&unsupported_subject_, "");

    // The description follows whichever buffer this unit has: Happy Hare's
    // system-level bias, or the unit's own pressure sensor.
    const float bias = info.buffer_bias(effective_unit);
    const bool has_bias = bias > -1.5f;
    lv_subject_copy_string(&description_subject_, has_bias ? helix::bias_description(bias) : "");
    lv_subject_set_int(&show_lean_subject_, has_bias ? 1 : 0);

    if (info.type == helix::AmsType::HAPPY_HARE) {
        lv_subject_set_int(&type_subject_, 1);

        // eSpooler
        if (!info.espooler_state.empty()) {
            lv_subject_set_int(&show_espooler_subject_, 1);
            if (info.espooler_state == "rewind") {
                lv_subject_copy_string(&espooler_value_subject_, lv_tr("Rewinding"));
            } else if (info.espooler_state == "assist") {
                lv_subject_copy_string(&espooler_value_subject_, lv_tr("Assisting"));
            } else {
                lv_subject_copy_string(&espooler_value_subject_, info.espooler_state.c_str());
            }
        } else {
            lv_subject_set_int(&show_espooler_subject_, 0);
        }

        // Gear sync
        lv_subject_copy_string(&gear_sync_value_subject_,
                               info.sync_drive ? lv_tr("Active") : lv_tr("Inactive"));

        // Flow rate
        if (info.sync_feedback_flow_rate >= 0) {
            auto flow_str = fmt::format("{:.0f}%", info.sync_feedback_flow_rate);
            lv_subject_copy_string(&flow_value_subject_, flow_str.c_str());
            lv_subject_set_int(&show_flow_subject_, 1);
        } else if (info.encoder_flow_rate >= 0) {
            auto flow_str = fmt::format("{}%", info.encoder_flow_rate);
            lv_subject_copy_string(&flow_value_subject_, flow_str.c_str());
            lv_subject_set_int(&show_flow_subject_, 1);
        } else {
            lv_subject_set_int(&show_flow_subject_, 0);
        }

    } else if (info.type == helix::AmsType::AFC) {
        lv_subject_set_int(&type_subject_, 2);

        bool found_health = false;
        if (effective_unit >= 0 && effective_unit < static_cast<int>(info.units.size())) {
            const auto& unit = info.units[effective_unit];
            if (unit.buffer_health.has_value()) {
                const auto& bh = unit.buffer_health.value();
                found_health = true;

                // State
                if (!bh.state.empty()) {
                    if (bh.state == "Advancing") {
                        lv_subject_copy_string(&afc_state_subject_,
                                               lv_tr("Feeding filament forward"));
                    } else if (bh.state == "Trailing") {
                        lv_subject_copy_string(&afc_state_subject_, lv_tr("Pulling filament back"));
                    } else {
                        auto s = fmt::format("{}: {}", lv_tr("State"), bh.state);
                        lv_subject_copy_string(&afc_state_subject_, s.c_str());
                    }
                } else {
                    lv_subject_copy_string(&afc_state_subject_, "");
                }

                // Distance
                if (bh.fault_detection_enabled && bh.distance_to_fault >= 0) {
                    auto d = fmt::format("{:.1f} mm {}", bh.distance_to_fault,
                                         lv_tr("remaining before clog detection triggers"));
                    lv_subject_copy_string(&afc_distance_subject_, d.c_str());
                    lv_subject_set_int(&show_distance_subject_, 1);
                } else {
                    lv_subject_set_int(&show_distance_subject_, 0);
                }
            }
        }
        if (!found_health) {
            lv_subject_copy_string(&afc_state_subject_, lv_tr("No buffer data available"));
            lv_subject_set_int(&show_distance_subject_, 0);
        }
    } else if (const helix::BufferHealth* fps = helix::pressure_sensor(info, effective_unit)) {
        // The target is the set_point the feeder regulates to; without one the
        // reading cannot be placed either side of it, so there is no lean.
        lv_subject_set_int(&type_subject_, 3);
        const int pressure = static_cast<int>(std::lround(fps->fps_value * 100.0f));
        auto text = fps->fps_set_point > 0.0f
                        ? fmt::format(fmt::runtime(lv_tr("Pressure: {}% (target {}%)")), pressure,
                                      static_cast<int>(std::lround(fps->fps_set_point * 100.0f)))
                        : fmt::format("{} {}%", lv_tr("Pressure:"), pressure);
        lv_subject_copy_string(&pressure_subject_, text.c_str());
    } else {
        // No buffer to describe. Stock CFS, AD5X IFS, tool changers, ACE,
        // Snapmaker and QIDI report none of this - see AmsBackendCfs's own note
        // that "Stock CFS reports none of these". Every body section binds
        // hidden unless buf_type names a buffer, so without a message here the
        // dialog renders as a title and two buttons over an empty box. The
        // modal is reachable from the AMS panel as well as the tile, so this
        // has to answer rather than rely on the tile's gate.
        lv_subject_set_int(&type_subject_, 0);
        lv_subject_set_int(&show_lean_subject_, 0);
        lv_subject_copy_string(&unsupported_subject_,
                               lv_tr("This filament system does not report buffer or flow data."));
    }
}

void BufferStatusModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_close");
    wire_cancel_button("btn_secondary");

    refresh();

    // Apply label/value color distinction AFTER theme_apply_current_palette_to_tree
    // (which runs in Modal::show and forces all labels white on dark backgrounds)
    if (dialog()) {
        auto muted = theme_manager_get_color("text_muted");
        static const char* label_names[] = {"lbl_espooler", "lbl_gear_sync", "lbl_clog",
                                            "lbl_flow",     "lbl_afc_clog",  nullptr};
        for (const char** name = label_names; *name; ++name) {
            lv_obj_t* lbl = lv_obj_find_by_name(dialog(), *name);
            if (lbl)
                lv_obj_set_style_text_color(lbl, muted, 0);
        }
    }

    // Every row is re-read whenever a backend sync lands or a backend appears
    // or vanishes, so the modal stays live while it is open. The handler
    // re-reads the backend rather than trusting the tick.
    auto& ams = helix::AmsState::instance();
    revision_observer_ = helix::ui::observe<int>(
        ams.get_ams_data_revision_subject(), this,
        [](BufferStatusModal* self, int) { self->refresh(); }, ams.get_subjects_lifetime());
    backend_observer_ = helix::ui::observe<int>(
        ams.get_backend_count_subject(), this,
        [](BufferStatusModal* self, int) { self->refresh(); }, ams.get_subjects_lifetime());

    // Drive the clog bar the dialog authored. Unconditional: it reads the same
    // AmsState subjects the home tile does, and clog_bar_body hides itself when
    // there is no detection, so there is nothing here to gate on.
    if (dialog()) {
        clog_bar_ = new helix::ui::UiClogBar(dialog());
    }
}

void BufferStatusModal::refresh() {
    // A vanished backend reads as an empty snapshot, which is the unsupported
    // message rather than whatever the last backend said.
    auto* backend = helix::AmsState::instance().get_backend();
    populate(backend ? backend->get_system_info() : helix::AmsSystemInfo{}, effective_unit_);
}

void BufferStatusModal::show_for(int effective_unit) {
    auto modal = std::make_unique<BufferStatusModal>();
    modal->effective_unit_ = effective_unit;
    // Stack-owned one-shot: ModalStack frees the instance when its entry goes
    // (#1382); a failed show leaves the unique_ptr to free it.
    Modal::show_owned(std::move(modal), lv_screen_active());
}
