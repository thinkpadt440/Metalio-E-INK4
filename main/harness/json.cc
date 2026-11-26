#include "json.h"

#include <cstring>
#include <cJSON.h>

namespace harness {
cJSON* ParseJsonMessage(const char* data, size_t length) {
    if (data == nullptr || length == 0 || length > kMaxJsonBytes ||
        std::memchr(data, '\0', length) != nullptr) return nullptr;
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(data, length, &end, false);
    if (!root) return nullptr;
    while (end < data + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    if (end != data + length || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}
}  // namespace harness
