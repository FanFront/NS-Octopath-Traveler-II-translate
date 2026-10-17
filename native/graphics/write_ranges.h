#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Exact union of CPU-visible destinations. Repeated counter reports retain
// their original order/results elsewhere; only overlap lookup is deduplicated.
class WriteRanges {
    struct Range {uintptr_t first,last;};
    std::vector<Range> ranges;
public:
    void clear(){ranges.clear();}
    size_t size()const{return ranges.size();}
    void include(const WriteRanges& other){
        if(this==&other)return;
        for(const auto& r:other.ranges){
            // A full address-space interval cannot be expressed as size_t.
            if(r.first==0&&r.last==UINTPTR_MAX){ranges.assign(1,r);return;}
            add(r.first,r.last-r.first+1);
        }
    }
    void add(uintptr_t address,size_t bytes){
        if(!bytes)return;
        uintptr_t first=address,last;
        if(bytes-1>UINTPTR_MAX-address){first=0;last=UINTPTR_MAX;}
        else last=address+bytes-1;
        auto begin=std::lower_bound(ranges.begin(),ranges.end(),first,[](const Range& r,uintptr_t a){return r.last<a&&a-r.last>1;});
        auto end=begin;
        while(end!=ranges.end()&&(last==UINTPTR_MAX||end->first<=last+1)){
            first=std::min(first,end->first);last=std::max(last,end->last);++end;
        }
        if(begin==end)ranges.insert(begin,{first,last});
        else {begin->first=first;begin->last=last;ranges.erase(begin+1,end);}
    }
    bool overlaps(uintptr_t address,size_t bytes,uint64_t& checks)const{
        if(!bytes||ranges.empty())return false;
        if(bytes-1>UINTPTR_MAX-address)return true;
        const auto last=address+bytes-1;
        if(address>ranges.back().last||last<ranges.front().first)return false;
        auto it=std::lower_bound(ranges.begin(),ranges.end(),address,[&](const Range& r,uintptr_t a){++checks;return r.last<a;});
        return it!=ranges.end()&&it->first<=last;
    }
};
