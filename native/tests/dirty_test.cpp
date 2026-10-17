#include "../graphics/nvn_dirty.h"
#include "../graphics/index_summary.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <random>
#include <vector>

void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void fault_handler(int,siginfo_t* info,void*){int saved=errno;if(nvn_dirty_fault(info->si_addr)){errno=saved;return;}_exit(99);}
static void summary_contract(){
    const size_t page=sysconf(_SC_PAGESIZE),size=page*193;
    auto* p=static_cast<uint8_t*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    require(p!=MAP_FAILED&&nvn_dirty_add_range(p,size),"summary fixture");
    std::mt19937 rng(7613);
    auto ranges=[&](size_t begin,size_t length,uint64_t stamp,bool summary){
        nvn_dirty_use_summary(summary);std::vector<std::pair<size_t,size_t>> out;size_t cursor=0,offset=0,bytes=0;
        while(nvn_dirty_next_range(p+begin,length,stamp,&cursor,&offset,&bytes)){
            require(bytes&&offset+bytes<=length&&cursor==offset+bytes,"dirty intersections remain within requested range");out.emplace_back(offset,bytes);
        }return out;
    };
    for(unsigned round=0;round<40;++round){
        require(nvn_dirty_protect(p,size),"protect summary fixture");const auto stamp=nvn_dirty_now();
        for(unsigned i=0;i<8;++i)p[(rng()%193)*page+rng()%page]=uint8_t(round);
        for(unsigned i=0;i<100;++i){
            size_t begin=rng()%size,length=1+rng()%(size-begin);
            nvn_dirty_use_summary(0);const auto exact=nvn_dirty_since(p+begin,length,stamp);
            nvn_dirty_use_summary(1);require(nvn_dirty_since(p+begin,length,stamp)==exact,"hierarchical query matches exact-page oracle");
            require(ranges(begin,length,stamp,false)==ranges(begin,length,stamp,true),"partial ranges match exact-page oracle across summary boundaries");
        }
    }
    require(nvn_dirty_protect(p,size),"summary tail protect");auto stamp=nvn_dirty_now();p[64*page]=1;p[65*page]=1;p[192*page]=1;
    const auto changes=ranges(64*page+3,128*page+5,stamp,true);
    require(changes==std::vector<std::pair<size_t,size_t>>{{0,2*page-3},{128*page-3,8}},"adjacent pages coalesce and both unaligned edges clip");
    uint64_t pages,blocks,skipped;nvn_dirty_query_stats(&pages,&blocks,&skipped);
    require(!nvn_dirty_since(p,size,nvn_dirty_now()),"unchanged large pool is clean");nvn_dirty_query_stats(&pages,&blocks,&skipped);
    require(pages==0&&skipped==193,"whole-pool clean query avoids page loads");
    nvn_dirty_remove_range(p);require(ranges(3,17,stamp,true)==std::vector<std::pair<size_t,size_t>>{{0,17}},"untracked range conservatively copies all bytes");
    require(nvn_dirty_add_range(p,size)&&nvn_dirty_since(p,size,stamp),"range replacement invalidates summary lifetime");
    nvn_dirty_remove_range(p);munmap(p,size);nvn_dirty_use_summary(1);
}
int main(){try{
    struct sigaction action{};action.sa_sigaction=fault_handler;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV,&action,nullptr);sigaction(SIGBUS,&action,nullptr);
    summary_contract();
    const size_t page=sysconf(_SC_PAGESIZE),size=page*4;
    auto* memory=static_cast<uint8_t*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    require(memory!=MAP_FAILED,"mmap");memset(memory,0x33,size);
    require(!nvn_dirty_add_range(memory+1,size-1),"Unaligned pools cannot protect neighboring memory");
    require(nvn_dirty_add_range(memory,size),"Register aligned pool");
    auto lifetime=nvn_dirty_now();
    nvn_dirty_remove_range(memory);
    require(nvn_dirty_now()>lifetime,"Removing a writable pool invalidates cached lifetime");
    require(nvn_dirty_add_range(memory,size),"Re-register same address");
    require(nvn_dirty_since(memory,size,lifetime),"Same-address pool replacement invalidates old contents");
    {
        IndexSummaries cache;
        auto* words=reinterpret_cast<uint32_t*>(memory+page);
        words[0]=2;words[1]=70000;words[2]=9;
        require(cache.maximum(words,3,2)==70000,"uint32 maximum");
        auto scans=cache.scans;
        for(unsigned i=0;i<100;i++)require(cache.maximum(words,3,2)==70000,"unchanged index reuse");
        require(cache.scans==scans&&cache.hits==100,"no repeated index scans");
        words[1]=1;
        require(cache.maximum(words,3,2)==9&&cache.scans==scans+1,"real CPU fault invalidates maximum");
        {NvnDirtyWrite lease(words,12);words[2]=8;}
        require(cache.maximum(words,3,2)==8,"host write lease invalidates maximum");
        require(cache.maximum(words,1,2)==2,"count participates in key");
        require(cache.maximum(words,3,0)==2&&cache.maximum(words,3,1)==2,"u8 and u16 use independent keys");
        scans=cache.scans;nvn_dirty_remove_range(memory);memset(words,0,12);words[1]=77;
        require(nvn_dirty_add_range(memory,size),"index source lifetime replacement");
        require(cache.maximum(words,3,2)==77&&cache.scans==scans+1,"same-address new pool is rescanned");
        uint16_t untracked[]={1,4,2};require(cache.maximum(untracked,3,1)==4,"untracked fallback");
        untracked[2]=19;require(cache.maximum(untracked,3,1)==19,"untracked changes never cached");
        cache.enabled=false;scans=cache.scans;cache.maximum(words,3,2);cache.maximum(words,3,2);
        require(cache.scans==scans+2,"disabled negative control always scans");
        cache.enabled=true;
        require(cache.maximum(memory+size,0,2)==0,"empty range does not dereference");
        for(unsigned i=0;i<20000;i++)cache.maximum(memory+i/2,1+i%2,0);
        require(cache.size()<=16384,"index summary cache is bounded");
    }
    nvn_dirty_remove_range(memory);memset(memory,0x33,size);require(nvn_dirty_add_range(memory,size),"reset fixture");
    require(nvn_dirty_tracked(memory+17,page*2)&&!nvn_dirty_tracked(memory+size-1,2),"Tracked ranges are bounded");
    require(nvn_dirty_protect(memory,page*2),"Protect two pages");auto stamp=nvn_dirty_now();
    volatile uint8_t* writer=memory;writer[3]=0x77;
    require(nvn_dirty_since(memory,page,stamp)&&!nvn_dirty_since(memory+page,page,stamp),"Write faults dirty only their page");
    require(nvn_dirty_fault_count()>0,"A real protection fault was handled");
    require(nvn_dirty_protect(memory,size),"Reprotect full pool");stamp=nvn_dirty_now();
    std::atomic<bool> started{false},protected_again{false};std::thread protector;
    {
        NvnDirtyWrite lease(memory,page);
        require(lease.locked,"Kernel destination has a write lease");
        protector=std::thread([&]{started=true;require(nvn_dirty_protect(memory,page),"Concurrent protect");protected_again=true;});
        while(!started)std::this_thread::yield();std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(!protected_again,"Re-protection must wait until kernel write finishes");
        int fd[2];require(pipe(fd)==0,"pipe");uint8_t data[8]={1,2,3,4,5,6,7,8};require(write(fd[1],data,8)==8,"pipe write");
        require(read(fd[0],memory,8)==8&&!memcmp(memory,data,8),"Kernel write into formerly protected destination");close(fd[0]);close(fd[1]);
    }
    protector.join();require(protected_again&&nvn_dirty_since(memory,page,stamp),"Kernel write dirties and protection resumes");
    // Concurrent faulting writers and protection repeatedly contend on BUSY.
    // No thread reads the other writer's bytes; this tests the protection state,
    // not undefined concurrent access to application data.
    std::atomic<bool> finish{false};std::atomic<unsigned> writes{0};
    std::thread first([&]{volatile uint8_t* p=memory;do{p[32]=0x81;writes.fetch_add(1);}while(!finish);});
    std::thread second([&]{volatile uint8_t* p=memory;do{p[64]=0x82;writes.fetch_add(1);}while(!finish);});
    for(unsigned i=0;i<2000;i++)require(nvn_dirty_protect(memory,page*2),"Protect/fault contention");
    finish=true;first.join();second.join();require(writes>0&&memory[32]==0x81&&memory[64]==0x82,"Concurrent writers complete");
    // Recycle slots while a read-side scanner is active. Immutable descriptors
    // cannot be reclaimed until the scanner has dropped its read-side guard.
    finish=false;std::thread scanner([&]{while(!finish){nvn_dirty_tracked(memory,page);nvn_dirty_since(memory,page,0);}});
    for(unsigned i=0;i<500;i++){nvn_dirty_remove_range(memory);require(nvn_dirty_add_range(memory,size),"Range slot reuse");}
    finish=true;scanner.join();require(nvn_dirty_protect(memory,size),"Final protection");
    nvn_dirty_remove_range(memory);require(!nvn_dirty_tracked(memory,page),"Remove tracking");memset(memory,0x55,size);munmap(memory,size);
    std::cout<<"Dirty pages: fault/reprotect/kernel-write lease/concurrency/retirement passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
