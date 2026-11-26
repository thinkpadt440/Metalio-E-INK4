#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct cJSON;

namespace harness {

struct Resource;

enum class ApprovalState { Pending, Approved, Rejected, Expired, Cancelled };
enum class ApprovalDecision { AllowOnce, Reject };

struct ApprovalInfo {
    std::string id, session_id, run_id;
    uint32_t version = 0;
    ApprovalState state = ApprovalState::Pending;
    std::string title, reason, tool, operation, expires_at;
    bool actionable = false;
    bool verified_detail = false;
};

using ApprovalPtr = std::shared_ptr<const ApprovalInfo>;
std::string_view RawJsonMember(std::string_view object, std::string_view key);
ApprovalPtr ReadApproval(const cJSON* json, std::string_view wire = {});

struct ApprovalRequest {
    ApprovalPtr approval;
    ApprovalDecision decision;
};

struct ApprovalResult {
    ApprovalPtr original;
    ApprovalPtr actual;
    std::string error;
};

struct ApprovalView {
    ApprovalPtr info;
    bool submitting = false;
    bool checking = false;
    bool web_only = false;
    std::string error;
};

class ApprovalTracker {
public:
    ApprovalView View(const std::string& id) const;
    std::vector<ApprovalPtr> Pending() const;
    bool TakePendingNotice();
    void Offline();
    void Snapshot(const std::vector<Resource>& resources);
    void Observe(const std::string& event);

    bool Queue(const ApprovalPtr& reviewed, ApprovalDecision decision);
    std::optional<ApprovalRequest> TakeRequest();
    void Finish(ApprovalResult result);
    std::optional<std::string> NextLookup(uint64_t now);
    void LookupResult(const std::string& id, ApprovalPtr result, bool missing);

private:
    struct Entry {
        ApprovalView view;
        uint64_t order = 0;
        uint64_t lookup_after = 0;
        bool notified = false;
    };

    void Apply(ApprovalPtr approval, bool authoritative);
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::optional<ApprovalRequest> queued_;
    uint64_t order_ = 0;
    bool online_ = false;
};

}  // namespace harness
