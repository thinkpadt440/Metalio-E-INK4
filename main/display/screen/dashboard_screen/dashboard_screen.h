#pragma once

#include <cstdint>

#include "lvgl.h"

struct DashboardMetrics {
    bool valid = false;
    uint32_t sessions = 0;
    uint32_t messages = 0;
    uint64_t total_tokens = 0;
    uint32_t active_days = 0;
    char peak_hour[16] = "--";
    char favorite_model[48] = "--";
};

class DashboardScreen {
public:
    static lv_obj_t* Create();
    // Metrics are published by a future Harness transport and read by the UI timer.
    static void UpdateMetrics(const DashboardMetrics& metrics);
};
