#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "lvgl.h"
#include "dashboard_screen/dashboard_screen.h"
#include "harness_screen/harness_screen.h"
#include "settings_screen/settings_screen.h"
#include "standby_screen/standby_screen.h"
#include "screen_common.h"
#include "ptt_wave.h"
#include "vk_key_handler.h"
#include "fontpack_lvgl.h"
#include "device_time.h"
#include "esp_partition.h"
#include "haptic_feedback.h"
#include "display_orient.h"
#include "application.h"
#include "power_policy.h"
#include "harness_screen/harness_voice.h"
#include "harness/service.h"
#include "harness_screen/chat_reader.h"
#include "local_recording_buffer.h"

static std::vector<unsigned char> font_blob;
static std::vector<uint32_t> pixels(480*800,0xffffff);
static const char* active = "none";
static bool on_home = false;
static bool asleep = false;
static bool boot_held = false;
bool BootKey_IsHeld() { return boot_held; }
static std::map<std::string,VkKeyScreenDesc> registry;
extern "C" {
const esp_partition_t* esp_partition_find_first(int,int,const char*) {
    static esp_partition_t part;
    part.size = font_blob.size(); return &part;
}
esp_err_t esp_partition_mmap(const esp_partition_t*,size_t offset,size_t,int,const void** data,esp_partition_mmap_handle_t* handle) {
    *data=font_blob.data()+offset;*handle=1;return 0;
}
void esp_partition_munmap(esp_partition_mmap_handle_t) {}
const char* esp_err_to_name(int) { return "host"; }
void HapticAttachClick(lv_obj_t*) {}
void HapticDetachClick(lv_obj_t*) {}
bool DisplayUiSetOrient(int) { return false; }
}
bool SettingsNetworkTab_IsWifi() { return true; }
bool StandbyScreen::IsActive() { return asleep; }
void ScreenSetIsHome(bool v) { on_home=v; }
bool ScreenIsHome() { return on_home; }
const char* VkKey_ActiveScreen() { return active; }
void VkKey_AttachScreen(lv_obj_t* screen,const char* name,const VkKeyScreenDesc& desc) {
    registry[name]=desc;
    lv_obj_add_event_cb(screen,[](lv_event_t* e){active=static_cast<const char*>(lv_event_get_user_data(e));},LV_EVENT_SCREEN_LOADED,const_cast<char*>(name));
}
void ScreenLoadReplace(lv_obj_t* screen) {
    lv_obj_t* old=lv_screen_active();lv_screen_load(screen);
    if(old!=screen)lv_obj_delete_async(old);
}
void ScreenNavigateTo(lv_obj_t* (*factory)()) { on_home=false;ScreenLoadReplace(factory()); }
void ScreenGoHome() { ScreenLoadReplace(DashboardScreen::Create()); }
bool ScreenLvAsync(void (*cb)(void*),void* data) { return lv_async_call(cb,data)==LV_RESULT_OK; }
EpdStatusBar ScreenCreateStatusBar(lv_obj_t* screen) {
    EpdStatusBar status;status.height=45;
    auto* label=lv_label_create(screen);lv_obj_set_style_text_font(label,fontpack_lv_font_ui(),0);
    lv_label_set_text(label, "Wi-Fi");
    lv_obj_set_pos(label, 16, 8);
    auto* battery = lv_label_create(screen);
    lv_obj_set_style_text_font(battery, fontpack_lv_font_ui(), 0);
    lv_label_set_text(battery, "100%");
    lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -16, 8);
    status.status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status.status_label, fontpack_lv_font_ui(), 0);
    lv_label_set_text(status.status_label, "12:34");
    lv_obj_align(status.status_label, LV_ALIGN_TOP_MID, 0, 8);
    status.notification_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status.notification_label, fontpack_lv_font_ui(), 0);
    lv_label_set_text(status.notification_label, "设备通知");
    lv_obj_align(status.notification_label, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    return status;
}
lv_obj_t* SettingsScreen::Create() {
    auto* screen=lv_obj_create(nullptr);ScreenSetIsHome(false);
    VkKey_AttachScreen(screen,"settings",VkKeyScreenDesc{SettingsScreen::Create});return screen;
}
static void Flush(lv_display_t* display,const lv_area_t* area,uint8_t* data) {
    auto* src=reinterpret_cast<uint32_t*>(data);
    for(int y=area->y1;y<=area->y2;y++)for(int x=area->x1;x<=area->x2;x++)pixels[y*480+x]=*src++;
    lv_display_flush_ready(display);
}
static void Pump() { for(int i=0;i<6;i++){Application::GetInstance().Pump();lv_tick_inc(100);lv_timer_handler();}lv_obj_update_layout(lv_screen_active());lv_refr_now(nullptr); }
static void Save(const char* name) {
    Pump();std::ofstream out(std::string(name)+".ppm",std::ios::binary);out<<"P6\n480 800\n255\n";
    for(uint32_t px:pixels){out.put((px>>16)&255);out.put((px>>8)&255);out.put(px&255);}
}
static lv_obj_t* Find(lv_obj_t* root,const char* text) {
    if (lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if(lv_obj_check_type(root,&lv_label_class)&&std::strcmp(lv_label_get_text(root),text)==0)return root;
    for(unsigned i=0;i<lv_obj_get_child_count(root);i++)if(auto* found=Find(lv_obj_get_child(root,i),text))return found;
    return nullptr;
}
static lv_obj_t* CheckChatLine(const char* speaker, const char* text) {
    auto* root = lv_screen_active();
    auto* role = Find(root, speaker);
    auto* body = Find(root, text);
    assert(role && body && lv_obj_get_parent(role) == lv_obj_get_parent(body));
    const auto* role_font = lv_obj_get_style_text_font(role, LV_PART_MAIN);
    const auto* body_font = lv_obj_get_style_text_font(body, LV_PART_MAIN);
    assert(fontpack_lv_font_bpp(role_font) == 4 && fontpack_lv_font_bpp(body_font) == 2);
    assert(lv_obj_get_x(body) == lv_obj_get_x(role) + lv_obj_get_width(role));
    assert(lv_obj_get_y(role) + role_font->line_height - role_font->base_line ==
           lv_obj_get_y(body) + body_font->line_height - body_font->base_line);
    return lv_obj_get_parent(role);
}

static std::string ChatText(const harness::ChatReader& reader) {
    std::string text;
    for (const auto& line : reader.Lines()) text += line.role + line.text + "\n";
    return text;
}

static void Click(const char* text) {
    auto* label=Find(lv_screen_active(),text);assert(label);
    auto* button=lv_obj_get_parent(label);assert(!lv_obj_has_state(button,LV_STATE_DISABLED));
    lv_obj_send_event(button,LV_EVENT_CLICKED,nullptr);Pump();
}
static int Timers() {int count=0;for(auto* t=lv_timer_get_next(nullptr);t;t=lv_timer_get_next(t))count++;return count;}
static void Validate(lv_obj_t* root) {
    if (lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;
    if(lv_obj_check_type(root,&lv_label_class)) {
        auto* parent=lv_obj_get_parent(root);
        int x=lv_obj_get_x(root),y=lv_obj_get_y(root),w=lv_obj_get_width(root),h=lv_obj_get_height(root);
        if(x<0||y<0||x+w>lv_obj_get_width(parent)||y+h>lv_obj_get_height(parent)) {
            std::fprintf(stderr,"OVERFLOW %s x=%d y=%d w=%d h=%d parent=%dx%d\n",lv_label_get_text(root),x,y,w,h,lv_obj_get_width(parent),lv_obj_get_height(parent));
            std::abort();
        }
    }
    if (HarnessScreen::IsActive() && lv_obj_check_type(root,&lv_label_class) && lv_obj_get_y(root)==58) {
        auto* parent=lv_obj_get_parent(root);
        assert(lv_obj_get_y(root)+lv_obj_get_height(root) <= lv_obj_get_height(parent)-52);
    }
    for(unsigned i=0;i<lv_obj_get_child_count(root);i++)Validate(lv_obj_get_child(root,i));
}
int main() {
    using device_time::FormatBeijing;
    assert(FormatBeijing("2026-11-24T20:30:45.123Z", true) == "2026-11-25 04:30:45");
    assert(FormatBeijing("2026-11-24T23:50:00-05:30") == "2026-11-25 13:20");
    assert(FormatBeijing("2026-11-25T12:30:00+08:00") == "2026-11-25 12:30");
    assert(FormatBeijing("2024-02-29T23:00:00+00:00") == "2024-03-01 07:00");
    assert(FormatBeijing("2026-02-29T12:30:00Z") == "2026-02-29T12:30:00Z");
    const auto sync_time = device_time::ParseHttpDate("Wed, 25 Nov 2026 09:14:03 GMT");
    assert(sync_time && !device_time::ParseHttpDate("invalid"));
    device_time::InitializeBeijingTimezone();
    std::tm local{};
    assert(localtime_r(&*sync_time, &local) && local.tm_hour == 17 && local.tm_min == 14);
    std::ifstream font(PREVIEW_ROOT "/use_font/font.fontpack",std::ios::binary);
    font_blob.assign(std::istreambuf_iterator<char>(font),{});assert(!font_blob.empty());
    lv_init();auto* display=lv_display_create(480,800);
    static std::vector<uint32_t> buffer(480*800);
    lv_display_set_buffers(display,buffer.data(),nullptr,buffer.size()*4,LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display,Flush);
    // Exercise actual bounded mono buffer code, including a reference channel and canaries.
    int16_t memory[] = {123,0,0,0,0,456};
    const int16_t stereo[] = {100,900,-32768,900,300,900,400,900,500,900};
    LocalRecordingBuffer recording;
    recording.Reset(memory+1,4);recording.AppendInterleaved(stereo,10,2);
    assert(recording.full() && recording.size()==4 && recording.peak()==32768);
    int16_t copied[2]{};
    assert(recording.CopyBytes(2, copied, sizeof(copied)) && copied[0]==-32768 && copied[1]==300);
    assert(!recording.CopyBytes(1, copied, sizeof(copied)));
    assert(!recording.CopyBytes(6, copied, sizeof(copied)));
    assert(!recording.CopyBytes(0, copied, 3));
    assert(memory[0]==123 && memory[1]==100 && memory[2]==-32768 && memory[4]==400 && memory[5]==456);
    recording.AppendInterleaved(stereo,10,0);assert(recording.size()==4);

    ScreenLoadReplace(DashboardScreen::Create());Pump();Validate(lv_screen_active());
    Save("dashboard");
    assert(Find(lv_screen_active(),"会话数"));
    assert(Find(lv_screen_active(),"消息数"));
    assert(Find(lv_screen_active(),"总 Token"));
    assert(Find(lv_screen_active(),"活跃天数"));
    assert(Find(lv_screen_active(),"高峰时段"));
    assert(Find(lv_screen_active(),"常用模型"));
    assert(!Find(lv_screen_active(),"token 速率") && !Find(lv_screen_active(),"显存占用"));

    using namespace harness;
    using Op = CatalogOperation;
    Session firmware{"sess_1", "固件开发", "pro", "DeepSeek V4.1-Pro", "ask", "请求批准", "2026-11-24T03:20:00Z", 1};
    Session planning{"sess_2", "项目规划", "flash", "DeepSeek V4.1-Flash", "full", "完全访问", "2026-11-23T03:20:00Z", 1};
    Session casual{"sess_casual", "固定闲聊", "flash", "DeepSeek V4.1-Flash", "ask", "请求批准", "2026-11-23T03:20:00Z", 1};
    Overview overview{53, 5980, 1900000000, 22, "14:00", "DeepSeek V4.1-Pro"};
    bool empty_list = false;
    bool directory_error = false;
    bool empty_authorizations = false;
    unsigned writes = 0;
    bool defer_casual = false;
    unsigned history_requests = 0;
    unsigned session_list_requests = 0;
    std::shared_ptr<CatalogCall> deferred;
    PreviewCatalog() = [&](const CatalogRequest& request, const std::shared_ptr<CatalogCall>& call) {
        switch (request.operation) {
            case Op::Overview:
                if (request.key == "all") call->Complete({CatalogError::None, overview});
                else if (request.key == "30d")
                    call->Complete({CatalogError::None, Overview{28, UINT32_MAX, 1000, 999, "13:00", "DeepSeek V4.1-Pro"}});
                else call->Complete({CatalogError::None, Overview{0, std::nullopt, 0, 0, "", ""}});
                break;
            case Op::Run:
            case Op::Approval: assert(false); break;
            case Op::History: {
                ++history_requests;
                if (request.key != firmware.id) {
                    call->Complete({CatalogError::None, HistoryPage{}});
                } else if (request.cursor == "older/+?") {
                    call->Complete({CatalogError::None, HistoryPage{
                        {{"msg_1", "这是较早的回复。", "2026-11-23T03:20:00Z", 0, true, true}}, {}}});
                } else {
                    assert(request.cursor.empty());
                    call->Complete({CatalogError::None, HistoryPage{
                        {{"msg_2", "这是最近的对话。", "2026-11-24T03:20:00Z", 0, false, true}}, "older/+?"}});
                }
                break;
            }
            case Op::Sessions: {
                ++session_list_requests;
                assert(request.cursor.empty());
                Sessions list;
                if (!empty_list) {
                    list.items.push_back(firmware);
                    for (int i = 2; i <= kRecentSessionLimit; ++i) {
                        auto item = planning;
                        if (i != 6) {
                            item.id = "recent_" + std::to_string(i);
                            item.title = "会话 " + std::to_string(i);
                        }
                        list.items.push_back(std::move(item));
                    }
                    list.next_cursor = "more_from_server";
                }
                call->Complete({CatalogError::None, std::move(list)});
                break;
            }
            case Op::Session:
                call->Complete({CatalogError::None, request.key == firmware.id ? firmware
                    : request.key == planning.id ? planning : casual});
                break;
            case Op::Casual:
                if (defer_casual) deferred = call;
                else call->Complete({CatalogError::None, casual});
                break;
            case Op::Models:
                if (directory_error) { call->Complete({CatalogError::RequestFailed, {}}); break; }
                if (request.cursor.empty()) call->Complete({CatalogError::None, Directory{
                    {{"pro", "DeepSeek V4.1-Pro", true}, {"locked", "中文受限模型", false}}, "models_2"}});
                else call->Complete({CatalogError::None, Directory{{{"flash", "DeepSeek V4.1-Flash", true}}, {}}});
                break;
            case Op::Authorizations:
                call->Complete({CatalogError::None, empty_authorizations ? Directory{}
                    : Directory{{{"ask", "请求批准", true}, {"full", "完全访问", true}}, {}}});
                break;
            case Op::SetModel:
            case Op::SetAuthorization: {
                ++writes;
                assert(request.key == firmware.id);
                if (request.settings_version != firmware.settings_version) {
                    call->Complete({CatalogError::Conflict, firmware});
                    break;
                }
                if (request.operation == Op::SetModel) {
                    assert(request.value == "flash");
                    firmware.model_id = "flash"; firmware.model_name = "DeepSeek V4.1-Flash";
                } else {
                    assert(request.value == "full");
                    firmware.authorization_id = "full"; firmware.authorization_name = "完全访问";
                }
                ++firmware.settings_version;
                call->Complete({CatalogError::None, firmware});
                break;
            }
        }
    };
    assert(Find(lv_screen_active(),"等待 Harness 连接"));
    PreviewReady() = true;
    PreviewConnectionLabel() = "接收（已连接）";
    ++PreviewRevision();
    Pump();Pump();
    assert(Find(lv_screen_active(),"53"));
    assert(Find(lv_screen_active(),"5,980"));
    assert(Find(lv_screen_active(),"1.9B"));
    assert(Find(lv_screen_active(),"22"));
    assert(Find(lv_screen_active(),"14:00"));
    assert(Find(lv_screen_active(),"DeepSeek\nV4.1-Pro"));
    Save("dashboard-sample");Validate(lv_screen_active());
    Click("30天");
    assert(Find(lv_screen_active(),"4,294,967,295"));
    assert(Find(lv_screen_active(),"999"));
    Click("7天");
    assert(Find(lv_screen_active(),"0"));
    assert(Find(lv_screen_active(),"--"));
    Click("全部");
    assert(Find(lv_screen_active(),"5,980"));
    int baseline=Timers();

    const char* dashboard_notice = "待审批 1 · 点击查看";
    assert(!Find(lv_screen_active(), dashboard_notice));
    auto dashboard_approval = std::make_shared<ApprovalInfo>();
    dashboard_approval->id = "approval_dashboard";
    dashboard_approval->session_id = firmware.id;
    dashboard_approval->reason = "核对联调结果。";
    PreviewApproval().info = dashboard_approval;
    Pump();Pump();
    assert(Find(lv_screen_active(), dashboard_notice));
    Save("dashboard-approval");Validate(lv_screen_active());

    PreviewReady() = false;
    Pump();Pump();
    assert(!Find(lv_screen_active(), dashboard_notice));
    PreviewReady() = true;
    Pump();Pump();
    Click(dashboard_notice);
    assert(HarnessScreen::IsActive() && !Find(lv_screen_active(), "审批详情"));
    Click("待审批 1");
    assert(Find(lv_screen_active(), "核对联调结果。"));
    Click("返回");
    PreviewApproval() = {};
    Pump();
    assert(!Find(lv_screen_active(), "待审批 1"));
    ScreenGoHome();Pump();
    assert(!Find(lv_screen_active(), dashboard_notice));

    assert(registry["home"].on_boot_click());Pump();assert(HarnessScreen::IsActive());
    Validate(lv_screen_active());Save("assistant-sessions");
    assert(Find(lv_screen_active(),"最近活动"));
    assert(!Find(lv_screen_active(),"示例"));
    assert(Find(lv_screen_active(), "2026-11-24 11:20"));
    const auto initial_session_requests = session_list_requests;
    Click("下一页");
    assert(Find(lv_screen_active(),"项目规划") && Find(lv_screen_active(),"会话 10"));
    assert(!Find(lv_screen_active(),"下一页"));
    Click("上一页");
    assert(session_list_requests == initial_session_requests);
    Click("固件开发");
    Validate(lv_screen_active());Save("assistant-chat");
    assert(Find(lv_screen_active(),"接收（已连接）"));
    assert(Find(lv_screen_active(),"模型\nV4.1-Pro"));
    assert(Find(lv_screen_active(),"授权级别\n请求批准"));
    CheckChatLine("我：", "这是最近的对话。");
    PreviewRun().run = {"run_ui", firmware.id, RunState::Running, 3, "正在检查网络", ""};
    Pump();
    assert(Find(lv_screen_active(),"任务运行中"));
    assert(Find(lv_screen_active(),"正在检查网络"));
    Click("取消任务");
    assert(Find(lv_screen_active(),"取消当前任务？"));
    Click("继续任务");
    assert(!PreviewRun().cancelling);
    Click("取消任务");Click("确认取消");
    assert(Find(lv_screen_active(),"正在提交取消…"));
    PreviewRun().run.state = RunState::Cancelling;
    PreviewRun().cancelling = false;
    Pump();
    assert(Find(lv_screen_active(),"正在取消，等待任务停止"));
    assert(!Find(lv_screen_active(),"取消任务"));
    PreviewRun().run.state = RunState::Cancelled;
    Pump();
    assert(Find(lv_screen_active(),"任务已取消"));
    assert(Find(lv_screen_active(),"模型\nV4.1-Pro"));
    PreviewRun().run.state = RunState::Completed;
    PreviewRun().run.progress = "执行完成";
    Pump();
    assert(!Find(lv_screen_active(), "任务已完成") && !Find(lv_screen_active(), "执行完成"));
    PreviewRun() = {};
    Pump();

    const unsigned before_older = history_requests;
    registry[active].on_key("vk_prev");Pump();
    assert(history_requests == before_older + 1);
    auto* archive = CheckChatLine("助手：", "这是较早的回复。");
    assert(lv_obj_get_y(archive) == 82);
    assert(!Find(lv_screen_active(), "正在查看历史"));
    {
        auto request = std::make_shared<ApprovalInfo>();
        request->id = "approval_ui";
        request->session_id = firmware.id;
        request->run_id = "run_ui";
        request->version = 1;
        request->title = "允许检查设备状态吗？";
        request->reason = "读取用于联调的示例状态。";
        request->tool = "inspect_status";
        request->expires_at = "2026-11-25T12:30:00+08:00";
        request->actionable = true;
        request->verified_detail = true;
        request->operation = "{\"items\":[";
        for (int n = 0; n < 36; ++n) request->operation += "\"读取连接和消息示例🙂\",";
        request->operation += "\"完整参数 9007199254740993\"]}";
        PreviewApproval().info = request;
        Pump();
        assert(Find(lv_screen_active(),"审批详情"));
        assert(Find(lv_screen_active(), "读取用于联调的示例状态。"));
        assert(Find(lv_screen_active(), "查看操作") && !Find(lv_screen_active(), "仅本次允许"));
        assert(!Find(lv_screen_active(), "1 / 1"));
        Validate(lv_screen_active());Save("assistant-approval-summary");
        Click("查看操作");
        assert(!PreviewApproval().submitting);
        auto* allow = Find(lv_screen_active(),"仅本次允许");
        assert(allow && lv_obj_has_state(lv_obj_get_parent(allow), LV_STATE_DISABLED));
        Validate(lv_screen_active());Save("assistant-approval-first");
        Click("返回");
        assert(Find(lv_screen_active(), "读取用于联调的示例状态。"));
        Click("查看操作");
        const auto starts_before_approval = Application::GetInstance().GetAudioService().starts;
        boot_held = true;
        registry[active].on_boot_long_press();Pump();
        assert(Application::GetInstance().GetAudioService().starts == starts_before_approval);
        boot_held = false;
        registry[active].on_boot_press_up();Pump();
        for (int n = 0; n < 24 && lv_obj_has_state(lv_obj_get_parent(allow), LV_STATE_DISABLED); ++n) {
            registry[active].on_key("vk_next");Pump();
        }
        assert(!lv_obj_has_state(lv_obj_get_parent(allow), LV_STATE_DISABLED));
        Save("assistant-approval-last");
        PreviewReady() = false;Pump();
        assert(Find(lv_screen_active(),"离线，等待同步审批"));
        assert(lv_obj_has_state(lv_obj_get_parent(allow), LV_STATE_DISABLED));
        assert(lv_obj_has_state(lv_obj_get_parent(Find(lv_screen_active(),"拒绝")), LV_STATE_DISABLED));
        PreviewReady() = true;Pump();
        Click("仅本次允许");
        assert(Find(lv_screen_active(),"正在提交审批…"));
        auto accepted = std::make_shared<ApprovalInfo>(*request);
        accepted->version = 2;
        accepted->state = ApprovalState::Approved;
        PreviewApproval() = {accepted, false, false, false, {}};
        Pump();
        assert(Find(lv_screen_active(),"审批已允许"));
        CheckChatLine("助手：", "这是较早的回复。");

        auto elsewhere = std::make_shared<ApprovalInfo>(*request);
        elsewhere->id = "approval_web_only";
        elsewhere->session_id = planning.id;
        elsewhere->actionable = false;
        elsewhere->operation.clear();
        PreviewApproval() = {elsewhere, false, false, false, {}};
        Pump();
        assert(Find(lv_screen_active(),"待审批 1"));
        assert(!Find(lv_screen_active(),"审批详情"));
        Click("待审批 1");
        assert(Find(lv_screen_active(),"审批详情"));
        assert(!Find(lv_screen_active(),"仅本次允许") && !Find(lv_screen_active(), "查看操作"));
        assert(Find(lv_screen_active(),"拒绝") && Find(lv_screen_active(), "请到 Harness 网页核对并允许"));
        Save("assistant-approval-web-only");
        Click("返回");
        CheckChatLine("助手：", "这是较早的回复。");
        PreviewApproval() = {};
        Pump();
    }
    const unsigned before_updates = history_requests;
    const char* delta = R"({"type":"message.delta","session_id":"sess_1","data":{"message_id":"msg_3","offset_bytes":0,"text":"看到新消息了🙂","created_at":"2026-11-24T03:21:00Z"}})";
    PreviewHistory().lock()->Publish(delta);
    PreviewHistory().lock()->Publish(delta);
    PreviewHistory().lock()->Publish(R"({"type":"message.delta","session_id":"other","data":{"message_id":"msg_other","offset_bytes":0,"text":"另一个会话","created_at":"2026-11-24T03:22:00Z"}})");
    Pump();
    assert(history_requests == before_updates);
    CheckChatLine("助手：", "这是较早的回复。");
    assert(Find(lv_screen_active(),"有新消息"));
    Save("assistant-history");
    Click("有新消息");
    CheckChatLine("我：", "这是最近的对话。");
    CheckChatLine("助手：", "看到新消息了🙂");
    PreviewHistory().lock()->Invalidate();
    for (int i = 0; i < 5; ++i) Pump();
    CheckChatLine("我：", "这是最近的对话。");
    assert(history_requests == before_updates + 1);

    directory_error = true;
    Click("模型\nV4.1-Pro");
    assert(Find(lv_screen_active(),"读取失败"));
    directory_error = false;
    Click("重试");
    auto* unavailable = Find(lv_screen_active(),"不可用 · 中文受限模型");
    assert(unavailable && lv_obj_has_state(lv_obj_get_parent(unavailable), LV_STATE_DISABLED));
    Save("assistant-model-picker");
    Click("DeepSeek V4.1-Pro");
    assert(writes == 0);
    Click("模型\nV4.1-Pro");Click("下一页");Click("DeepSeek V4.1-Flash");
    assert(Find(lv_screen_active(),"模型\nV4.1-Flash"));
    assert(writes == 1);

    empty_authorizations = true;
    Click("授权级别\n请求批准");assert(Find(lv_screen_active(),"暂无可显示的项目"));Click("取消");
    empty_authorizations = false;
    Click("授权级别\n请求批准");
    ++firmware.settings_version; // A different client saved a new version.
    Click("完全访问");
    assert(Find(lv_screen_active(),"设置已在其他端变更，已刷新，请重新选择。"));
    assert(Find(lv_screen_active(),"授权级别\n请求批准"));
    Click("授权级别\n请求批准");Click("完全访问");
    assert(Find(lv_screen_active(),"授权级别\n完全访问"));
    assert(writes == 3);

    // Server updates refresh the open session, and settings remain separate.
    firmware.authorization_id = "ask";firmware.authorization_name = "请求批准";
    ++firmware.settings_version;++PreviewRevision();Pump();
    assert(Find(lv_screen_active(),"授权级别\n请求批准"));
    Click("返回");Click("下一页");Click("项目规划");
    assert(Find(lv_screen_active(),"授权级别\n完全访问"));
    assert(Find(lv_screen_active(),"暂无消息"));
    Click("返回");Click("上一页");Click("固件开发");
    assert(Find(lv_screen_active(),"模型\nV4.1-Flash"));
    Click("返回");
    empty_list = true;++PreviewRevision();Pump();
    assert(Find(lv_screen_active(),"暂无可显示的项目"));
    empty_list = false;++PreviewRevision();Pump();

    auto& app=Application::GetInstance();auto& audio=app.GetAudioService();
    // A delayed casual resolution never starts recording after BOOT release.
    defer_casual = true;
    boot_held = true;assert(registry[active].on_boot_long_press());Pump();
    assert(deferred && !audio.active);
    boot_held = false;registry[active].on_boot_press_up();Pump();
    deferred->Complete({CatalogError::None, casual});deferred.reset();Pump();
    assert(!audio.active && Find(lv_screen_active(),"固定闲聊"));
    Click("返回");defer_casual = false;
    boot_held = true;assert(registry[active].on_boot_long_press());Pump();
    assert(HarnessScreen::IsActive() && audio.active);
    assert(Find(lv_screen_active(),"固定闲聊"));
    assert(Find(lv_screen_active(),"接收（录音中）"));
    auto* recording_screen = lv_screen_active();
    auto* clock = Find(recording_screen, "12:34");
    lv_obj_t* wave = nullptr;
    lv_obj_t* notice = nullptr;
    for (unsigned i = 0; i < lv_obj_get_child_count(recording_screen); ++i) {
        auto* child = lv_obj_get_child(recording_screen, i);
        if (lv_obj_get_child_count(child) == 27) wave = child;
        if (lv_obj_check_type(child, &lv_label_class) &&
            std::strcmp(lv_label_get_text(child), "设备通知") == 0) notice = child;
    }
    assert(wave && !lv_obj_has_flag(wave, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_y(wave) + lv_obj_get_height(wave) <= 45);
    assert(clock && lv_obj_get_style_text_opa(clock, LV_PART_MAIN) == LV_OPA_TRANSP);
    auto* center_bar = lv_obj_get_child(wave, 13);
    const int first_height = lv_obj_get_height(center_bar);
    bool animated = false;
    for (int i = 0; i < 16; ++i) {
        lv_tick_inc(kPttWaveFrameMs);
        lv_timer_handler();
        lv_obj_update_layout(recording_screen);
        animated |= lv_obj_get_height(center_bar) != first_height;
    }
    assert(animated);
    Save("assistant-casual-recording");
    assert(notice);
    lv_obj_remove_flag(notice, LV_OBJ_FLAG_HIDDEN);
    Pump();
    assert(lv_obj_has_flag(wave, LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_flag(notice, LV_OBJ_FLAG_HIDDEN);
    Pump();
    assert(!lv_obj_has_flag(wave, LV_OBJ_FLAG_HIDDEN));
    audio.Feed(16000);boot_held=false;registry[active].on_boot_press_up();Pump();
    assert(!audio.active && harness_voice::Snapshot().duration_ms==1000);
    assert(harness_voice::Snapshot().phase==harness_voice::Phase::Complete);
    assert(PowerPolicy::GetInstance().idle() && audio.frozen);
    assert(Find(lv_screen_active(),"正在发送录音…"));
    assert(lv_obj_has_flag(wave, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_text_opa(clock, LV_PART_MAIN) == LV_OPA_COVER);
    const auto starts_before_pending = audio.starts;
    boot_held=true;registry[active].on_boot_long_press();Pump();
    assert(audio.starts == starts_before_pending && audio.frozen);
    boot_held=false;registry[active].on_boot_press_up();Pump();
    PreviewRecording().phase = UploadPhase::RetryRequired;
    PreviewRecording().error = "confirmation_unknown";
    Pump();
    assert(Find(lv_screen_active(),"发送未确认，录音已保留"));
    Click("放弃录音");assert(Find(lv_screen_active(),"放弃本地录音？"));
    Click("保留");assert(audio.frozen);
    Click("重试发送");assert(PreviewRecording().phase == UploadPhase::Queued);
    PreviewRecording().phase = UploadPhase::Accepted;
    PreviewRecording().error.clear();
    audio.ReleaseLocalRecording();
    Pump();
    assert(Find(lv_screen_active(),"录音已接受"));
    Save("assistant-casual-recorded");

    PreviewRecording().run_id = "casual_run";
    PreviewRun().run = {"casual_run", casual.id, RunState::Transcribing, 2, "", ""};
    Pump();
    assert(!Find(lv_screen_active(), "智能体思考中…"));
    PreviewRecording().transcript_id = "casual_text";
    PreviewRun().run.state = RunState::Running;
    PreviewHistory().lock()->Publish(R"({"type":"asr.final","session_id":"sess_casual","run_id":"casual_run","data":{"message_id":"casual_text","text":"语音转写示例。","created_at":"2026-11-24T03:21:00Z"}})");
    Pump();
    CheckChatLine("我：", "语音转写示例。");
    CheckChatLine("助手：", "智能体思考中…");
    assert(!Find(lv_screen_active(), "录音已接受"));
    auto* user_line = Find(lv_screen_active(), "语音转写示例。");
    auto* thinking_line = Find(lv_screen_active(), "智能体思考中…");
    assert(lv_obj_get_y(thinking_line) - lv_obj_get_y(user_line) < 52);
    Save("assistant-thinking");
    PreviewHistory().lock()->Publish(R"({"type":"message.delta","session_id":"sess_casual","run_id":"casual_run","data":{"message_id":"casual_reply","offset_bytes":0,"text":"收到语音。","created_at":"2026-11-24T03:21:01Z"}})");
    Pump();Pump();
    CheckChatLine("助手：", "收到语音。");
    assert(!Find(lv_screen_active(), "智能体思考中…"));
    PreviewRun() = {};

    {
        auto previous_handler = PreviewCatalog();
        PreviewCatalog() = [&](const CatalogRequest& request, const std::shared_ptr<CatalogCall>& call) {
            assert(request.operation == Op::History && request.cursor.empty());
            std::string body;
            for (int i = 1; i <= 100; ++i) {
                body += std::to_string(i) + " · 中文正文与表情🙂。\n";
            }
            call->Complete({CatalogError::None, HistoryPage{
                {{"long", std::move(body), "2026-11-24T03:20:00Z", 0, false, true}}, {}}});
        };
        ChatReader reader;
        reader.SetLayout(448, 500, fontpack_lv_font_ui(), fontpack_lv_font_get(30, 4), 6);
        reader.Bind("page_test");
        reader.Update();
        reader.Update();
        reader.SetThinkingFor("long");
        const auto newest = ChatText(reader);
        assert(newest.find("智能体思考中…") != std::string::npos);
        assert(newest.find("100 ·") != std::string::npos && newest.find("1 · 中文") == std::string::npos);
        reader.Turn(-1);
        assert(ChatText(reader) != newest && ChatText(reader).find("中文") != std::string::npos);
        assert(std::string(reader.Status()).empty());
        assert(ChatText(reader).find("智能体思考中…") == std::string::npos);
        reader.Turn(1);
        assert(ChatText(reader) == newest);
        PreviewCatalog() = std::move(previous_handler);
    }

    // A release before queued initialization must never start capture or reuse the old clip.
    unsigned starts=audio.starts;
    boot_held=true;
    harness_voice::Begin("sess_casual", 1);harness_voice::End();Pump();
    assert(audio.starts==starts && harness_voice::Snapshot().phase==harness_voice::Phase::Idle);
    app.initialization_hook=[](){harness_voice::End();};
    harness_voice::Begin("sess_casual", 1);Pump();assert(!audio.active && PowerPolicy::GetInstance().idle());
    audio.fail_start=true;harness_voice::Begin("sess_casual", 1);Pump();
    assert(harness_voice::Snapshot().phase==harness_voice::Phase::Error && PowerPolicy::GetInstance().idle());
    audio.fail_start=false;
    harness_voice::Begin("sess_casual", 1);Pump();audio.failed=true;audio.active=false;Pump();Pump();
    assert(harness_voice::Snapshot().phase==harness_voice::Phase::Error && PowerPolicy::GetInstance().idle());

    // The 30s capacity ends capture and releases both power needs through the real controller.
    harness_voice::Begin("sess_casual", 1);Pump();audio.Feed(16000*31);Pump();Pump();
    assert(harness_voice::Snapshot().duration_ms==30000 && PowerPolicy::GetInstance().idle());
    assert(audio.frozen);
    PreviewRecording().phase = UploadPhase::Accepted;
    audio.ReleaseLocalRecording();
    boot_held=true;registry[active].on_boot_long_press();Pump();assert(audio.active);
    assert(!lv_obj_has_flag(wave, LV_OBJ_FLAG_HIDDEN));
    HarnessScreen::OnEnterStandby();asleep=true;Pump();
    assert(lv_obj_has_flag(wave, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_text_opa(clock, LV_PART_MAIN) == LV_OPA_COVER);
    assert(!audio.active && PowerPolicy::GetInstance().idle());asleep=false;boot_held=false;

    ScreenGoHome();Pump();assert(!audio.active && PowerPolicy::GetInstance().idle());
    boot_held=true;registry["home"].on_boot_long_press();Pump();
    assert(HarnessScreen::IsActive() && audio.active);
    assert(Find(lv_screen_active(),"固定闲聊"));
    ScreenGoHome();Pump();assert(!audio.active && PowerPolicy::GetInstance().idle());boot_held=false;

    Click("设置");assert(std::strcmp(active,"settings")==0);ScreenGoHome();Pump();
    for(int i=0;i<60;i++) {
        Click("进入助手");assert(HarnessScreen::IsActive());
        Click("固件开发");
        registry[active].on_key("vk_next");Pump();
        ScreenGoHome();Pump();
        assert(on_home);assert(Timers()==baseline);assert(PowerPolicy::GetInstance().idle());
    }
    std::printf("PASS: 480x800 dashboard ranges, catalog/history pagination, streaming, approval review and settings conflict, BOOT casual/current-session PTT, bounded mono PCM, 60 navigation cycles; timers=%d\n",baseline);
    Validate(lv_screen_active());
}