#include "../frame_rate_patch.h"
#include <cassert>
#include <cstdio>
#include <vector>
namespace {
struct alignas(8) Region {uintptr_t address;bool writable;std::array<uint32_t,4> words;};
struct Memory {
    std::vector<Region> regions;
    unsigned lookups=0;
    void* resolve(uintptr_t address,size_t size,bool write){
        ++lookups;
        for(auto& r:regions)if(address>=r.address&&address-r.address<=sizeof(r.words)&&size<=sizeof(r.words)-(address-r.address)&&(!write||r.writable))
            return reinterpret_cast<uint8_t*>(r.words.data())+address-r.address;
        return nullptr;
    }
};
constexpr uintptr_t base=0x10000000;
Memory fixture(){
    Memory m;
    const uintptr_t offsets[]={0x075ad0e0,0x06f017e8,0x076fef68,0x076fe648,0x076d1398};
    const std::array<std::array<uint32_t,2>,5> initial={{{2,2},{1,1},{0x41f00000,0x41f00000},{7,0x41f00000},{0x420551ec,0x420551ec}}};
    for(unsigned i=0;i<5;++i){
        uintptr_t pointer=0x20000000+i*0x1000;Region source{base+offsets[i],false,{}};memcpy(source.words.data(),&pointer,8);m.regions.push_back(source);
        m.regions.push_back({pointer+(i==3?0x7a4:0),true,{initial[i][0],initial[i][1],0xdeadbeef,0xcafef00d}});
    }
    return m;
}
void guards(const Memory& m){for(size_t i=1;i<m.regions.size();i+=2)assert(m.regions[i].words[2]==0xdeadbeef&&m.regions[i].words[3]==0xcafef00d);}
}
int main(){
    using Patch=ot2::FrameRatePatch;Patch patch;auto m=fixture();
    patch.configure(base,0x9000000,Patch::supported_build);
    assert(patch.apply(m,60)==Patch::changed);
    assert(m.regions[1].words[0]==1&&m.regions[3].words[0]==0);
    assert(std::bit_cast<float>(m.regions[5].words[0])==60&&std::bit_cast<float>(m.regions[5].words[1])==60);
    assert(m.regions[7].words[0]==7&&std::bit_cast<float>(m.regions[7].words[1])==60);
    assert(std::abs(std::bit_cast<float>(m.regions[9].words[0])-1000.0f/60)<0.001);guards(m);
    assert(patch.apply(m,60)==Patch::unchanged);
    assert(patch.apply(m,30)==Patch::changed);assert(m.regions[1].words[0]==2&&m.regions[3].words[0]==1);
    assert(std::bit_cast<float>(m.regions[5].words[0])==30&&std::bit_cast<float>(m.regions[7].words[1])==30);guards(m);
    // Invalid LAST field cannot leave the first four settings half-applied.
    for(int failure=0;failure<6;++failure){
        m=fixture();
        if(failure==0)m.regions[9].writable=false;
        if(failure==1)m.regions[8].words[0]=m.regions[8].words[1]=0;
        if(failure==2)m.regions[9].words[1]=0x7fc00000;
        if(failure==3)m.regions[8].words[0]=0xffffffff,m.regions[8].words[1]=0xffffffff;
        if(failure==4)m.regions[8].words[0]=m.regions[0].words[0];
        if(failure==5)m.regions[7].words[0]=8;
        auto old=m.regions;auto result=patch.apply(m,60);assert(result==Patch::invalid||result==Patch::not_ready);
        for(size_t i=0;i<old.size();++i)assert(m.regions[i].words==old[i].words);
    }
    auto build=Patch::supported_build;build[31]=1;m=fixture();patch.configure(base,0x9000000,build);
    assert(patch.apply(m,60)==Patch::unsupported&&m.lookups==0);
    patch.configure(base,0x1000,Patch::supported_build);assert(patch.apply(m,60)==Patch::invalid);
    patch.configure(base,0x9000000,Patch::supported_build);assert(patch.apply(m,45)==Patch::invalid);
    puts("PASS real 1.1.1 setting pairs 30/60/30, guarded writes, exact Build ID, null/overflow/readonly/NaN/alias rejection, no partial writes");
}
