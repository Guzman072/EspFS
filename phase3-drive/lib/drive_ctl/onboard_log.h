#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include "drive_fsm.h"

// 呼叫端提供緩衝區。板上用 PSRAM，native 測試用一般記憶體。
class OnboardLog {
public:
    OnboardLog(char* data = nullptr, size_t capacity = 0) : data_(data), capacity_(data ? capacity : 0) {}

    void capture(const char* line) {
        if (capacity_ == 0) return;
        const size_t length = std::strlen(line);
        if (length >= capacity_) return;
        while (capacity_ - size_ < length + 1) {
            // 從最舊端移除完整一行，讓 dump 保持原行格式。
            char ch;
            do {
                ch = data_[head_];
                if (++head_ == capacity_) head_ = 0;
                --size_;
            } while (ch != '\n');
        }
        const size_t tail = (head_ + size_) % capacity_;
        const size_t first = std::min(length, capacity_ - tail);
        std::memcpy(data_ + tail, line, first);
        std::memcpy(data_, line + first, length - first);
        data_[(tail + length) % capacity_] = '\n';
        size_ += length + 1;
    }

    static bool storageAllowed(DriveState state, bool stopSampling) {
        return state == DriveState::LATCHED || (state == DriveState::DISARMED && !stopSampling);
    }

    template <typename Flush>
    void update(DriveState state, bool stopSampling, Flush flush) {
        if (state != lastState_) {
            lastState_ = state;
            pending_ = storageAllowed(state, false);
        }
        if (!pending_ || !storageAllowed(state, stopSampling)) return;
        pending_ = false;
        if (size_ == 0) return;
        const size_t first = std::min(size_, capacity_ - head_);
        if (flush(data_ + head_, first, data_, size_ - first)) clear();
    }

    void clear() { head_ = size_ = 0; }

private:
    char* data_;
    size_t capacity_;
    size_t head_ = 0;
    size_t size_ = 0;
    DriveState lastState_ = DriveState::BOOT;
    bool pending_ = false;
};
