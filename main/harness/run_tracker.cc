#include "run_tracker.h"

#include "client.h"
#include "json.h"

#include <algorithm>
#include <cmath>
#include <cJSON.h>
#include <memory>
#include <utility>

namespace harness {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr size_t kMaxTracked = 16;
constexpr size_t kFinishedCapacity = 32;
constexpr size_t kMaxPersisted = 12;

const cJSON* Field(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}
std::string Text(const cJSON* object, const char* key) {
    const auto* item = Field(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
bool ValidId(const std::string& id) {
    return !id.empty() && id.size() <= 128 && id != "." && id != ".." &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) { return c > 32 && c < 127; });
}
uint32_t Version(const cJSON* object) {
    const auto* item = Field(object, "state_version");
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 || item->valuedouble > UINT32_MAX ||
        std::floor(item->valuedouble) != item->valuedouble) return 0;
    return static_cast<uint32_t>(item->valuedouble);
}
Json Parse(const std::string& data) {
    return Json(ParseJsonMessage(data.data(), data.size()), cJSON_Delete);
}
std::string LimitText(std::string text) {
    constexpr size_t kMaxDisplayBytes = 144;
    if (text.size() <= kMaxDisplayBytes) return text;
    size_t end = kMaxDisplayBytes;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    text.resize(end);
    text += "…";
    return text;
}
}  // namespace

std::optional<RunInfo> ReadRun(const cJSON* object) {
    RunInfo run;
    run.id = Text(object, "run_id");
    run.session_id = Text(object, "session_id");
    run.version = Version(object);
    if (!ValidId(run.id) || !ValidId(run.session_id) || !run.version) return {};
    const auto state = Text(object, "status");
    if (state == "queued") run.state = RunState::Queued;
    else if (state == "transcribing") run.state = RunState::Transcribing;
    else if (state == "running") run.state = RunState::Running;
    else if (state == "waiting_approval") run.state = RunState::WaitingApproval;
    else if (state == "cancelling") run.state = RunState::Cancelling;
    else if (state == "completed") run.state = RunState::Completed;
    else if (state == "failed") run.state = RunState::Failed;
    else if (state == "cancelled") run.state = RunState::Cancelled;
    else return {};
    run.progress = LimitText(Text(object, "last_progress"));
    run.error = LimitText(Text(Field(object, "error"), "code"));
    return run;
}

RunTracker::Entry* RunTracker::FindOrAdd(const std::string& session) {
    auto found = sessions_.find(session);
    if (found != sessions_.end()) return &found->second;
    if (sessions_.size() >= kMaxTracked) {
        auto victim = sessions_.end();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if ((it->second.view.run.Finished() || it->second.view.run.id.empty()) &&
                (victim == sessions_.end() || it->second.touched < victim->second.touched)) victim = it;
        }
        if (victim == sessions_.end()) return nullptr;
        sessions_.erase(victim);
    }
    return &sessions_[session];
}

bool RunTracker::WasFinished(const std::string& id) const {
    return std::find(finished_.begin(), finished_.end(), id) != finished_.end();
}
void RunTracker::RememberFinished(const std::string& id) {
    if (WasFinished(id)) return;
    if (finished_.size() == kFinishedCapacity) finished_.erase(finished_.begin());
    finished_.push_back(id);
}

void RunTracker::SetHint(const std::string& session, const std::string& run) {
    if (!ValidId(session) || !ValidId(run) || WasFinished(run)) return;
    auto* entry = FindOrAdd(session);
    if (!entry || entry->view.run.id == run ||
        (!entry->view.run.id.empty() && !entry->view.run.Finished())) return;
    entry->view = {{run, session, RunState::Queued, 0, {}, {}}, true, false, {}};
    entry->refused_cancel.clear();
    entry->touched = ++touches_;
    entry->lookup_after = 0;
    entry->accepted_cancel = false;
    ++id_revision_;
}

void RunTracker::Hint(const std::string& session, const std::string& run) {
    std::lock_guard<std::mutex> lock(mutex_);
    SetHint(session, run);
}

void RunTracker::Apply(RunInfo info, bool authoritative) {
    const bool finished = info.Finished();
    if (!finished && WasFinished(info.id)) return;
    auto* entry = FindOrAdd(info.session_id);
    if (!entry) return;
    auto& view = entry->view;
    const bool same = view.run.id == info.id;
    if (!view.run.id.empty() && !same && finished) {
        RememberFinished(info.id);
        return;
    }
    if (same && (view.run.version > info.version || (view.run.Finished() && !finished))) return;
    if (same && view.run.version == info.version) {
        if (authoritative && !entry->accepted_cancel) {
            view.confirming = false;
            if (!info.progress.empty()) view.run.progress = std::move(info.progress);
        }
        return;
    }
    const bool previous_active = !view.run.id.empty() && !view.run.Finished();
    if (!same) {
        view = {};
        entry->refused_cancel.clear();
        entry->accepted_cancel = false;
    }
    if (finished) RememberFinished(info.id);
    if (previous_active != !finished || (!same && !finished)) ++id_revision_;
    view.run = std::move(info);
    if (finished || view.run.state == RunState::Cancelling) {
        view.cancelling = false;
        view.cancel_error.clear();
        entry->accepted_cancel = false;
    }
    view.confirming = entry->accepted_cancel;
    entry->touched = ++touches_;
}

