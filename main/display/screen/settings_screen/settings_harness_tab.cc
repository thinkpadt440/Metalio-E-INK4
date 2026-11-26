#include "settings_harness_tab.h"

#include "application.h"
#include "assets/lang_config.h"
#include "harness/service.h"
#include "product_ui.h"
#include "settings_common.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <utility>

namespace {
struct SaveRequest {
    harness::ServerOriginResult result = harness::ServerOriginResult::StorageError;
    std::atomic<bool> done{false};
};

struct HarnessSettingsUi {
    lv_obj_t* address = nullptr;
    lv_obj_t* connection = nullptr;
    lv_obj_t* save = nullptr;
    lv_obj_t* feedback = nullptr;
    lv_obj_t* keyboard = nullptr;
    lv_timer_t* timer = nullptr;
};

HarnessSettingsUi s_ui;
std::shared_ptr<SaveRequest> s_request;  // Only accessed by LVGL; the job owns its own reference.

const char* Text(const char* zh, const char* en) {
    return std::strcmp(Lang::CODE, "zh-CN") == 0 ? zh : en;
}

const char* StatusText() {
    using harness::ConnectionState;
    switch (harness::ServiceStatus().state) {
        case ConnectionState::Unconfigured: return Text("未配置", "Not configured");
        case ConnectionState::WaitingNetwork: return Text("等待网络", "Waiting for network");
        case ConnectionState::Connecting:
        case ConnectionState::Hello: return Text("连接中", "Connecting");
        case ConnectionState::Syncing: return Text("同步中", "Syncing");
        case ConnectionState::Ready: return Text("已连接", "Connected");
        case ConnectionState::Backoff: return Text("重连中", "Reconnecting");
        case ConnectionState::Stopped: return Text("已断开", "Disconnected");
    }
    return "";
}

const char* SaveMessage(harness::ServerOriginResult result) {
    using harness::ServerOriginResult;
    switch (result) {
        case ServerOriginResult::Saved: return Text("已保存，正在重启…", "Saved. Restarting...");
        case ServerOriginResult::Unchanged: return Text("地址未更改", "Address unchanged");
        case ServerOriginResult::Invalid:
            return Text("地址格式不正确，只需填写协议、主机和端口。",
                        "Enter the scheme, host and optional port.");
        case ServerOriginResult::RecordingPending:
            return Text("请先在助手中处理未确认的录音，再保存。",
                        "Finish or discard the pending recording in Assistant.");
        case ServerOriginResult::StorageError:
            return Text("保存失败，请重试。", "Could not save. Try again.");
    }
    return "";
}

void Update(lv_timer_t*) {
    product_ui::SetText(s_ui.connection, StatusText());
    if (!s_request || !s_request->done.load()) return;
    product_ui::SetText(s_ui.feedback, SaveMessage(s_request->result));
    if (s_request->result != harness::ServerOriginResult::Saved) {
        s_request.reset();
        lv_obj_clear_state(s_ui.save, LV_STATE_DISABLED);
        lv_obj_clear_state(s_ui.address, LV_STATE_DISABLED);
    }
}

void OnSave(lv_event_t*) {
    if (s_request) return;
    SettingsHarnessTab_HideKeyboard();
    auto origin = harness::NormalizeServerOrigin(lv_textarea_get_text(s_ui.address));
    if (!origin.empty() && !harness::ValidServerOrigin(origin)) {
        product_ui::SetText(s_ui.feedback, SaveMessage(harness::ServerOriginResult::Invalid));
        return;
    }
    lv_textarea_set_text(s_ui.address, origin.c_str());
    lv_obj_add_state(s_ui.save, LV_STATE_DISABLED);
    lv_obj_add_state(s_ui.address, LV_STATE_DISABLED);
    product_ui::SetText(s_ui.feedback, Text("正在保存…", "Saving..."));
    s_request = std::make_shared<SaveRequest>();
    Application::GetInstance().Schedule([request = s_request, origin = std::move(origin)]() mutable {
        request->result = harness::SaveServerOrigin(std::move(origin));
        request->done.store(true);
        if (request->result == harness::ServerOriginResult::Saved) {
            Application::GetInstance().Reboot();
        }
    });
}

void OnEdit(lv_event_t*) {
    if (!s_request) lv_obj_clear_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
}

void OnKeyboardClosed(lv_event_t*) {
    SettingsHarnessTab_HideKeyboard();
}

lv_obj_t* AddLabel(lv_obj_t* page, const char* text) {
    auto* label = lv_label_create(page);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
    lv_label_set_text(label, text);
    return label;
}

constexpr const char* kUrlKeys[] = {
    "1", "2", "3", "4", "5", "\n",
    "6", "7", "8", "9", "0", "\n",
    ".", ":", "/", "-", LV_SYMBOL_BACKSPACE, "\n",
    "http://", "https://", "\n",
    "abc", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

constexpr auto KeyOnClick(lv_buttonmatrix_ctrl_t width) {
    return static_cast<lv_buttonmatrix_ctrl_t>(
        width | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
}
constexpr auto kKey = KeyOnClick(LV_BUTTONMATRIX_CTRL_WIDTH_1);
constexpr auto kWideKey = KeyOnClick(LV_BUTTONMATRIX_CTRL_WIDTH_2);
constexpr auto kRepeatKey = LV_BUTTONMATRIX_CTRL_WIDTH_1;
constexpr lv_buttonmatrix_ctrl_t kUrlKeyControls[] = {
    kKey, kKey, kKey, kKey, kKey,
    kKey, kKey, kKey, kKey, kKey,
    kKey, kKey, kKey, kKey, kRepeatKey,
    kKey, kKey,
    kWideKey, kRepeatKey, kRepeatKey, kWideKey
};
}  // namespace

void SettingsHarnessTab_Build(lv_obj_t* page) {
    AddLabel(page, Text("服务端地址", "Server address"));
    s_ui.address = lv_textarea_create(page);
    lv_obj_set_width(s_ui.address, lv_pct(100));
    lv_obj_set_height(s_ui.address, 54);
    lv_obj_set_style_bg_color(s_ui.address, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_ui.address, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_ui.address, kSettingsBorderW, 0);
    lv_obj_set_style_text_color(s_ui.address, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_ui.address, fontpack_lv_font_ui(), 0);
    lv_textarea_set_one_line(s_ui.address, true);
    lv_textarea_set_max_length(s_ui.address, harness::kMaxServerOriginLength + 1);
    lv_textarea_set_placeholder_text(s_ui.address, "http://192.168.1.10:8780");
    const auto current = harness::ConfiguredServerOrigin();
    lv_textarea_set_text(s_ui.address, current.c_str());
    lv_textarea_set_cursor_pos(s_ui.address, 0);
    lv_obj_add_event_cb(s_ui.address, OnEdit, LV_EVENT_CLICKED, nullptr);

    AddLabel(page, Text("清空地址可停用连接。", "Clear the address to disable the connection."));
    AddLabel(page, Text("连接状态", "Connection status"));
    s_ui.connection = AddLabel(page, StatusText());
    s_ui.save = SettingsCreateSelectableOption(page, Text("保存并重启", "Save and restart"), OnSave, 0);
    s_ui.feedback = AddLabel(page, s_request ? Text("正在保存…", "Saving...") : "");

    s_ui.keyboard = lv_keyboard_create(page);
    lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_ui.keyboard, lv_pct(100));
    lv_obj_set_height(s_ui.keyboard, 262);
    lv_obj_set_style_pad_all(s_ui.keyboard, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_ui.keyboard, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.keyboard, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.keyboard, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_ui.keyboard, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_ui.keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_ui.keyboard, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_ui.keyboard, LV_FONT_DEFAULT, LV_PART_ITEMS);
    constexpr auto pressed_key = static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_PRESSED;
    lv_obj_set_style_bg_color(s_ui.keyboard, lv_color_black(), pressed_key);
    lv_obj_set_style_text_color(s_ui.keyboard, lv_color_white(), pressed_key);
    lv_keyboard_set_map(s_ui.keyboard, LV_KEYBOARD_MODE_SPECIAL, kUrlKeys, kUrlKeyControls);
    lv_keyboard_set_mode(s_ui.keyboard, LV_KEYBOARD_MODE_SPECIAL);
    lv_keyboard_set_textarea(s_ui.keyboard, s_ui.address);
    lv_obj_add_event_cb(s_ui.keyboard, OnKeyboardClosed, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_ui.keyboard, OnKeyboardClosed, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(s_ui.address, OnKeyboardClosed, LV_EVENT_READY, nullptr);

    if (s_request) {
        lv_obj_add_state(s_ui.save, LV_STATE_DISABLED);
        lv_obj_add_state(s_ui.address, LV_STATE_DISABLED);
    }
    s_ui.timer = lv_timer_create(Update, 500, nullptr);
    Update(nullptr);
}

void SettingsHarnessTab_Reset() {
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    s_ui = {};
}

void SettingsHarnessTab_HideKeyboard() {
    if (s_ui.keyboard) lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
}
