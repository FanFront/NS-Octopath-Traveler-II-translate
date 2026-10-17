#include "../guest_clock.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    ot2::GuestClock clock;
    uint64_t raw = 100;
    auto now = [&] { return raw; };
    assert(clock.read(now) == 100);
    clock.freeze(1, true, now);
    raw = 200;
    assert(clock.read(now) == 100);
    clock.freeze(1, true, now); // Duplicate Android callback is idempotent.
    clock.freeze(2, true, now);
    clock.freeze(1, false, now);
    raw = 300;
    assert(clock.read(now) == 100);
    clock.freeze(2, false, now);
    assert(clock.read(now) == 100);
    raw = 310;
    assert(clock.read(now) == 110);
    clock.freeze(1, false, now);
    clock.freeze(1, true, now);
    raw = 1000;
    clock.freeze(1, false, now);
    assert(clock.read(now) == 110);
    raw = 1010;
    assert(clock.read(now) == 120);

    ot2::GuestClock concurrent;
    auto real_now = [] { return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); };
    std::atomic<bool> done{false};
    std::atomic<uint64_t> reads{0};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) readers.emplace_back([&] {
        auto previous = concurrent.read(real_now);
        do {
            const auto current = concurrent.read(real_now);
            assert(current >= previous);
            previous = current;
            reads.fetch_add(1, std::memory_order_relaxed);
        } while (!done.load());
    });
    while (reads.load() < 1000) std::this_thread::yield();
    for (unsigned i = 0; i < 10000; ++i) {
        concurrent.freeze(1, true, real_now);
        concurrent.freeze(2, true, real_now);
        concurrent.freeze(1, false, real_now);
        concurrent.freeze(2, false, real_now);
    }
    done.store(true);
    for (auto& reader : readers) reader.join();
    printf("guest clock: pauses, nested reasons, continuity and concurrent monotonic reads PASS (%llu reads)\n", (unsigned long long)reads.load());
}
