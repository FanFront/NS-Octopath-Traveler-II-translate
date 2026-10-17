#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

// Presentation timestamps, never display refresh ticks. A bounded two-second
// window keeps the HUD responsive even when a 60-frame log takes six seconds.
class FrameStatsWindow {
    std::array<uint64_t,241> times{};
    size_t first=0,count=0;
    bool actual=false;
    uint64_t at(size_t i) const {return times[(first+i)%times.size()];}
public:
    struct Sample {double fps=0,mean_ms=0,p95_ms=0;int source=-1;};
    void reset(){first=count=0;}
    void add(uint64_t time,bool displayed){
        if(!time)return;
        if(count&&displayed!=actual)reset();
        actual=displayed;
        if(count&&time<=at(count-1))return;
        if(count==times.size()){first=(first+1)%times.size();--count;}
        times[(first+count++)%times.size()]=time;
        while(count>2&&time-at(1)>2000000000ULL){first=(first+1)%times.size();--count;}
    }
    Sample sample(uint64_t now) const {
        if(count<2)return {};
        if(now>at(count-1)&&now-at(count-1)>2000000000ULL)return {0,0,0,actual?1:0};
        std::array<uint64_t,240> gaps{};
        for(size_t i=1;i<count;++i)gaps[i-1]=at(i)-at(i-1);
        std::sort(gaps.begin(),gaps.begin()+count-1);
        const size_t n=count-1,index=(95*n+99)/100-1;
        double elapsed=double(at(count-1)-at(0));
        return {n*1e9/elapsed,elapsed/n/1e6,gaps[index]/1e6,actual?1:0};
    }
};
