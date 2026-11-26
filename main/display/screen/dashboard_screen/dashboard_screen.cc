#include "dashboard_screen.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "harness/service.h"
#include "product_ui.h"
#include "settings_screen/settings_screen.h"
#include "standby_screen/standby_screen.h"
#include "harness_screen/harness_screen.h"
#include "vk_key_handler.h"

namespace {
constexpr const char* kScreenId = "home";
constexpr int kRangeCount = 3;
constexpr const char* kRanges[] = {"all", "30d", "7d"};
constexpr uint32_t kRefreshIntervalMs = 15000;

struct DashboardUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* values[6] = {};
    lv_obj_t* range_buttons[kRangeCount] = {};
    lv_obj_t* data_status = nullptr;
    lv_timer_t* timer = nullptr;
    int selected_range = 0;
    std::optional<harness::Overview> metrics[kRangeCount];
    std::shared_ptr<harness::CatalogCall> pending;
    uint32_t last_request = 0;
    bool request_due = true;
    bool request_failed = false;
    bool was_ready = false;
};

void RefreshMetrics(DashboardUi* ui);

void SetSelectedRange(DashboardUi* ui, int selected) {
    ui->selected_range = selected;
    for (int i = 0; i < kRangeCount; ++i) {
        const bool active = i == selected;
        lv_obj_set_style_bg_color(ui->range_buttons[i],
                                  active ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(ui->range_buttons[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(ui->range_buttons[i],
                                    active ? lv_color_white() : lv_color_black(), 0);
    }
}

void SelectRange(int selected) {
    auto* screen = lv_screen_active();
    auto* ui = screen == nullptr
        ? nullptr
        : static_cast<DashboardUi*>(lv_obj_get_user_data(screen));
    if (ui == nullptr || ui->screen != screen) return;
    if (ui->selected_range == selected && ui->pending) return;
    ui->pending.reset();
    ui->request_due = true;
    ui->request_failed = false;
    SetSelectedRange(ui, selected);
    RefreshMetrics(ui);
}

void OnRangeAll(lv_event_t*) { SelectRange(0); }
void OnRange30d(lv_event_t*) { SelectRange(1); }
void OnRange7d(lv_event_t*) { SelectRange(2); }

void FormatCount(uint64_t value, char* out, size_t capacity) {
    if (capacity == 0) return;
    // The configured Newlib nano formatter cannot print 64-bit integers.
    char reversed[20];
    size_t length = 0;
    do {
        reversed[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    size_t pos = 0;
    for (size_t i = 0; i < length && pos + 1 < capacity; ++i) {
        if (i != 0 && (length - i) % 3 == 0 && pos + 1 < capacity) out[pos++] = ',';
        if (pos + 1 < capacity) out[pos++] = reversed[length - 1 - i];
    }
    out[pos] = '\0';
}

void FormatTokens(uint64_t value, char* out, size_t capacity) {
    if (value >= 1000000000ULL) {
        std::snprintf(out, capacity, "%.1fB", static_cast<double>(value) / 1000000000.0);
    } else if (value >= 1000000ULL) {
        std::snprintf(out, capacity, "%.1fM", static_cast<double>(value) / 1000000.0);
    } else if (value >= 1000ULL) {
        std::snprintf(out, capacity, "%.1fK", static_cast<double>(value) / 1000.0);
    } else {
        FormatCount(value, out, capacity);
    }
}
void RefreshMetrics(DashboardUi* ui) {
    const bool ready = harness::ServiceReady();
    auto& metrics = ui->metrics[ui->selected_range];
    if (ui->pending) {
        if (auto reply = ui->pending->Take()) {
            if (auto* value = std::get_if<harness::Overview>(&reply->data)) {
                metrics = std::move(*value);
            }
            ui->request_failed = reply->error != harness::CatalogError::None;
            ui->pending.reset();
        }
    }
    if (ready && !ui->pending &&
        (ui->request_due || !ui->was_ready || lv_tick_elaps(ui->last_request) >= kRefreshIntervalMs)) {
        ui->pending = harness::SubmitCatalog({harness::CatalogOperation::Overview,
                                              kRanges[ui->selected_range], {}, {}, 0});
        ui->last_request = lv_tick_get();
        ui->request_due = false;
    }
    ui->was_ready = ready;

    if (!metrics) {
        for (auto* label : ui->values) product_ui::SetText(label, "--");
    } else {
        const std::optional<uint64_t> counts[] = {
            metrics->sessions, metrics->messages, metrics->total_tokens, metrics->active_days};
        for (size_t i = 0; i < 4; ++i) {
            char text[32] = "--";
            if (counts[i]) {
                if (i == 2) FormatTokens(*counts[i], text, sizeof(text));
                else FormatCount(*counts[i], text, sizeof(text));
            }
            product_ui::SetText(ui->values[i], text);
        }
        product_ui::SetText(ui->values[4], metrics->peak_hour.empty() ? "--" : metrics->peak_hour.c_str());
        std::string model = metrics->favorite_model;
        if (model.rfind("DeepSeek ", 0) == 0) model[8] = '\n';
        product_ui::SetText(ui->values[5], model.empty() ? "--" : model.c_str());
    }
    const char* status = !ready ? (metrics ? "离线 · 显示上次统计" : "等待 Harness 连接")
        : ui->pending && !metrics ? "正在读取统计…"
        : ui->request_failed ? (metrics ? "刷新失败 · 显示上次统计" : "读取失败 · 点击周期重试") : "";
    product_ui::SetText(ui->data_status, status);
}

void OnTimer(lv_timer_t* timer) {
    auto* ui = static_cast<DashboardUi*>(lv_timer_get_user_data(timer));
    if (lv_screen_active() != ui->screen || StandbyScreen::IsActive()) return;
    RefreshMetrics(ui);
}

void OnDelete(lv_event_t* event) {
    auto* ui = static_cast<DashboardUi*>(lv_event_get_user_data(event));
    if (ui->timer != nullptr) lv_timer_delete(ui->timer);
    delete ui;
}

void OpenSettingsAsync(void*) {
    if (ScreenIsHome() && !StandbyScreen::IsActive()) {
        ScreenNavigateTo(SettingsScreen::Create);
    }
}
void OnSettings(lv_event_t*) { lv_async_call(OpenSettingsAsync, nullptr); }
void OnAssistant(lv_event_t*) { HarnessScreen::RequestOpen(); }
bool OnBootClick() { HarnessScreen::RequestOpen(); return true; }
bool OnBootLongPress() { HarnessScreen::RequestOpen(true); return true; }
bool OnKey(const char* key) {
    return std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_next") == 0;
}

lv_obj_t* CreateMetricCard(lv_obj_t* parent, int x, int y, int width, int height,
                           const char* title, const char* note, lv_obj_t** value_out) {
    lv_obj_t* card = product_ui::Box(parent, x, y, width, height, 1);
    lv_obj_set_style_radius(card, 8, 0);
    product_ui::Label(card, 12, 10, width - 24, title);
    *value_out = product_ui::Label(card, 12, 40, width - 24, "--",
                                   product_ui::HeadingFont());
    lv_obj_set_height(*value_out, 42);
    lv_label_set_long_mode(*value_out, LV_LABEL_LONG_DOT);
    if (*note) product_ui::Label(card, 12, height - 34, width - 24, note);
    return card;
}

lv_obj_t* CreateRangeButton(lv_obj_t* parent, int x, const char* text,
                            lv_event_cb_t callback) {
    lv_obj_t* button = product_ui::Button(parent, x, 44, 70, 38, text, callback);
    lv_obj_set_style_radius(button, 6, 0);
    return button;
}
}  // namespace

lv_obj_t* DashboardScreen::Create() {
    ESP_LOGI("Dashboard", "create vertical usage overview");
    auto* ui = new DashboardUi;
    ui->screen = product_ui::Screen();
    lv_obj_set_user_data(ui->screen, ui);

    const EpdStatusBar status = ScreenCreateStatusBar(ui->screen);
    const int width = LV_HOR_RES - 32;
    const int top = status.height + 12;
    const int height = LV_VER_RES - top;
    lv_obj_t* body = product_ui::Box(ui->screen, 16, top, width, height);

    product_ui::Label(body, 0, 0, 190, "使用概览", product_ui::HeadingFont());
    ui->range_buttons[0] = CreateRangeButton(body, width - 236, "全部", OnRangeAll);
    ui->range_buttons[1] = CreateRangeButton(body, width - 158, "30天", OnRange30d);
    ui->range_buttons[2] = CreateRangeButton(body, width - 80, "7天", OnRange7d);
    SetSelectedRange(ui, 0);

    const int gap = 10;
    const int card_width = (width - gap) / 2;
    const int card_height = 132;
    const int first_y = 94;
    CreateMetricCard(body, 0, first_y, card_width, card_height,
                     "会话数", "当前周期", &ui->values[0]);
    CreateMetricCard(body, card_width + gap, first_y, card_width, card_height,
                     "消息数", "用户与助手", &ui->values[1]);
    CreateMetricCard(body, 0, first_y + card_height + gap, card_width, card_height,
                     "总 Token", "输入和输出", &ui->values[2]);
    CreateMetricCard(body, card_width + gap, first_y + card_height + gap,
                     card_width, card_height, "活跃天数",
                     "活跃日期", &ui->values[3]);
    CreateMetricCard(body, 0, first_y + 2 * (card_height + gap), card_width, card_height,
                     "高峰时段", "请求最高时段", &ui->values[4]);
    CreateMetricCard(body, card_width + gap, first_y + 2 * (card_height + gap),
                     card_width, card_height, "常用模型",
                     "", &ui->values[5]);
    lv_obj_set_style_text_font(ui->values[5], fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_line_space(ui->values[5], 4, 0);
    lv_obj_set_height(ui->values[5], card_height - 50);

    ui->data_status = product_ui::Label(body, 0, first_y + 3 * card_height + 2 * gap + 8,
                                        width, "");
    const int button_y = height - 72;
    const int button_width = (width - 12) / 2;
    product_ui::Button(body, 0, button_y, button_width, 58, "进入助手",
                       OnAssistant, true);
    product_ui::Button(body, button_width + 12, button_y, button_width, 58, "设置",
                       OnSettings);

    RefreshMetrics(ui);
    ui->timer = lv_timer_create(OnTimer, 250, ui);
    lv_obj_add_event_cb(ui->screen, OnDelete, LV_EVENT_DELETE, ui);
    ScreenSetIsHome(true);
    VkKey_AttachScreen(ui->screen, kScreenId,
                       VkKeyScreenDesc{DashboardScreen::Create, OnKey, OnBootClick,
                                       OnBootLongPress});
    return ui->screen;
}
