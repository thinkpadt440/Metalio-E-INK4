#pragma once

#include "approval_tracker.h"
#include "history.h"
#include "run_tracker.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace harness {

class ClientIo;

struct Overview {
    std::optional<uint64_t> sessions, messages, total_tokens, active_days;
    std::string peak_hour;
    std::string favorite_model;
};

struct Session {
    std::string id, title, model_id, model_name, authorization_id, authorization_name, updated_at;
    uint32_t settings_version = 0;
    std::string active_run_id;
};

struct DirectoryOption {
    std::string id, name;
    bool available = false;
};

template <typename Item>
struct CatalogPage {
    std::vector<Item> items;
    std::string next_cursor;
};
using Sessions = CatalogPage<Session>;
using Directory = CatalogPage<DirectoryOption>;

enum class CatalogOperation { Overview, History, Run, Approval, Sessions, Session, Casual, Models, Authorizations, SetModel, SetAuthorization };
enum class CatalogError { None, Offline, Busy, RequestFailed, InvalidResponse, Conflict, CursorExpired, NotFound };
inline constexpr int kRecentSessionLimit = 10;
inline constexpr int kDirectoryPageSize = 3;

struct CatalogRequest {
    CatalogOperation operation;
    std::string key;     // Dashboard range or session ID.
    std::string cursor;
    std::string value;   // Selected model or authorization ID.
    uint32_t settings_version = 0;
};

struct CatalogReply {
    CatalogError error = CatalogError::None;
    std::variant<std::monostate, Overview, Session, Sessions, Directory, HistoryPage, RunInfo, ApprovalPtr> data;
};

class CatalogCall {
public:
    void Complete(CatalogReply reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        reply_ = std::move(reply);
    }
    std::optional<CatalogReply> Take() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(reply_, std::nullopt);
    }
private:
    std::mutex mutex_;
    std::optional<CatalogReply> reply_;
};

// Runs on the network task. The firmware and host tests use the same parser.
CatalogReply FetchCatalog(ClientIo& io, const std::string& device_id, const CatalogRequest& request);
bool IsSessionUpdate(const std::string& event);

}  // namespace harness
