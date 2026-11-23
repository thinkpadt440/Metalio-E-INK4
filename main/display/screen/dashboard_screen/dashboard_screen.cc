#include "dashboard_screen.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_log.h>

#include "board.h"
#include "harness_screen/harness_screen.h"
#include "product_ui.h"
#include "settings_screen/settings_network_tab.h"
#include "settings_screen/settings_screen.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

LV_FONT_DECLARE(font_misans_regular_160_2);

namespace {
constexpr const char* kScreenId = "home";

struct DashboardUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* clock = nullptr;
    lv_obj_t* date = nullptr;
    lv_obj_t* network = nullptr;
    lv_timer_t* timer = nullptr;
    unsigned ticks = 0;
};

const lv_font_t* ClockFont() {
    static lv_font_t font = font_misans_regular_160_2;
    font.line_height = 134;
    font.base_line = 10;
    return &font;
}

void RefreshClock(DashboardUi* ui) {
    const time_t now = time(nullptr);
    struct tm local = {};
    char clock[8] = "00:00";
    char date[80] = "时间待同步";
    if (localtime_r(&now, &local) != nullptr && local.tm_year >= 125) {
        static const char* weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        std::snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        std::snprintf(date, sizeof(date), "%04d年%02d月%02d日  %s", local.tm_year + 1900,
                      local.tm_mon + 1, local.tm_mday, weekdays[local.tm_wday]);
    }
    product_ui::SetText(ui->clock, clock);
    product_ui::SetText(ui->date, date);
}

void OnTimer(lv_timer_t* timer) {
    auto* ui = static_cast<DashboardUi*>(lv_timer_get_user_data(timer));
    if (lv_screen_active() != ui->screen || StandbyScreen::IsActive()) {
        return;
    }
    RefreshClock(ui);
    // The screen is first created inside the Board constructor. Read Board only after boot.
    if (++ui->ticks % 5 == 0) {
        const bool wifi = SettingsNetworkTab_IsWifi();
        const bool ready = Board::GetInstance().IsNetworkReady();
        const char* text = wifi ? (ready ? "Wi-Fi 已连接" : "Wi-Fi 未连接")
                                : (ready ? "4G 已连接" : "4G 未连接");
        product_ui::SetText(ui->network, text);
    }
}

void OnDelete(lv_event_t* event) {
    auto* ui = static_cast<DashboardUi*>(lv_event_get_user_data(event));
    lv_timer_delete(ui->timer);
    delete ui;
}

void OpenSettingsAsync(void*) {
    if (ScreenIsHome() && !StandbyScreen::IsActive()) {
        ScreenNavigateTo(SettingsScreen::Create);
    }
}

void OnSettings(lv_event_t*) {
    lv_async_call(OpenSettingsAsync, nullptr);
}

void OnAssistant(lv_event_t*) {
    HarnessScreen::RequestOpen();
}

bool OnBootLongPress() {
    HarnessScreen::RequestOpen();
    return true;
}

bool OnKey(const char* key) {
    return std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_next") == 0;
}
}  // namespace

lv_obj_t* DashboardScreen::Create() {
    ESP_LOGI("Dashboard", "create dashboard");
    auto* ui = new DashboardUi;
    ui->screen = product_ui::Screen();
    const EpdStatusBar status = ScreenCreateStatusBar(ui->screen);
    const int width = LV_HOR_RES - 48;
    const int top = status.height + 20;
    const int height = LV_VER_RES - top;
    lv_obj_t* body = product_ui::Box(ui->screen, 24, top, width, height);

    product_ui::Label(body, 0, 0, width, "工作台", product_ui::HeadingFont());
    ui->clock = product_ui::Label(body, 0, 64, width, "00:00", ClockFont());
    lv_label_set_long_mode(ui->clock, LV_LABEL_LONG_CLIP);
    ui->date = product_ui::Label(body, 0, 228, width, "时间待同步");

    lv_obj_t* card = product_ui::Box(body, 0, 286, width, 116, 1);
    product_ui::Label(card, 20, 16, width - 40, "设备网络");
    ui->network = product_ui::Label(card, 20, 60, width - 40, "正在获取网络状态");
    product_ui::Label(body, 0, 428, width, "助手尚未连接");
    product_ui::Label(body, 0, 466, width, "进入助手，查看界面演示。");

    product_ui::Button(body, 0, height - 194, width, 72, "进入助手", OnAssistant, true);
    product_ui::Button(body, 0, height - 106, width, 56, "设置", OnSettings);
    product_ui::Label(body, 0, height - 34, width, "长按 BOOT 进入助手");

    RefreshClock(ui);
    ui->timer = lv_timer_create(OnTimer, 1000, ui);
    lv_obj_add_event_cb(ui->screen, OnDelete, LV_EVENT_DELETE, ui);
    ScreenSetIsHome(true);
    VkKey_AttachScreen(ui->screen, kScreenId,
                       VkKeyScreenDesc{DashboardScreen::Create, OnKey, nullptr, OnBootLongPress});
    return ui->screen;
}
