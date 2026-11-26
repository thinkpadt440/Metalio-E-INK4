#pragma once

#include <cstdint>
#include <string>

namespace harness_voice {
enum class Phase { Idle, Preparing, Recording, Complete, TooShort, Error };
struct State {
    Phase phase = Phase::Idle;
    uint32_t duration_ms = 0;
    int peak = 0;
};
void Begin(std::string session_id, uint32_t settings_version);
void End();
bool IsCapturing();
State Snapshot();
}  // namespace harness_voice
