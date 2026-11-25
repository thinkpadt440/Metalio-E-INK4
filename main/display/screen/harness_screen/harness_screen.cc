#include "harness_screen.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "boot_key_handler.h"
#include "harness_voice.h"
#include "harness/service.h"
#include "haptic_feedback.h"
#include "product_ui.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

namespace {
constexpr const char* kScreenId = "harness";
constexpr int kPageCount = 2;
constexpr int kRecentSessionCount = 3;
constexpr int kCasualSessionIndex = kRecentSessionCount;
constexpr int kSessionCount = kRecentSessionCount + 1;

enum class View { Sessions, Chat };
enum class Picker { None, Model, Authorization };
enum class Action { Previous, Next, Back };

struct SessionInfo {
    const char* name;
    const char* preview;
    const char* updated;
    const char* pages[kPageCount];
};

constexpr SessionInfo kSessions[kSessionCount] = {
    {
        "E-INK4 固件 UI 改版", "讨论墨水屏统计页与助手交互", "今天 10:42",
        {
            "用户：重新设计 dashboard 和助手会话流程。\n\n"
            "助手：概览显示使用统计；进入助手先选择最近活动的 Session。",
            "用户：模型与授权级别要支持切换。\n\n"
            "助手：在当前会话页底部调整，两项配置分别保留在各自 Session。"
        }
    },
    {
        "ESP-IDF 串口排错", "检查 WSL 中的设备串口映射", "昨天 18:26",
        {
            "用户：app-flash 找不到 /dev/ttyACM0。\n\n"
            "助手：检查 USB 设备是否已附加到 WSL，再确认串口节点。",
            "用户：usbipd list 能看出电脑是否重新识别设备吗？\n\n"
            "助手：可以先检查设备状态，再用 ls /dev/ttyACM* 查看 Linux 端口。"
        }
    },
    {
        "Harness 接入方案", "梳理 Session、模型与授权映射", "周一 15:08",
        {
            "用户：设备端需要显示最近活动的 Harness Session。\n\n"
            "助手：选择 Session 后打开对应聊天；BOOT 长按沿用当前 Session。",
            "用户：还要有默认闲聊 Session。\n\n"
            "助手：未进入聊天页时长按 BOOT，会打开固定闲聊并开始录音。"
        }
    },
    {
        "固定闲聊", "独立于最近工作会话的默认对话", "固定入口",
        {
            "固定闲聊 Session 已打开。\n\n"
            "服务接入后，这里会显示该会话的真实对话记录。",
            "闲聊 Session 与从列表选择的工作 Session 分开保存模型和授权级别。"
        }
    }
};

constexpr const char* kModels[] = {
    "服务端默认",
    "DeepSeek Chat",
    "DeepSeek Reasoner"
};
constexpr const char* kAuthorizationLevels[] = {
    "受限",
    "标准",
    "扩展"
};

struct HarnessUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* sessions_panel = nullptr;
    lv_obj_t* chat_panel = nullptr;
    lv_obj_t* session_title = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* page_indicator = nullptr;
    lv_obj_t* message = nullptr;
    lv_obj_t* model_button = nullptr;
    lv_obj_t* authorization_button = nullptr;
    lv_obj_t* picker_panel = nullptr;
    lv_obj_t* picker_title = nullptr;
    lv_obj_t* picker_hint = nullptr;
    lv_obj_t* picker_options[3] = {};
    lv_timer_t* timer = nullptr;
    View view = View::Sessions;
    Picker picker = Picker::None;
    int session_index = 0;
    int page_index = 0;
    int model_index[kSessionCount] = {};
    int authorization_index[kSessionCount] = {1, 1, 1, 1};
};

HarnessUi* ActiveUi() {
    if (!HarnessScreen::IsActive()) return nullptr;
    lv_obj_t* screen = lv_screen_active();
    return screen == nullptr
        ? nullptr
        : static_cast<HarnessUi*>(lv_obj_get_user_data(screen));
}

void SetButtonText(lv_obj_t* button, const char* text) {
    if (button == nullptr) return;
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label != nullptr) product_ui::SetText(label, text);
}

void UpdateSettingsButtons(HarnessUi* ui) {
    char text[80];
    std::snprintf(text, sizeof(text), "模型\n%s", kModels[ui->model_index[ui->session_index]]);
    SetButtonText(ui->model_button, text);
    std::snprintf(text, sizeof(text), "授权级别\n%s",
                  kAuthorizationLevels[ui->authorization_index[ui->session_index]]);
    SetButtonText(ui->authorization_button, text);
}

