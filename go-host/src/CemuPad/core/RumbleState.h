#pragma once
#include "Types.h"
#include <deque>
#include <mutex>

namespace CemuPad {
// Bounded VPAD motor mailbox. PPC publishes; the authenticated transport polls.
// Match the existing local VPAD adapter: OR each pair of 120-Hz bits at 60 Hz.
class RumbleState {
public:
    void Permit(bool active, std::uint64_t epoch) {
        std::lock_guard lock(mutex);
        if (!active || epoch != ownershipEpoch) Clear();
        permitted = active; ownershipEpoch = epoch;
    }
    void Level(bool on,LocalTimeUs until){std::lock_guard lock(mutex);level=permitted&&on;levelUntil=until;}
    bool Push(std::span<const std::uint8_t> bytes, unsigned bits, LocalTimeUs now) {
        std::lock_guard lock(mutex);
        if (!bits || bytes.empty()) { Clear(); return true; }
        if (!permitted) return true;
        if (bits > 120 || bytes.size() < (bits + 7) / 8) return false;
        Advance(now);
        if (queue.size() >= 5) return false;
        Pattern pattern;
        for (unsigned i = 0; i < bits; i += 2)
            pattern.samples.push_back((bytes[i / 8] & (3u << (i % 8))) != 0);
        if (queue.empty()) started = now;
        queue.push_back(std::move(pattern));
        return true;
    }
    bool Poll(LocalTimeUs now, bool freshInput) {
        std::lock_guard lock(mutex);
        if (!freshInput) { Clear(); permitted = false; return false; }
        Advance(now);
        if(permitted&&level&&now<levelUntil)return true;
        if (!permitted || queue.empty()) return false;
        auto index = static_cast<std::size_t>((now - started) * 60 / 1'000'000);
        return queue.front().samples.at(index);
    }
    void Cancel() { std::lock_guard lock(mutex); Clear(); permitted = false; }
private:
    struct Pattern { std::deque<bool> samples; };
    void Clear() { queue.clear(); started = 0; level=false;levelUntil=0; }
    void Advance(LocalTimeUs& now) {
        // The producer and consumer capture trusted local time before locking.
        // Their lock order can differ from capture order at a pattern boundary.
        if (now < started) now = started;
        while (!queue.empty()) {
            auto duration = (queue.front().samples.size() * 1'000'000 + 59) / 60;
            if (now - started < duration) break;
            started += duration; queue.pop_front();
        }
    }
    std::mutex mutex;
    std::deque<Pattern> queue;
    LocalTimeUs started = 0;
    std::uint64_t ownershipEpoch = 0;
    bool permitted = false,level=false;LocalTimeUs levelUntil=0;
};
}
