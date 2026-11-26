#pragma once

#include "lvgl.h"

constexpr uint32_t kPttWaveFrameMs = 55;

lv_obj_t* CreatePttWave(lv_obj_t* parent, lv_coord_t status_height);
void DrawPttWaveFrame(lv_obj_t* wave, uint32_t frame);
