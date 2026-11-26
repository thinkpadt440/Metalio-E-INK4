#include "service.h"
#include "socket_io.h"
#include "transport.h"
#include "application.h"
#include "board.h"
#include "device_time.h"
#include "harness_screen/harness_voice.h"
#include "power_policy.h"
#include "system_info.h"
#include <esp_app_desc.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <sys/time.h>

namespace harness {
namespace {
constexpr char kSettingsNamespace[] = "harness";
constexpr char kServerOriginKey[] = "server_origin";
std::mutex s_mutex;
bool s_server_change_in_progress = false;  // Protected by s_mutex.
std::string s_server_origin;
ConnectionSnapshot s_status;
uint32_t s_ready_sleep_epoch = 0;  // Protected by s_mutex with s_status.
std::weak_ptr<HistoryInbox> s_history;
std::shared_ptr<UploadTicket> s_recording;
RunTracker s_runs;
ApprovalTracker s_approvals;

bool RunAllowsApproval(const ApprovalPtr& approval) {
    const auto task = s_runs.View(approval->session_id);
    return task.run.id.empty() || (task.run.id == approval->run_id &&
        !task.run.Finished() && task.run.state != RunState::Cancelling && !task.cancelling);
}

std::string LoadRunIds() {
    nvs_handle_t handle;
    if (nvs_open("harness_runs", NVS_READONLY, &handle) != ESP_OK) return {};
    size_t size = 0;
    std::string data;
    if (nvs_get_blob(handle, "active", nullptr, &size) == ESP_OK && size <= kMaxJsonBytes) {
        data.resize(size);
        if (nvs_get_blob(handle, "active", data.data(), &size) != ESP_OK) data.clear();
    }
    nvs_close(handle);
    return data;
}

bool SaveRunIds(const std::string& data) {
    if (data.empty() || data.size() > kMaxJsonBytes) return false;
    nvs_handle_t handle;
    if (nvs_open("harness_runs", NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool saved = nvs_set_blob(handle, "active", data.data(), data.size()) == ESP_OK &&
                       nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return saved;
}

std::shared_ptr<UploadTicket> RecordingTicket() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_recording;
}

std::string HashRecording(size_t bytes) {
    auto& audio = Application::GetInstance().GetAudioService();
    uint8_t buffer[kMaxAudioChunkBytes];
    uint8_t digest[32];
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts(&context, 0) == 0;
    for (size_t offset = 0; ok && offset < bytes; offset += sizeof(buffer)) {
        const auto count = std::min(sizeof(buffer), bytes - offset);
        ok = audio.ReadFrozenRecording(offset, buffer, count) &&
             mbedtls_sha256_update(&context, buffer, count) == 0;
    }
    ok = ok && mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    if (!ok) return {};
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (uint8_t byte : digest) {
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 15]);
    }
    return result;
}
std::atomic<bool> s_started{false};
std::atomic<bool> s_stop{false};
std::atomic<uint32_t> s_catalog_revision{0};

bool ReadyLocked(PowerPolicy::HarnessNetworkState power) {
    return power.AllowsWorkAt(s_ready_sleep_epoch) && !s_stop && !s_server_change_in_progress &&
           s_status.state == ConnectionState::Ready;
}

struct CatalogWork {
    CatalogRequest request;
    std::string connection_id;
    std::weak_ptr<CatalogCall> call;
};
constexpr size_t kMaxCatalogQueue = 8;
std::deque<CatalogWork> s_catalog_queue;

void ProcessCatalog(ClientIo& io, const std::string& device_id, const ConnectionSnapshot& status) {
    CatalogWork work;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        while (!s_catalog_queue.empty() && s_catalog_queue.front().call.expired()) {
            s_catalog_queue.pop_front();
        }
        if (s_catalog_queue.empty()) return;
        work = std::move(s_catalog_queue.front());
        s_catalog_queue.pop_front();
    }
    if (auto call = work.call.lock()) {
        if (status.state == ConnectionState::Ready && status.connection_id == work.connection_id) {
            auto reply = FetchCatalog(io, device_id, work.request);
            if (const auto* session = std::get_if<Session>(&reply.data)) {
                s_runs.Hint(session->id, session->active_run_id);
            }
            call->Complete(std::move(reply));
        } else {
            call->Complete({CatalogError::Offline, {}});
        }
    }
}
void Save(const ConnectionSnapshot& status) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (status.state == ConnectionState::Ready && s_status.state != ConnectionState::Ready) {
        ESP_LOGI("Harness", "WebSocket sync ready");
    }
    s_status = status;
}
uint64_t Now(){return static_cast<uint64_t>(esp_timer_get_time()/1000);}
std::optional<std::string> ReadSavedServerOrigin() {
    nvs_handle_t handle;
    if (nvs_open(kSettingsNamespace, NVS_READONLY, &handle) != ESP_OK) return std::nullopt;
    char stored[kMaxServerOriginLength + 1];
    size_t length = sizeof(stored);
    const auto result = nvs_get_str(handle, kServerOriginKey, stored, &length);
    nvs_close(handle);
    if (result != ESP_OK) return std::nullopt;
    return std::string(stored);
}

