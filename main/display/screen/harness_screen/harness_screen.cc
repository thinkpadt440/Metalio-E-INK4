#include "harness_screen.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "product_ui.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

namespace {
constexpr const char* kScreenId = "harness";
constexpr const char* kDemoPages[] = {
    "这是用于验证界面的演示文本。\n\n"
    "这里将呈现助手的回复、任务进度和需要你确认的操作。\n\n"
    "当前尚未连接服务，这些文字不是模型生成的回复。",
    "墨水屏适合安静地阅读。\n\n"
    "较长的回复会分成多页。你可以点击下方按钮，也可以使用机身的翻页键。\n\n"
    "这一页用于检查中文换行、文字边距和翻页响应。",
    "你已经读到最后一页。\n\n"
    "点击“返回主页”，或按 HOME，即可回到工作台。\n\n"
    "点击“重读演示”可回到第一页。接入服务后，这里会显示真实的会话内容。",
};
constexpr int kPageCount = sizeof(kDemoPages) / sizeof(kDemoPages[0]);
enum class Action { Demo, Previous, Next, Home };

struct HarnessUi {
    lv_obj_t* title = nullptr;
    lv_obj_t* text = nullptr;
    lv_obj_t* page = nullptr;
    lv_obj_t* previous = nullptr;
    lv_obj_t* next = nullptr;
    lv_obj_t* demo = nullptr;
    bool showing_demo = false;
    int page_index = 0;
};

void Render(HarnessUi* ui) {
    product_ui::SetText(ui->title, ui->showing_demo ? "演示回复" : "你的随身助手");
    product_ui::SetText(ui->text, ui->showing_demo ? kDemoPages[ui->page_index]
        : "连接服务后，可在这里查看会话、任务状态与助手回复。\n\n"
          "先点击“演示文本”，体验阅读和翻页。");
    char page[24];
    std::snprintf(page, sizeof(page), "%d / %d", ui->showing_demo ? ui->page_index + 1 : 0,
                  ui->showing_demo ? kPageCount : 0);
    product_ui::SetText(ui->page, page);
    auto set_enabled = [](lv_obj_t* button, bool enabled) {
        if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
        else lv_obj_add_state(button, LV_STATE_DISABLED);
    };
    set_enabled(ui->previous, ui->showing_demo && ui->page_index > 0);
    set_enabled(ui->next, ui->showing_demo && ui->page_index + 1 < kPageCount);
    product_ui::SetText(lv_obj_get_child(ui->demo, 0), ui->showing_demo ? "重读演示" : "演示文本");
    ESP_LOGI("HarnessUI", "render demo=%d page=%d", ui->showing_demo, ui->page_index + 1);
}

void ApplyAction(void* argument) {
    if (!HarnessScreen::IsActive() || StandbyScreen::IsActive()) return;
    const auto action = static_cast<Action>(reinterpret_cast<intptr_t>(argument));
    if (action == Action::Home) {
        ScreenGoHome();
        return;
    }
    auto* ui = static_cast<HarnessUi*>(lv_obj_get_user_data(lv_screen_active()));
    if (action == Action::Demo) {
        ui->showing_demo = true;
        ui->page_index = 0;
    } else if (ui->showing_demo && action == Action::Previous && ui->page_index > 0) {
        --ui->page_index;
    } else if (ui->showing_demo && action == Action::Next && ui->page_index + 1 < kPageCount) {
        ++ui->page_index;
    } else {
        return;
    }
    Render(ui);
}

void RequestAction(Action action) {
    ScreenLvAsync(ApplyAction, reinterpret_cast<void*>(static_cast<intptr_t>(action)));
}

void OnDemo(lv_event_t*) { RequestAction(Action::Demo); }
void OnPrevious(lv_event_t*) { RequestAction(Action::Previous); }
void OnNext(lv_event_t*) { RequestAction(Action::Next); }
void OnHome(lv_event_t*) { RequestAction(Action::Home); }

bool OnKey(const char* key) {
    if (std::strcmp(key, "vk_prev") == 0) {
        RequestAction(Action::Previous);
        return true;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        RequestAction(Action::Next);
        return true;
    }
    return false;
}

bool OnBootClick() {
    RequestAction(Action::Demo);
    return true;
}

bool OnBootLongPress() { return true; }

void OnDelete(lv_event_t* event) {
    delete static_cast<HarnessUi*>(lv_event_get_user_data(event));
}

void OpenAsync(void*) {
    if (!HarnessScreen::IsActive() && !StandbyScreen::IsActive()) {
        ScreenNavigateTo(HarnessScreen::Create);
    }
}
}  // namespace

bool HarnessScreen::IsActive() {
    const char* active = VkKey_ActiveScreen();
    return active != nullptr && std::strcmp(active, kScreenId) == 0;
}

void HarnessScreen::RequestOpen() {
    ScreenLvAsync(OpenAsync);
}

lv_obj_t* HarnessScreen::Create() {
    ESP_LOGI("HarnessUI", "create assistant shell (offline)");
    auto* ui = new HarnessUi;
    lv_obj_t* screen = product_ui::Screen();
    lv_obj_set_user_data(screen, ui);
    const EpdStatusBar status = ScreenCreateStatusBar(screen);
    const int width = LV_HOR_RES - 48;
    const int top = status.height + 20;
    const int height = LV_VER_RES - top;
    lv_obj_t* body = product_ui::Box(screen, 24, top, width, height);
    product_ui::Label(body, 0, 0, width, "助手", product_ui::HeadingFont());
    product_ui::Label(body, 0, 44, width, "尚未连接服务");

    lv_obj_t* card = product_ui::Box(body, 0, 104, width, height - 280, 1);
    ui->title = product_ui::Label(card, 20, 22, width - 40, "", product_ui::HeadingFont());
    ui->text = product_ui::Label(card, 20, 82, width - 40, "");

    const int button_width = 132;
    ui->previous = product_ui::Button(body, 0, height - 156, button_width, 52, "上一页", OnPrevious);
    ui->page = product_ui::Label(body, button_width, height - 142, width - button_width * 2, "0 / 0");
    lv_obj_set_style_text_align(ui->page, LV_TEXT_ALIGN_CENTER, 0);
    ui->next = product_ui::Button(body, width - button_width, height - 156,
                                  button_width, 52, "下一页", OnNext);
    const int half = (width - 16) / 2;
    ui->demo = product_ui::Button(body, 0, height - 88, half, 52, "演示文本", OnDemo);
    product_ui::Button(body, half + 16, height - 88, half, 52, "返回主页", OnHome, true);
    product_ui::Label(body, 0, height - 34, width, "演示内容 · 不发送请求");

    Render(ui);
    lv_obj_add_event_cb(screen, OnDelete, LV_EVENT_DELETE, ui);
    ScreenSetIsHome(false);
    VkKey_AttachScreen(screen, kScreenId,
                       VkKeyScreenDesc{HarnessScreen::Create, OnKey, OnBootClick, OnBootLongPress});
    return screen;
}