void RenderPicker(HarnessUi* ui) {
    if (ui->picker == Picker::None) return;
    const bool choosing_model = ui->picker == Picker::Model;
    product_ui::SetText(ui->picker_title, choosing_model ? "选择模型" : "选择授权级别");
    product_ui::SetText(ui->picker_hint, choosing_model
        ? "示意选项 · 实际列表由 Harness 提供"
        : "示意档位 · 实际权限范围由 Harness 提供");

    const int selected_index = choosing_model
        ? ui->model_index[ui->session_index]
        : ui->authorization_index[ui->session_index];
    for (int i = 0; i < 3; ++i) {
        const char* option = choosing_model ? kModels[i] : kAuthorizationLevels[i];
        SetButtonText(ui->picker_options[i], option);
        lv_obj_set_style_bg_color(ui->picker_options[i],
                                  i == selected_index ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(ui->picker_options[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(ui->picker_options[i],
                                    i == selected_index ? lv_color_white() : lv_color_black(), 0);
    }
}
void Render(HarnessUi* ui) {
    if (ui->view != View::Chat) return;
    const auto voice = harness_voice::Snapshot();
    const bool preparing = voice.phase == harness_voice::Phase::Preparing;
    const bool recording = voice.phase == harness_voice::Phase::Recording;
    product_ui::SetText(ui->session_title, kSessions[ui->session_index].name);
    const char* status = preparing ? "接收（准备录音）"
        : recording ? "接收（录音中）" : harness::ConnectionStatusLabel();
    product_ui::SetText(ui->status, status);

    char page[24];
    std::snprintf(page, sizeof(page), "%d / %d", ui->page_index + 1, kPageCount);
    product_ui::SetText(ui->page_indicator, page);

    if (preparing) {
        product_ui::SetText(ui->message, "正在准备麦克风，请继续按住 BOOT。");
    } else if (recording) {
        product_ui::SetText(ui->message, "正在录音，松开 BOOT 结束。\n\n最长可录制 30 秒。");
    } else if (voice.phase == harness_voice::Phase::Complete) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "本次录音 %.1f 秒。\n\n录音上传功能待接入，尚未发送。",
                      voice.duration_ms / 1000.0);
        product_ui::SetText(ui->message, text);
    } else if (voice.phase == harness_voice::Phase::Error) {
        product_ui::SetText(ui->message, "录音未成功，请稍后重试。");
    } else {
        product_ui::SetText(ui->message, kSessions[ui->session_index].pages[ui->page_index]);
    }

    UpdateSettingsButtons(ui);
    if (ui->picker != Picker::None) RenderPicker(ui);
}

void ShowSessions(HarnessUi* ui) {
    ui->view = View::Sessions;
    ui->picker = Picker::None;
    lv_obj_add_flag(ui->chat_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui->sessions_panel, LV_OBJ_FLAG_HIDDEN);
}

void EnterChat(HarnessUi* ui, int session_index) {
    ui->session_index = session_index;
    ui->page_index = 0;
    ui->view = View::Chat;
    ui->picker = Picker::None;
    lv_obj_add_flag(ui->picker_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->sessions_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui->chat_panel, LV_OBJ_FLAG_HIDDEN);
    Render(ui);
}

void OnSessionSelected(lv_event_t* event) {
    auto* ui = ActiveUi();
    if (ui == nullptr) return;
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(event)));
    if (index >= 0 && index < kRecentSessionCount) EnterChat(ui, index);
}

void OnModelPicker(lv_event_t*) {
    auto* ui = ActiveUi();
    if (ui == nullptr || ui->view != View::Chat) return;
    ui->picker = Picker::Model;
    lv_obj_clear_flag(ui->picker_panel, LV_OBJ_FLAG_HIDDEN);
    RenderPicker(ui);
}
void OnAuthorizationPicker(lv_event_t*) {
    auto* ui = ActiveUi();
    if (ui == nullptr || ui->view != View::Chat) return;
    ui->picker = Picker::Authorization;
    lv_obj_clear_flag(ui->picker_panel, LV_OBJ_FLAG_HIDDEN);
    RenderPicker(ui);
}
void OnPickerCancel(lv_event_t*) {
    auto* ui = ActiveUi();
    if (ui == nullptr) return;
    ui->picker = Picker::None;
    lv_obj_add_flag(ui->picker_panel, LV_OBJ_FLAG_HIDDEN);
}

