#include "harness_voice.h"

#include <atomic>

#include "application.h"
#include "boot_key_handler.h"
#include "harness_screen.h"
#include "harness/service.h"
#include "power_policy.h"
#include "standby_screen/standby_screen.h"

namespace harness_voice {
namespace {
std::atomic<uint32_t> s_generation{0};
std::atomic<bool> s_held{false};
std::atomic<uint32_t> s_started_generation{0};
std::atomic<bool> s_finishing{false};
std::atomic<Phase> s_phase{Phase::Idle};
std::atomic<uint32_t> s_samples{0};
std::atomic<int> s_peak{0};
std::string s_session;
uint32_t s_settings_version = 0;
bool s_power_held = false;  // Only touched on Application's event loop.

void ReleasePower() {
    if (s_power_held) {
        PowerPolicy::GetInstance().Release(PowerNeed::LocalMic);
        PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        s_power_held = false;
    }
}
}  // namespace

bool IsCapturing() {
    const auto phase = s_phase.load();
    return phase == Phase::Preparing || phase == Phase::Recording;
}

void Begin(std::string session_id, uint32_t settings_version) {
    if (harness::RecordingStatus().Pending() || IsCapturing()) return;
    s_held.store(true);
    s_samples = 0;
    s_peak = 0;
    s_finishing.store(false);
    const uint32_t generation = ++s_generation;
    s_phase.store(Phase::Preparing);
    Application::GetInstance().Schedule([generation, session_id = std::move(session_id), settings_version]() {
        auto& app = Application::GetInstance();
        if (generation != s_generation.load()) return;
        if (!s_held.load() || !BootKey_IsHeld() || !HarnessScreen::IsActive() || StandbyScreen::IsActive()) {
            s_phase = Phase::Idle;
            return;
        }
        if (!app.IsBootReady() || app.IsXiaozhiVoiceActive()) {
            s_phase.store(Phase::Error);
            ReleasePower();
            return;
        }
        app.EnsureAudioServiceRunning();
        if (generation != s_generation.load()) return;
        if (!s_held.load() || !BootKey_IsHeld() || !HarnessScreen::IsActive() || StandbyScreen::IsActive()) {
            s_phase = Phase::Idle;
            return;
        }
        if (!app.GetAudioService().StartLocalRecording()) {
            s_phase.store(Phase::Error);
            ReleasePower();
            return;
        }
        s_session = session_id;
        s_settings_version = settings_version;
        s_started_generation.store(generation);
        if (!s_power_held) {
            PowerPolicy::GetInstance().Acquire(PowerNeed::LocalMic);
            PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
            s_power_held = true;
        }
        s_phase.store(Phase::Recording);
        // Release can arrive while codec/tasks are being initialized.
        if (!s_held.load() || !BootKey_IsHeld()) End();
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
            Phase next = Phase::Idle;
            if (s_started_generation.load() == generation) {
                s_samples = info.samples;
                s_peak = info.peak;
                if (info.failed) next = Phase::Error;
                else if (info.samples < 3200) next = Phase::TooShort;
                else if (service.FreezeLocalRecording() &&
                         harness::QueueRecording(s_session, s_settings_version, info.samples)) {
                    next = Phase::Complete;
                } else {
                    next = Phase::Error;
                }
                if (next != Phase::Complete) service.ReleaseLocalRecording();
            }
            s_phase.store(next);
        }
        ReleasePower();
        s_finishing.store(false);
    });
}

State Snapshot() {
    State state;
    state.phase = s_phase.load();
    if (state.phase == Phase::Idle || state.phase == Phase::Preparing || state.phase == Phase::Error) return state;
    if (state.phase == Phase::Recording) {
        const auto info = Application::GetInstance().GetAudioService().GetLocalRecordingInfo();
        state.duration_ms = info.samples / 16;
        state.peak = info.peak;
        if (!info.active && !s_finishing.exchange(true)) End();
    } else {
        state.duration_ms = s_samples.load() / 16;
        state.peak = s_peak.load();
    }
    return state;
}
}  // namespace harness_voice
