#pragma once

#include <ctime>
#include <optional>
#include <string>

namespace device_time {
void InitializeBeijingTimezone();
std::optional<time_t> ParseHttpDate(const std::string& value);
std::string FormatBeijing(const std::string& timestamp, bool seconds = false);
}  // namespace device_time
