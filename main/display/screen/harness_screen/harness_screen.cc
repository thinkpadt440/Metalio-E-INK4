#include "harness_screen.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "boot_key_handler.h"
#include "harness_voice.h"
#include "haptic_feedback.h"
#include "offline_demo.h"
#include "product_ui.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

namespace {
constexpr const char* kScreenId = "harness";
std::atomic<bool> s_approval_visible{false};
enum class View { Sessions, Chat };
enum class Picker { None, Model, Authorization };
enum class Action { Previous, Next, Back, Approve, Reject };

struct HarnessUi {
    offline_demo::KeepAwake awake;
    lv_obj_t* screen = nullptr;
    lv_obj_t* sessions_panel = nullptr;
    lv_obj_t* chat_panel = nullptr;
    lv_obj_t* session_title = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* page_indicator = nullptr;
    lv_obj_t* message = nullptr;
    lv_obj_t* model_button = nullptr;
    lv_obj_t* authorization_button = nullptr;
    lv_obj_t* approval_panel = nullptr;
    lv_obj_t* picker_panel = nullptr;
    lv_obj_t* picker_title = nullptr;
    lv_obj_t* picker_options[2] = {};
    product_ui::KeyHints key_hints;
    lv_timer_t* timer = nullptr;
    View view = View::Sessions;
    Picker picker = Picker::None;
    int session_index = 0;
    int page_index = 0;
    int record_target = -1;
    bool recording_error = false;
};

HarnessUi* ActiveUi() {
    if (!HarnessScreen::IsActive()) return nullptr;
    auto* screen = lv_screen_active();
    return screen == nullptr ? nullptr : static_cast<HarnessUi*>(lv_obj_get_user_data(screen));
}
void Visible(lv_obj_t* object, bool visible) {
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
void SetButtonText(lv_obj_t* button, const char* text) {
    product_ui::SetText(lv_obj_get_child(button, 0), text);
}
void RenderKeyHints(HarnessUi* ui, bool approval = false) {
    if (approval) {
        ui->key_hints.SetActions("仅本次允许", "拒绝");
    } else if (ui->view == View::Chat && ui->picker == Picker::None && ui->record_target < 0) {
        ui->key_hints.SetActions("上一页", "下一页");
    } else {
        ui->key_hints.SetActions("—", "—");
    }
}
void RenderPicker(HarnessUi* ui) {
    if (ui->picker == Picker::None) return;
    const bool model = ui->picker == Picker::Model;
    const auto& s = offline_demo::State().sessions[ui->session_index];
    product_ui::SetText(ui->picker_title, model ? "选择模型" : "选择授权级别");
    for (int i = 0; i < 2; ++i) {
        SetButtonText(ui->picker_options[i], model ? offline_demo::kModels[i] : offline_demo::kAuthorizations[i]);
        const bool selected = i == (model ? s.model : s.authorization);
        lv_obj_set_style_bg_color(ui->picker_options[i], selected ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_color(ui->picker_options[i], selected ? lv_color_white() : lv_color_black(), 0);
    }
}
const char* Status(offline_demo::Phase phase) {
    using Phase = offline_demo::Phase;
    switch (phase) {
        case Phase::Transcribing: return "接收（转写中）";
        case Phase::Thinking: return "接收（分析中）";
        case Phase::WaitingApproval: return "接收（等待审批）";
        case Phase::Executing: return "接收（执行中）";
        case Phase::Testing: return "接收（检查中）";
        case Phase::Complete: return "接收（已完成）";
        case Phase::Rejected: return "接收（已拒绝）";
        default: return "接收（就绪）";
    }
}
void Render(HarnessUi* ui) {
    if (ui->view != View::Chat) return;
    auto& s = offline_demo::State().sessions[ui->session_index];
    const auto voice = harness_voice::Snapshot();
    const bool preparing = ui->record_target >= 0 && voice.phase == harness_voice::Phase::Preparing;
    const bool recording = ui->record_target >= 0 && voice.phase == harness_voice::Phase::Recording;
    const bool approval = s.phase == offline_demo::Phase::WaitingApproval;
    product_ui::SetText(ui->session_title, offline_demo::kNames[ui->session_index]);
    product_ui::SetText(ui->status, preparing ? "接收（准备录音）" :
        recording ? "接收（录音中）" : ui->recording_error ? "接收（录音失败）" : Status(s.phase));
    s_approval_visible.store(approval);
    RenderKeyHints(ui, approval);
    Visible(ui->approval_panel, approval);
    Visible(ui->message, !approval);
    Visible(ui->model_button, !approval);
    Visible(ui->authorization_button, !approval);
    Visible(ui->page_indicator, !approval);
    if (approval) {
        ui->picker = Picker::None;
        Visible(ui->picker_panel, false);
        return;
    }
    if (ui->page_index >= static_cast<int>(s.pages.size())) ui->page_index = static_cast<int>(s.pages.size()) - 1;
    if (ui->page_index < 0) ui->page_index = 0;
    char text[80];
    std::snprintf(text, sizeof(text), "%d / %u", ui->page_index + 1, static_cast<unsigned>(s.pages.size()));
    product_ui::SetText(ui->page_indicator, text);
    if (preparing) {
        product_ui::SetText(ui->message, "正在准备麦克风，请继续按住 BOOT。");
    } else if (recording) {
        product_ui::SetText(ui->message, "正在录音，松开 BOOT 结束。");
    } else if (ui->recording_error) {
        product_ui::SetText(ui->message, "录音未成功，请重新长按 BOOT。");
    } else if (s.phase == offline_demo::Phase::Transcribing) {
        product_ui::SetText(ui->message, "录音已结束。\n\n正在准备转写与回复…");
    } else if (s.phase == offline_demo::Phase::Thinking) {
        product_ui::SetText(ui->message, ui->session_index == offline_demo::kCoder
            ? "你\n帮我检查统计页面并运行测试。\n\n助手\n正在分析项目，准备执行步骤…"
            : s.pages.back().c_str());
    } else {
        product_ui::SetText(ui->message, s.pages[ui->page_index].c_str());
    }
    std::snprintf(text, sizeof(text), "模型\n%s", s.model == 0 ? "V4.1-Flash" : "V4.1-Pro");
    SetButtonText(ui->model_button, text);
    std::snprintf(text, sizeof(text), "授权级别\n%s", offline_demo::kAuthorizations[s.authorization]);
    SetButtonText(ui->authorization_button, text);
    const bool busy = offline_demo::Busy(s) || ui->record_target >= 0;
    for (auto* button : {ui->model_button, ui->authorization_button}) {
        if (busy) lv_obj_add_state(button, LV_STATE_DISABLED);
        else lv_obj_remove_state(button, LV_STATE_DISABLED);
    }
    RenderPicker(ui);
}
void ShowSessions(HarnessUi* ui) {
    if (ui->record_target >= 0) return;
    s_approval_visible.store(false);
    ui->view = View::Sessions;
    ui->picker = Picker::None;
    Visible(ui->picker_panel, false);
    Visible(ui->chat_panel, false);
    Visible(ui->sessions_panel, true);
    RenderKeyHints(ui);
}
void EnterChat(HarnessUi* ui, int index) {
    ui->session_index = index;
    ui->page_index = static_cast<int>(offline_demo::State().sessions[index].pages.size()) - 1;
    ui->view = View::Chat;
    ui->picker = Picker::None;
    ui->recording_error = false;
    Visible(ui->picker_panel, false);
    Visible(ui->sessions_panel, false);
    Visible(ui->chat_panel, true);
    Render(ui);
}
void OnSessionSelected(lv_event_t* event) {
    auto* ui = ActiveUi();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (ui != nullptr && index >= 0 && index < offline_demo::kSessionCount) EnterChat(ui, index);
}
void OpenPicker(Picker picker) {
    auto* ui = ActiveUi();
    if (ui == nullptr || ui->view != View::Chat || ui->record_target >= 0 ||
        offline_demo::Busy(offline_demo::State().sessions[ui->session_index])) return;
    ui->picker = picker;
    Visible(ui->picker_panel, true);
    RenderKeyHints(ui);
    RenderPicker(ui);
}
void OnModelPicker(lv_event_t*) { OpenPicker(Picker::Model); }
void OnAuthorizationPicker(lv_event_t*) { OpenPicker(Picker::Authorization); }
void OnPickerCancel(lv_event_t*) {
    if (auto* ui = ActiveUi()) {
        ui->picker = Picker::None;
        Visible(ui->picker_panel, false);
        Render(ui);
    }
}
void SelectPickerOption(int index) {
    auto* ui = ActiveUi();
    if (ui == nullptr || ui->picker == Picker::None) return;
    auto& s = offline_demo::State().sessions[ui->session_index];
    if (ui->picker == Picker::Model) s.model = index;
    else {
        s.authorization = index;
        // Re-selecting request approval also replays the scenario after a decision.
        if (ui->session_index == offline_demo::kCoder && index == 0)
            offline_demo::Start(offline_demo::kCoder, lv_tick_get(), false);
    }
    ui->picker = Picker::None;
    Visible(ui->picker_panel, false);
    Render(ui);
}
void OnPickerOption0(lv_event_t*) { SelectPickerOption(0); }
void OnPickerOption1(lv_event_t*) { SelectPickerOption(1); }
void ApplyAction(void* argument) {
    if (!HarnessScreen::IsActive() || StandbyScreen::IsActive()) return;
    auto* ui = ActiveUi();
    if (ui == nullptr || ui->view != View::Chat || ui->record_target >= 0) return;
    const auto action = static_cast<Action>(reinterpret_cast<intptr_t>(argument));
    auto& s = offline_demo::State().sessions[ui->session_index];
    if (s.phase == offline_demo::Phase::WaitingApproval) {
        if (action == Action::Approve || action == Action::Reject) {
            if (offline_demo::Decide(action == Action::Approve, lv_tick_get()))
                ui->page_index = static_cast<int>(s.pages.size()) - 1;
        } else if (action == Action::Back) ShowSessions(ui);
        Render(ui);
        return;
    }
    // A delayed/double approval key must never decide a later request.
    if (action == Action::Approve || action == Action::Reject || ui->picker != Picker::None) return;
    if (action == Action::Back) ShowSessions(ui);
    else if (action == Action::Previous && ui->page_index > 0) --ui->page_index;
    else if (action == Action::Next && ui->page_index + 1 < static_cast<int>(s.pages.size())) ++ui->page_index;
    Render(ui);
}
void RequestAction(Action action) { ScreenLvAsync(ApplyAction, reinterpret_cast<void*>(static_cast<intptr_t>(action))); }
void OnBack(lv_event_t*) { RequestAction(Action::Back); }
void OnApprove(lv_event_t*) { RequestAction(Action::Approve); }
void OnReject(lv_event_t*) { RequestAction(Action::Reject); }
bool OnKey(const char* key) {
    // Input callbacks may run outside LVGL; only inspect the published flag.
    const bool approval = s_approval_visible.load();
    // Approval mapping corrected from device feedback; normal pagination is unchanged.
    if (std::strcmp(key, "vk_next") == 0) {
        RequestAction(approval ? Action::Reject : Action::Next); return true;
    }
    if (std::strcmp(key, "vk_prev") == 0) {
        RequestAction(approval ? Action::Approve : Action::Previous); return true;
    }
    return false;
}
void OnReset(lv_event_t*) {
    auto* ui = ActiveUi();
    if (ui == nullptr || ui->record_target >= 0) return;
    offline_demo::State().Reset();
    ui->recording_error = false;
}
void CreatePicker(HarnessUi* ui, int width, int height) {
    const int panel_width = width - 32;
    const int panel_height = 308;
    ui->picker_panel = product_ui::Box(ui->chat_panel, 16, (height - panel_height) / 2,
                                      panel_width, panel_height, 2);
    ui->picker_title = product_ui::Label(ui->picker_panel, 14, 14, panel_width - 28, "",
                                        product_ui::HeadingFont());
    ui->picker_options[0] = product_ui::Button(ui->picker_panel, 14, 78, panel_width - 28, 56, "", OnPickerOption0);
    ui->picker_options[1] = product_ui::Button(ui->picker_panel, 14, 144, panel_width - 28, 56, "", OnPickerOption1);
    product_ui::Button(ui->picker_panel, 14, 236, panel_width - 28, 50, "取消", OnPickerCancel);
    Visible(ui->picker_panel, false);
}
void BeginRecordingAsync(void*) {
    if (!HarnessScreen::IsActive() || StandbyScreen::IsActive() || !BootKey_IsHeld()) return;
    auto* ui = ActiveUi();
    if (ui == nullptr || ui->record_target >= 0) return;
    if (ui->view == View::Sessions) EnterChat(ui, offline_demo::kCasual);
    if (offline_demo::Busy(offline_demo::State().sessions[ui->session_index])) return;
    ui->picker = Picker::None;
    ui->record_target = ui->session_index;
    ui->recording_error = false;
    Visible(ui->picker_panel, false);
    harness_voice::Begin();
    Render(ui);
}
bool OnBootClick() { return true; }
bool OnBootLongPress() { ScreenLvAsync(BeginRecordingAsync); return true; }
bool OnBootPressUp() { harness_voice::End(); return true; }

void OnTimer(lv_timer_t* timer) {
    auto* ui = static_cast<HarnessUi*>(lv_timer_get_user_data(timer));
    if (lv_screen_active() != ui->screen || StandbyScreen::IsActive()) return;
    if (ui->record_target >= 0) {
        const auto voice = harness_voice::Snapshot();
        if (voice.phase == harness_voice::Phase::Complete || voice.phase == harness_voice::Phase::Error ||
            voice.phase == harness_voice::Phase::Idle) {
            const int target = ui->record_target;
            ui->record_target = -1;
            ui->recording_error = voice.phase == harness_voice::Phase::Error;
            if (voice.phase == harness_voice::Phase::Complete && voice.duration_ms >= 200)
                offline_demo::Start(target, lv_tick_get(), true);
        }
    }
    for (int i = 0; i < offline_demo::kSessionCount; ++i) {
        if (offline_demo::Advance(i, lv_tick_get()) && ui->view == View::Chat && ui->session_index == i)
            ui->page_index = static_cast<int>(offline_demo::State().sessions[i].pages.size()) - 1;
    }
    Render(ui);
}
void OnUnload(lv_event_t* event) {
    s_approval_visible.store(false);
    auto* ui = static_cast<HarnessUi*>(lv_event_get_user_data(event));
    ui->record_target = -1;
    harness_voice::End();
}
void OnDelete(lv_event_t* event) {
    auto* ui = static_cast<HarnessUi*>(lv_event_get_user_data(event));
    if (ui->timer != nullptr) lv_timer_delete(ui->timer);
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
void HarnessScreen::OnEnterStandby() {
    s_approval_visible.store(false);
    ScreenLvAsync([](void*) { if (auto* ui = ActiveUi()) ui->record_target = -1; });
    harness_voice::End();
}
lv_obj_t* HarnessScreen::Create() {
    ESP_LOGI("HarnessUI", "create companion sessions");
    auto* ui = new HarnessUi;
    ui->screen = product_ui::Screen();
    lv_obj_set_user_data(ui->screen, ui);
    const auto status = ScreenCreateStatusBar(ui->screen);
    const int width = LV_HOR_RES - 32;
    const int top = status.height + 12;
    const int height = LV_VER_RES - top - product_ui::kKeyHintsHeight - 8;
    auto* body = product_ui::Box(ui->screen, 16, top, width, height);
    ui->sessions_panel = product_ui::Box(body, 0, 0, width, height);
    product_ui::Label(ui->sessions_panel, 0, 0, width - 130, "最近活动", product_ui::HeadingFont());
    for (int i = 0; i < offline_demo::kSessionCount; ++i) {
        auto* row = product_ui::Box(ui->sessions_panel, 0, 74 + i * 146, width, 130, 1);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, OnSessionSelected, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        HapticAttachClick(row);
        product_ui::Label(row, 14, 12, width - 28, offline_demo::kNames[i], product_ui::HeadingFont());
        product_ui::Label(row, 14, 57, width - 28, i == offline_demo::kCasual ? "工作安排与日常交流" : "项目检查、代码修改与测试");
        product_ui::Label(row, 14, 96, width - 28, i == offline_demo::kCasual ? "今天 09:12" : "今天 10:26");
    }
    product_ui::Button(ui->sessions_panel, 0, height - 72, width, 58, "重置测试", OnReset);
    ui->chat_panel = product_ui::Box(body, 0, 0, width, height);
    Visible(ui->chat_panel, false);
    product_ui::Button(ui->chat_panel, 0, 0, 74, 46, "返回", OnBack);
    ui->session_title = product_ui::Label(ui->chat_panel, 88, 0, width - 100, "", product_ui::HeadingFont());
    ui->status = product_ui::Label(ui->chat_panel, 88, 42, width - 176, "接收（就绪）");
    ui->page_indicator = product_ui::Label(ui->chat_panel, width - 82, 42, 82, "");
    lv_obj_set_style_text_align(ui->page_indicator, LV_TEXT_ALIGN_RIGHT, 0);
    const int controls_y = height - 76;
    ui->message = product_ui::Label(ui->chat_panel, 0, 82, width, "");
    lv_obj_set_height(ui->message, controls_y - 98);
    const int button_width = (width - 10) / 2;
    ui->model_button = product_ui::Button(ui->chat_panel, 0, controls_y, button_width, 76, "", OnModelPicker);
    ui->authorization_button = product_ui::Button(ui->chat_panel, button_width + 10, controls_y, button_width, 76, "", OnAuthorizationPicker);
    ui->approval_panel = product_ui::Box(ui->chat_panel, 0, 82, width, height - 82);
    product_ui::Label(ui->approval_panel, 0, 0, width, "需要你的批准", product_ui::HeadingFont());
    product_ui::Label(ui->approval_panel, 0, 60, width,
        "编码助手申请执行\n\n修改统计页面，补充周期筛选检查，并运行项目构建。\n\n范围：当前固件项目\n授权：仅本次操作\n\n拒绝后将停止本次任务。");
    product_ui::Button(ui->approval_panel, 0, height - 82 - 76, button_width, 76, "仅本次允许", OnApprove, true);
    product_ui::Button(ui->approval_panel, button_width + 10, height - 82 - 76, button_width, 76, "拒绝", OnReject);
    Visible(ui->approval_panel, false);
    CreatePicker(ui, width, height);
    ui->key_hints = product_ui::CreateKeyHints(ui->screen);
    ui->timer = lv_timer_create(OnTimer, 100, ui);
    lv_obj_add_event_cb(ui->screen, OnUnload, LV_EVENT_SCREEN_UNLOADED, ui);
    lv_obj_add_event_cb(ui->screen, OnDelete, LV_EVENT_DELETE, ui);
    ScreenSetIsHome(false);
    VkKey_AttachScreen(ui->screen, kScreenId, VkKeyScreenDesc{
        HarnessScreen::Create, OnKey, OnBootClick, OnBootLongPress, nullptr, OnBootPressUp});
    ShowSessions(ui);
    return ui->screen;
}
