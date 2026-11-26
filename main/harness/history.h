#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct cJSON;

namespace harness {

struct MessagePart {
    std::string id;
    std::string content;
    std::string created_at;
    uint32_t offset = 0;
    bool assistant = false;
    bool ends_message = false;

    uint32_t End() const { return offset + static_cast<uint32_t>(content.size()); }
};

struct HistoryPage {
    std::vector<MessagePart> messages;
    std::string before;
};

struct HistoryEvent {
    enum class Kind { Delta, Completed, Refresh };
    Kind kind = Kind::Refresh;
    MessagePart message;
    uint32_t content_bytes = 0;
};

inline constexpr int kHistoryLimit = 10;

bool ReadHistoryPage(const cJSON* root, const std::string& requested_cursor, HistoryPage& page);
std::optional<HistoryEvent> ReadHistoryEvent(const std::string& json, const std::string& session_id);

enum class MergeResult { Unchanged, Changed, Refresh };
MergeResult MergeHistoryEvent(HistoryPage& page, HistoryEvent event);

// The network worker publishes only events for the open session. Reconnect or
// overflow invalidates any queued deltas and asks the reader for a new snapshot.
class HistoryInbox {
public:
    explicit HistoryInbox(std::string session_id);
    void Publish(const std::string& json);
    void Invalidate();
    std::vector<HistoryEvent> Take();

private:
    const std::string session_id_;
    std::mutex mutex_;
    std::vector<HistoryEvent> events_;
    size_t bytes_ = 0;
    bool invalid_ = false;
};

}  // namespace harness
