#pragma once
#include <algorithm>
#include <span>
#include <vector>

namespace ot2 {
struct CpuCapacity { unsigned id, capacity; };
// The caller supplies only CPUs in the thread's current allowed mask. Missing
// topology or symmetric cores leave placement to the platform. Never assume
// that higher CPU numbers or clocks identify the faster microarchitecture.
inline std::vector<unsigned> highest_capacity_cpus(std::span<const CpuCapacity> allowed) {
    if(allowed.empty())return {};
    unsigned low=allowed.front().capacity,high=low;
    for(auto cpu:allowed){if(!cpu.capacity)return {};low=std::min(low,cpu.capacity);high=std::max(high,cpu.capacity);}
    if(low==high)return {};
    std::vector<unsigned> selected;
    for(auto cpu:allowed)if(cpu.capacity==high)selected.push_back(cpu.id);
    return selected;
}
}
