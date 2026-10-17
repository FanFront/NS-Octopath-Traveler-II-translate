#include "../graphics/cpu_capacity.h"
#include <array>
#include <cassert>
int main(){
    using ot2::CpuCapacity;using ot2::highest_capacity_cpus;
    // Deliberately shuffled CPU IDs: topology, not index, defines the choice.
    const std::array mixed={CpuCapacity{1,1024},CpuCapacity{8,400},CpuCapacity{3,1024},CpuCapacity{12,700}};
    assert((highest_capacity_cpus(mixed)==std::vector<unsigned>{1,3}));
    const std::array symmetric={CpuCapacity{0,792},CpuCapacity{5,792}};
    assert(highest_capacity_cpus(symmetric).empty());
    const std::array unknown={CpuCapacity{0,0},CpuCapacity{7,1024}};
    assert(highest_capacity_cpus(unknown).empty());
    const std::array restricted={CpuCapacity{9,512},CpuCapacity{3,700}};
    assert((highest_capacity_cpus(restricted)==std::vector<unsigned>{3}));
    assert(highest_capacity_cpus({}).empty());
}