RunView RunTracker::View(const std::string& session) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = sessions_.find(session);
    return entry == sessions_.end() ? RunView{} : entry->second.view;
}

void RunTracker::Snapshot(const std::vector<Resource>& resources) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : sessions_) {
        auto& entry = pair.second;
        if (!entry.view.run.id.empty() && !entry.view.run.Finished()) {
            entry.view.confirming = true;
            entry.lookup_after = 0;
        }
    }
    for (const auto& resource : resources) {
        if (resource.kind != "run") continue;
        auto json = Parse(resource.json);
        if (auto run = ReadRun(json.get())) Apply(std::move(*run), true);
    }
}

void RunTracker::Observe(const std::string& event) {
    const auto json = Parse(event);
    const auto type = Text(json.get(), "type");
    if (type.rfind("run.", 0) != 0) return;
    const auto* data = Field(json.get(), "data");
    std::lock_guard<std::mutex> lock(mutex_);
    if (type == "run.progress") {
        const auto found = sessions_.find(Text(json.get(), "session_id"));
        if (found == sessions_.end()) return;
        auto& view = found->second.view;
        if (!view.confirming && !view.run.Finished() && view.run.id == Text(json.get(), "run_id") &&
            view.run.version == Version(data)) view.run.progress = LimitText(Text(data, "text"));
    } else if (auto run = ReadRun(data)) {
        Apply(std::move(*run), false);
    }
}

bool RunTracker::QueueCancel(const std::string& session, const std::string& run) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(session);
    if (found == sessions_.end() || run.empty() || cancel_) return false;
    auto& entry = found->second;
    auto& view = entry.view;
    if (view.run.id != run || view.run.Finished() || view.run.state == RunState::Cancelling ||
        view.confirming || view.cancelling || entry.refused_cancel == run) return false;
    view.cancelling = true;
    view.cancel_error.clear();
    cancel_ = view.run;
    return true;
}

std::optional<RunInfo> RunTracker::TakeCancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto value = std::exchange(cancel_, std::nullopt);
    if (!value) return {};
    const auto found = sessions_.find(value->session_id);
    if (found == sessions_.end()) return {};
    const auto& view = found->second.view;
    // A terminal event can arrive while the UI request is queued.
    if (view.run.id != value->id || view.run.Finished() || view.run.state == RunState::Cancelling) return {};
    return value;
}

void RunTracker::FinishCancel(const CancelResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(result.session_id);
    if (found == sessions_.end()) return;
    auto& entry = found->second;
    auto& view = entry.view;
    if (view.run.id != result.run_id || view.run.Finished() || view.run.state == RunState::Cancelling) return;
    entry.accepted_cancel = result.error.empty();
    view.cancelling = entry.accepted_cancel;
    view.cancel_error = result.error;
    if (result.error == "cancel_not_supported") {
        entry.refused_cancel = result.run_id;
    } else {
        view.confirming = true;
        entry.lookup_after = 0;
    }
}

std::optional<RunInfo> RunTracker::NextLookup(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : sessions_) {
        auto& entry = pair.second;
        if ((entry.view.confirming || entry.view.run.state == RunState::Cancelling) &&
            now >= entry.lookup_after) {
            entry.lookup_after = now + 5000;
            return entry.view.run;
        }
    }
    return {};
}

void RunTracker::LookupResult(const RunInfo& requested, const std::optional<RunInfo>& run, bool missing) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(requested.session_id);
    if (found == sessions_.end() || found->second.view.run.id != requested.id) return;
    auto& view = found->second.view;
    if (run && run->id == requested.id && run->session_id == requested.session_id) {
        Apply(*run, true);
    } else if (missing && (view.confirming || view.run.state == RunState::Cancelling)) {
        RememberFinished(requested.id);
        view = {};
        view.cancel_error = "resource_not_found";
        found->second.accepted_cancel = false;
        ++id_revision_;
    }
}

uint32_t RunTracker::IdRevision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return id_revision_;
}

std::string RunTracker::SaveIds(const std::string& identity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json json(cJSON_CreateObject(), cJSON_Delete);
    if (!json || !cJSON_AddStringToObject(json.get(), "target", identity.c_str())) return {};
    auto* items = cJSON_AddArrayToObject(json.get(), "active");
    if (!items) return {};
    size_t count = 0;
    for (const auto& pair : sessions_) {
        const auto& run = pair.second.view.run;
        if (run.id.empty() || run.Finished()) continue;
        if (++count > kMaxPersisted) break;
        auto* entry = cJSON_CreateObject();
        if (!entry) return {};
        cJSON_AddItemToArray(items, entry);
        if (!cJSON_AddStringToObject(entry, "session_id", pair.first.c_str()) ||
            !cJSON_AddStringToObject(entry, "run_id", run.id.c_str())) return {};
    }
    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(json.get()), cJSON_free);
    return text ? text.get() : "";
}

void RunTracker::RestoreIds(const std::string& identity, const std::string& data) {
    const auto json = Parse(data);
    if (Text(json.get(), "target") != identity) return;
    const auto* items = Field(json.get(), "active");
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > kMaxPersisted) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const cJSON* item;
    cJSON_ArrayForEach(item, items) {
        SetHint(Text(item, "session_id"), Text(item, "run_id"));
    }
}

}  // namespace harness
