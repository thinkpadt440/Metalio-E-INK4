#include "harness_screen.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "boot_key_handler.h"
#include "harness_voice.h"
#include "product_ui.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

namespace {
constexpr const char* kScreenId = "harness";
constexpr const char* kDemoPages[] = {
    "这是用于验证排版的演示文本。\n\n"
    "助手的回复将显示在这里。较长的内容可以使用音量键上下翻页：音量加为上一页，音量减为下一页。\n\n"
    "BOOT 短按进入助手；按住超过半秒开始录音，松手结束。录音时，标题会显示“助手（录音中）”。\n\n"
    "当前服务尚未接入。演示文本不是模型回复；录音只保留在本机内存中，不会发送。",
    "阅读与翻页\n\n"
    "内容区已向上、向下扩展，去掉了屏幕上的上一页和下一页按钮。\n\n"
    "使用机身音量键可以逐页阅读。到达第一页或最后一页后，继续按键不会越界。\n\n"
    "页码保留在底部，你可以随时确认当前阅读位置。HOME 可以回到工作台。",
    "你已经读到最后一页。\n\n"
    "点击“返回主页”，或按 HOME，即可回到工作台。\n\n"
    "点击“重读演示”可回到第一页。接入服务后，这里会显示真实的会话内容。",
};
constexpr int kPageCount = sizeof(kDemoPages) / sizeof(kDemoPages[0]);
enum class Action { Demo, Previous, Next, Home };

struct HarnessUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* heading = nullptr;
    lv_obj_t* text = nullptr;
    lv_obj_t* page = nullptr;
    lv_obj_t* demo = nullptr;
    lv_timer_t* timer = nullptr;
    bool showing_demo = false;
    int page_index = 0;
};

void Render(HarnessUi* ui) {
    const auto voice = harness_voice::Snapshot();
    const bool busy = voice.phase == harness_voice::Phase::Preparing ||
                      voice.phase == harness_voice::Phase::Recording;
    const char* heading = voice.phase == harness_voice::Phase::Preparing ? "助手（准备录音）"
        : voice.phase == harness_voice::Phase::Recording ? "助手（录音中）" : "助手（未连接）";
    product_ui::SetText(ui->heading, heading);
    char recording[256];
    const char* text;
    if (busy) {
        text = voice.phase == harness_voice::Phase::Preparing ? "正在准备麦克风，请继续按住 BOOT。"
            : "正在录音，松开 BOOT 结束。\n\n最长可录制 30 秒。";
    } else if (ui->showing_demo) {
        text = kDemoPages[ui->page_index];
    } else if (voice.phase == harness_voice::Phase::Complete) {
        std::snprintf(recording, sizeof(recording),
                      "已录制 %.1f 秒。\n\n服务未连接，录音尚未发送。\n\n再次长按 BOOT 可重新录音。",
                      voice.duration_ms / 1000.0);
        text = recording;
    } else if (voice.phase == harness_voice::Phase::Error) {
        text = "录音未成功，请稍后重试。\n\n可在设置的音频测试中检查麦克风。";
    } else {
        text = "按住 BOOT 开始说话，松手结束。\n\n"
               "连接服务后，这里会显示会话、任务状态与助手回复。\n\n"
               "点击“演示文本”可检查排版，使用音量键上下翻页。";
    }
    product_ui::SetText(ui->text, text);
    char page[24];
    std::snprintf(page, sizeof(page), "%d / %d", ui->showing_demo && !busy ? ui->page_index + 1 : 0,
                  ui->showing_demo && !busy ? kPageCount : 0);
    product_ui::SetText(ui->page, page);
    product_ui::SetText(lv_obj_get_child(ui->demo, 0), ui->showing_demo ? "重读演示" : "演示文本");
}

void OnTimer(lv_timer_t* timer) {
    auto* ui = static_cast<HarnessUi*>(lv_timer_get_user_data(timer));
    if (lv_screen_active() == ui->screen && !StandbyScreen::IsActive()) Render(ui);
}

