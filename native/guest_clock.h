#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

namespace ot2 {
// Only guest elapsed time is paused. Host waits, rendering and calendar time
// keep using their real clocks. Readers also run inside counter trap handlers,
// so they must not acquire a lock held by another guest thread.
class GuestClock {
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::mutex writer_;
    uint32_t reasons_ = 0;
    std::atomic<uint64_t> sequence_{0}, offset_{0}, frozen_raw_{0};
    std::atomic<bool> frozen_{false};
public:
    template<class Now> uint64_t read(Now now) const {
        for (;;) {
            const auto version = sequence_.load();
            if (version & 1) continue;
            const auto offset = offset_.load();
            const auto frozen = frozen_.load();
            const auto raw = frozen ? frozen_raw_.load() : now();
            // Sample real time inside the version check: a timestamp sampled
            // before a concurrent resume could precede the new offset.
            if (sequence_.load() == version) return raw - offset;
        }
    }
    template<class Now> void freeze(unsigned reason, bool on, Now now) {
        if (reason >= 32) return;
        std::lock_guard lock(writer_);
        const auto next = on ? reasons_ | (1u << reason) : reasons_ & ~(1u << reason);
        if (bool(next) != bool(reasons_)) {
            sequence_.fetch_add(1);
            const auto raw = now();
            if (next) frozen_raw_.store(raw);
            else offset_.fetch_add(raw - frozen_raw_.load());
            frozen_.store(next != 0);
            sequence_.fetch_add(1);
        }
        reasons_ = next;
    }
};
}