Config FirmwareConfig() {
    Config c;
    c.base_url = ConfiguredServerOrigin();
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
    auto& power = PowerPolicy::GetInstance();
    std::optional<PowerNeedHold> network_need;
#ifdef CONFIG_HARNESS_ONLINE_STANDBY
    std::optional<PowerNeedHold> online_standby_need;
#endif
    {
#ifdef CONFIG_BOARD_TYPE_METALIO_E_INK_4
        bool clock_synced = false;
        SocketIo io(*network, [&board, &clock_synced](const std::string& date) {
            if (clock_synced) return;
            const auto stamp = device_time::ParseHttpDate(date);
            if (!stamp) return;
            const timeval clock{*stamp, 0};
            if (settimeofday(&clock, nullptr) == 0) {
                clock_synced = true;
                board.OnNetworkTimeSynced();
                ESP_LOGI("Harness", "System clock synchronized from gateway");
            }
        });
#else
        SocketIo io(*network);
#endif
        Client client(io,config,Now,Save);
        uint64_t last_network_try=0;
        std::string ready_connection;
        std::string last_upload_run;
        const std::string target = config.base_url + "\n" + config.device_id;
        s_runs.RestoreIds(target, LoadRunIds());
        uint32_t persisted = s_runs.IdRevision();
        uint64_t persist_after = 0;
        bool did_lookup_last = false;
        auto sleep_epoch = power.GetHarnessNetworkState().sleep_epoch;
        while (!s_stop.load()) {
            const auto initial = power.GetHarnessNetworkState();
            const bool allowed = initial.allowed && !board.IsWifiConfigMode();
            if (allowed) {
                if (!network_need) network_need.emplace(PowerNeed::HarnessNetwork);
#ifdef CONFIG_HARNESS_ONLINE_STANDBY
                if (!online_standby_need) online_standby_need.emplace(PowerNeed::HarnessOnlineStandby);
#endif
            } else {
#ifdef CONFIG_HARNESS_ONLINE_STANDBY
                online_standby_need.reset();
#endif
                network_need.reset();
            }

            bool available = false;
            if (allowed && initial.AllowsWorkAt(sleep_epoch)) {
                available = board.IsNetworkReady();
                if (!available && Now() - last_network_try >= 1000) {
                    last_network_try = Now();
                    available = board.EnsureNetworkReady(200);
                }
            }
            const auto before_tick = power.GetHarnessNetworkState();
            client.Tick(available && before_tick.AllowsWorkAt(sleep_epoch) &&
                        !board.IsWifiConfigMode() && !s_stop);
            const auto after_tick = power.GetHarnessNetworkState();
            // A full sleep/wake cycle can occur while a network call blocks.
            if (!after_tick.AllowsWorkAt(before_tick.sleep_epoch) ||
                board.IsWifiConfigMode() || s_stop) {
                client.Tick(false);
            }
            if (sleep_epoch != after_tick.sleep_epoch) {
                last_network_try = 0;
                sleep_epoch = after_tick.sleep_epoch;
            }
            const auto status = client.Status();
            if (status.state == ConnectionState::Ready) {
                if (ready_connection != status.connection_id) {
                    const auto resources = client.Resources();
                    s_runs.Snapshot(resources);
                    s_approvals.Snapshot(resources);
                }
                const auto events = client.DrainEvents();
                for (const auto& event : events) {
                    s_runs.Observe(event);
                    s_approvals.Observe(event);
                }
                if (auto recording = RecordingTicket()) {
                    const auto upload = recording->Status();
                    if (upload.phase == UploadPhase::Accepted) {
                        if (last_upload_run != upload.run_id) {
                            last_upload_run = upload.run_id;
                            s_runs.Hint(upload.session_id, upload.run_id);
                        }
                        for (const auto& event : events) recording->Observe(event);
                    }
                }
                if (ready_connection != status.connection_id ||
                    std::any_of(events.begin(), events.end(), IsSessionUpdate)) {
                    ++s_catalog_revision;
                }
                std::shared_ptr<HistoryInbox> history;
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    history = s_history.lock();
                }
                if (history) {
                    if (ready_connection != status.connection_id) history->Invalidate();
                    else for (const auto& event : events) history->Publish(event);
                }
                ready_connection = status.connection_id;
                std::lock_guard<std::mutex> lock(s_mutex);
                s_ready_sleep_epoch = sleep_epoch;
            } else {
                if (!ready_connection.empty()) s_approvals.Offline();
                ready_connection.clear();
            }
            if (auto result = client.TakeApprovalResult()) s_approvals.Finish(std::move(*result));
            if (!client.ApprovalPending()) {
                if (auto request = s_approvals.TakeRequest(); request &&
                    (!RunAllowsApproval(request->approval) || !client.SendApproval(*request))) {
                    s_approvals.Finish({request->approval, {}, "not_ready"});
                }
            }
            if (auto result = client.TakeCancelResult()) s_runs.FinishCancel(*result);
            if (!client.CancelPending()) {
                if (auto run = s_runs.TakeCancel(); run && !client.SendCancel(*run)) {
                    s_runs.FinishCancel({run->session_id, run->id, "not_ready"});
                }
            }
            client.AdvanceUpload(RecordingTicket());
            const auto revision = s_runs.IdRevision();
            const auto now = Now();
            if (revision != persisted && now >= persist_after) {
                if (SaveRunIds(s_runs.SaveIds(target))) persisted = revision;
                else {
                    persist_after = now + 5000;
                    ESP_LOGW("Harness", "Could not store active run IDs");
                }
            }
            if (!client.UploadSending() && !client.CancelPending() && !client.ApprovalPending()) {
                const auto current = client.Status();
                const bool can_lookup = current.state == ConnectionState::Ready && !did_lookup_last;
                const auto approval = can_lookup ? s_approvals.NextLookup(now) : std::nullopt;
                const auto lookup = can_lookup && !approval ? s_runs.NextLookup(now) : std::nullopt;
                if (approval) {
                    auto reply = FetchCatalog(io, config.device_id, {CatalogOperation::Approval, *approval, {}, {}, 0});
                    auto* detail = std::get_if<ApprovalPtr>(&reply.data);
                    s_approvals.LookupResult(*approval, detail ? std::move(*detail) : ApprovalPtr{},
                                             reply.error == CatalogError::NotFound);
                } else if (lookup) {
                    auto reply = FetchCatalog(io, config.device_id, {CatalogOperation::Run, lookup->id, {}, {}, 0});
                    const auto* info = std::get_if<RunInfo>(&reply.data);
                    s_runs.LookupResult(*lookup, info ? std::optional<RunInfo>(*info) : std::nullopt,
                                        reply.error == CatalogError::NotFound);
                } else {
                    ProcessCatalog(io, config.device_id, current);
                }
                did_lookup_last = lookup.has_value() || approval.has_value();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        s_approvals.Offline();
        client.Stop();
        Save(client.Status());
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto& work : s_catalog_queue) {
            if (auto call = work.call.lock()) call->Complete({CatalogError::Offline, {}});
        }
        s_catalog_queue.clear();
    }
}
void Run(void*) {
    Worker();  // Let all C++ locals destruct before deleting the FreeRTOS task.
    s_started=false;
    vTaskDelete(nullptr);
}
}  // namespace
ConnectionSnapshot ServiceStatus(){std::lock_guard<std::mutex> lock(s_mutex);return s_status;}
const char* ConnectionStatusLabel() {
    const auto power = PowerPolicy::GetInstance().GetHarnessNetworkState();
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto state = s_status.state == ConnectionState::Ready && !ReadyLocked(power)
        ? ConnectionState::WaitingNetwork : s_status.state;
    return ConnectionLabel(state);
}
bool ServiceReady() {
    const auto power = PowerPolicy::GetInstance().GetHarnessNetworkState();
    std::lock_guard<std::mutex> lock(s_mutex);
    return ReadyLocked(power);
}
ApprovalView TrackedApproval(const std::string& id) {
    auto view = s_approvals.View(id);
    if (view.info && view.info->state == ApprovalState::Pending && !RunAllowsApproval(view.info)) {
        view.checking = true;
    }
    return view;
}
std::vector<ApprovalPtr> PendingApprovals() { return s_approvals.Pending(); }
bool RequestApproval(const ApprovalPtr& approval, ApprovalDecision decision) {
    return ServiceReady() && approval && RunAllowsApproval(approval) && s_approvals.Queue(approval, decision);
}
RunView TrackedRun(const std::string& session_id) { return s_runs.View(session_id); }
bool RequestRunCancel(const std::string& session_id, const std::string& run_id) {
    return ServiceReady() && s_runs.QueueCancel(session_id, run_id);
}
bool QueueRecording(std::string session_id, uint32_t settings_version, uint32_t samples) {
    if (session_id.empty() || !settings_version || samples < 3200 || samples > 480000) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_started || s_stop || s_server_change_in_progress ||
        (s_recording && s_recording->Status().Pending())) return false;
    char id[40];
    std::snprintf(id, sizeof(id), "utt_%08x%08x%08x",
                  static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()),
                  static_cast<unsigned>(esp_random()));
    const size_t bytes = samples * sizeof(int16_t);
    s_recording = std::make_shared<UploadTicket>(std::move(session_id), settings_version, id, RecordingSource{
        bytes,
        [](size_t offset, void* out, size_t count) {
            return Application::GetInstance().GetAudioService().ReadFrozenRecording(offset, out, count);
        },
        [bytes] { return HashRecording(bytes); },
        [] { Application::GetInstance().GetAudioService().ReleaseLocalRecording(); },
    });
    return true;
}

