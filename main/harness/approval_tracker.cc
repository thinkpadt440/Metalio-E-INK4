#include "approval_tracker.h"

#include "client.h"
#include "json.h"

#include <algorithm>
#include <cmath>
#include <cJSON.h>
#include <utility>

namespace harness {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr size_t kMaxApprovals = 64;
constexpr size_t kMaxCachedBytes = 96 * 1024;

const cJSON* Field(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

std::string Text(const cJSON* object, const char* key) {
    const auto* value = Field(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

bool Id(const std::string& value) {
    return !value.empty() && value != "." && value != ".." && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 32 && c < 127; });
}

size_t Bytes(const ApprovalInfo& approval) {
    return approval.title.size() + approval.reason.size() + approval.tool.size() +
           approval.operation.size() + approval.expires_at.size();
}

bool SameTarget(const ApprovalInfo& a, const ApprovalInfo& b) {
    return a.id == b.id && a.session_id == b.session_id && a.run_id == b.run_id;
}

std::string CompactOperation(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    bool quoted = false;
    bool escaped = false;
    for (char ch : value) {
        if (!quoted && (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t')) continue;
        output += ch;
        if (!escaped && ch == '"') quoted = !quoted;
        escaped = quoted && !escaped && ch == '\\';
    }
    return output;
}

Json Parse(const std::string& text) {
    return Json(ParseJsonMessage(text.data(), text.size()), cJSON_Delete);
}
}  // namespace

std::string_view RawJsonMember(std::string_view object, std::string_view key) {
    // Input has already passed cJSON parsing. Preserve value lexemes for the
    // review screen, including integers that cannot round-trip through double.
    const auto whitespace = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    size_t position = 0;
    const auto skip_space = [&] {
        while (position < object.size() && whitespace(object[position])) ++position;
    };
    const auto skip_string = [&] {
        ++position;
        while (position < object.size()) {
            const char ch = object[position++];
            if (ch == '"') return;
            if (ch == '\\' && position < object.size()) ++position;
        }
    };
    skip_space();
    if (position == object.size() || object[position++] != '{') return {};
    std::string_view match;
    while (position < object.size()) {
        skip_space();
        if (position == object.size() || object[position] != '"') break;
        const size_t key_start = position;
        skip_string();
        const auto name = object.substr(key_start + 1, position - key_start - 2);
        skip_space();
        if (position == object.size() || object[position++] != ':') return {};
        skip_space();
        const size_t value_start = position;
        int depth = 0;
        while (position < object.size()) {
            const char ch = object[position];
            if (ch == '"') {
                skip_string();
                continue;
            }
            if (!depth && (ch == ',' || ch == '}')) break;
            if (ch == '{' || ch == '[') ++depth;
            else if (ch == '}' || ch == ']') --depth;
            ++position;
        }
        if (name == key) {
            if (!match.empty()) return {};
            size_t end = position;
            while (end > value_start && whitespace(object[end - 1])) --end;
            match = object.substr(value_start, end - value_start);
        }
        if (position == object.size() || object[position++] != ',') break;
    }
    return match;
}

ApprovalPtr ReadApproval(const cJSON* json, std::string_view wire) {
    auto approval = std::make_shared<ApprovalInfo>();
    approval->id = Text(json, "approval_id");
    approval->session_id = Text(json, "session_id");
    approval->run_id = Text(json, "run_id");
    const auto* version = Field(json, "approval_version");
    if (!Id(approval->id) || !Id(approval->session_id) || !Id(approval->run_id) ||
        !cJSON_IsNumber(version) || version->valuedouble < 1 || version->valuedouble > UINT32_MAX ||
        std::floor(version->valuedouble) != version->valuedouble) return {};
    approval->version = static_cast<uint32_t>(version->valuedouble);
    const auto status = Text(json, "status");
    if (status == "pending") approval->state = ApprovalState::Pending;
    else if (status == "approved") approval->state = ApprovalState::Approved;
    else if (status == "rejected") approval->state = ApprovalState::Rejected;
    else if (status == "expired") approval->state = ApprovalState::Expired;
    else if (status == "cancelled") approval->state = ApprovalState::Cancelled;
    else return {};

    if (approval->state == ApprovalState::Pending) {
        approval->title = Text(json, "title");
        approval->reason = Text(json, "reason");
        approval->tool = Text(json, "tool_name");
        approval->expires_at = Text(json, "expires_at");
        const auto* operation = Field(json, "operation");
        const auto raw = RawJsonMember(wire, "operation");
        unsigned operations = 0;
        const cJSON* item;
        cJSON_ArrayForEach(item, json) {
            if (item->string && std::string_view(item->string) == "operation") ++operations;
        }
        approval->verified_detail = !wire.empty() && operations == 1 && !raw.empty();
        if (approval->verified_detail && cJSON_IsObject(operation) && raw.front() == '{') {
            approval->operation = CompactOperation(raw);
        }
        approval->actionable = cJSON_IsTrue(Field(json, "device_actionable")) &&
            !approval->title.empty() && !approval->reason.empty() && !approval->tool.empty() &&
            !approval->operation.empty() && !approval->expires_at.empty() && Bytes(*approval) <= kMaxJsonBytes;
    }
    return approval;
}

ApprovalView ApprovalTracker::View(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(id);
    return found == entries_.end() ? ApprovalView{} : found->second.view;
}

std::vector<ApprovalPtr> ApprovalTracker::Pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<uint64_t, ApprovalPtr>> ordered;
    if (online_) {
        for (const auto& pair : entries_) {
            const auto& entry = pair.second;
            if (entry.view.info->state == ApprovalState::Pending && !entry.view.checking)
                ordered.emplace_back(entry.order, entry.view.info);
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    std::vector<ApprovalPtr> pending;
    pending.reserve(ordered.size());
    for (auto& item : ordered) pending.push_back(std::move(item.second));
    return pending;
}

bool ApprovalTracker::TakePendingNotice() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_) return false;

    bool notify = false;
    for (auto& pair : entries_) {
        auto& entry = pair.second;
        if (entry.notified || entry.view.checking ||
            entry.view.info->state != ApprovalState::Pending) continue;
        entry.notified = true;
        notify = true;
    }
    return notify;
}

void ApprovalTracker::Apply(ApprovalPtr approval, bool authoritative) {
    if (!approval) return;
    auto found = entries_.find(approval->id);
    if (found != entries_.end()) {
        auto& entry = found->second;
        const auto& prior = *entry.view.info;
        if (!SameTarget(prior, *approval) || approval->version < prior.version ||
            (prior.state != ApprovalState::Pending && approval->state == ApprovalState::Pending)) return;
        const bool detail_changed = prior.verified_detail && approval->verified_detail &&
            (prior.operation != approval->operation || prior.actionable != approval->actionable ||
             prior.title != approval->title || prior.reason != approval->reason || prior.tool != approval->tool);
        if (prior.state == ApprovalState::Pending && approval->state == ApprovalState::Pending) {
            if (detail_changed) entry.view.web_only = true;
            if (!approval->verified_detail || (prior.version == approval->version && prior.verified_detail)) {
                if (authoritative && approval->verified_detail) {
                    entry.view.checking = false;
                    entry.view.submitting = false;
                }
                return;
            }
        } else if (prior.version == approval->version) {
            return;
        }
    } else if (entries_.size() >= kMaxApprovals) {
        auto victim = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.view.info->state == ApprovalState::Pending) continue;
            if (victim == entries_.end() || it->second.order < victim->second.order) victim = it;
        }
        if (victim == entries_.end()) return;
        entries_.erase(victim);
    }