void SelectPickerOption(int index) {
    auto* ui = ActiveUi();
    if (ui == nullptr || ui->picker == Picker::None) return;
    if (ui->picker == Picker::Model) ui->model_index[ui->session_index] = index;
    else ui->authorization_index[ui->session_index] = index;
    ui->picker = Picker::None;
    lv_obj_add_flag(ui->picker_panel, LV_OBJ_FLAG_HIDDEN);
    Render(ui);
}
void OnPickerOption0(lv_event_t*) { SelectPickerOption(0); }
void OnPickerOption1(lv_event_t*) { SelectPickerOption(1); }
void OnPickerOption2(lv_event_t*) { SelectPickerOption(2); }

void ApplyAction(void* argument) {
    if (!HarnessScreen::IsActive() || StandbyScreen::IsActive()) return;
    auto* ui = ActiveUi();
    if (ui == nullptr || ui->view != View::Chat) return;

    const Action action = static_cast<Action>(reinterpret_cast<intptr_t>(argument));
    const auto voice = harness_voice::Snapshot();
    if (voice.phase == harness_voice::Phase::Preparing ||
        voice.phase == harness_voice::Phase::Recording ||
        ui->picker != Picker::None) return;

    if (action == Action::Back) {
        ShowSessions(ui);
    } else if (action == Action::Previous && ui->page_index > 0) {
        --ui->page_index;
        Render(ui);
    } else if (action == Action::Next && ui->page_index + 1 < kPageCount) {
        ++ui->page_index;
        Render(ui);
    }
}
void RequestAction(Action action) {
    ScreenLvAsync(ApplyAction, reinterpret_cast<void*>(static_cast<intptr_t>(action)));
}
void OnBack(lv_event_t*) { RequestAction(Action::Back); }

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

