#include "harness_screen.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#include "boot_key_handler.h"
#include "chat_reader.h"
#include "device_time.h"
#include "approval_panel.h"

#include <algorithm>
#include "harness/service.h"
#include "harness_voice.h"
#include "haptic_feedback.h"
#include "product_ui.h"
#include "ptt_wave.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

namespace {
using namespace harness;
constexpr const char* kScreenId = "harness";
constexpr size_t kMaxPageCursors = 32;
constexpr size_t kSessionsPerScreen = 5;
constexpr int kChatBodyTop = 82;
constexpr int kChatLineSpace = 6;
std::atomic<uint32_t> s_boot_release{0};

enum class View { Sessions, Chat };
enum class Picker { None, Model, Authorization };

template <typename Item>
struct Pager {
    CatalogPage<Item> page;
    std::vector<std::string> cursors{""};
    size_t index = 0;
    size_t target = 0;
    std::shared_ptr<CatalogCall> call;
    bool loaded = false;
    CatalogError error = CatalogError::None;

    bool CanNext() const {
        if (page.next_cursor.empty() || index + 1 >= kMaxPageCursors) return false;
        for (size_t i = 0; i <= index; ++i) {
            if (cursors[i] == page.next_cursor) return false;
        }
        return true;
    }

    void Load(CatalogOperation operation, int direction = 0) {
        if (call || !ServiceReady() ||
            (direction < 0 && index == 0) || (direction > 0 && !CanNext())) return;
        target = direction < 0 ? index - 1 : direction > 0 ? index + 1 : index;
        if (direction > 0) {
            cursors.resize(index + 1);
            cursors.push_back(page.next_cursor);
        }
        RequestTarget(operation);
    }

    void Retry(CatalogOperation operation) {
        if (!call && ServiceReady() && error != CatalogError::None) RequestTarget(operation);
    }

    void Take() {
        if (!call) return;
        auto reply = call->Take();
        if (!reply) return;
        if (auto* value = std::get_if<CatalogPage<Item>>(&reply->data)) {
            page = std::move(*value);
            index = target;
            loaded = true;
        }
        error = reply->error;
        call.reset();
    }

private:
    void RequestTarget(CatalogOperation operation) {
        error = CatalogError::None;
        call = SubmitCatalog({operation, {}, cursors[target], {}, 0});
    }
};

struct Row {
    lv_obj_t* box = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* updated = nullptr;
};

struct TranscriptRow {
    lv_obj_t* body = nullptr;
    lv_obj_t* role = nullptr;
};

struct HarnessUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* ptt_wave = nullptr;
    lv_obj_t* clock_label = nullptr;
    lv_obj_t* notification_label = nullptr;
    lv_timer_t* ptt_wave_timer = nullptr;
    uint32_t ptt_wave_frame = 0;
    lv_obj_t* sessions_panel = nullptr;
    lv_obj_t* sessions_status = nullptr;
    Row rows[kSessionsPerScreen];
    lv_obj_t* sessions_previous = nullptr;
    lv_obj_t* sessions_next = nullptr;
    lv_obj_t* sessions_retry = nullptr;
    lv_obj_t* chat_panel = nullptr;
    lv_obj_t* session_title = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* message = nullptr;
    lv_obj_t* transcript = nullptr;
    std::vector<TranscriptRow> transcript_rows;
    const lv_font_t* body_font = nullptr;
    const lv_font_t* role_font = nullptr;
    int chat_width = 0;
    int chat_bottom = 0;
    lv_obj_t* message_status = nullptr;
    lv_obj_t* task_status = nullptr;
    lv_obj_t* task_progress = nullptr;
    lv_obj_t* cancel_task = nullptr;
    lv_obj_t* cancel_confirm = nullptr;
    std::string cancel_target;
    std::string task_progress_text;
    std::string task_progress_run;
    RunState task_progress_state = RunState::Queued;
    uint32_t last_progress_at = 0;
    lv_obj_t* latest_button = nullptr;
    ChatReader history;
    ApprovalPanel approval_panel;
    lv_obj_t* chat_approval = nullptr;
    lv_obj_t* list_approval = nullptr;
    std::string last_auto_approval;
    const char* approval_notice = nullptr;
    uint32_t approval_notice_at = 0;
    lv_obj_t* retry_recording = nullptr;
    lv_obj_t* discard_recording = nullptr;
    lv_obj_t* discard_confirm = nullptr;
    bool confirming_discard = false;
    bool upload_conflict_seen = false;
    lv_obj_t* session_retry = nullptr;
    lv_obj_t* model_button = nullptr;
    lv_obj_t* authorization_button = nullptr;
    lv_obj_t* picker_panel = nullptr;
    lv_obj_t* picker_title = nullptr;
    lv_obj_t* picker_status = nullptr;
    lv_obj_t* picker_options[kDirectoryPageSize] = {};
    lv_obj_t* options_previous = nullptr;
    lv_obj_t* options_next = nullptr;
    lv_obj_t* options_retry = nullptr;
    lv_timer_t* timer = nullptr;
    View view = View::Sessions;
    Picker picker = Picker::None;
    std::vector<Session> sessions;
    std::shared_ptr<CatalogCall> sessions_call;
    size_t session_page = 0;
    CatalogError sessions_error = CatalogError::None;
    Pager<DirectoryOption> options;
    std::optional<Session> session;
    std::shared_ptr<CatalogCall> session_call;
    std::string target_id;
    std::string target_title;
    const char* notice = nullptr;
    CatalogError session_error = CatalogError::None;
    uint32_t seen_revision = 0;
    uint32_t boot_release = 0;
    bool refresh_sessions = true;
    bool refresh_session = false;
    bool casual = false;
    bool saving = false;
    bool recovering_settings = false;
    bool record_after_load = false;
    bool voice_used = false;
};

HarnessUi* ActiveUi() {
    if (!HarnessScreen::IsActive()) return nullptr;
    auto* screen = lv_screen_active();
    return screen ? static_cast<HarnessUi*>(lv_obj_get_user_data(screen)) : nullptr;
}

