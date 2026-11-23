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

static std::vector<unsigned char> font_blob;
static std::vector<uint32_t> pixels(480*800,0xffffff);
static const char* active = "none";
static bool on_home = false;
static bool asleep = false;
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
static void Pump() { for(int i=0;i<6;i++){lv_tick_inc(20);lv_timer_handler();}lv_obj_update_layout(lv_screen_active());lv_refr_now(nullptr); }
static void Save(const char* name) {
    Pump();std::ofstream out(std::string(name)+".ppm",std::ios::binary);out<<"P6\n480 800\n255\n";
    for(uint32_t px:pixels){out.put((px>>16)&255);out.put((px>>8)&255);out.put(px&255);}
}
static lv_obj_t* Find(lv_obj_t* root,const char* text) {
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
static int Timers() {int count=0;for(auto* t=lv_timer_get_next(nullptr);t;t=lv_timer_get_next(t))count++;return count;}
static void Validate(lv_obj_t* root) {
    if(lv_obj_check_type(root,&lv_label_class)) {
        auto* parent=lv_obj_get_parent(root);
        int x=lv_obj_get_x(root),y=lv_obj_get_y(root),w=lv_obj_get_width(root),h=lv_obj_get_height(root);
        if(x<0||y<0||x+w>lv_obj_get_width(parent)||y+h>lv_obj_get_height(parent)) {
            std::fprintf(stderr,"OVERFLOW %s x=%d y=%d w=%d h=%d parent=%dx%d\n",lv_label_get_text(root),x,y,w,h,lv_obj_get_width(parent),lv_obj_get_height(parent));
            std::abort();
        }
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
    ScreenLoadReplace(DashboardScreen::Create());Pump();Validate(lv_screen_active());Save("dashboard");
    int baseline=Timers();
    // Every entry path targets the new assistant, including a BOOT callback queued off LVGL.
    assert(registry["home"].on_boot_long_press());Pump();assert(HarnessScreen::IsActive());
    Validate(lv_screen_active());Save("assistant-empty");
    Key("vk_next");assert(Find(lv_screen_active(),"0 / 0"));
    Click("演示文本");Validate(lv_screen_active());Save("assistant-page-1");
    Key("vk_prev");assert(Find(lv_screen_active(),"1 / 3"));
    Click("下一页");Validate(lv_screen_active());Save("assistant-page-2");
    Key("vk_next");Validate(lv_screen_active());Save("assistant-page-3");
    Key("vk_next");assert(Find(lv_screen_active(),"3 / 3"));
    Click("重读演示");assert(Find(lv_screen_active(),"1 / 3"));
    asleep=true;Key("vk_next");assert(Find(lv_screen_active(),"1 / 3"));asleep=false;
    Click("返回主页");assert(on_home);Validate(lv_screen_active());
    Click("设置");assert(std::strcmp(active,"settings")==0);ScreenGoHome();Pump();
    // Deleting a dashboard must release its timer; pending actions must ignore a departed page.
    for(int i=0;i<60;i++) {
        Click("进入助手");assert(HarnessScreen::IsActive());
        registry[active].on_key("vk_next");ScreenGoHome();Pump();
        assert(on_home);assert(Timers()==baseline);
    }
    std::printf("PASS: entry buttons, BOOT, settings, pagination boundaries, standby gating, 60 navigation cycles; timers=%d\n",baseline);
    Validate(lv_screen_active());
}
