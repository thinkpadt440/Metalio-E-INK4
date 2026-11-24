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
#include "vk_key_handler.h"
#include "fontpack_lvgl.h"
#include "esp_partition.h"
#include "haptic_feedback.h"
#include "display_orient.h"
#include "application.h"
#include "power_policy.h"
#include "harness_screen/harness_voice.h"
#include "local_recording_buffer.h"
#include "harness_screen/offline_demo.h"

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
    lv_label_set_text(label,"Wi-Fi                    100%");lv_obj_set_pos(label,16,8);
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
static void Pump() { for(int i=0;i<6;i++){Application::GetInstance().Pump();lv_tick_inc(10);lv_timer_handler();}lv_obj_update_layout(lv_screen_active());lv_refr_now(nullptr); }
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
static void Click(const char* text) {
    auto* label=Find(lv_screen_active(),text);assert(label);
    auto* button=lv_obj_get_parent(label);assert(!lv_obj_has_state(button,LV_STATE_DISABLED));
    lv_obj_send_event(button,LV_EVENT_CLICKED,nullptr);Pump();
}
static void Key(const char* key) { assert(registry[active].on_key);registry[active].on_key(key);Pump(); }
static void Wait(unsigned ms) {
    for (unsigned t=0;t<ms;t+=10) { Application::GetInstance().Pump();lv_tick_inc(10);lv_timer_handler(); }
    lv_obj_update_layout(lv_screen_active());lv_refr_now(nullptr);
}
static int Timers() {int count=0;for(auto* t=lv_timer_get_next(nullptr);t;t=lv_timer_get_next(t))count++;return count;}
static void Validate(lv_obj_t* root) {
    if (lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;
    if(lv_obj_check_type(root,&lv_label_class)) {
        const char* text=lv_label_get_text(root);
        for (const char* word : {"演示", "预设", "脚本", "模拟"}) assert(std::strstr(text,word)==nullptr);
        auto* parent=lv_obj_get_parent(root);
        int x=lv_obj_get_x(root),y=lv_obj_get_y(root),w=lv_obj_get_width(root),h=lv_obj_get_height(root);
        if(x<0||y<0||x+w>lv_obj_get_width(parent)||y+h>lv_obj_get_height(parent)) {
            std::fprintf(stderr,"OVERFLOW %s x=%d y=%d w=%d h=%d parent=%dx%d\n",lv_label_get_text(root),x,y,w,h,lv_obj_get_width(parent),lv_obj_get_height(parent));
            std::abort();
        }
    }
    if (HarnessScreen::IsActive() && lv_obj_check_type(root,&lv_label_class) && lv_obj_get_y(root)==60) {
        auto* parent=lv_obj_get_parent(root);
        assert(lv_obj_get_y(root)+lv_obj_get_height(root) <= lv_obj_get_height(parent)-134);
    }
    if (HarnessScreen::IsActive() && lv_obj_check_type(root,&lv_label_class) && lv_obj_get_y(root)==58) {
        auto* parent=lv_obj_get_parent(root);
        assert(lv_obj_get_y(root)+lv_obj_get_height(root) <= lv_obj_get_height(parent)-52);
    }
    for(unsigned i=0;i<lv_obj_get_child_count(root);i++)Validate(lv_obj_get_child(root,i));
}
int main() {
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
    assert(memory[0]==123 && memory[1]==100 && memory[2]==-32768 && memory[4]==400 && memory[5]==456);
    recording.AppendInterleaved(stereo,10,0);assert(recording.size()==4);

    // Deterministic script timings, duplicate decisions, and bounded history.
    using namespace offline_demo;
    State().Reset();
    auto& coder = State().sessions[kCoder];
    assert(coder.authorization==1);
    coder.authorization=0;
    Start(kCoder,100,false);
    assert(!Advance(kCoder,2099) && coder.phase==Phase::Thinking);
    assert(Advance(kCoder,2100) && coder.phase==Phase::WaitingApproval);
    assert(Decide(true,2200));
    assert(!Decide(false,2201) && coder.phase==Phase::Executing);
    assert(Advance(kCoder,4200) && coder.phase==Phase::Testing);
    assert(Advance(kCoder,6200) && coder.phase==Phase::Complete);
    Start(kCoder,7000,false);Advance(kCoder,9000);
    assert(Decide(false,9100) && coder.phase==Phase::Rejected);
    assert(!Advance(kCoder,99999));
    for (int i=0;i<50;i++) Append(coder,"bounded history");
    assert(coder.pages.size()==10);
    State().Reset();

    ScreenLoadReplace(DashboardScreen::Create());Pump();Validate(lv_screen_active());
    Save("dashboard");
    assert(Find(lv_screen_active(),"53"));
    assert(Find(lv_screen_active(),"5,980"));
    assert(Find(lv_screen_active(),"8.3M"));
    assert(Find(lv_screen_active(),"68"));
    assert(Find(lv_screen_active(),"DeepSeek\nV4.1-Pro"));
    Click("30天");assert(Find(lv_screen_active(),"28"));assert(Find(lv_screen_active(),"2,486"));
    assert(Find(lv_screen_active(),"22"));Validate(lv_screen_active());Save("dashboard-30d");
    Click("7天");assert(Find(lv_screen_active(),"9"));assert(Find(lv_screen_active(),"412"));
    assert(Find(lv_screen_active(),"6"));assert(Find(lv_screen_active(),"652.6K"));Validate(lv_screen_active());Save("dashboard-7d");
    Click("全部");assert(Find(lv_screen_active(),"5,980"));
    // Exercise zero, grouping boundaries and maximum 32-bit count through the real UI.
    DashboardMetrics edge;
    edge.valid=true;edge.sessions=0;edge.messages=UINT32_MAX;edge.active_days=999;
    DashboardScreen::UpdateMetrics(edge);Wait(1100);
    assert(Find(lv_screen_active(),"0"));
    assert(Find(lv_screen_active(),"4,294,967,295"));
    assert(Find(lv_screen_active(),"999"));
    edge.sessions=1000;edge.messages=1000000;edge.active_days=1001;
    DashboardScreen::UpdateMetrics(edge);Wait(1100);
    assert(Find(lv_screen_active(),"1,000"));
    assert(Find(lv_screen_active(),"1,000,000"));
    assert(Find(lv_screen_active(),"1,001"));
    DashboardScreen::UpdateMetrics(DashboardMetrics{});Wait(1100);
    assert(Find(lv_screen_active(),"5,980"));
    int baseline=Timers();

    assert(registry["home"].on_boot_click());Pump();
    Validate(lv_screen_active());Save("assistant-sessions");
    assert(Find(lv_screen_active(),"日常闲聊") && Find(lv_screen_active(),"编码助手"));
    assert(!Find(lv_screen_active(),"离线演示") && !Find(lv_screen_active(),"示例"));
    Click("编码助手");Validate(lv_screen_active());Save("assistant-coder");
    assert(Find(lv_screen_active(),"模型\nV4.1-Pro"));
    assert(Find(lv_screen_active(),"授权级别\n完全访问"));
    assert(Find(lv_screen_active(),"2 / 2"));
    Key("vk_prev");assert(Find(lv_screen_active(),"1 / 2"));
    Key("vk_prev");assert(Find(lv_screen_active(),"1 / 2"));
    Key("vk_next");assert(Find(lv_screen_active(),"2 / 2"));
    Key("vk_next");assert(Find(lv_screen_active(),"2 / 2"));
    Click("模型\nV4.1-Pro");Validate(lv_screen_active());
    assert(Find(lv_screen_active(),"DeepSeek V4.1-Flash"));
    assert(Find(lv_screen_active(),"DeepSeek V4.1-Pro"));Save("assistant-model-picker");
    Click("DeepSeek V4.1-Flash");assert(Find(lv_screen_active(),"模型\nV4.1-Flash"));
    Click("授权级别\n完全访问");Save("assistant-authorization-picker");
    Click("请求审批");assert(coder.phase==Phase::Thinking);
    assert(!Find(lv_screen_active(),"需要你的批准"));
    Wait(2100);Validate(lv_screen_active());
    assert(coder.phase==Phase::WaitingApproval);
    assert(Find(lv_screen_active(),"需要你的批准"));
    assert(!Find(lv_screen_active(),"授权级别\n请求审批"));
    Save("assistant-approval");
    auto& app=Application::GetInstance();auto& audio=app.GetAudioService();
    unsigned starts=audio.starts;
    boot_held=true;registry[active].on_boot_long_press();Pump();
    assert(audio.starts==starts);boot_held=false;registry[active].on_boot_press_up();Pump();
    // Approval actions swapped after device feedback: PREV approves, NEXT rejects.
    Key("vk_prev");assert(coder.phase==Phase::Executing);
    Save("assistant-approved");
    Wait(2100);assert(coder.phase==Phase::Testing);Save("assistant-testing");
    Wait(2100);assert(coder.phase==Phase::Complete);Validate(lv_screen_active());Save("assistant-complete");
    assert(Find(lv_screen_active(),"接收（已完成）"));
    Click("授权级别\n请求审批");Click("请求审批");Wait(2100);
    Key("vk_next");assert(coder.phase==Phase::Rejected);
    Wait(4500);assert(coder.phase==Phase::Rejected);Save("assistant-rejected");

    // Scenario/settings survive returning home. Reset gives a repeatable presentation.
    ScreenGoHome();Pump();Click("进入助手");Click("编码助手");
    assert(Find(lv_screen_active(),"接收（已拒绝）"));
    assert(Find(lv_screen_active(),"模型\nV4.1-Flash"));
    Click("返回");Click("重置测试");Click("日常闲聊");
    assert(Find(lv_screen_active(),"模型\nV4.1-Flash"));
    Save("assistant-casual");
    boot_held=true;registry[active].on_boot_long_press();Pump();Wait(200);
    assert(audio.active);Validate(lv_screen_active());Save("assistant-casual-recording");
    audio.Feed(16000);boot_held=false;registry[active].on_boot_press_up();Pump();Wait(200);
    assert(!audio.active && harness_voice::Snapshot().duration_ms==1000);
    assert(State().sessions[kCasual].phase==Phase::Transcribing);
    Wait(1100);assert(State().sessions[kCasual].phase==Phase::Thinking);
    Save("assistant-casual-transcribed");
    Wait(2100);assert(State().sessions[kCasual].phase==Phase::Complete);
    Validate(lv_screen_active());Save("assistant-casual-reply");

    Click("返回");Click("编码助手");
    assert(Find(lv_screen_active(),"授权级别\n完全访问"));
    boot_held=true;registry[active].on_boot_long_press();Pump();assert(audio.active);
    audio.Feed(16000);boot_held=false;registry[active].on_boot_press_up();Pump();Wait(200);
    Wait(1100);Wait(2100);assert(coder.phase==Phase::Executing);
    assert(!Find(lv_screen_active(),"需要你的批准"));
    Wait(2100);Wait(2100);assert(coder.phase==Phase::Complete);

    // PTT continues to use the real controller with host hardware stubs.
    starts=audio.starts;
    harness_voice::Begin();harness_voice::End();Pump();
    assert(audio.starts==starts && harness_voice::Snapshot().phase==harness_voice::Phase::Idle);
    app.initialization_hook=[](){harness_voice::End();};
    harness_voice::Begin();Pump();assert(!audio.active);
    audio.fail_start=true;harness_voice::Begin();Pump();
    assert(harness_voice::Snapshot().phase==harness_voice::Phase::Error);
    audio.fail_start=false;
    harness_voice::Begin();Pump();audio.failed=true;audio.active=false;Pump();Pump();
    assert(harness_voice::Snapshot().phase==harness_voice::Phase::Error);
    harness_voice::Begin();Pump();audio.Feed(16000*31);Pump();Pump();
    assert(harness_voice::Snapshot().duration_ms==30000);
    State().Reset();
    boot_held=true;registry[active].on_boot_long_press();Pump();assert(audio.active);
    HarnessScreen::OnEnterStandby();asleep=true;Pump();assert(!audio.active);
    asleep=false;boot_held=false;Wait(200);
    assert(State().sessions[kCasual].phase==Phase::Ready);

    ScreenGoHome();Pump();
    boot_held=true;registry["home"].on_boot_long_press();Pump();
    assert(HarnessScreen::IsActive() && audio.active && Find(lv_screen_active(),"日常闲聊"));
    ScreenGoHome();Pump();assert(!audio.active);boot_held=false;

    Click("设置");assert(std::strcmp(active,"settings")==0);
    assert(PowerPolicy::GetInstance().idle());
    ScreenGoHome();Pump();
    for(int i=0;i<60;i++) {
        Click("进入助手");Click("编码助手");Key("vk_prev");
        ScreenGoHome();Pump();assert(on_home && Timers()==baseline);
    }
    Click("设置");assert(PowerPolicy::GetInstance().idle());ScreenGoHome();Pump();
    Validate(lv_screen_active());
    std::printf("PASS: offline dashboard periods, two persistent sessions, exactly 2s approval, physical left approve/right reject, scripted execution/rejection, PTT/canned reply, bounded history, 60 navigation cycles; timers=%d\n",baseline);
}
