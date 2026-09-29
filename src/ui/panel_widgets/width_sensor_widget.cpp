// SPDX-License-Identifier: GPL-3.0-or-later

#include "width_sensor_widget.h"

#include "format_utils.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "static_subject_registry.h"
#include "theme_manager.h"
#include "width_sensor_manager.h"

#include <spdlog/spdlog.h>

static ObserverGuard s_diameter_observer;
static bool s_subjects_initialized = false;

static void width_sensor_widget_init_subjects() {
    if (s_subjects_initialized) {
        return;
    }

    auto& wsm = helix::sensors::WidthSensorManager::instance();

    // Observe raw diameter (int, µm * 1000) and format to text subject
    s_diameter_observer = helix::ui::observe_int_sync<helix::sensors::WidthSensorManager>(
        wsm.get_diameter_subject(), &wsm,
        [](helix::sensors::WidthSensorManager* m, int diameter) {
            auto* text_subj = m->get_diameter_text_subject();
            if (diameter >= 0) {
                float diameter_mm = diameter / 1000.0f;
                char buf[16];
                helix::format::format_diameter_mm(diameter_mm, buf, sizeof(buf));
                lv_subject_copy_string(text_subj, buf);
            } else {
                lv_subject_copy_string(text_subj, helix::format::UNAVAILABLE);
            }
        },
        wsm.get_subjects_lifetime());

    s_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("WidthSensorWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            s_diameter_observer.release();
            s_subjects_initialized = false;
            spdlog::trace("[WidthSensorWidget] Subjects deinitialized");
        }
    });

    spdlog::debug("[WidthSensorWidget] Subjects initialized (diameter text observer)");
}

namespace helix {
void register_width_sensor_widget() {
    register_widget_factory(
        "width_sensor", [](const std::string&) { return std::make_unique<WidthSensorWidget>(); });
    register_widget_subjects("width_sensor", width_sensor_widget_init_subjects);
}
} // namespace helix

using namespace helix;

WidthSensorWidget::~WidthSensorWidget() {
    detach();
}

void WidthSensorWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    widget_obj_ = widget_obj;
    if (widget_obj_)
        lv_obj_set_user_data(widget_obj_, this);
}

void WidthSensorWidget::detach() {
    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
}
