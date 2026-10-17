#include "../graphics/cache_budget.h"
#include <cassert>
#include <cstdio>
int main(){
    constexpr auto M=CacheBudget::MiB;
    CacheBudgetPolicy p;
    auto b=p.update(16*1024*M,5*1024*M,1);
    assert(b.descriptors>8192&&b.texel_views>8192&&!b.pressure);
    const auto large=b;
    b=p.update(16*1024*M,4900*M,2);
    assert(b.descriptors==large.descriptors&&b.texel_views==large.texel_views);
    b=p.update(16*1024*M,3*1024*M,3);
    assert(b.descriptors<large.descriptors&&b.texel_views<large.texel_views);
    b=p.update(16*1024*M,600*M,4);
    assert(b.pressure&&b.descriptors==1024&&b.geometry_bytes==8*M);
    assert(p.update(16*1024*M,5*1024*M,33).pressure);
    assert(p.update(16*1024*M,2400*M,35).pressure);
    b=p.update(16*1024*M,5*1024*M,36);assert(!b.pressure&&b.descriptors>8192);
    b=p.update(16*1024*M,5*1024*M,37,false);assert(b.descriptors==8192&&b.texel_views==8192);
    assert(p.update(16*1024*M,600*M,38,false).pressure);
    assert(p.update(0,0,39).pressure);
    CacheBudgetPolicy unknown;assert(unknown.update(0,0,0).descriptors==8192);
    assert(unknown.update(1024,2048,1).texel_views==8192);
    assert(unknown.update(16*1024*M,0,2).pressure);
    CacheBudgetPolicy driver;
    driver.allocation_pressure(100);
    assert(driver.update(16*1024*M,8*1024*M,129).pressure);
    assert(!driver.update(16*1024*M,8*1024*M,130).pressure);
    // A disabled experiment must not take budget away from the existing
    // descriptor/texel working set. Low-memory protection still overrides it.
    CacheBudgetPolicy without_resources,with_resources;
    auto original=without_resources.update(16*1024*M,5*1024*M,1,true,false);
    auto shared=with_resources.update(16*1024*M,5*1024*M,1,true,true);
    assert(original.descriptors>shared.descriptors&&original.texel_views>=shared.texel_views);
    assert(without_resources.update(16*1024*M,600*M,2,true,false).pressure);
    for(uint64_t total=2*1024*M;total<=128*1024*M;total+=1024*M){
        CacheBudgetPolicy sample;
        for(uint64_t available=128*M;available<=total;available+=128*M){
            b=sample.update(total,available,available/M);
            assert(b.descriptors<=65536&&b.texel_views<=131072);
            assert(b.allowance<=768*M&&b.allowance<=total/16);
            assert(b.geometry_bytes<=64*M&&b.free_buffer_bytes<=128*M);
            assert(b.resource_bytes<=192*M&&b.resource_entries<=32768);
        }
    }
    puts("PASS adaptive cache budget: live headroom, caps, hysteresis, cooldown, unknown and control");
}
