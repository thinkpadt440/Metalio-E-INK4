#include "history.h"

#include "json.h"

#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

namespace harness {
namespace {

const cJSON* Field(const cJSON* root, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(root, name);
}

const char* Text(const cJSON* value) {
    return cJSON_IsString(value) ? value->valuestring : nullptr;
}

bool ReadOffset(const cJSON* item, const char* name, uint32_t& result) {
    const auto* value = Field(item, name);
    if (!cJSON_IsNumber(value)) return false;
    const double number = value->valuedouble;
    if (number < 0 || number > UINT32_MAX || number != std::floor(number)) return false;
    result = static_cast<uint32_t>(number);
    return true;
}

bool ReadId(const cJSON* item, const char* name, std::string& result) {
    const auto* id = Text(Field(item, name));
    if (!id || !*id || std::strlen(id) > 128) return false;
    result = id;
    for (unsigned char ch : result) {
        if (ch <= 32 || ch >= 127) return false;
    }
    return true;
}

bool IsUtf8(const char* text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p) {
        const unsigned char first = *p++;
        if (first < 0x80) continue;
        int continuation;
        if (first >= 0xc2 && first <= 0xdf) continuation = 1;
        else if (first >= 0xe0 && first <= 0xef) continuation = 2;
        else if (first >= 0xf0 && first <= 0xf4) continuation = 3;
        else return false;
        if ((first == 0xe0 && *p < 0xa0) || (first == 0xed && *p >= 0xa0) ||
            (first == 0xf0 && *p < 0x90) || (first == 0xf4 && *p >= 0x90)) return false;
        for (int i = 0; i < continuation; ++i) {
            if ((*p & 0xc0) != 0x80) return false;
            ++p;
        }
    }
    return true;
}

bool ReadPart(const cJSON* item, const char* id_field, const char* text_field, MessagePart& part, bool has_offset = true) {
    const auto* text = Text(Field(item, text_field));
    const auto* time = Text(Field(item, "created_at"));
    if (!text || !time || !*time || std::strlen(time) > 64 ||
        !ReadId(item, id_field, part.id) || (has_offset && !ReadOffset(item, "offset_bytes", part.offset)) ||
        std::strlen(text) > UINT32_MAX - part.offset || !IsUtf8(text)) return false;
    part.content = text;
    part.created_at = time;
    return true;
}

size_t HistoryBytes(const HistoryPage& page) {
    size_t bytes = 0;
    for (const auto& part : page.messages) bytes += part.content.size();
    return bytes;
}

}  // namespace

bool ReadHistoryPage(const cJSON* root, const std::string& requested_cursor, HistoryPage& page) {
    const auto* before = Field(root, "before_cursor");
    if (!cJSON_IsNull(before)) {
        const auto* cursor = Text(before);
        if (!cursor || !*cursor || std::strlen(cursor) > 1024 || requested_cursor == cursor) return false;
        page.before = cursor;
    }
    const auto* items = Field(root, "items");
    if (!cJSON_IsArray(items)) return false;
    const cJSON* item;
    cJSON_ArrayForEach(item, items) {
        MessagePart next;
        const auto* role = Text(Field(item, "role"));
        const auto* ends = Field(item, "ends_message");
        if (!role || (std::strcmp(role, "user") != 0 && std::strcmp(role, "assistant") != 0) ||
            !cJSON_IsBool(ends) || !ReadPart(item, "id", "content", next)) return false;
        next.assistant = std::strcmp(role, "assistant") == 0;
        next.ends_message = cJSON_IsTrue(ends);
        if (!page.messages.empty() && page.messages.back().id == next.id) {
            auto& last = page.messages.back();
            if (last.End() != next.offset || last.ends_message || last.assistant != next.assistant) return false;
            last.content += next.content;
            last.ends_message = next.ends_message;
            continue;
        }
        if (page.messages.size() >= kHistoryLimit ||
            std::any_of(page.messages.begin(), page.messages.end(), [&](const auto& value) { return value.id == next.id; })) {
            return false;
        }
        page.messages.push_back(std::move(next));
    }
    return !page.messages.empty() || page.before.empty();
}

