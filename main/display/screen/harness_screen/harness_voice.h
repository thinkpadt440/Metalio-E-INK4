#pragma once

#include <cstdint>

namespace harness_voice {
enum class Phase { Idle, Preparing, Recording, Complete, Error };
struct State {
    Phase phase = Phase::Idle;
    uint32_t duration_ms = 0;
    int peak = 0;
};
void Begin();
void End();
State Snapshot();
}  // namespace harness_voice
