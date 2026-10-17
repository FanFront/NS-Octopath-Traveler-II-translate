#include "../graphics/write_ranges.h"
#include <cstdio>
#include <random>
#include <stdexcept>
#include <utility>
int main(){
    auto require=[](bool ok){if(!ok)throw std::runtime_error("CPU write-range union contract");};
    WriteRanges ranges;uint64_t checks=0;
    require(!ranges.overlaps(0,1,checks));ranges.add(0,0);require(ranges.size()==0);
    for(size_t i=0;i<1024;i++)ranges.add(i*16,4);
    for(size_t i=0;i<1024;i++){
        require(ranges.overlaps(i*16,4,checks));
        auto before=checks;require(!ranges.overlaps(i*16+4,12,checks));require(checks-before<=11);
        require(ranges.overlaps(i*16+3,1,checks));
    }
    ranges.add(4,12);require(ranges.size()==1023&&ranges.overlaps(4,12,checks));
    ranges.clear();require(!ranges.overlaps(0,UINTPTR_MAX,checks));
    std::mt19937_64 random(9173);std::vector<std::pair<uintptr_t,size_t>> writes;
    for(unsigned i=0;i<2000;i++){
        auto address=random()%100000;size_t bytes=random()%128;
        writes.emplace_back(address,bytes);ranges.add(address,bytes);
        for(unsigned j=0;j<30;j++){
            auto q=random()%100000;size_t n=random()%128;bool expected=false;
            for(auto [a,b]:writes)expected|=n&&b&&(q>=a?q-a<b:a-q<n);
            require(ranges.overlaps(q,n,checks)==expected);
        }
    }
    ranges.clear();ranges.add(UINTPTR_MAX-15,16);
    require(ranges.overlaps(UINTPTR_MAX,1,checks)&&!ranges.overlaps(UINTPTR_MAX-16,1,checks));
    require(ranges.overlaps(UINTPTR_MAX-4,8,checks));
    ranges.add(UINTPTR_MAX-4,8);require(ranges.overlaps(0,1,checks));
    WriteRanges merged;merged.add(500,20);merged.include(ranges);
    require(merged.overlaps(0,1,checks)&&merged.overlaps(UINTPTR_MAX,1,checks));
    merged.clear();ranges.clear();WriteRanges second;
    for(unsigned i=0;i<500;++i){
        ranges.add(random()%100000,random()%64);
        second.add(random()%100000,random()%64);
    }
    merged.include(ranges);merged.include(second);merged.include(merged);
    for(unsigned i=0;i<10000;++i){
        const auto address=random()%100000,bytes=random()%128;
        require(merged.overlaps(address,bytes,checks)==(ranges.overlaps(address,bytes,checks)||second.overlaps(address,bytes,checks)));
    }
    puts("PASS write ranges: exact union, holes, adjacency, duplicates, binary lookup, randomized oracle, overflow");
}
