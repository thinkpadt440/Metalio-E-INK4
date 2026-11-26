#pragma once

#include <cstddef>

struct cJSON;

namespace harness {
constexpr size_t kMaxJsonBytes = 8192;

// Parses a complete object within the supplied range. Caller owns/deletes the result.
cJSON* ParseJsonMessage(const char* data, size_t length);
}  // namespace harness