    auto& entry = entries_[approval->id];
    if (!entry.order) entry.order = ++order_;
    size_t bytes = Bytes(*approval);
    for (const auto& pair : entries_) {
        if (pair.first != approval->id && pair.second.view.info) bytes += Bytes(*pair.second.view.info);
    }
    if (bytes > kMaxCachedBytes - 1024) {
        auto compact = std::make_shared<ApprovalInfo>(*approval);
        compact->operation.clear();
        compact->reason.clear();
        compact->actionable = false;
        if (compact->title.size() > 96) compact->title = "待审批操作";
        if (compact->tool.size() > 96) compact->tool.clear();
        if (compact->expires_at.size() > 96) compact->expires_at.clear();
        approval = std::move(compact);
    }
    const bool unverified = approval->state == ApprovalState::Pending && !approval->verified_detail;
    const bool web_only = approval->state == ApprovalState::Pending && entry.view.web_only;
    entry.view = {std::move(approval), false, unverified, web_only, {}};
}

void ApprovalTracker::Offline() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_) return;
    online_ = false;
    queued_.reset();
    for (auto& pair : entries_) {
        auto& view = pair.second.view;
        if (view.info->state != ApprovalState::Pending) continue;
        view.checking = true;
        view.submitting = false;
    }
}

void ApprovalTracker::Snapshot(const std::vector<Resource>& resources) {
    std::lock_guard<std::mutex> lock(mutex_);
    online_ = true;
    for (auto& pair : entries_) {
        auto& entry = pair.second;
        if (entry.view.info->state != ApprovalState::Pending) continue;
        entry.view.checking = true;
        entry.view.submitting = false;
        entry.lookup_after = 0;
    }
    for (const auto& resource : resources) {
        if (resource.kind != "approval") continue;
        auto json = Parse(resource.json);
        Apply(ReadApproval(json.get()), true);
    }
}

