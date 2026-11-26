#pragma once

#include "harness/approval_tracker.h"
#include "lvgl.h"

#include <string>
#include <vector>

namespace harness {

class ApprovalPanel {
public:
    void Create(lv_obj_t* parent, int width, int height);
    void Open(ApprovalPtr approval, const std::string& session_title);
    void Close();
    void Turn(int direction);
    bool IsOpen() const { return static_cast<bool>(approval_); }
    const std::string& Id() const { return approval_->id; }
    const char* Update(bool ready);

private:
    void Back();
    void BuildPages();
    void Decide(ApprovalDecision decision);

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* heading_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* page_number_ = nullptr;
    lv_obj_t* approve_ = nullptr;
    lv_obj_t* reject_ = nullptr;
    ApprovalPtr approval_;
    std::string session_title_;
    std::vector<size_t> pages_;
    std::string text_;
    size_t page_ = 0;
    size_t viewed_ = 0;
    uint32_t last_turn_ = 0;
    int body_width_ = 0;
    int body_height_ = 0;
    bool complete_ = false;
    bool reviewing_operation_ = false;
};

}  // namespace harness
