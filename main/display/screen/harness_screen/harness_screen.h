#pragma once

#include "lvgl.h"

class HarnessScreen {
public:
    static lv_obj_t* Create();
    static void RequestOpen();
    static bool IsActive();
};
