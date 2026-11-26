#include "ptt_wave.h"

#include <algorithm>
#include <cstdlib>

namespace {
constexpr int kPttWaveBars = 27;
constexpr int kPttWaveFrames = 16;
constexpr lv_coord_t kPttWaveBarW = 2;
constexpr lv_coord_t kPttWaveBarGap = 2;
// The original PTT curve is scaled to half the status bar height.
constexpr uint8_t kPttWaveCurve[kPttWaveFrames] = {
    12, 16, 22, 30, 38, 44, 42, 36, 28, 20, 14, 10, 12, 18, 26, 34,
};

lv_obj_t* CreatePttWaveBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, kPttWaveBarW);
    lv_obj_set_height(bar, 2);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 1, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

}  // namespace

lv_obj_t* CreatePttWave(lv_obj_t* parent, lv_coord_t status_height) {
    const auto bar_h = std::max<lv_coord_t>(12, status_height / 2);
    lv_obj_t* host = lv_obj_create(parent);
    lv_obj_remove_style_all(host);
    lv_obj_set_size(host, LV_SIZE_CONTENT, bar_h);
    lv_obj_align(host, LV_ALIGN_TOP_MID, 0, (status_height - bar_h) / 2);
    lv_obj_set_style_bg_opa(host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(host, 0, 0);
    lv_obj_set_style_pad_all(host, 0, 0);
    lv_obj_set_style_pad_column(host, kPttWaveBarGap, 0);
    lv_obj_set_style_radius(host, 0, 0);
    lv_obj_set_flex_flow(host, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(host, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(host, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < kPttWaveBars; ++i) {
        CreatePttWaveBar(host);
    }
    return host;
}

void DrawPttWaveFrame(lv_obj_t* wave, uint32_t frame) {
    const auto height = lv_obj_get_style_height(wave, LV_PART_MAIN);
    constexpr int center = kPttWaveBars / 2;
    for (int index = 0; index < kPttWaveBars; ++index) {
        const int distance = std::abs(index - center);
        const auto phase = (frame + distance) % kPttWaveFrames;
        const auto bar_height = std::max<lv_coord_t>(2, kPttWaveCurve[phase] * height / 44);
        lv_obj_set_height(lv_obj_get_child(wave, index), bar_height);
    }
}
