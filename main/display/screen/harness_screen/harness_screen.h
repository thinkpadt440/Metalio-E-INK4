#pragma once

#include "lvgl.h"

class HarnessScreen {
public:
    static lv_obj_t* Create();
    static void RequestOpen(bool record_if_held = false);
    static bool IsActive();
    static void OnEnterStandby();
};
