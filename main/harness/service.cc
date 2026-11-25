#include "service.h"
#include "socket_io.h"
#include "application.h"
#include "board.h"
#include "power_policy.h"
#include "standby_screen/standby_screen.h"
#include "system_info.h"
#include <esp_app_desc.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace harness {
namespace {
std::mutex s_mutex;
ConnectionSnapshot s_status;
std::atomic<bool> s_started{false};
std::atomic<bool> s_stop{false};
void Save(const ConnectionSnapshot& status){std::lock_guard<std::mutex> lock(s_mutex);s_status=status;}
uint64_t Now(){return static_cast<uint64_t>(esp_timer_get_time()/1000);}
Config FirmwareConfig() {
    Config c;
    c.base_url=CONFIG_HARNESS_SERVER_URL;
    if(!c.base_url.empty()&&c.base_url.back()=='/')c.base_url.pop_back();
    c.device_id=CONFIG_HARNESS_DEVICE_ID;
    if(c.device_id.empty()) {
        c.device_id="metalio_";
        for(char ch:SystemInfo::GetMacAddress())if(ch!=':')c.device_id+=ch;
    }
    c.firmware_version=esp_app_get_description()->version;
    char nonce[32];std::snprintf(nonce,sizeof(nonce),"cmd_%08x%08x",static_cast<unsigned>(esp_random()),static_cast<unsigned>(esp_random()));
    c.command_prefix=nonce;
    return c;
}
void Worker() {
    const auto config=FirmwareConfig();
    auto& board=Board::GetInstance();
    auto* network=board.GetNetwork();
    if(!network){ConnectionSnapshot s;s.state=ConnectionState::Stopped;s.error="network_unavailable";Save(s);return;}
    PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
    {
        SocketIo io(*network);
        Client client(io,config,Now,Save);
        uint64_t last_network_try=0;
        while(!s_stop.load()) {
            bool available=false;
            if(!StandbyScreen::IsActive()&&!board.IsWifiConfigMode()) {
                available=board.IsNetworkReady();
                if(!available&&Now()-last_network_try>=1000) {
                    last_network_try=Now();
                    available=board.EnsureNetworkReady(200);
                }
            }
            client.Tick(available);
            Save(client.Status());
            // Business consumers/recording are wired in later phases. The bounded
            // event queue intentionally resyncs on overflow rather than dropping text.
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        client.Stop();
        Save(client.Status());
    }
    PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
}
void Run(void*) {
    Worker();  // Let all C++ locals destruct before deleting the FreeRTOS task.
    s_started=false;
    vTaskDelete(nullptr);
}
}  // namespace
ConnectionSnapshot ServiceStatus(){std::lock_guard<std::mutex> lock(s_mutex);return s_status;}
const char* ConnectionStatusLabel(){return ConnectionLabel(ServiceStatus().state);}
void StopService(){s_stop=true;}
void StartService() {
    bool expected=false;if(!s_started.compare_exchange_strong(expected,true))return;
    s_stop=false;
    auto config=FirmwareConfig();
    if(config.base_url.empty()){Save(ConnectionSnapshot{});return;}
    if(!ValidConfig(config)){ConnectionSnapshot s;s.error="invalid_config";Save(s);return;}
    ConnectionSnapshot s;s.state=ConnectionState::WaitingNetwork;Save(s);
    if(xTaskCreate(Run,"harness",16384,nullptr,2,nullptr)!=pdPASS) {
        s.state=ConnectionState::Stopped;s.error="task_allocation_failed";Save(s);s_started=false;
        ESP_LOGE("Harness","Unable to create client task");
    }
}
}  // namespace harness
