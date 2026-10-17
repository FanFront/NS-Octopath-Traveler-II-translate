#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Translation admission and GPU retirement use the same sequence numbers,
// but advance independently. A completed encode is never a completed fence.
class CompletionTimeline {
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t completed=0;
public:
    void complete(uint64_t sequence) {
        {std::lock_guard lock(mutex);completed=std::max(completed,sequence);}
        changed.notify_all();
    }
    bool wait(uint64_t sequence,uint64_t timeout=UINT64_MAX) {
        std::unique_lock lock(mutex);
        auto ready=[&]{return completed>=sequence;};
        if(ready())return true;
        if(!timeout)return false;
        if(timeout==UINT64_MAX){changed.wait(lock,ready);return true;}
        return changed.wait_for(lock,std::chrono::nanoseconds(std::min<uint64_t>(timeout,INT64_MAX)),ready);
    }
};
