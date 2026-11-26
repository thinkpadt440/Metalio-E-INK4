#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Storage is supplied by the owner (PSRAM on device). External synchronization is required.
class LocalRecordingBuffer {
public:
    void Reset(int16_t* data, size_t capacity) {
        data_ = data;
        capacity_ = capacity;
        count_ = 0;
        peak_ = 0;
    }
    void AppendInterleaved(const int16_t* data, size_t samples, size_t channels) {
        if (channels == 0 || data_ == nullptr) return;
        for (size_t i = 0; i + channels <= samples && count_ < capacity_; i += channels) {
            data_[count_++] = data[i];
            const int value = data[i];
            peak_ = std::max(peak_, value < 0 ? -value : value);
        }
    }
    bool CopyBytes(size_t offset, void* out, size_t bytes) const {
        const size_t stored = count_ * sizeof(int16_t);
        if (!data_ || !out || bytes == 0 || (offset | bytes) % sizeof(int16_t) != 0 ||
            offset > stored || bytes > stored - offset) return false;
        std::memcpy(out, reinterpret_cast<const uint8_t*>(data_) + offset, bytes);
        return true;
    }
    size_t size() const { return count_; }
    bool full() const { return count_ == capacity_; }
    int peak() const { return peak_; }
private:
    int16_t* data_ = nullptr;
    size_t capacity_ = 0;
    size_t count_ = 0;
    int peak_ = 0;
};