UploadStatus RecordingStatus() {
    auto ticket = RecordingTicket();
    return ticket ? ticket->Status() : UploadStatus{};
}

bool RequestRecordingAction(UploadAction action) {
    if (action == UploadAction::Retry && !ServiceReady()) return false;
    auto ticket = RecordingTicket();
    return ticket && ticket->Request(action);
}

uint32_t CatalogRevision() { return s_catalog_revision.load(); }
std::shared_ptr<HistoryInbox> SubscribeHistory(const std::string& session_id) {
    auto inbox = std::make_shared<HistoryInbox>(session_id);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_history = inbox;
    return inbox;
}
std::shared_ptr<CatalogCall> SubmitCatalog(CatalogRequest request) {
    auto call = std::make_shared<CatalogCall>();
    const auto power = PowerPolicy::GetInstance().GetHarnessNetworkState();
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!ReadyLocked(power)) {
        call->Complete({CatalogError::Offline, {}});
    } else if (s_catalog_queue.size() >= kMaxCatalogQueue) {
        call->Complete({CatalogError::Busy, {}});
    } else {
        s_catalog_queue.push_back({std::move(request), s_status.connection_id, call});
    }
    return call;
}
void InitializeServerOrigin() {
    // Even a read from NVS can enter a flash operation that disables the cache.
    const auto stored = ReadSavedServerOrigin();
    auto origin = stored ? *stored : NormalizeServerOrigin(CONFIG_HARNESS_SERVER_URL);
    if (ValidServerOrigin(origin)) {
        ESP_LOGI("Harness", "Server origin: %s (%s)", origin.c_str(),
                 stored ? "saved" : "firmware default");
    } else if (origin.empty()) {
        ESP_LOGI("Harness", "Server origin is empty, connection disabled");
    } else {
        ESP_LOGW("Harness", "Invalid server origin in %s", stored ? "NVS" : "firmware default");
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_server_origin = std::move(origin);
}

std::string ConfiguredServerOrigin() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_server_origin;
}