lv_obj_t* CreateSessionRow(HarnessUi* ui, int index, int y, int width) {
    lv_obj_t* row = product_ui::Box(ui->sessions_panel, 0, y, width, 112, 1);
    lv_obj_set_style_radius(row, 7, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, OnSessionSelected, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    HapticAttachClick(row);

    product_ui::Label(row, 14, 9, width - 28, kSessions[index].name,
                      product_ui::HeadingFont());
    product_ui::Label(row, 14, 49, width - 28, kSessions[index].preview);
    lv_obj_t* updated = product_ui::Label(row, 14, 80, width - 28, kSessions[index].updated);
    lv_obj_set_style_text_color(updated, lv_color_make(90, 90, 90), 0);
    return row;
}

void CreatePicker(HarnessUi* ui, int width, int height) {
    const int panel_width = width - 32;
    const int panel_height = 382;
    const int panel_y = (height - panel_height) / 2;
    ui->picker_panel = product_ui::Box(ui->chat_panel, 16, panel_y,
                                       panel_width, panel_height, 2);
    lv_obj_set_style_radius(ui->picker_panel, 8, 0);
    ui->picker_title = product_ui::Label(ui->picker_panel, 14, 12,
                                         panel_width - 28, "选择模型",
                                         product_ui::HeadingFont());
    ui->picker_hint = product_ui::Label(ui->picker_panel, 14, 52,
                                        panel_width - 28, "");
    for (int i = 0; i < 3; ++i) {
        lv_event_cb_t callback = i == 0 ? OnPickerOption0
            : i == 1 ? OnPickerOption1 : OnPickerOption2;
        ui->picker_options[i] = product_ui::Button(
            ui->picker_panel, 14, 91 + i * 57, panel_width - 28, 48,
            "", callback);
    }
    product_ui::Button(ui->picker_panel, 14, panel_height - 58,
                       panel_width - 28, 44, "取消", OnPickerCancel);
    lv_obj_add_flag(ui->picker_panel, LV_OBJ_FLAG_HIDDEN);
}

void BeginRecordingAsync(void*) {
    if (!HarnessScreen::IsActive() || StandbyScreen::IsActive() || !BootKey_IsHeld()) return;
    auto* ui = ActiveUi();
    if (ui == nullptr) return;
    if (ui->view == View::Sessions) EnterChat(ui, kCasualSessionIndex);
    ui->picker = Picker::None;
    lv_obj_add_flag(ui->picker_panel, LV_OBJ_FLAG_HIDDEN);
    harness_voice::Begin();
    Render(ui);
}

bool OnBootClick() { return true; }
bool OnBootLongPress() {
    ScreenLvAsync(BeginRecordingAsync);
    return true;
}
bool OnBootPressUp() { harness_voice::End(); return true; }

void OnTimer(lv_timer_t* timer) {
    auto* ui = static_cast<HarnessUi*>(lv_timer_get_user_data(timer));
    if (lv_screen_active() == ui->screen && !StandbyScreen::IsActive()) Render(ui);
}
void OnUnload(lv_event_t*) { harness_voice::End(); }
void OnDelete(lv_event_t* event) {
    auto* ui = static_cast<HarnessUi*>(lv_event_get_user_data(event));
    if (ui->timer != nullptr) lv_timer_delete(ui->timer);
    delete ui;
}
void OpenAsync(void* argument) {
    if (StandbyScreen::IsActive()) return;
    const bool record_if_held = argument != nullptr;
    if (!HarnessScreen::IsActive()) ScreenNavigateTo(HarnessScreen::Create);
    if (record_if_held) BeginRecordingAsync(nullptr);
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
    ESP_LOGI("HarnessUI", "create session selector and assistant chat");
    auto* ui = new HarnessUi;
    ui->screen = product_ui::Screen();
    lv_obj_set_user_data(ui->screen, ui);

    const EpdStatusBar status = ScreenCreateStatusBar(ui->screen);
    const int width = LV_HOR_RES - 32;
    const int top = status.height + 12;
    const int height = LV_VER_RES - top;
    lv_obj_t* body = product_ui::Box(ui->screen, 16, top, width, height);

    ui->sessions_panel = product_ui::Box(body, 0, 0, width, height);
    product_ui::Label(ui->sessions_panel, 0, 0, width - 100, "最近活动",
                      product_ui::HeadingFont());
    lv_obj_t* demo_label = product_ui::Label(ui->sessions_panel, width - 92, 8, 92, "示例");
    lv_obj_set_style_text_align(demo_label, LV_TEXT_ALIGN_RIGHT, 0);
    product_ui::Label(ui->sessions_panel, 0, 40, width, "选择会话继续");
    for (int i = 0; i < kRecentSessionCount; ++i) {
        CreateSessionRow(ui, i, 76 + i * 124, width);
    }

    ui->chat_panel = product_ui::Box(body, 0, 0, width, height);
    lv_obj_add_flag(ui->chat_panel, LV_OBJ_FLAG_HIDDEN);
    product_ui::Button(ui->chat_panel, 0, 0, 74, 46, "返回", OnBack);
    ui->session_title = product_ui::Label(ui->chat_panel, 88, 0, width - 100,
                                          "", product_ui::HeadingFont());
    lv_obj_set_height(ui->session_title, 38);
    lv_label_set_long_mode(ui->session_title, LV_LABEL_LONG_DOT);
    ui->status = product_ui::Label(ui->chat_panel, 88, 42, width - 176,
                                   "接收（未连接）");
    ui->page_indicator = product_ui::Label(ui->chat_panel, width - 82, 42, 82, "1 / 2");
    lv_obj_set_style_text_align(ui->page_indicator, LV_TEXT_ALIGN_RIGHT, 0);

    const int controls_height = 76;
    const int control_gap = 10;
    const int button_width = (width - control_gap) / 2;
    const int controls_y = height - controls_height;
    ui->message = product_ui::Label(ui->chat_panel, 0, 82, width,
                                    "请选择一个 Session。");
    lv_obj_set_height(ui->message, controls_y - 98);
    ui->model_button = product_ui::Button(ui->chat_panel, 0, controls_y,
                                          button_width, controls_height, "",
                                          OnModelPicker);
    ui->authorization_button = product_ui::Button(
        ui->chat_panel, button_width + control_gap, controls_y,
        button_width, controls_height, "", OnAuthorizationPicker);

    CreatePicker(ui, width, height);
    ui->timer = lv_timer_create(OnTimer, 500, ui);
    lv_obj_add_event_cb(ui->screen, OnUnload, LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_add_event_cb(ui->screen, OnDelete, LV_EVENT_DELETE, ui);
    ScreenSetIsHome(false);
    VkKey_AttachScreen(ui->screen, kScreenId,
                       VkKeyScreenDesc{HarnessScreen::Create, OnKey, OnBootClick,
                                       OnBootLongPress, nullptr, OnBootPressUp});
    ShowSessions(ui);
    return ui->screen;
}