void Show(lv_obj_t* object, bool visible) {
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void Enable(lv_obj_t* object, bool enabled) {
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

void SetButton(lv_obj_t* button, const std::string& text) {
    product_ui::SetText(lv_obj_get_child(button, 0), text.c_str());
}

void ClipLabel(lv_obj_t* label, int height) {
    lv_obj_set_height(label, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

const char* ListStatus(bool ready, bool pending, CatalogError error, bool empty) {
    if (!ready) return "等待 Harness 连接";
    if (pending && empty) return "正在读取…";
    if (error != CatalogError::None) return "读取失败";
    return empty ? "暂无可显示的项目" : "";
}

CatalogOperation DirectoryOperation(Picker picker) {
    return picker == Picker::Model ? CatalogOperation::Models : CatalogOperation::Authorizations;
}

void RenderSessions(HarnessUi* ui, bool ready) {
    const size_t first = ui->session_page * kSessionsPerScreen;
    product_ui::SetText(ui->sessions_status, ui->approval_notice ? ui->approval_notice :
                        ListStatus(ready, !!ui->sessions_call, ui->sessions_error, ui->sessions.empty()));
    for (size_t i = 0; i < kSessionsPerScreen; ++i) {
        const bool visible = first + i < ui->sessions.size();
        auto& row = ui->rows[i];
        Show(row.box, visible);
        Enable(row.box, ready && !ui->sessions_call);
        if (!visible) continue;
        const auto& item = ui->sessions[first + i];
        product_ui::SetText(row.title, item.title.c_str());
        const auto time = device_time::FormatBeijing(item.updated_at);
        product_ui::SetText(row.updated, time.c_str());
    }
    Show(ui->sessions_previous, ui->session_page > 0);
    Show(ui->sessions_next, first + kSessionsPerScreen < ui->sessions.size());
    Show(ui->sessions_retry, ready && !ui->sessions_call && ui->sessions_error != CatalogError::None);
}

void RenderPicker(HarnessUi* ui, bool ready) {
    const auto& options = ui->options;
    const bool model = ui->picker == Picker::Model;
    const auto& selected = model ? ui->session->model_id : ui->session->authorization_id;
    product_ui::SetText(ui->picker_title, model ? "选择模型" : "选择授权级别");
    product_ui::SetText(ui->picker_status,
                        ListStatus(ready, !!options.call, options.error, options.page.items.empty()));
    for (int i = 0; i < kDirectoryPageSize; ++i) {
        auto* button = ui->picker_options[i];
        const bool visible = static_cast<size_t>(i) < options.page.items.size();
        Show(button, visible);
        if (!visible) continue;
        const auto& option = options.page.items[i];
        SetButton(button, option.available ? option.name : "不可用 · " + option.name);
        const bool active = option.id == selected;
        const auto background = active ? lv_color_black() : lv_color_white();
        if (!lv_color_eq(lv_obj_get_style_bg_color(button, LV_PART_MAIN), background)) {
            lv_obj_set_style_bg_color(button, background, 0);
            lv_obj_set_style_text_color(button, active ? lv_color_white() : lv_color_black(), 0);
        }
        Enable(button, ready && option.available && !options.call && !ui->session_call);
    }
    Show(ui->options_previous, options.index > 0);
    Show(ui->options_next, options.CanNext());
    Enable(ui->options_previous, ready && !options.call);
    Enable(ui->options_next, ready && !options.call);
    Show(ui->options_retry, ready && !options.call && options.error != CatalogError::None);
}

bool IsRecording(const HarnessUi* ui) {
    if (!ui->voice_used) return false;
    const auto phase = harness_voice::Snapshot().phase;
    return phase == harness_voice::Phase::Preparing || phase == harness_voice::Phase::Recording;
}

const char* RecordingNotice(const UploadStatus& recording, bool current) {
    switch (recording.phase) {
        case UploadPhase::Queued:
        case UploadPhase::Sending:
            return current ? "正在发送录音…" : "另一会话正在发送录音";
        case UploadPhase::WaitingNetwork:
            return "录音已保留，连接恢复后确认";
        case UploadPhase::RetryRequired:
            if (recording.error == "session_busy") return "原会话忙，录音已保留";
            if (recording.error == "settings_conflict") return "原会话设置已变更，请放弃后重录";
            if (!recording.Retryable()) return "录音无法发送，请放弃后重录";
            return "发送未确认，录音已保留";
        case UploadPhase::Accepted:
            if (!current) return "";
            if (recording.error == "asr_no_speech") return "录音已接受，未检测到语音";
            if (recording.error == "asr_failed") return "录音已接受，转写失败";
            return "录音已接受";
        default:
            return "";
    }
}

const char* RunLabel(const RunView& task, bool ready) {
    if (task.run.id.empty()) return task.cancel_error == "resource_not_found" ? "任务已无法查询" : "";
    if (task.run.Finished()) {
        if (task.run.state == RunState::Completed) return "";
        if (task.run.state == RunState::Cancelled) return "任务已取消";
        if (task.run.error == "asr_no_speech") return "未检测到语音";
        if (task.run.error == "asr_failed") return "录音转写失败";
        return "任务失败";
    }
    if (!ready) return "离线，等待同步任务";
    if (task.cancelling) return task.confirming ? "已提交取消，等待任务停止" : "正在提交取消…";
    if (task.confirming) return "正在核对任务状态…";
    switch (task.run.state) {
        case RunState::Queued: return "任务排队中";
        case RunState::Transcribing: return "正在转写";
        case RunState::Running: return "任务运行中";
        case RunState::WaitingApproval: return "任务等待审批";
        case RunState::Cancelling: return "正在取消，等待任务停止";
        default: return "";
    }
}

bool CanCancel(const RunView& task, bool ready) {
    return ready && !task.run.id.empty() && !task.run.Finished() &&
        task.run.state != RunState::Cancelling && !task.confirming && !task.cancelling &&
        task.cancel_error != "cancel_not_supported";
}

void RenderTask(HarnessUi* ui, const RunView& task, bool ready, bool recording) {
    if (!ui->cancel_target.empty() && (!CanCancel(task, ready) || task.run.id != ui->cancel_target)) {
        ui->cancel_target.clear();
    }
    Show(ui->cancel_confirm, !ui->cancel_target.empty());
    Show(ui->cancel_task, CanCancel(task, ready) && !recording && !ui->confirming_discard &&
         ui->picker == Picker::None && ui->cancel_target.empty());
    product_ui::SetText(ui->task_status, RunLabel(task, ready));

    std::string progress;
    if (task.run.state != RunState::Completed) {
        if (task.run.progress != "任务已完成" && task.run.progress != "执行完成") {
            progress = task.run.progress;
        }
        if (task.cancel_error == "cancel_not_supported") progress = "服务端不支持取消该任务";
        else if (task.cancel_error == "confirmation_unknown") progress = "取消结果未确认";
        else if (!task.cancel_error.empty() && task.cancel_error != "resource_not_found")
            progress = "取消请求未完成";
    }
    const auto tick = lv_tick_get();
    if (ui->task_progress_run != task.run.id || ui->task_progress_state != task.run.state ||
        !task.cancel_error.empty() || lv_tick_elaps(ui->last_progress_at) >= 1000) {
        ui->task_progress_run = task.run.id;
        ui->task_progress_state = task.run.state;
        if (ui->task_progress_text != progress) {
            ui->task_progress_text = std::move(progress);
            ui->last_progress_at = tick;
        }
    }
    product_ui::SetText(ui->task_progress, ui->task_progress_text.c_str());
}

int LayoutChatHeader(HarnessUi* ui) {
    int top = kChatBodyTop;
    const char* notice = lv_label_get_text(ui->message_status);
    Show(ui->message_status, *notice);
    if (*notice) {
        lv_point_t size;
        lv_text_get_size(&size, notice, ui->body_font, 0, 8, ui->chat_width, LV_TEXT_FLAG_NONE);
        top += size.y + 6;
    }

    const auto place_row = [&top](lv_obj_t* label, lv_obj_t* button) {
        const bool has_label = *lv_label_get_text(label);
        const bool has_button = !lv_obj_has_flag(button, LV_OBJ_FLAG_HIDDEN);
        Show(label, has_label);
        if (has_label) lv_obj_set_y(label, top + (has_button ? 4 : 0));
        if (has_button) lv_obj_set_y(button, top);
        if (has_button || has_label) top += has_button ? 44 : 38;
    };
    place_row(ui->task_status, ui->cancel_task);
    place_row(ui->task_progress, ui->chat_approval);
    return top;
}

void RenderTranscript(HarnessUi* ui, bool visible, int top) {
    Show(ui->transcript, visible);
    if (!visible) return;

    const int height = ui->chat_bottom - top;
    lv_obj_set_y(ui->transcript, top);
    lv_obj_set_height(ui->transcript, height);
    ui->history.Resize(height);
    const auto& lines = ui->history.Lines();
    const int baseline = ui->history.Baseline();
    const int row_height = ui->history.RowHeight();
    const int body_offset = baseline - (ui->body_font->line_height - ui->body_font->base_line);
    const int role_offset = baseline - (ui->role_font->line_height - ui->role_font->base_line);

    while (ui->transcript_rows.size() < lines.size()) {
        auto* body = product_ui::Label(ui->transcript, 0, 0, ui->chat_width, "", ui->body_font);
        lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_height(body, ui->body_font->line_height);
        ui->transcript_rows.push_back({body, nullptr});
    }
    for (size_t i = 0; i < ui->transcript_rows.size(); ++i) {
        auto& row = ui->transcript_rows[i];
        const bool in_page = i < lines.size();
        const bool has_role = in_page && !lines[i].role.empty();
        if (has_role && !row.role) {
            row.role = product_ui::Label(ui->transcript, 0, 0, ui->chat_width, "", ui->role_font);
            lv_label_set_long_mode(row.role, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_height(row.role, ui->role_font->line_height);
        }
        if (row.role) Show(row.role, has_role);
        Show(row.body, in_page && !lines[i].text.empty());
        if (!in_page) continue;

        const auto& line = lines[i];
        const int y = static_cast<int>(i) * row_height;
        if (has_role) {
            lv_obj_set_y(row.role, y + role_offset);
            lv_obj_set_width(row.role, line.role_width);
            product_ui::SetText(row.role, line.role.c_str());
        }
        lv_obj_set_pos(row.body, line.role_width, y + body_offset);
        lv_obj_set_width(row.body, ui->chat_width - line.role_width);
        product_ui::SetText(row.body, line.text.c_str());
    }
}

void StopPttWave(HarnessUi* ui) {
    if (!ui->ptt_wave_timer) return;
    lv_timer_delete(ui->ptt_wave_timer);
    ui->ptt_wave_timer = nullptr;
    Show(ui->ptt_wave, false);
    lv_obj_remove_local_style_prop(ui->clock_label, LV_STYLE_TEXT_OPA, LV_PART_MAIN);
}

void UpdatePttWaveFrame(HarnessUi* ui) {
    const bool show = lv_obj_has_flag(ui->notification_label, LV_OBJ_FLAG_HIDDEN);
    if (show == lv_obj_has_flag(ui->ptt_wave, LV_OBJ_FLAG_HIDDEN)) Show(ui->ptt_wave, show);
    if (show) DrawPttWaveFrame(ui->ptt_wave, ui->ptt_wave_frame++);
}

void UpdatePttWave(HarnessUi* ui, bool show) {
    if (!show) {
        StopPttWave(ui);
        return;
    }
    if (ui->ptt_wave_timer) return;
    ui->ptt_wave_timer = lv_timer_create([](lv_timer_t* timer) {
        auto* ui = static_cast<HarnessUi*>(lv_timer_get_user_data(timer));
        if (!BootKey_IsHeld() || StandbyScreen::IsActive() || lv_screen_active() != ui->screen) {
            StopPttWave(ui);
            return;
        }
        UpdatePttWaveFrame(ui);
    }, kPttWaveFrameMs, ui);
    if (!ui->ptt_wave_timer) return;

    ui->ptt_wave_frame = 0;
    // The shared status bar keeps updating its clock and managing notifications.
    lv_obj_set_style_text_opa(ui->clock_label, LV_OPA_TRANSP, LV_PART_MAIN);
    UpdatePttWaveFrame(ui);
}

void RenderChat(HarnessUi* ui, bool ready) {
    const auto voice = ui->voice_used ? harness_voice::Snapshot() : harness_voice::State{};
    product_ui::SetText(ui->session_title,
                        (ui->session ? ui->session->title : ui->target_title).c_str());
    const bool preparing = voice.phase == harness_voice::Phase::Preparing;
    const bool recording = voice.phase == harness_voice::Phase::Recording;
    UpdatePttWave(ui, recording && BootKey_IsHeld());
    const auto task = ui->session ? TrackedRun(ui->session->id) : RunView{};
    RenderTask(ui, task, ready, preparing || recording);
    product_ui::SetText(ui->status, preparing ? "接收（准备录音）"
        : recording ? "接收（录音中）" : ConnectionStatusLabel());

    const char* text = nullptr;
    if (preparing) text = "正在准备麦克风，请继续按住 BOOT。";
    else if (recording) text = "正在录音，松开 BOOT 发送。";
    else if (voice.phase == harness_voice::Phase::TooShort) text = "录音太短，请长按 BOOT 重录。";
    else if (voice.phase == harness_voice::Phase::Error) text = "录音未成功，请稍后重试。";
    else if (!ui->session && !ready) text = "等待 Harness 连接";
    else if (ui->session_error != CatalogError::None && !ui->session) text = "会话读取失败";
    else if (!ui->session) text = "正在读取会话…";
    const char* history_status = ui->approval_notice ? ui->approval_notice :
        ui->saving ? "正在保存会话设置…"
        : ui->session_error != CatalogError::None && ui->session ? "会话设置读取失败"
        : ui->notice ? ui->notice : ui->session ? ui->history.Status() : "";
    const auto upload = RecordingStatus();
    const bool current = ui->session && upload.session_id == ui->session->id;
    const bool thinking = ready && current && upload.phase == UploadPhase::Accepted &&
        upload.error.empty() && !upload.transcript_id.empty() && (task.run.id.empty() ||
        (task.run.id == upload.run_id && (task.run.state == RunState::Queued ||
                                         task.run.state == RunState::Transcribing ||
                                         task.run.state == RunState::Running)));
    ui->history.SetThinkingFor(thinking ? upload.transcript_id : std::string{});
    const char* upload_notice = RecordingNotice(upload, current);
    const bool show_upload = *upload_notice && (upload.Pending() ||
        (current && (!upload.error.empty() || (ui->voice_used && upload.transcript_id.empty() && !*history_status))));
    product_ui::SetText(ui->message_status, show_upload && !ui->approval_notice ? upload_notice : history_status);
    const int message_top = LayoutChatHeader(ui);
    Show(ui->message, text != nullptr);
    if (text) {
        lv_obj_set_y(ui->message, message_top);
        product_ui::SetText(ui->message, text);
    }
    RenderTranscript(ui, text == nullptr, message_top);
    lv_obj_set_y(ui->session_retry, message_top + 54);
    if (!upload.HasActions()) ui->confirming_discard = false;
    Show(ui->discard_confirm, ui->confirming_discard);
    const bool upload_actions = upload.HasActions() && ui->picker == Picker::None;
    Enable(ui->discard_recording, !ui->confirming_discard);
    Show(ui->model_button, !upload_actions);
    Show(ui->authorization_button, !upload_actions);
    Show(ui->retry_recording, upload_actions && upload.Retryable());
    Enable(ui->retry_recording, ready && !ui->confirming_discard);
    Show(ui->discard_recording, upload_actions);
    const char* latest = ui->history.LatestButton();
    Show(ui->latest_button, ui->session && !preparing && !recording &&
         ui->picker == Picker::None && !ui->confirming_discard && ui->cancel_target.empty() && *latest);
    SetButton(ui->latest_button, latest);
    Show(ui->session_retry, ready && ui->session_error != CatalogError::None && !ui->session_call &&
         !ui->confirming_discard);

    const bool configurable = ready && ui->session && !ui->session_call &&
                              ui->session_error == CatalogError::None && !preparing && !recording &&
                              !ui->confirming_discard && ui->cancel_target.empty() && !(current && upload.Pending());
    Enable(ui->model_button, configurable);
    Enable(ui->authorization_button, configurable);
    std::string model_name = ui->session ? ui->session->model_name : "—";
    // Keep DeepSeek's version and tier visible on a narrow button.
    if (model_name.rfind("DeepSeek V", 0) == 0) model_name.erase(0, 9);
    SetButton(ui->model_button, "模型\n" + model_name);
    SetButton(ui->authorization_button,
              ui->session ? "授权级别\n" + ui->session->authorization_name : "授权级别\n—");
    if (ui->picker != Picker::None && ui->session) RenderPicker(ui, ready);
}

void StartRecording(HarnessUi* ui) {
    if (!ui->session || ui->session_call || ui->session_error != CatalogError::None ||
        !ServiceReady() || !BootKey_IsHeld() || ui->confirming_discard || !ui->cancel_target.empty() ||
        ui->approval_panel.IsOpen()) return;
    if (RecordingStatus().Pending()) return;
    const auto task = TrackedRun(ui->session->id);
    if ((!task.run.id.empty() && !task.run.Finished()) ||
        (!ui->session->active_run_id.empty() && task.run.id != ui->session->active_run_id)) {
        ui->notice = "当前会话正在处理任务";
        return;
    }
    ui->picker = Picker::None;
    ui->options = {};
    Show(ui->picker_panel, false);
    ui->history.ShowLatest();
    ui->voice_used = true;
    harness_voice::Begin(ui->session->id, ui->session->settings_version);
}

void LoadSession(HarnessUi* ui) {
    if (ui->session_call || !ServiceReady()) return;
    ui->session_error = CatalogError::None;
    const auto operation = ui->casual && !ui->session ? CatalogOperation::Casual : CatalogOperation::Session;
    ui->session_call = SubmitCatalog({operation, ui->target_id, {}, {}, 0});
    ui->refresh_session = false;
}

void TakeSession(HarnessUi* ui) {
    if (!ui->session_call) return;
    auto reply = ui->session_call->Take();
    if (!reply) return;
    ui->session_call.reset();
    const bool saving = std::exchange(ui->saving, false);
    if (auto* session = std::get_if<Session>(&reply->data)) {
        ui->target_id = session->id;
        ui->history.Bind(session->id);
        ui->session = std::move(*session);
        if (saving) ui->notice = reply->error == CatalogError::Conflict
            ? "设置已在其他端变更，已刷新，请重新选择。" : "设置已保存";
        if (std::exchange(ui->record_after_load, false) && ui->boot_release == s_boot_release.load()) {
            StartRecording(ui);
        }
    } else if (saving) {
        ui->notice = "保存结果未知，正在重新读取设置…";
        ui->refresh_session = true;
        ui->recovering_settings = true;
    } else {
        ui->session_error = reply->error;
        ui->record_after_load = false;
    }
    if (!saving && std::exchange(ui->recovering_settings, false)) {
        ui->notice = ui->session_error == CatalogError::None
            ? "已重新读取服务端设置" : nullptr;
    }
}

ApprovalPtr ChooseApproval(HarnessUi* ui, const std::vector<ApprovalPtr>& pending, bool current_only) {
    if (ui->view == View::Chat && ui->session) {
        for (const auto& item : pending) {
            if (item->session_id == ui->session->id) return item;
        }
    }
    return current_only || pending.empty() ? ApprovalPtr{} : pending.front();
}

void OpenApproval(HarnessUi* ui, ApprovalPtr item) {
    if (!item || IsRecording(ui) || ui->saving || ui->picker != Picker::None ||
        ui->confirming_discard || !ui->cancel_target.empty()) return;
    const auto view = TrackedApproval(item->id);
    if (view.info != item || view.checking || !ServiceReady()) return;
    std::string title = item->session_id;
    if (ui->session && ui->session->id == item->session_id) {
        title = ui->session->title;
    } else {
        for (const auto& listed : ui->sessions) {
            if (listed.id == item->session_id) {
                title = listed.title;
                break;
            }
        }
    }
    if (ui->view == View::Chat && ui->session && ui->session->id == item->session_id)
        ui->last_auto_approval = item->id;
    ui->approval_notice = nullptr;
    ui->approval_panel.Open(std::move(item), title);
}

void UpdateApprovals(HarnessUi* ui, bool ready) {
    if (ui->approval_notice && lv_tick_elaps(ui->approval_notice_at) > 5000) ui->approval_notice = nullptr;
    if (const char* result = ui->approval_panel.Update(ready)) {
        ui->approval_notice = result;
        ui->approval_notice_at = lv_tick_get();
    }
    const auto pending = ready ? PendingApprovals() : std::vector<ApprovalPtr>{};
    const auto current = ChooseApproval(ui, pending, true);
    if (current && !ui->approval_panel.IsOpen() && ui->last_auto_approval != current->id) {
        OpenApproval(ui, current);
        ui->approval_panel.Update(ready);
    }
    const bool reviewing = ui->approval_panel.IsOpen();
    Show(ui->sessions_panel, !reviewing && ui->view == View::Sessions);
    Show(ui->chat_panel, !reviewing && ui->view == View::Chat);
    const bool show = !pending.empty() && !reviewing;
    Show(ui->list_approval, show && ui->view == View::Sessions);
    Show(ui->chat_approval, show && ui->view == View::Chat && !IsRecording(ui) &&
         ui->picker == Picker::None && ui->cancel_target.empty() && !ui->confirming_discard);
    if (show) {
        const std::string count = std::to_string(pending.size());
        SetButton(ui->list_approval, "待审批 " + count);
        SetButton(ui->chat_approval, current ? "查看审批" : "待审批 " + count);
    }
}

void OnApprovalNotice(lv_event_t*) {
    auto* ui = ActiveUi();
    if (!ui || ui->approval_panel.IsOpen() || !ServiceReady()) return;
    OpenApproval(ui, ChooseApproval(ui, PendingApprovals(), false));
    UpdateApprovals(ui, ServiceReady());
}

void Update(HarnessUi* ui) {
    const auto revision = CatalogRevision();
    if (ui->seen_revision != revision) {
        ui->seen_revision = revision;
        ui->refresh_sessions = true;
        ui->refresh_session = ui->view == View::Chat;
    }
    const bool ready = ServiceReady();
    if (ui->view == View::Sessions) {
        if (ui->sessions_call) {
            if (auto reply = ui->sessions_call->Take()) {
                ui->sessions_call.reset();
                ui->sessions_error = reply->error;
                if (auto* list = std::get_if<Sessions>(&reply->data)) {
                    ui->sessions = std::move(list->items);
                    if (ui->session_page * kSessionsPerScreen >= ui->sessions.size()) {
                        ui->session_page = 0;
                    }
                }
            }
        }
        if (ui->refresh_sessions && !ui->sessions_call && ready) {
            ui->refresh_sessions = false;
            ui->sessions_error = CatalogError::None;
            ui->sessions_call = SubmitCatalog({CatalogOperation::Sessions, {}, {}, {}, 0});
        }
        UpdateApprovals(ui, ready);
        RenderSessions(ui, ready);
        return;
    }
    TakeSession(ui);
    const auto upload = RecordingStatus();
    const bool conflict = ui->session && upload.session_id == ui->session->id &&
        upload.phase == UploadPhase::RetryRequired && upload.error == "settings_conflict";
    if (conflict && !ui->upload_conflict_seen) ui->refresh_session = true;
    ui->upload_conflict_seen = conflict;
    ui->history.Update();
    if (ui->refresh_session && !IsRecording(ui)) LoadSession(ui);
    if (ui->picker != Picker::None) {
        ui->options.Take();
        if (!ui->options.loaded && !ui->options.call && ui->options.error == CatalogError::None)
            ui->options.Load(DirectoryOperation(ui->picker));
    }
    UpdateApprovals(ui, ready);
    RenderChat(ui, ready);
}

void ShowSessions(HarnessUi* ui) {
    if (IsRecording(ui) || ui->saving) return;
    ui->session_call.reset();
    ui->record_after_load = false;
    ui->view = View::Sessions;
    StopPttWave(ui);
    ui->history.Bind("");
    ui->approval_panel.Close();
    ui->last_auto_approval.clear();
    ui->approval_notice = nullptr;
    ui->cancel_target.clear();
    ui->confirming_discard = false;
    ui->picker = Picker::None;
    ui->options = {};
    Show(ui->chat_panel, false);
    Show(ui->sessions_panel, true);
    Update(ui);
}

void EnterChat(HarnessUi* ui, const Session* selected) {
    ui->view = View::Chat;
    ui->picker = Picker::None;
    ui->session.reset();
    ui->session_call.reset();
    ui->options = {};
    ui->casual = selected == nullptr;
    ui->target_id = selected ? selected->id : "";
    ui->target_title = selected ? selected->title : "固定闲聊";
    ui->voice_used = false;
    ui->recovering_settings = false;
    ui->notice = nullptr;
    ui->session_error = CatalogError::None;
    ui->refresh_session = true;
    if (ui->sessions_call) ui->refresh_sessions = true;
    ui->sessions_call.reset();
    Show(ui->picker_panel, false);
    Show(ui->sessions_panel, false);
    Show(ui->chat_panel, true);
    Update(ui);
}

void OnSessionSelected(lv_event_t* event) {
    auto* ui = ActiveUi();
    if (!ui || ui->view != View::Sessions || ui->sessions_call || !ServiceReady()) return;
    const auto row = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const size_t index = ui->session_page * kSessionsPerScreen + row;
    if (index < ui->sessions.size()) EnterChat(ui, &ui->sessions[index]);
}

void OpenPicker(Picker picker) {
    auto* ui = ActiveUi();
    if (!ui || !ui->session || ui->session_call || IsRecording(ui) || !ServiceReady() ||
        ui->confirming_discard || !ui->cancel_target.empty()) return;
    ui->picker = picker;
    ui->options = {};
    ui->notice = nullptr;
    ui->voice_used = false;
    Show(ui->picker_panel, true);
    Update(ui);
}
void OnModelPicker(lv_event_t*) { OpenPicker(Picker::Model); }
void OnAuthorizationPicker(lv_event_t*) { OpenPicker(Picker::Authorization); }

void ClosePicker(HarnessUi* ui) {
    ui->picker = Picker::None;
    ui->options = {};
    Show(ui->picker_panel, false);
}
void OnPickerCancel(lv_event_t*) {
    if (auto* ui = ActiveUi()) ClosePicker(ui);
}

void OnOption(lv_event_t* event) {
    auto* ui = ActiveUi();
    if (!ui || !ui->session || ui->session_call || ui->picker == Picker::None ||
        ui->options.call || !ServiceReady()) return;
    auto* button = lv_event_get_target_obj(event);
    const auto index = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button));
    if (index >= ui->options.page.items.size()) return;
    const auto& option = ui->options.page.items[index];
    if (!option.available) return;

    const bool model = ui->picker == Picker::Model;
    const auto& selected = model ? ui->session->model_id : ui->session->authorization_id;
    if (option.id != selected) {
        ui->saving = true;
        ui->session_call = SubmitCatalog({model ? CatalogOperation::SetModel : CatalogOperation::SetAuthorization,
            ui->session->id, {}, option.id, ui->session->settings_version});
    }
    ClosePicker(ui);
    RenderChat(ui, ServiceReady());
}

void ChangeSessionPage(int direction) {
    if (auto* ui = ActiveUi(); ui && ui->view == View::Sessions) {
        if (direction < 0 && ui->session_page > 0) {
            --ui->session_page;
        } else if (direction > 0 && (ui->session_page + 1) * kSessionsPerScreen < ui->sessions.size()) {
            ++ui->session_page;
        }
        RenderSessions(ui, ServiceReady());
    }
}
void ChangeOptionPage(int direction) {
    if (auto* ui = ActiveUi(); ui && ui->picker != Picker::None) {
        ui->options.Load(DirectoryOperation(ui->picker), direction);
        RenderPicker(ui, ServiceReady());
    }
}
void OnSessionsPrevious(lv_event_t*) { ChangeSessionPage(-1); }
void OnSessionsNext(lv_event_t*) { ChangeSessionPage(1); }
void OnSessionsRetry(lv_event_t*) {
    if (auto* ui = ActiveUi(); ui && ui->view == View::Sessions) {
        ui->refresh_sessions = true;
        Update(ui);
    }
}
void OnOptionsPrevious(lv_event_t*) { ChangeOptionPage(-1); }
void OnOptionsNext(lv_event_t*) { ChangeOptionPage(1); }
void OnOptionsRetry(lv_event_t*) {
    if (auto* ui = ActiveUi(); ui && ui->picker != Picker::None) {
        ui->options.Retry(DirectoryOperation(ui->picker));
        RenderPicker(ui, ServiceReady());
    }
}

void OnSessionRetry(lv_event_t*) {
    if (auto* ui = ActiveUi()) {
        ui->notice = nullptr;
        LoadSession(ui);
        RenderChat(ui, ServiceReady());
    }
}
void OnBack(lv_event_t*) {
    if (auto* ui = ActiveUi(); ui && ui->picker == Picker::None) ShowSessions(ui);
}

void OnHistoryKey(void* argument) {
    auto* ui = ActiveUi();
    if (!ui || StandbyScreen::IsActive()) return;
    if (ui->approval_panel.IsOpen()) {
        ui->approval_panel.Turn(argument ? 1 : -1);
        UpdateApprovals(ui, ServiceReady());
        return;
    }
    if (ui->view != View::Chat || ui->picker != Picker::None ||
        IsRecording(ui) || ui->confirming_discard || !ui->cancel_target.empty()) return;
    ui->notice = nullptr;
    ui->voice_used = false;
    ui->history.Turn(argument ? 1 : -1);
    RenderChat(ui, ServiceReady());
}

bool OnKey(const char* key) {
    if (std::strcmp(key, "vk_prev") == 0) {
        ScreenLvAsync(OnHistoryKey);
        return true;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        ScreenLvAsync(OnHistoryKey, reinterpret_cast<void*>(1));
        return true;
    }
    return false;
}

void OnRetryRecording(lv_event_t*) {
    if (auto* ui = ActiveUi()) {
        RequestRecordingAction(UploadAction::Retry);
        RenderChat(ui, ServiceReady());
    }
}
void OnDiscardRecording(lv_event_t*) {
    if (auto* ui = ActiveUi(); ui && ui->cancel_target.empty() && RecordingStatus().HasActions()) {
        ui->confirming_discard = true;
        RenderChat(ui, ServiceReady());
    }
}
void OnDiscardCancel(lv_event_t*) {
    if (auto* ui = ActiveUi()) {
        ui->confirming_discard = false;
        RenderChat(ui, ServiceReady());
    }
}
void OnDiscardConfirm(lv_event_t*) {
    if (auto* ui = ActiveUi()) {
        RequestRecordingAction(UploadAction::Discard);
        ui->confirming_discard = false;
        RenderChat(ui, ServiceReady());
    }
}

void OnCancelTask(lv_event_t*) {
    auto* ui = ActiveUi();
    if (!ui || !ui->session || ui->picker != Picker::None || IsRecording(ui) ||
        ui->confirming_discard) return;
    const auto task = TrackedRun(ui->session->id);
    if (CanCancel(task, ServiceReady())) ui->cancel_target = task.run.id;
    RenderChat(ui, ServiceReady());
}
void OnKeepTask(lv_event_t*) {
    if (auto* ui = ActiveUi()) {
        ui->cancel_target.clear();
        RenderChat(ui, ServiceReady());
    }
}
void OnCancelTaskConfirmed(lv_event_t*) {
    if (auto* ui = ActiveUi(); ui && ui->session && !ui->cancel_target.empty()) {
        RequestRunCancel(ui->session->id, ui->cancel_target);
        ui->cancel_target.clear();
        RenderChat(ui, ServiceReady());
    }
}

void OnLatest(lv_event_t*) {
    auto* ui = ActiveUi();
    if (!ui || ui->view != View::Chat || ui->picker != Picker::None || IsRecording(ui)) return;
    ui->notice = nullptr;
    ui->voice_used = false;
    ui->history.ShowLatest();
    RenderChat(ui, ServiceReady());
}

void CreateSessions(HarnessUi* ui, lv_obj_t* body, int width, int height) {
    ui->sessions_panel = product_ui::Box(body, 0, 0, width, height);
    product_ui::Label(ui->sessions_panel, 0, 0, width - 150, "最近活动", product_ui::HeadingFont());
    ui->list_approval = product_ui::Button(ui->sessions_panel, width - 142, 0, 142, 42,
                                           "待审批", OnApprovalNotice);
    Show(ui->list_approval, false);
    ui->sessions_status = product_ui::Label(ui->sessions_panel, 0, 39, width, "");
    for (size_t i = 0; i < kSessionsPerScreen; ++i) {
        auto& row = ui->rows[i];
        row.box = product_ui::Box(ui->sessions_panel, 0, 75 + i * 106, width, 94, 1);
        lv_obj_set_style_radius(row.box, 7, 0);
        lv_obj_add_flag(row.box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row.box, OnSessionSelected, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        HapticAttachClick(row.box);
        row.title = product_ui::Label(row.box, 14, 9, width - 28, "", product_ui::HeadingFont());
        ClipLabel(row.title, 38);
        row.updated = product_ui::Label(row.box, 14, 52, width - 28, "");
        ClipLabel(row.updated, 30);
    }
    ui->sessions_previous = product_ui::Button(ui->sessions_panel, 0, height - 58, 120, 46, "上一页", OnSessionsPrevious);
    ui->sessions_next = product_ui::Button(ui->sessions_panel, width - 120, height - 58, 120, 46, "下一页", OnSessionsNext);
    ui->sessions_retry = product_ui::Button(ui->sessions_panel, (width - 120) / 2, height - 58, 120, 46, "重试", OnSessionsRetry);
}

void CreatePicker(HarnessUi* ui, int width, int height) {
    const int panel_width = width - 32;
    const int panel_height = 394;
    ui->picker_panel = product_ui::Box(ui->chat_panel, 16, (height - panel_height) / 2,
                                       panel_width, panel_height, 2);
    lv_obj_set_style_radius(ui->picker_panel, 8, 0);
    ui->picker_title = product_ui::Label(ui->picker_panel, 14, 12, panel_width - 28, "",
                                         product_ui::HeadingFont());
    ui->picker_status = product_ui::Label(ui->picker_panel, 14, 52, panel_width - 28, "");
    for (int i = 0; i < kDirectoryPageSize; ++i) {
        auto* button = product_ui::Button(ui->picker_panel, 14, 91 + i * 57, panel_width - 28, 48, "", OnOption);
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        ClipLabel(lv_obj_get_child(button, 0), 32);
        ui->picker_options[i] = button;
    }
    ui->options_previous = product_ui::Button(ui->picker_panel, 14, 266, 112, 48, "上一页", OnOptionsPrevious);
    ui->options_next = product_ui::Button(ui->picker_panel, panel_width - 126, 266, 112, 48, "下一页", OnOptionsNext);
    ui->options_retry = product_ui::Button(ui->picker_panel, (panel_width - 112) / 2, 266, 112, 48, "重试", OnOptionsRetry);
    product_ui::Button(ui->picker_panel, 14, 330, panel_width - 28, 46, "取消", OnPickerCancel);
    Show(ui->picker_panel, false);
}

void BeginRecordingAsync(void*) {
    if (StandbyScreen::IsActive() || !BootKey_IsHeld()) return;
    auto* ui = ActiveUi();
    if (!ui || ui->approval_panel.IsOpen()) return;
    if (ui->view == View::Sessions) {
        ui->record_after_load = ServiceReady();
        ui->boot_release = s_boot_release.load();
        EnterChat(ui, nullptr);
    } else {
        StartRecording(ui);
        RenderChat(ui, ServiceReady());
    }
}

bool OnBootClick() { return true; }
bool OnBootLongPress() { ScreenLvAsync(BeginRecordingAsync); return true; }
bool OnBootPressUp() { ++s_boot_release; harness_voice::End(); return true; }

void OnTimer(lv_timer_t* timer) {
    auto* ui = static_cast<HarnessUi*>(lv_timer_get_user_data(timer));
    if (lv_screen_active() == ui->screen && !StandbyScreen::IsActive()) Update(ui);
}
void OnUnload(lv_event_t* event) {
    StopPttWave(static_cast<HarnessUi*>(lv_event_get_user_data(event)));
    ++s_boot_release;
    harness_voice::End();
}
void OnDelete(lv_event_t* event) {
    auto* ui = static_cast<HarnessUi*>(lv_event_get_user_data(event));
    if (ui->timer) lv_timer_delete(ui->timer);
    if (ui->ptt_wave_timer) lv_timer_delete(ui->ptt_wave_timer);
    delete ui;
}
void OpenAsync(void* argument) {
    if (StandbyScreen::IsActive()) return;
    if (!HarnessScreen::IsActive()) ScreenNavigateTo(HarnessScreen::Create);
    if (argument) BeginRecordingAsync(nullptr);
}
}  // namespace

bool HarnessScreen::IsActive() {
    const char* active = VkKey_ActiveScreen();
    return active && std::strcmp(active, kScreenId) == 0;
}

void HarnessScreen::RequestOpen(bool record_if_held) {
    ScreenLvAsync(OpenAsync, record_if_held ? reinterpret_cast<void*>(1) : nullptr);
}

void HarnessScreen::OnEnterStandby() {
    if (auto* ui = ActiveUi()) StopPttWave(ui);
    ++s_boot_release;
    harness_voice::End();
}

lv_obj_t* HarnessScreen::Create() {
    auto* ui = new HarnessUi;
    ui->screen = product_ui::Screen();
    lv_obj_set_user_data(ui->screen, ui);

    const auto status = ScreenCreateStatusBar(ui->screen);
    ui->clock_label = status.status_label;
    ui->notification_label = status.notification_label;
    ui->ptt_wave = CreatePttWave(ui->screen, status.height);
    const int width = LV_HOR_RES - 32;
    const int top = status.height + 12;
    const int height = LV_VER_RES - top;
    auto* body = product_ui::Box(ui->screen, 16, top, width, height);
    CreateSessions(ui, body, width, height);

    ui->chat_panel = product_ui::Box(body, 0, 0, width, height);
    Show(ui->chat_panel, false);
    product_ui::Button(ui->chat_panel, 0, 0, 74, 46, "返回", OnBack);
    ui->session_title = product_ui::Label(ui->chat_panel, 88, 0, width - 100, "", product_ui::HeadingFont());
    ClipLabel(ui->session_title, 38);
    ui->status = product_ui::Label(ui->chat_panel, 88, 42, width - 222, "");
    ui->latest_button = product_ui::Button(ui->chat_panel, width - 133, 38, 133, 40, "", OnLatest);
    Show(ui->latest_button, false);
    ui->message_status = product_ui::Label(ui->chat_panel, 0, kChatBodyTop, width, "");

    const int button_width = (width - 10) / 2;
    const int controls_y = height - 84;
    ui->task_status = product_ui::Label(ui->chat_panel, 0, 0, width - 138, "");
    ClipLabel(ui->task_status, 34);
    ui->cancel_task = product_ui::Button(ui->chat_panel, width - 122, 0, 122, 40,
                                         "取消任务", OnCancelTask);
    Show(ui->cancel_task, false);
    ui->task_progress = product_ui::Label(ui->chat_panel, 0, 0, width - 150, "");
    ClipLabel(ui->task_progress, 36);
    ui->chat_approval = product_ui::Button(ui->chat_panel, width - 142, 0, 142, 40,
                                          "查看审批", OnApprovalNotice);
    Show(ui->chat_approval, false);
    ui->chat_width = width;
    ui->chat_bottom = controls_y - 12;
    ui->body_font = fontpack_lv_font_ui();
    if (!ui->body_font) ui->body_font = LV_FONT_DEFAULT;
    ui->role_font = fontpack_lv_font_get(30, 4);
    if (!ui->role_font) ui->role_font = ui->body_font;
    const int message_height = ui->chat_bottom - kChatBodyTop;
    ui->transcript = product_ui::Box(ui->chat_panel, 0, kChatBodyTop, width, message_height);
    lv_obj_set_style_bg_opa(ui->transcript, LV_OPA_TRANSP, 0);
    ui->message = product_ui::Label(ui->chat_panel, 0, kChatBodyTop, width, "");
    ui->history.SetLayout(width, message_height, ui->body_font, ui->role_font, kChatLineSpace);
    ui->session_retry = product_ui::Button(ui->chat_panel, 0, 0, 120, 46, "重试", OnSessionRetry);
    ui->model_button = product_ui::Button(ui->chat_panel, 0, controls_y, button_width, 76, "", OnModelPicker);
    ui->authorization_button = product_ui::Button(ui->chat_panel, button_width + 10, controls_y,
                                                button_width, 76, "", OnAuthorizationPicker);
    const lv_font_t* settings_font = fontpack_lv_font_get(20, 2);
    for (auto* button : {ui->model_button, ui->authorization_button}) {
        auto* label = lv_obj_get_child(button, 0);
        if (settings_font) lv_obj_set_style_text_font(label, settings_font, 0);
        lv_obj_set_style_text_line_space(label, 0, 0);
        ClipLabel(label, 70);
    }
    ui->retry_recording = product_ui::Button(ui->chat_panel, 0, controls_y,
                                             button_width, 76, "重试发送", OnRetryRecording);
    ui->discard_recording = product_ui::Button(ui->chat_panel, button_width + 10, controls_y,
                                               button_width, 76, "放弃录音", OnDiscardRecording);
    Show(ui->retry_recording, false);
    Show(ui->discard_recording, false);
    CreatePicker(ui, width, height);
    ui->discard_confirm = product_ui::Box(ui->chat_panel, 16, 190, width - 32, 240, 2);
    product_ui::Label(ui->discard_confirm, 14, 14, width - 60, "放弃本地录音？", product_ui::HeadingFont());
    product_ui::Label(ui->discard_confirm, 14, 66, width - 60, "服务端可能已接受，放弃本地录音不会取消任务。");
    product_ui::Button(ui->discard_confirm, 14, 176, 150, 46, "保留", OnDiscardCancel);
    product_ui::Button(ui->discard_confirm, width - 200, 176, 150, 46, "确认放弃", OnDiscardConfirm);
    Show(ui->discard_confirm, false);

    ui->cancel_confirm = product_ui::Box(ui->chat_panel, 16, 190, width - 32, 240, 2);
    product_ui::Label(ui->cancel_confirm, 14, 14, width - 60, "取消当前任务？", product_ui::HeadingFont());
    product_ui::Label(ui->cancel_confirm, 14, 66, width - 60, "请求停止后续执行，已完成的操作仍然有效。");
    product_ui::Button(ui->cancel_confirm, 14, 176, 150, 46, "继续任务", OnKeepTask);
    product_ui::Button(ui->cancel_confirm, width - 200, 176, 150, 46, "确认取消", OnCancelTaskConfirmed);
    Show(ui->cancel_confirm, false);

    ui->approval_panel.Create(body, width, height);
    ui->timer = lv_timer_create(OnTimer, 150, ui);
    lv_obj_add_event_cb(ui->screen, OnUnload, LV_EVENT_SCREEN_UNLOADED, ui);
    lv_obj_add_event_cb(ui->screen, OnDelete, LV_EVENT_DELETE, ui);
    ScreenSetIsHome(false);
    VkKey_AttachScreen(ui->screen, kScreenId, VkKeyScreenDesc{
        HarnessScreen::Create, OnKey, OnBootClick, OnBootLongPress, nullptr, OnBootPressUp});
    ShowSessions(ui);
    return ui->screen;
}