ServerOriginResult SaveServerOrigin(std::string origin) {
    origin = NormalizeServerOrigin(std::move(origin));
    if (!origin.empty() && !ValidServerOrigin(origin)) return ServerOriginResult::Invalid;
    const auto stored = ReadSavedServerOrigin();
    if (stored && *stored == origin) return ServerOriginResult::Unchanged;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (harness_voice::IsCapturing() || (s_recording && s_recording->Status().Pending())) {
            return ServerOriginResult::RecordingPending;
        }
        s_server_change_in_progress = true;
    }

    nvs_handle_t handle;
    bool saved = false;
    if (nvs_open(kSettingsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        saved = nvs_set_str(handle, kServerOriginKey, origin.c_str()) == ESP_OK &&
                nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    if (saved) return ServerOriginResult::Saved;

    std::lock_guard<std::mutex> lock(s_mutex);
    s_server_change_in_progress = false;
    return ServerOriginResult::StorageError;
}

void StopService(){s_stop=true;}
void StartService() {
    bool expected=false;if(!s_started.compare_exchange_strong(expected,true))return;
    s_stop=false;
    auto config=FirmwareConfig();
    if (config.base_url.empty() || !ValidConfig(config)) {
        ConnectionSnapshot status;
        if (!config.base_url.empty()) status.error = "invalid_config";
        Save(status);
        s_started = false;
        return;
    }
    ConnectionSnapshot s;s.state=ConnectionState::WaitingNetwork;Save(s);
    if(xTaskCreate(Run,"harness",16384,nullptr,2,nullptr)!=pdPASS) {
        s.state=ConnectionState::Stopped;s.error="task_allocation_failed";Save(s);s_started=false;
        ESP_LOGE("Harness","Unable to create client task");
    }
}
}  // namespace harness
