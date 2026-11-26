#pragma once

#include "harness/catalog.h"
#include "lvgl.h"

#include <deque>
#include <memory>

namespace harness {

struct ChatLine {
    std::string role;
    std::string text;
    int role_width = 0;
};

class ChatReader {
public:
    void Bind(const std::string& session_id);
    void Update();
    void Turn(int direction);
    void ShowLatest();

    void SetLayout(int width, int height, const lv_font_t* font, const lv_font_t* role_font,
                   int line_space);
    void Resize(int height);
    void SetThinkingFor(const std::string& transcript_id);
    int Baseline() const;
    int RowHeight() const;
    const std::vector<ChatLine>& Lines() const { return displayed_; }
    const char* Status() const;
    const char* LatestButton() const;

private:
    void Request(const std::string& cursor);
    void TakeReply();
    void ApplyEvents();
    void Render(const HistoryPage& page, bool last_screen, size_t anchor = 0);
    void EnterArchive();
    void Paginate(size_t anchor, bool last_screen);
    void UpdateDisplayedText();

    struct RolePrefix {
        size_t start;
        size_t length;
        int width;
    };
    size_t NextLine(size_t position, const RolePrefix*& prefix) const;

    std::string session_id_;
    std::shared_ptr<HistoryInbox> inbox_;
    std::shared_ptr<CatalogCall> request_;
    HistoryPage latest_;
    HistoryPage origin_;
    HistoryPage shown_;
    std::deque<std::string> newer_;
    std::string cursor_;
    std::string requested_cursor_;
    std::string thinking_after_;
    std::string text_;
    std::vector<RolePrefix> roles_;
    std::vector<ChatLine> displayed_;
    std::vector<size_t> screens_;
    size_t screen_ = 0;
    uint32_t last_render_ = 0;
    uint32_t last_fetch_ = 0;
    int width_ = 0;
    int height_ = 0;
    int line_space_ = 0;
    int archive_direction_ = 0;
    const lv_font_t* font_ = nullptr;
    const lv_font_t* role_font_ = nullptr;
    CatalogError error_ = CatalogError::None;
    bool loaded_ = false;
    bool following_ = true;
    bool showing_thinking_ = false;
    bool needs_refresh_ = false;
    bool dirty_ = false;
    bool unread_ = false;
    bool attempted_ = false;
};

}  // namespace harness
