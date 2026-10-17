#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>

namespace ot2 {
// Ordered translation jobs can finish ahead of the presenter. Attribute only
// jobs through its dependency; do not charge pacing or GPU-only tail waits to CPU.
class HintWork {
    struct Job {uint64_t sequence,begin,end;};
    std::deque<Job> jobs;
    bool incomplete=false;
    size_t capacity;
public:
    struct Frame {uint64_t active=0,first=0,last=0;};
    explicit HintWork(size_t limit=4096):capacity(std::max<size_t>(1,limit)){}
    void clear(){jobs.clear();incomplete=false;}
    bool record(uint64_t sequence,uint64_t begin,uint64_t end){
        if(end<=begin||(!jobs.empty()&&sequence<=jobs.back().sequence)){clear();incomplete=true;return false;}
        bool kept=jobs.size()<capacity;
        if(!kept){jobs.pop_front();incomplete=true;}
        jobs.push_back({sequence,begin,end});return kept;
    }
    Frame consume(uint64_t dependency){
        Frame frame;bool consumed=false;
        while(!jobs.empty()&&jobs.front().sequence<=dependency){
            auto j=jobs.front();jobs.pop_front();consumed=true;
            if(!frame.first)frame.first=j.begin;
            frame.last=j.end;
            auto duration=j.end-j.begin;
            if(duration>std::numeric_limits<uint64_t>::max()-frame.active)incomplete=true;
            else frame.active+=duration;
        }
        if(incomplete){frame={};if(consumed)incomplete=false;}
        return frame;
    }
    size_t size()const{return jobs.size();}
};
// Full frame budget paired with first encoder job -> completed GPU dependency.
// Deliberate frame pacing happens after the report and is not work time.
inline uint64_t hint_frame_budget(int interval){return uint64_t(std::clamp(interval,1,5))*16666667;}
}
