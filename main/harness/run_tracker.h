#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct cJSON;

namespace harness {

struct Resource;

enum class RunState { Queued, Transcribing, Running, WaitingApproval, Cancelling, Completed, Failed, Cancelled };

struct RunInfo {
    std::string id;
    std::string session_id;
    RunState state = RunState::Queued;
    uint32_t version = 0;
    std::string progress;
    std::string error;

    bool Finished() const {
        return state == RunState::Completed || state == RunState::Failed || state == RunState::Cancelled;
    }
};

struct CancelResult {
    std::string session_id;
    std::string run_id;
    std::string error;
};

struct RunView {
    RunInfo run;
    bool confirming = false;
    bool cancelling = false;
    std::string cancel_error;
};

std::optional<RunInfo> ReadRun(const cJSON* object);

class RunTracker {
public:
    RunView View(const std::string& session) const;
    void Hint(const std::string& session, const std::string& run);
    void Snapshot(const std::vector<Resource>& resources);
    void Observe(const std::string& event);

    bool QueueCancel(const std::string& session, const std::string& run);
    std::optional<RunInfo> TakeCancel();
    void FinishCancel(const CancelResult& result);

    std::optional<RunInfo> NextLookup(uint64_t now);
    void LookupResult(const RunInfo& requested, const std::optional<RunInfo>& run, bool missing);

    uint32_t IdRevision() const;
    std::string SaveIds(const std::string& identity) const;
    void RestoreIds(const std::string& identity, const std::string& data);

private:
    struct Entry {
        RunView view;
        uint64_t lookup_after = 0;
        uint64_t touched = 0;
        std::string refused_cancel;
        bool accepted_cancel = false;
    };

    Entry* FindOrAdd(const std::string& session);
    void SetHint(const std::string& session, const std::string& run);
    void Apply(RunInfo info, bool authoritative);
    void RememberFinished(const std::string& id);
    bool WasFinished(const std::string& id) const;

    mutable std::mutex mutex_;
    std::map<std::string, Entry> sessions_;
    std::vector<std::string> finished_;
    std::optional<RunInfo> cancel_;
    uint64_t touches_ = 0;
    uint32_t id_revision_ = 0;
};

}  // namespace harness