std::optional<HistoryEvent> ReadHistoryEvent(const std::string& json, const std::string& session_id) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(ParseJsonMessage(json.data(), json.size()), cJSON_Delete);
    const auto* session = Text(Field(root.get(), "session_id"));
    const auto* type = Text(Field(root.get(), "type"));
    if (!session || session_id != session || !type) return std::nullopt;
    HistoryEvent event;
    const auto* data = Field(root.get(), "data");
    if (std::strcmp(type, "message.delta") == 0) {
        if (ReadPart(data, "message_id", "text", event.message)) {
            event.kind = HistoryEvent::Kind::Delta;
            event.message.assistant = true;
        }
    } else if (std::strcmp(type, "message.completed") == 0) {
        if (ReadId(data, "message_id", event.message.id) &&
            ReadOffset(data, "content_bytes", event.content_bytes)) event.kind = HistoryEvent::Kind::Completed;
    } else if (std::strcmp(type, "run.accepted") == 0) {
        const auto* source = Text(Field(data, "source"));
        if (!source || std::strcmp(source, "web") != 0) return std::nullopt;
    } else if (std::strcmp(type, "asr.final") == 0) {
        if (ReadPart(data, "message_id", "text", event.message, false)) {
            event.kind = HistoryEvent::Kind::Delta;
            event.message.ends_message = true;
        }
    } else {
        return std::nullopt;
    }
    return event;
}

MergeResult MergeHistoryEvent(HistoryPage& page, HistoryEvent event) {
    if (event.kind == HistoryEvent::Kind::Refresh) return MergeResult::Refresh;
    const auto& incoming = event.message;
    auto existing = std::find_if(page.messages.begin(), page.messages.end(),
                                 [&](const auto& part) { return part.id == incoming.id; });
    if (event.kind == HistoryEvent::Kind::Completed) {
        return existing != page.messages.end() && existing->End() == event.content_bytes
            ? MergeResult::Unchanged : MergeResult::Refresh;
    }
    constexpr size_t kMaxLiveBytes = 2 * kMaxJsonBytes;
    if (existing == page.messages.end()) {
        if (incoming.offset != 0 || page.messages.size() >= 2 * kHistoryLimit ||
            HistoryBytes(page) + incoming.content.size() > kMaxLiveBytes ||
            (!page.messages.empty() && incoming.created_at < page.messages.back().created_at)) {
            return MergeResult::Refresh;
        }
        page.messages.push_back(std::move(event.message));
        return MergeResult::Changed;
    }
    if (existing->assistant != incoming.assistant || incoming.offset > existing->End()) return MergeResult::Refresh;
    const auto overlap_start = std::max(incoming.offset, existing->offset);
    const auto overlap_end = std::min(incoming.End(), existing->End());
    if (overlap_start < overlap_end &&
        incoming.content.compare(overlap_start - incoming.offset, overlap_end - overlap_start,
                                 existing->content, overlap_start - existing->offset,
                                 overlap_end - overlap_start) != 0) return MergeResult::Refresh;
    if (incoming.End() <= existing->End()) return MergeResult::Unchanged;
    const auto appended = incoming.End() - existing->End();
    if (HistoryBytes(page) + appended > kMaxLiveBytes) return MergeResult::Refresh;
    existing->content.append(incoming.content, existing->End() - incoming.offset, appended);
    return MergeResult::Changed;
}

HistoryInbox::HistoryInbox(std::string session_id) : session_id_(std::move(session_id)) {}

void HistoryInbox::Publish(const std::string& json) {
    auto event = ReadHistoryEvent(json, session_id_);
    if (!event) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (invalid_) return;
    const auto size = event->message.content.size();
    if (events_.size() >= 32 || bytes_ + size > 16 * 1024) {
        events_.clear();
        bytes_ = 0;
        invalid_ = true;
        return;
    }
    bytes_ += size;
    events_.push_back(std::move(*event));
}

void HistoryInbox::Invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
    bytes_ = 0;
    invalid_ = true;
}

std::vector<HistoryEvent> HistoryInbox::Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    bytes_ = 0;
    if (std::exchange(invalid_, false)) return {HistoryEvent{}};
    return std::exchange(events_, {});
}

}  // namespace harness
