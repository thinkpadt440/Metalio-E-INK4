#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include "local_recording_buffer.h"
struct LocalRecordingInfo { bool active; bool failed; uint32_t samples; int peak; };
class PreviewAudioService {
public:
    bool active = false;
    bool fail_start = false;
    bool failed = false;
    unsigned starts = 0;
    std::vector<int16_t> pcm = std::vector<int16_t>(16000 * 30);
    LocalRecordingBuffer buffer;
    bool StartLocalRecording() {
        if (fail_start) return false;
        buffer.Reset(pcm.data(), pcm.size()); active = true; failed = false; ++starts; return true;
    }
    void StopLocalRecording() { active = false; }
    LocalRecordingInfo GetLocalRecordingInfo() { return {active, failed, static_cast<uint32_t>(buffer.size()), buffer.peak()}; }
    void Feed(size_t frames) {
        std::vector<int16_t> data(frames * 2, 1000);
        buffer.AppendInterleaved(data.data(), data.size(), 2);
        if (buffer.full()) active = false;
    }
};
class Application {
public:
    static Application& GetInstance() { static Application app; return app; }
    bool IsBootReady() { return true; }
    bool IsXiaozhiVoiceActive() { return false; }
    void Schedule(std::function<void()> action) { queue.push_back(std::move(action)); }
    void EnsureAudioServiceRunning() { if (initialization_hook) { auto hook = std::move(initialization_hook); hook(); } }
    PreviewAudioService& GetAudioService() { return audio; }
    void Pump() {
        auto actions = std::move(queue); queue.clear();
        for (auto& action : actions) action();
    }
    std::function<void()> initialization_hook;
private:
    std::deque<std::function<void()>> queue;
    PreviewAudioService audio;
};