void ApprovalTracker::Observe(const std::string& event) {
    auto json = Parse(event);
    const auto type = Text(json.get(), "type");
    if (type != "approval.requested" && type != "approval.resolved") return;
    auto approval = ReadApproval(Field(json.get(), "data"), RawJsonMember(event, "data"));
    if (!approval || approval->session_id != Text(json.get(), "session_id") ||
        approval->run_id != Text(json.get(), "run_id") ||
        (type == "approval.requested") != (approval->state == ApprovalState::Pending)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Apply(std::move(approval), false);
}

bool ApprovalTracker::Queue(const ApprovalPtr& reviewed, ApprovalDecision decision) {
    if (!reviewed) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(reviewed->id);
    if (!online_ || queued_ || found == entries_.end()) return false;
    auto& view = found->second.view;
    if (view.info != reviewed || view.info->state != ApprovalState::Pending || view.submitting || view.checking ||
        (decision == ApprovalDecision::AllowOnce && (!view.info->actionable || view.web_only))) return false;
    view.submitting = true;
    view.error.clear();
    queued_ = ApprovalRequest{reviewed, decision};
    return true;
}

std::optional<ApprovalRequest> ApprovalTracker::TakeRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto request = std::exchange(queued_, std::nullopt);
    if (!request || !online_) return {};
    const auto found = entries_.find(request->approval->id);
    if (found == entries_.end() || found->second.view.info != request->approval) return {};
    auto& view = found->second.view;
    if (!view.submitting || view.checking ||
        (request->decision == ApprovalDecision::AllowOnce && view.web_only)) {
        view.submitting = false;
        return {};
    }
    return request;
}

void ApprovalTracker::Finish(ApprovalResult result) {
    if (!result.original) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(result.original->id);
    if (found == entries_.end() || found->second.view.info != result.original) return;
    auto& entry = found->second;
    auto& view = entry.view;
    if (view.info->state != ApprovalState::Pending) return;
    view.submitting = false;
    if (result.actual && SameTarget(*result.original, *result.actual) &&
        result.actual->version >= result.original->version &&
        (result.actual->state == ApprovalState::Pending || result.actual->version > result.original->version)) {
        const bool can_resolve = result.actual->state != ApprovalState::Pending || result.actual->verified_detail;
        Apply(std::move(result.actual), true);
        view.checking = !can_resolve;
        if (view.checking) entry.lookup_after = 0;
    } else {
        view.checking = true;
        entry.lookup_after = 0;
    }
    if (view.info->state == ApprovalState::Pending) {
        view.error = std::move(result.error);
        if (view.error == "approval_requires_web") view.web_only = true;
    }
}

std::optional<std::string> ApprovalTracker::NextLookup(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_) return {};
    for (auto& pair : entries_) {
        auto& entry = pair.second;
        if (entry.view.checking && now >= entry.lookup_after) {
            entry.lookup_after = now + 4000;
            return pair.first;
        }
    }
    return {};
}

void ApprovalTracker::LookupResult(const std::string& id, ApprovalPtr result, bool missing) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(id);
    if (found == entries_.end() || !found->second.view.checking) return;
    if (missing) {
        entries_.erase(found);
    } else if (result && result->id == id) {
        Apply(std::move(result), true);
    }
}

}  // namespace harness
