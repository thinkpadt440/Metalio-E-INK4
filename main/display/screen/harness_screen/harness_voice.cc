#include "harness_voice.h"

#include <atomic>

#include "application.h"
#include "harness_screen.h"
#include "power_policy.h"
#include "standby_screen/standby_screen.h"

namespace harness_voice {
namespace {
std::atomic<uint32_t> s_generation{0};
std::atomic<bool> s_held{false};
std::atomic<uint32_t> s_started_generation{0};
std::atomic<bool> s_finishing{false};
std::atomic<Phase> s_phase{Phase::Idle};
bool s_power_held = false;  // Only touched on Application's event loop.

void ReleasePower() {
    if (s_power_held) {
        PowerPolicy::GetInstance().Release(PowerNeed::LocalMic);
        PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        s_power_held = false;
    }
}
}  // namespace

void Begin() {
    s_held.store(true);
    s_finishing.store(false);
    const uint32_t generation = ++s_generation;
    s_phase.store(Phase::Preparing);
    Application::GetInstance().Schedule([generation]() {
        auto& app = Application::GetInstance();
        if (generation != s_generation.load() || !s_held.load() ||
            !HarnessScreen::IsActive() || StandbyScreen::IsActive()) return;
        if (!app.IsBootReady() || app.IsXiaozhiVoiceActive()) {
            s_phase.store(Phase::Error);
            ReleasePower();
            return;
        }
        app.EnsureAudioServiceRunning();
        if (generation != s_generation.load() || !s_held.load() ||
            !HarnessScreen::IsActive() || StandbyScreen::IsActive()) return;
        if (!app.GetAudioService().StartLocalRecording()) {
            s_phase.store(Phase::Error);
            ReleasePower();
            return;
        }
        s_started_generation.store(generation);
        if (!s_power_held) {
            PowerPolicy::GetInstance().Acquire(PowerNeed::LocalMic);
            PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
            s_power_held = true;
        }
        s_phase.store(Phase::Recording);
        // Release can arrive while codec/tasks are being initialized.
        if (!s_held.load()) End();
    });
}

void End() {
    s_held.store(false);
    const uint32_t generation = s_generation.load();
    Application::GetInstance().GetAudioService().StopLocalRecording();
    Application::GetInstance().Schedule([generation]() {
        if (generation != s_generation.load()) return;
        auto& service = Application::GetInstance().GetAudioService();
        service.StopLocalRecording();
        const auto info = service.GetLocalRecordingInfo();
        const Phase previous = s_phase.load();
        if (previous == Phase::Recording || previous == Phase::Preparing) {
            s_phase.store(s_started_generation.load() != generation ? Phase::Idle
                : (info.failed ? Phase::Error : (info.samples > 0 ? Phase::Complete : Phase::Idle)));
        }
        ReleasePower();
        s_finishing.store(false);
    });
}

State Snapshot() {
    State state;
    state.phase = s_phase.load();
    if (state.phase == Phase::Idle || state.phase == Phase::Preparing || state.phase == Phase::Error) return state;
    const auto info = Application::GetInstance().GetAudioService().GetLocalRecordingInfo();
    state.duration_ms = info.samples * 1000 / 16000;
    state.peak = info.peak;
    if (state.phase == Phase::Recording && !info.active && !s_finishing.exchange(true)) End();
    return state;
}
}  // namespace harness_voice
