#include "dashboard_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <esp_log.h>

#include "board.h"
#include "harness_screen/harness_screen.h"
#include "product_ui.h"
#include "settings_screen/settings_network_tab.h"
#include "settings_screen/settings_screen.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

namespace {
constexpr const char* kScreenId = "home";
std::mutex s_metrics_mutex;
DashboardMetrics s_metrics;
double s_peak_rate = 0;

struct DashboardUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* cards[4] = {};
    lv_obj_t* rate = nullptr;
    lv_obj_t* instant = nullptr;
    lv_obj_t* peak = nullptr;
    lv_obj_t* generated = nullptr;
    lv_obj_t* context = nullptr;
    lv_obj_t* ttft = nullptr;
    lv_obj_t* usage = nullptr;
    lv_obj_t* elapsed = nullptr;
    lv_obj_t* tasks = nullptr;
    lv_obj_t* slots = nullptr;
    lv_obj_t* vram = nullptr;
    lv_obj_t* vram_total = nullptr;
    lv_obj_t* network = nullptr;
    lv_timer_t* timer = nullptr;
    unsigned ticks = 0;
    bool has_metrics = false;
};

void RefreshMetrics(DashboardUi* ui) {
    DashboardMetrics metrics;
    double peak;
    {
        std::lock_guard<std::mutex> lock(s_metrics_mutex);
        metrics = s_metrics;
        peak = s_peak_rate;
    }
    ui->has_metrics = metrics.valid;
    auto value = [](lv_obj_t* label, const char* format, double number) {
        char text[64];
        std::snprintf(text, sizeof(text), format, number);
        product_ui::SetText(label, text);
    };
    if (metrics.valid) {
        value(ui->rate, "%.1f", metrics.token_rate);
        value(ui->instant, "瞬时 %.1f", metrics.instant_rate);
        value(ui->peak, "峰值 %.1f", peak);
        value(ui->generated, "%.0f", metrics.generated_tokens);
        value(ui->context, "上下文 %.0f", metrics.context_tokens);
        value(ui->ttft, "TTFT %.0fms", metrics.ttft_ms);
        if (metrics.context_capacity > 0) {
            value(ui->usage, "使用率 %.1f%%", 100.0 * metrics.context_tokens / metrics.context_capacity);
        } else {
            product_ui::SetText(ui->usage, "使用率 --");
        }
        value(ui->elapsed, "耗时 %.1fs", metrics.elapsed_seconds);
        value(ui->tasks, "%.0f", metrics.active_tasks);
        value(ui->slots, "总槽位 %.0f", metrics.total_slots);
    } else {
        product_ui::SetText(ui->rate, "--");
        product_ui::SetText(ui->instant, "瞬时 --");
        product_ui::SetText(ui->peak, "峰值 --");
        product_ui::SetText(ui->generated, "--");
        product_ui::SetText(ui->context, "上下文 --");
        product_ui::SetText(ui->ttft, "TTFT --");
        product_ui::SetText(ui->usage, "使用率 --");
        product_ui::SetText(ui->elapsed, "耗时 --");
        product_ui::SetText(ui->tasks, "--");
        product_ui::SetText(ui->slots, "总槽位 --");
    }
    if (metrics.gpu == DashboardGpu::Absent) {
        lv_obj_add_flag(ui->cards[3], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(ui->cards[2], LV_HOR_RES - 32);
    } else {
        lv_obj_remove_flag(ui->cards[3], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(ui->cards[2], (LV_HOR_RES - 44) / 2);
        if (metrics.valid && metrics.gpu == DashboardGpu::Present) {
            value(ui->vram, "%.1f GiB", metrics.vram_used_gib);
            value(ui->vram_total, "总计 %.1f GiB", metrics.vram_total_gib);
        } else {
            product_ui::SetText(ui->vram, "--");
            product_ui::SetText(ui->vram_total, "待检测");
        }
    }
}

void OnTimer(lv_timer_t* timer) {
    auto* ui = static_cast<DashboardUi*>(lv_timer_get_user_data(timer));
    if (lv_screen_active() != ui->screen || StandbyScreen::IsActive()) return;
    RefreshMetrics(ui);
    // Initial screen construction happens inside Board construction; delay Board reads.
    if (++ui->ticks % 5 == 0) {
        const bool wifi = SettingsNetworkTab_IsWifi();
        const bool ready = Board::GetInstance().IsNetworkReady();
        product_ui::SetText(ui->network, wifi
            ? (ready ? (ui->has_metrics ? "Wi-Fi 已连接" : "Wi-Fi 已连接 · 指标待接入") : "Wi-Fi 未连接")
            : (ready ? (ui->has_metrics ? "4G 已连接" : "4G 已连接 · 指标待接入") : "4G 未连接"));
    }
}

void OnDelete(lv_event_t* event) {
    auto* ui = static_cast<DashboardUi*>(lv_event_get_user_data(event));
    lv_timer_delete(ui->timer);
    delete ui;
}

void OpenSettingsAsync(void*) {
    if (ScreenIsHome() && !StandbyScreen::IsActive()) ScreenNavigateTo(SettingsScreen::Create);
}
void OnSettings(lv_event_t*) { lv_async_call(OpenSettingsAsync, nullptr); }
void OnAssistant(lv_event_t*) { HarnessScreen::RequestOpen(); }
bool OnBootClick() { HarnessScreen::RequestOpen(); return true; }
bool OnBootLongPress() { HarnessScreen::RequestOpen(true); return true; }
bool OnKey(const char* key) {
    return std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_next") == 0;
}

lv_obj_t* Metric(lv_obj_t* card, int y, const char* text, bool large = false) {
    lv_obj_t* label = product_ui::Label(card, 12, y, (LV_HOR_RES - 44) / 2 - 24, text,
                                       large ? product_ui::HeadingFont() : nullptr);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, large ? 38 : 31);
    return label;
}
}  // namespace

