#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "power_policy.h"

// UI-only scripted demo. No network, ASR, filesystem writes or tool execution.
namespace offline_demo {
constexpr int kCasual = 0;
constexpr int kCoder = 1;
constexpr int kSessionCount = 2;
constexpr const char* kNames[] = {"日常闲聊", "编码助手"};
constexpr const char* kModels[] = {"DeepSeek V4.1-Flash", "DeepSeek V4.1-Pro"};
constexpr const char* kAuthorizations[] = {"请求审批", "完全访问"};

enum class Phase { Ready, Transcribing, Thinking, WaitingApproval, Executing, Testing, Complete, Rejected };
struct Session {
    int model = 0;
    int authorization = 1;
    Phase phase = Phase::Ready;
    uint32_t since = 0;
    std::vector<std::string> pages;
};
struct Demo {
    Session sessions[kSessionCount];
    uint32_t added_messages = 0;
    uint32_t added_tokens = 0;
    Demo() { Reset(); }
    void Reset() {
        added_messages = added_tokens = 0;
        sessions[kCasual] = Session{};
        sessions[kCoder] = Session{};
        sessions[kCasual].pages = {
            "你\n最近工作比较忙，怎么安排一天更高效？\n\n助手\n先选出今天最重要的三件事，把精力最好的时段留给需要专注的工作。",
            "你\n下午总被会议打断怎么办？\n\n助手\n把零散会议集中安排，给自己留一段不受打扰的时间。结束工作前，花五分钟记下明天的第一件事。"
        };
        sessions[kCoder].model = 1;
        sessions[kCoder].pages = {
            "你\n帮我给统计页面加一个最近七天的筛选，并检查切换后数据是否正确。\n\n助手\n我会检查统计数据与筛选逻辑，再补上对应的测试。",
            "助手\n已完成最近七天的筛选。\n\n修改：统计页与周期选择逻辑\n检查：空数据、边界日期、周期切换\n结果：构建通过，检查项全部通过。"
        };
    }
};
inline Demo& State() { static Demo state; return state; }
inline bool Busy(const Session& s) {
    return s.phase == Phase::Transcribing || s.phase == Phase::Thinking ||
           s.phase == Phase::WaitingApproval || s.phase == Phase::Executing || s.phase == Phase::Testing;
}
inline void Append(Session& s, const char* text) {
    constexpr size_t kMaxPages = 10;
    if (s.pages.size() >= kMaxPages) s.pages.erase(s.pages.begin());
    s.pages.emplace_back(text);
    ++State().added_messages;
    State().added_tokens += 360;
}
inline void Start(int index, uint32_t now, bool voice) {
    auto& s = State().sessions[index];
    s.phase = voice ? Phase::Transcribing : Phase::Thinking;
    s.since = now;
}
inline bool Advance(int index, uint32_t now) {
    auto& s = State().sessions[index];
    const uint32_t elapsed = now - s.since;
    if (s.phase == Phase::Transcribing && elapsed >= 1000) {
        Append(s, index == kCoder
            ? "你\n帮我检查项目，修改统计页面，并运行测试。"
            : "你\n帮我安排一下今天的工作。");
        s.phase = Phase::Thinking;
    } else if (s.phase == Phase::Thinking && elapsed >= 2000) {
        if (index == kCoder && s.authorization == 0) {
            s.phase = Phase::WaitingApproval;
        } else if (index == kCoder) {
            s.phase = Phase::Executing;
            Append(s, "助手\n正在检查统计页面。\n\n已定位到周期筛选逻辑，接下来补充边界处理并运行检查。");
        } else {
            s.phase = Phase::Complete;
            Append(s, "助手\n建议先处理最重要的一项工作，留出一小时专注时间。\n\n上午完成核心任务，下午集中沟通；下班前整理进度和明天的优先事项。");
        }
    } else if (s.phase == Phase::Executing && elapsed >= 2000) {
        s.phase = Phase::Testing;
        Append(s, "助手\n统计页面已更新。\n\n正在检查周期切换与空数据，并进行固件构建。");
    } else if (s.phase == Phase::Testing && elapsed >= 2000) {
        s.phase = Phase::Complete;
        Append(s, "助手\n检查完成。\n\n周期筛选、边界日期、空数据均通过检查；固件构建通过。\n\n已整理修改说明，等待你确认后续安排。");
    } else {
        return false;
    }
    s.since = now;
    return true;
}
inline bool Decide(bool approve, uint32_t now) {
    auto& s = State().sessions[kCoder];
    if (s.phase != Phase::WaitingApproval) return false;
    s.since = now;
    if (approve) {
        s.phase = Phase::Executing;
        Append(s, "你\n允许本次操作。\n\n助手\n已收到批准，继续修改统计页面并运行检查。");
    } else {
        s.phase = Phase::Rejected;
        Append(s, "你\n拒绝本次操作。\n\n助手\n已停止修改与测试，没有执行本次操作。\n\n你可以先查看方案，再决定是否重新授权。");
    }
    return true;
}
// Existing power API only: keep the presentation awake, retain manual POWER.
class KeepAwake {
public:
    KeepAwake() { PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit); }
    ~KeepAwake() { PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit); }
    KeepAwake(const KeepAwake&) = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;
};
}  // namespace offline_demo
