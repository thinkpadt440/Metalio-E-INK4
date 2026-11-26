#include "chat_reader.h"

#include "harness/service.h"
#include "src/misc/lv_text_private.h"

#include <algorithm>

namespace harness {
namespace {

constexpr size_t kMaxReturnCursors = 24;
constexpr uint32_t kPaintIntervalMs = 1000;
constexpr uint32_t kRecoveryIntervalMs = 2000;

bool Adjacent(const HistoryPage& older, const HistoryPage& newer) {
    if (older.messages.empty() || newer.messages.empty()) return true;
    const auto& end = older.messages.back();
    const auto& start = newer.messages.front();
    if (end.id != start.id) return end.ends_message && start.offset == 0;
    return !end.ends_message && end.End() == start.offset && end.assistant == start.assistant;
}

}  // namespace

void ChatReader::Bind(const std::string& session_id) {
    if (session_id_ == session_id) return;
    const int width = width_;
    const int height = height_;
    const int spacing = line_space_;
    const auto* font = font_;
    const auto* role_font = role_font_;
    *this = ChatReader{};
    SetLayout(width, height, font, role_font, spacing);
    session_id_ = session_id;
    if (!session_id.empty()) {
        inbox_ = SubscribeHistory(session_id);
        needs_refresh_ = true;
    }
}

void ChatReader::SetLayout(int width, int height, const lv_font_t* font, const lv_font_t* role_font,
                           int line_space) {
    width_ = width;
    height_ = height;
    font_ = font;
    role_font_ = role_font;
    line_space_ = line_space;
}

int ChatReader::Baseline() const {
    return std::max(font_->line_height - font_->base_line,
                    role_font_->line_height - role_font_->base_line);
}

int ChatReader::RowHeight() const {
    return Baseline() + std::max(font_->base_line, role_font_->base_line) + line_space_;
}

void ChatReader::Resize(int height) {
    if (height_ == height) return;
    height_ = height;
    if (!screens_.empty()) Paginate(screens_[screen_], following_);
}

size_t ChatReader::NextLine(size_t position, const RolePrefix*& prefix) const {
    const auto role = std::lower_bound(roles_.begin(), roles_.end(), position,
        [](const RolePrefix& item, size_t offset) { return item.start < offset; });
    prefix = role != roles_.end() && role->start == position ? &*role : nullptr;
    const size_t body = position + (prefix ? prefix->length : 0);
    const int available = std::max(1, width_ - (prefix ? prefix->width : 0));
    lv_text_attributes_t attributes{0, 0, available, LV_TEXT_FLAG_NONE};
    return body + lv_text_get_next_line(text_.c_str() + body, text_.size() - body,
                                         font_, nullptr, &attributes);
}

void ChatReader::SetThinkingFor(const std::string& transcript_id) {
    if (thinking_after_ == transcript_id) return;
    thinking_after_ = transcript_id;
    if (following_ && loaded_) {
        Render(latest_, true);
        dirty_ = false;
    }
}

void ChatReader::Render(const HistoryPage& page, bool last_screen, size_t anchor) {
    shown_ = page;
    text_.clear();
    roles_.clear();
    showing_thinking_ = false;
    last_render_ = lv_tick_get();
    const lv_text_attributes_t attributes{0, 0, width_, LV_TEXT_FLAG_NONE};
    const auto append = [&](const char* role, const std::string& content) {
        if (!text_.empty()) {
            const size_t end = text_.find_last_not_of("\r\n");
            if (end != std::string::npos) text_.resize(end + 1);
            text_ += '\n';
        }
        const auto length = std::char_traits<char>::length(role);
        roles_.push_back({text_.size(), length, lv_text_get_width(role, length, role_font_, &attributes)});
        text_ += role;
        text_ += content;
    };
    for (const auto& message : page.messages) {
        const char* role = message.assistant ? (message.offset ? "助手（续）：" : "助手：")
                                             : (message.offset ? "我（续）：" : "我：");
        append(role, message.content);
    }
    if (following_ && !thinking_after_.empty() && !page.messages.empty()) {
        const auto& last = page.messages.back();
        if (!last.assistant && last.ends_message && last.id == thinking_after_) {
            append("助手：", "智能体思考中…");
            showing_thinking_ = true;
        }
    }
    Paginate(anchor, last_screen);
}

void ChatReader::Paginate(size_t anchor, bool last_screen) {
    screens_.clear();
    screen_ = 0;
    if (!text_.empty() && font_) {
        screens_.push_back(0);
        const int rows = std::max(1, (height_ + line_space_) / RowHeight());
        size_t position = 0;
        int lines = 0;
        while (position < text_.size()) {
            const RolePrefix* prefix;
            const size_t next = NextLine(position, prefix);
            if (next <= position) break;
            position = next;
            if (++lines == rows && position < text_.size()) {
                screens_.push_back(position);
                lines = 0;
            }
        }
        screen_ = last_screen ? screens_.size() - 1 :
            static_cast<size_t>(std::upper_bound(screens_.begin(), screens_.end(), anchor) - screens_.begin() - 1);
    }
    UpdateDisplayedText();
}

void ChatReader::UpdateDisplayedText() {
    displayed_.clear();
    if (screens_.empty()) {
        if (loaded_) displayed_.push_back({"", "暂无消息", 0});
        return;
    }
    const size_t end = screen_ + 1 < screens_.size() ? screens_[screen_ + 1] : text_.size();
    size_t position = screens_[screen_];
    while (position < end) {
        const RolePrefix* prefix;
        const size_t next = NextLine(position, prefix);
        if (next <= position) break;
        const size_t body = position + (prefix ? prefix->length : 0);
        size_t body_end = next;
        while (body_end > body && (text_[body_end - 1] == '\n' || text_[body_end - 1] == '\r')) --body_end;
        displayed_.push_back({text_.substr(position, body - position),
                              text_.substr(body, body_end - body), prefix ? prefix->width : 0});
        position = next;
    }
    while (!displayed_.empty() && displayed_.back().role.empty() && displayed_.back().text.empty()) {
        displayed_.pop_back();
    }
}

const char* ChatReader::Status() const {
    if (error_ == CatalogError::CursorExpired) return "历史已更新，请回到最新";
    if (error_ != CatalogError::None) return loaded_ ? "读取失败，按翻页键或点最新重试" : "消息读取失败";
    if (request_ && archive_direction_ != 0) return "正在读取历史…";
    if (!loaded_) return ServiceReady() ? "正在读取消息…" : "等待 Harness 连接";
    return "";
}

const char* ChatReader::LatestButton() const {
    if (unread_) return "有新消息";
    if (error_ != CatalogError::None && !request_) return loaded_ ? "最新" : "重试消息";
    return following_ ? "" : "最新";
}

void ChatReader::Request(const std::string& cursor) {
    if (request_ || session_id_.empty() || !ServiceReady()) return;
    requested_cursor_ = cursor;
    error_ = CatalogError::None;
    if (archive_direction_ == 0) {
        attempted_ = true;
        last_fetch_ = lv_tick_get();
        needs_refresh_ = false;
    }
    request_ = SubmitCatalog({CatalogOperation::History, session_id_, cursor, {}, 0});
}

void ChatReader::TakeReply() {
    if (!request_) return;
    auto reply = request_->Take();
    if (!reply) return;
    request_.reset();
    const int direction = std::exchange(archive_direction_, 0);
    error_ = reply->error;
    auto* page = std::get_if<HistoryPage>(&reply->data);
    if (!page) {
        if (direction == 0 && (loaded_ || error_ == CatalogError::Offline)) needs_refresh_ = true;
        return;
    }
    if (direction == 0) {
        loaded_ = true;
        if (!following_ && !page->messages.empty()) {
            const auto& last = page->messages.back();
            unread_ |= latest_.messages.empty() || last.id != latest_.messages.back().id ||
                       last.End() > latest_.messages.back().End();
        }
        latest_ = std::move(*page);
        dirty_ = true;
        return;
    }

    if (!(direction < 0 ? Adjacent(*page, shown_) : Adjacent(shown_, *page))) {
        error_ = CatalogError::CursorExpired;
        return;
    }
    if (direction < 0) {
        if (newer_.size() == kMaxReturnCursors) {
            if (newer_.front().empty()) origin_ = {};
            newer_.pop_front();
        }
        newer_.push_back(cursor_);
    } else {
        newer_.pop_back();
    }
    cursor_ = std::move(requested_cursor_);
    Render(*page, direction < 0);
}

void ChatReader::ApplyEvents() {
    if (!inbox_ || (request_ && archive_direction_ == 0)) return;
    auto events = inbox_->Take();
    for (auto& event : events) {
        if (!loaded_ || needs_refresh_) {
            needs_refresh_ = true;
            break;
        }
        switch (MergeHistoryEvent(latest_, std::move(event))) {
            case MergeResult::Refresh: needs_refresh_ = true; break;
            case MergeResult::Changed: dirty_ = true; unread_ |= !following_; break;
            case MergeResult::Unchanged: break;
        }
    }
}

void ChatReader::Update() {
    TakeReply();
    ApplyEvents();
    if (needs_refresh_ && (!attempted_ || lv_tick_elaps(last_fetch_) >= kRecoveryIntervalMs)) Request("");
    if (following_ && dirty_ && (screens_.empty() || lv_tick_elaps(last_render_) >= kPaintIntervalMs)) {
        Render(latest_, true);
        dirty_ = false;
    }
}

void ChatReader::EnterArchive() {
    if (!following_) return;
    following_ = false;
    origin_ = shown_;
    cursor_.clear();
    newer_.clear();
    if (showing_thinking_) Render(origin_, false, screens_[screen_]);
}

void ChatReader::Turn(int direction) {
    if (!loaded_ || screens_.empty()) return;
    if (direction < 0) {
        if (screen_ > 0) {
            --screen_;
            UpdateDisplayedText();
            EnterArchive();
        } else if (!request_ && ServiceReady() && !shown_.before.empty() && error_ != CatalogError::CursorExpired) {
            EnterArchive();
            archive_direction_ = -1;
            Request(shown_.before);
        }
    } else if (screen_ + 1 < screens_.size()) {
        ++screen_;
        if (screen_ + 1 == screens_.size() && cursor_.empty() && !unread_) ShowLatest();
        else UpdateDisplayedText();
    } else if (!following_ && !request_) {
        if (newer_.empty() || error_ == CatalogError::CursorExpired) {
            ShowLatest();
        } else if (newer_.back().empty()) {
            newer_.pop_back();
            cursor_.clear();
            Render(origin_, false);
        } else if (ServiceReady()) {
            archive_direction_ = 1;
            Request(newer_.back());
        }
    }
}

void ChatReader::ShowLatest() {
    const bool reload = error_ != CatalogError::None || needs_refresh_;
    if (archive_direction_ != 0) request_.reset();
    archive_direction_ = 0;
    following_ = true;
    unread_ = false;
    cursor_.clear();
    newer_.clear();
    origin_ = {};
    Render(latest_, true);
    dirty_ = false;
    if (reload) Request("");
}

}  // namespace harness