void DashboardScreen::UpdateMetrics(const DashboardMetrics& metrics) {
    std::lock_guard<std::mutex> lock(s_metrics_mutex);
    s_metrics = metrics;
    if (metrics.valid && std::isfinite(metrics.token_rate)) {
        s_peak_rate = std::max(s_peak_rate, metrics.token_rate);
    }
}

lv_obj_t* DashboardScreen::Create() {
    ESP_LOGI("Dashboard", "create telemetry dashboard");
    auto* ui = new DashboardUi;
    ui->screen = product_ui::Screen();
    const EpdStatusBar status = ScreenCreateStatusBar(ui->screen);
    const int width = LV_HOR_RES - 32;
    const int top = status.height + 16;
    const int height = LV_VER_RES - top;
    lv_obj_t* body = product_ui::Box(ui->screen, 16, top, width, height);
    product_ui::Label(body, 0, 0, width, "工作台", product_ui::HeadingFont());

    const int card_width = (width - 12) / 2;
    for (int i = 0; i < 4; ++i) {
        ui->cards[i] = product_ui::Box(body, (i % 2) * (card_width + 12),
                                      58 + (i / 2) * 262, card_width, 250, 1);
    }
    Metric(ui->cards[0], 14, "token 速率");
    ui->rate = Metric(ui->cards[0], 55, "--", true);
    Metric(ui->cards[0], 98, "单位 token/s");
    ui->instant = Metric(ui->cards[0], 150, "瞬时 --");
    ui->peak = Metric(ui->cards[0], 194, "峰值 --");

    Metric(ui->cards[1], 14, "生成 token");
    ui->generated = Metric(ui->cards[1], 55, "--", true);
    ui->context = Metric(ui->cards[1], 100, "上下文 --");
    ui->ttft = Metric(ui->cards[1], 137, "TTFT --");
    ui->usage = Metric(ui->cards[1], 174, "使用率 --");
    ui->elapsed = Metric(ui->cards[1], 211, "耗时 --");

    Metric(ui->cards[2], 14, "推理任务");
    ui->tasks = Metric(ui->cards[2], 65, "--", true);
    Metric(ui->cards[2], 110, "进行中");
    ui->slots = Metric(ui->cards[2], 177, "总槽位 --");

    Metric(ui->cards[3], 14, "显存占用");
    ui->vram = Metric(ui->cards[3], 65, "--", true);
    ui->vram_total = Metric(ui->cards[3], 119, "待检测");
    Metric(ui->cards[3], 177, "单位 GiB");
    ui->network = product_ui::Label(body, 0, 588, width, "指标数据待接入");

    const int half = (width - 12) / 2;
    product_ui::Button(body, 0, height - 106, half, 58, "进入助手", OnAssistant, true);
    product_ui::Button(body, half + 12, height - 106, half, 58, "设置", OnSettings);
    product_ui::Label(body, 0, height - 34, width, "BOOT 进入助手 / 长按录音");
    RefreshMetrics(ui);
    ui->timer = lv_timer_create(OnTimer, 1000, ui);
    lv_obj_add_event_cb(ui->screen, OnDelete, LV_EVENT_DELETE, ui);
    ScreenSetIsHome(true);
    VkKey_AttachScreen(ui->screen, kScreenId,
                       VkKeyScreenDesc{DashboardScreen::Create, OnKey, OnBootClick, OnBootLongPress});
    return ui->screen;
}
