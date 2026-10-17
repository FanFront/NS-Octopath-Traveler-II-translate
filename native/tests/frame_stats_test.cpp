#include "../graphics/frame_stats.h"
#include <cassert>
#include <cmath>
#include <cstdio>
int main(){
    FrameStatsWindow stats;assert(stats.sample(0).source==-1);
    for(unsigned i=0;i<=60;++i)stats.add(1000000000ULL+i*33333333ULL,true);
    auto s=stats.sample(3000000000ULL);assert(std::abs(s.fps-30)<0.001&&std::abs(s.p95_ms-33.333333)<0.001&&s.source==1);
    stats.add(1,true);stats.add(3000000000ULL-20,true); // stale/duplicate driver replies
    assert(std::abs(stats.sample(3000000000ULL).fps-30)<0.001);
    assert(stats.sample(6000000000ULL).fps==0); // frozen game must not keep showing 30
    stats.reset();for(unsigned i=0;i<=60;++i)stats.add(1000000000ULL+i*16666667ULL,false);
    s=stats.sample(2000000020ULL);assert(std::abs(s.fps-60)<0.001&&s.source==0);
    // Frame-time p95 must expose real long frames instead of smoothing them away.
    stats.reset();uint64_t t=1000000000ULL;stats.add(t,true);
    for(unsigned i=0;i<20;++i){t+=i<18?16666667:100000000;stats.add(t,true);}
    s=stats.sample(t);assert(s.p95_ms==100&&std::abs(s.mean_ms-25)<0.001);
    // Switching source cannot mix queue timestamps with actual display timestamps.
    stats.add(t+10,false);assert(stats.sample(t+10).source==-1);
    puts("PASS actual/fallback FPS, stale/duplicate timestamps, pause/reset, long-frame p95");
}