void ApplyAction(void* argument) {
    if (!HarnessScreen::IsActive() || StandbyScreen::IsActive()) return;
    const auto action = static_cast<Action>(reinterpret_cast<intptr_t>(argument));
    if (action == Action::Home) { ScreenGoHome(); return; }
    const auto voice = harness_voice::Snapshot();
    if (voice.phase == harness_voice::Phase::Preparing || voice.phase == harness_voice::Phase::Recording) return;
    auto* ui = static_cast<HarnessUi*>(lv_obj_get_user_data(lv_screen_active()));
    if (action == Action::Demo) {
        ui->showing_demo = true;
        ui->page_index = 0;
    } else if (ui->showing_demo && action == Action::Previous && ui->page_index > 0) {
        --ui->page_index;
    } else if (ui->showing_demo && action == Action::Next && ui->page_index + 1 < kPageCount) {
        ++ui->page_index;
    } else return;
    Render(ui);
}
void RequestAction(Action action) {
    ScreenLvAsync(ApplyAction, reinterpret_cast<void*>(static_cast<intptr_t>(action)));
}
void OnDemo(lv_event_t*) { RequestAction(Action::Demo); }
void OnHome(lv_event_t*) { RequestAction(Action::Home); }
bool OnKey(const char* key) {
    if (std::strcmp(key, "vk_prev") == 0) { RequestAction(Action::Previous); return true; }
    if (std::strcmp(key, "vk_next") == 0) { RequestAction(Action::Next); return true; }
    return false;
}

void BeginRecordingAsync(void*) {
    if (!HarnessScreen::IsActive() || StandbyScreen::IsActive() || !BootKey_IsHeld()) return;
    auto* ui = static_cast<HarnessUi*>(lv_obj_get_user_data(lv_screen_active()));
    ui->showing_demo = false;
    harness_voice::Begin();
    Render(ui);
}
bool OnBootClick() { return true; }
bool OnBootLongPress() { ScreenLvAsync(BeginRecordingAsync); return true; }
bool OnBootPressUp() { harness_voice::End(); return true; }
void OnUnload(lv_event_t*) { harness_voice::End(); }
void OnDelete(lv_event_t* event) {
    auto* ui = static_cast<HarnessUi*>(lv_event_get_user_data(event));
    lv_timer_delete(ui->timer);
    delete ui;
}
void OpenAsync(void* argument) {
    if (StandbyScreen::IsActive()) return;
    if (!HarnessScreen::IsActive()) ScreenNavigateTo(HarnessScreen::Create);
    if (argument != nullptr) BeginRecordingAsync(nullptr);
}
}  // namespace

bool HarnessScreen::IsActive() {
    const char* active = VkKey_ActiveScreen();
    return active != nullptr && std::strcmp(active, kScreenId) == 0;
}
void HarnessScreen::RequestOpen(bool record_if_held) {
    ScreenLvAsync(OpenAsync, record_if_held ? reinterpret_cast<void*>(1) : nullptr);
}
void HarnessScreen::OnEnterStandby() { harness_voice::End(); }

lv_obj_t* HarnessScreen::Create() {
    ESP_LOGI("HarnessUI", "create assistant with hardware PTT");
    auto* ui = new HarnessUi;
    ui->screen = product_ui::Screen();
    lv_obj_set_user_data(ui->screen, ui);
    const EpdStatusBar status = ScreenCreateStatusBar(ui->screen);
    const int width = LV_HOR_RES - 32;
    const int top = status.height + 12;
    const int height = LV_VER_RES - top;
    lv_obj_t* body = product_ui::Box(ui->screen, 16, top, width, height);
    ui->heading = product_ui::Label(body, 0, 0, width, "助手（未连接）", product_ui::HeadingFont());
    ui->text = product_ui::Label(body, 0, 58, width, "");

    ui->demo = product_ui::Button(body, 0, height - 52, 160, 44, "演示文本", OnDemo);
    ui->page = product_ui::Label(body, 164, height - 44, width - 328, "0 / 0");
    lv_obj_set_style_text_align(ui->page, LV_TEXT_ALIGN_CENTER, 0);
    product_ui::Button(body, width - 160, height - 52, 160, 44, "返回主页", OnHome, true);
    Render(ui);
    ui->timer = lv_timer_create(OnTimer, 500, ui);
    lv_obj_add_event_cb(ui->screen, OnUnload, LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_add_event_cb(ui->screen, OnDelete, LV_EVENT_DELETE, ui);
    ScreenSetIsHome(false);
    VkKey_AttachScreen(ui->screen, kScreenId,
                       VkKeyScreenDesc{HarnessScreen::Create, OnKey, OnBootClick, OnBootLongPress,
                                       nullptr, OnBootPressUp});
    return ui->screen;
}
