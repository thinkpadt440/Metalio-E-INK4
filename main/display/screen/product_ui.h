#pragma once

#include <cstring>

#include "display_orient.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"

namespace product_ui {

inline const lv_font_t* HeadingFont() {
    const lv_font_t* font = fontpack_lv_font_get(30, 2);
    return font != nullptr ? font : fontpack_lv_font_ui();
}

inline lv_obj_t* Screen() {
    DisplayUiSetOrient(kDisplayUiPortrait);
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    return screen;
}

inline lv_obj_t* Box(lv_obj_t* parent, int x, int y, int width, int height, int border = 0) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, border, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

inline lv_obj_t* Label(lv_obj_t* parent, int x, int y, int width, const char* text,
                       const lv_font_t* font = nullptr) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font != nullptr ? font : fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_line_space(label, 8, 0);
    lv_label_set_text(label, text);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

inline void SetText(lv_obj_t* label, const char* text) {
    if (std::strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

inline lv_obj_t* Button(lv_obj_t* parent, int x, int y, int width, int height,
                        const char* text, lv_event_cb_t callback, bool primary = false) {
    lv_obj_t* button = Box(parent, x, y, width, height, 2);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, primary ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_text_color(button, primary ? lv_color_white() : lv_color_black(), 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    HapticAttachClick(button);
    lv_obj_t* label = Label(button, 0, 0, width - 12, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return button;
}

}  // namespace product_ui
