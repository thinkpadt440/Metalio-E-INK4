#pragma once

#include <cstdint>
#include "lvgl.h"

enum class DashboardGpu { Unknown, Absent, Present };

struct DashboardMetrics {
    bool valid = false;
    double token_rate = 0;
    double instant_rate = 0;
    uint32_t generated_tokens = 0;
    uint32_t context_tokens = 0;
    uint32_t context_capacity = 0;
    uint32_t ttft_ms = 0;
    double elapsed_seconds = 0;
    uint32_t active_tasks = 0;
    uint32_t total_slots = 0;
    DashboardGpu gpu = DashboardGpu::Unknown;
    double vram_used_gib = 0;
    double vram_total_gib = 0;
};

class DashboardScreen {
public:
    static lv_obj_t* Create();
    // A future transport can publish a snapshot from its worker; the UI polls without network I/O.
    static void UpdateMetrics(const DashboardMetrics& metrics);
};
