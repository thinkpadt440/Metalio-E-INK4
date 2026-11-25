#pragma once
#include <cstdint>
using EventBits_t = uint32_t;
constexpr int pdFALSE=0, pdTRUE=1;
constexpr EventBits_t BIT0=1, BIT1=2;
inline unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
