#include "approval_panel.h"

#include "harness/service.h"
#include "product_ui.h"
#include "src/misc/lv_text_private.h"

#include <algorithm>
#include <cstdio>

namespace harness {
namespace {
constexpr int kLineSpace = 6;

ApprovalPanel* Panel(lv_event_t* event) {
    return static_cast<ApprovalPanel*>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
}

void Show(lv_obj_t* object, bool visible) {
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void Enable(lv_obj_t* object, bool enabled) {
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

bool Invisible(uint32_t ch) {
    return ch < 0x20 || (ch >= 0x7f && ch <= 0xa0) || ch == 0xad || ch == 0x61c ||
           ch == 0x1680 || ch == 0x180e || (ch >= 0x2000 && ch <= 0x200f) ||
           (ch >= 0x2028 && ch <= 0x202f) || (ch >= 0x205f && ch <= 0x206f) ||
           ch == 0x3000 || ch == 0xfeff || (ch >= 0xfe00 && ch <= 0xfe0f) ||
           (ch >= 0xe0000 && ch <= 0xe01ef);
}

void EscapeCodePoint(std::string& out, uint32_t ch) {
    char escaped[16];
    if (ch <= 0xffff) {
        std::snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned>(ch));
    } else {
        const auto supplementary = ch - 0x10000;
        std::snprintf(escaped, sizeof(escaped), "\\u%04X\\u%04X",
            static_cast<unsigned>(0xd800 + (supplementary >> 10)),
            static_cast<unsigned>(0xdc00 + (supplementary & 0x3ff)));
    }
    out += escaped;
}

bool AppendReadable(std::string& out, const std::string& value, const lv_font_t* font, bool keep_lines) {
    bool valid = true;
    uint32_t offset = 0;
    while (offset < value.size()) {
        const uint32_t start = offset;
        const auto ch = lv_text_encoded_next(value.c_str(), &offset);
        if (offset <= start || offset > value.size() || ch == 0 || ch > 0x10ffff) {
            valid = false;
            offset = start + 1;
            EscapeCodePoint(out, static_cast<unsigned char>(value[start]));
            continue;
        }
        if (keep_lines && (ch == '\n' || ch == '\r' || ch == '\t')) {
            out += ch == '\n' ? "\n" : ch == '\r' ? "" : "    ";
            continue;
        }
        lv_font_glyph_dsc_t glyph{};
        const bool glyph_ok = lv_font_get_glyph_dsc(font, &glyph, ch, 0) && !glyph.is_placeholder;
        if (Invisible(ch) || !glyph_ok) EscapeCodePoint(out, ch);
        else out.append(value, start, offset - start);
    }
    return valid;
}

const char* ResultLabel(ApprovalState state) {
    switch (state) {
        case ApprovalState::Approved: return "审批已允许";
        case ApprovalState::Rejected: return "审批已拒绝";
        case ApprovalState::Expired: return "审批已过期";
        case ApprovalState::Cancelled: return "审批已取消";
        default: return "";
    }
}
}  // namespace

void ApprovalPanel::Create(lv_obj_t* parent, int width, int height) {
    panel_ = product_ui::Box(parent, 0, 0, width, height);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_CLICKABLE);
    auto* back = product_ui::Button(panel_, 0, 0, 82, 44, "返回", [](lv_event_t* event) {
        Panel(event)->Back();
    });
    lv_obj_set_user_data(back, this);
    heading_ = product_ui::Label(panel_, 96, 1, width - 96, "审批详情", product_ui::HeadingFont());
    lv_obj_set_height(heading_, 42);
    lv_label_set_long_mode(heading_, LV_LABEL_LONG_DOT);
    status_ = product_ui::Label(panel_, 0, 54, width, "");
    lv_obj_set_height(status_, 56);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
    body_width_ = width;
    body_height_ = height - 234;
    body_ = product_ui::Label(panel_, 0, 122, body_width_, "");
    lv_obj_set_height(body_, body_height_);
    lv_obj_set_style_text_line_space(body_, kLineSpace, 0);
    page_number_ = product_ui::Label(panel_, 0, height - 99, body_width_, "");
    lv_obj_set_style_text_align(page_number_, LV_TEXT_ALIGN_CENTER, 0);
    const int choice_width = (width - 10) / 2;
    approve_ = product_ui::Button(panel_, 0, height - 60, choice_width, 52, "查看操作",
        [](lv_event_t* event) { Panel(event)->Decide(ApprovalDecision::AllowOnce); }, true);
    lv_obj_set_user_data(approve_, this);
    reject_ = product_ui::Button(panel_, choice_width + 10, height - 60, choice_width, 52, "拒绝",
        [](lv_event_t* event) { Panel(event)->Decide(ApprovalDecision::Reject); });
    lv_obj_set_user_data(reject_, this);
    for (auto* choice : {approve_, reject_}) {
        lv_obj_set_style_bg_color(choice, lv_color_make(225, 225, 225), LV_STATE_DISABLED);
        lv_obj_set_style_text_color(choice, lv_color_make(102, 102, 102), LV_STATE_DISABLED);
        lv_obj_set_style_border_color(choice, lv_color_make(153, 153, 153), LV_STATE_DISABLED);
    }
    Show(panel_, false);
}

void ApprovalPanel::Open(ApprovalPtr approval, const std::string& session_title) {
    approval_ = std::move(approval);
    session_title_ = session_title;
    reviewing_operation_ = false;
    BuildPages();
    Show(panel_, true);
}

void ApprovalPanel::Close() {
    approval_.reset();
    text_.clear();
    pages_.clear();
    Show(panel_, false);
}

void ApprovalPanel::Back() {
    if (!reviewing_operation_) {
        Close();
        return;
    }
    reviewing_operation_ = false;
    BuildPages();
    Update(ServiceReady());
}

void ApprovalPanel::BuildPages() {
    text_.clear();
    page_ = viewed_ = 0;
    last_turn_ = lv_tick_get() - 300;
    pages_.clear();
    pages_.push_back(0);
    const auto* font = fontpack_lv_font_ui();
    complete_ = font != nullptr;
    if (!font) return;
    if (reviewing_operation_) {
        text_ += "会话：";
        complete_ &= AppendReadable(text_, session_title_, font, false);
        text_ += "\n工具：";
        complete_ &= AppendReadable(text_, approval_->tool, font, false);
        text_ += "\n\n";
        complete_ &= AppendReadable(text_, approval_->operation, font, true);
    } else if (!approval_->reason.empty()) {
        complete_ &= AppendReadable(text_, approval_->reason, font, true);
    } else {
        text_ = "暂无审批描述";
    }

    const int rows = std::max(1, (body_height_ + kLineSpace) /
        (static_cast<int>(font->line_height) + kLineSpace));
    lv_text_attributes_t attributes{0, kLineSpace, body_width_, LV_TEXT_FLAG_NONE};
    size_t offset = 0;
    int lines = 0;
    while (offset < text_.size()) {
        const auto length = lv_text_get_next_line(text_.c_str() + offset,
            text_.size() - offset, font, nullptr, &attributes);
        if (!length || length > text_.size() - offset) {
            complete_ = false;
            break;
        }
        offset += length;
        if (++lines == rows && offset < text_.size()) {
            pages_.push_back(offset);
            lines = 0;
        }
    }
}

const char* ApprovalPanel::Update(bool ready) {
    if (!IsOpen()) return nullptr;
    const auto view = TrackedApproval(approval_->id);
    if (!view.info || view.info->state != ApprovalState::Pending) {
        const auto* result = view.info ? ResultLabel(view.info->state) : "审批已不可用";
        Close();
        return result;
    }
    if (view.info != approval_) {
        approval_ = view.info;
        reviewing_operation_ = false;
        BuildPages();
    }
    const bool available = ready && !view.submitting && !view.checking;
    const bool local_allow = complete_ && approval_->actionable && !view.web_only;
    const bool reviewed = viewed_ + 1 == pages_.size();
    Show(approve_, local_allow);
    Enable(approve_, available && reviewed);
    product_ui::SetText(lv_obj_get_child(approve_, 0), reviewing_operation_ ? "仅本次允许" : "查看操作");
    Enable(reject_, available);
    const char* status = !ready ? "离线，等待同步审批" :
        view.submitting ? "正在提交审批…" :
        view.checking ? "正在核对审批状态…" :
        !local_allow ? "请到 Harness 网页核对并允许" :
        !reviewed ? "请用音量键逐页读完详情" :
        !view.error.empty() ? "上次提交未确认，请核对后重试" : "";
    product_ui::SetText(status_, status);

    const size_t end = page_ + 1 < pages_.size() ? pages_[page_ + 1] : text_.size();
    auto page_text = text_.substr(pages_[page_], end - pages_[page_]);
    while (!page_text.empty() && page_text.back() == '\n') page_text.pop_back();
    product_ui::SetText(body_, page_text.c_str());
    Show(page_number_, pages_.size() > 1);
    char counter[24];
    std::snprintf(counter, sizeof(counter), "%u / %u", static_cast<unsigned>(page_ + 1),
                  static_cast<unsigned>(pages_.size()));
    product_ui::SetText(page_number_, counter);
    return nullptr;
}

void ApprovalPanel::Turn(int direction) {
    if (!IsOpen() || lv_tick_elaps(last_turn_) < 300) return;
    last_turn_ = lv_tick_get();
    if (direction < 0 && page_ > 0) --page_;
    if (direction > 0 && page_ + 1 < pages_.size()) ++page_;
    viewed_ = std::max(viewed_, page_);
}

void ApprovalPanel::Decide(ApprovalDecision decision) {
    if (!IsOpen() || !ServiceReady()) return;
    const auto current = TrackedApproval(approval_->id);
    if (current.info != approval_ || current.info->state != ApprovalState::Pending ||
        current.submitting || current.checking) return;
    if (decision == ApprovalDecision::AllowOnce) {
        if (!complete_ || !approval_->actionable || current.web_only || viewed_ + 1 != pages_.size()) return;
        if (!reviewing_operation_) {
            reviewing_operation_ = true;
            BuildPages();
            Update(true);
            return;
        }
    }
    if (RequestApproval(approval_, decision)) {
        Enable(approve_, false);
        Enable(reject_, false);
        product_ui::SetText(status_, "正在提交审批…");
    }
}

}  // namespace harness
