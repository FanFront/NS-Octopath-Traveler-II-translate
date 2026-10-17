// SPDX-License-Identifier: MIT
// Write-protect tracking follows D2R Mobile (jhaizhou, MIT). Range lifetime and
// host-write leases are adapted for Android's signal and pread paths.
#include "nvn_dirty.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace {
enum : uint8_t { writable,protected_page,busy };
struct Page {std::atomic<uint8_t> state{writable};std::atomic<uint64_t> generation{0};std::atomic<bool> unprotected{false};};
struct Range {
    uintptr_t base,end;size_t page_size,count;std::unique_ptr<Page[]> pages;
    std::unique_ptr<std::atomic<uint64_t>[]> blocks;std::atomic<uint64_t> newest{0};
    Range(uintptr_t a,size_t size,size_t page):base(a),end(a+size),page_size(page),count(size/page),pages(std::make_unique<Page[]>(count)),blocks(std::make_unique<std::atomic<uint64_t>[]>((count+63)/64)){}
};
std::atomic<bool> use_summary{true};
std::atomic<uint64_t> query_pages{0},query_blocks{0},query_skipped{0};
void publish_max(std::atomic<uint64_t>& value,uint64_t stamp){auto old=value.load();while(old<stamp&&!value.compare_exchange_weak(old,stamp)){};}
void publish(Range& r,size_t page,uint64_t stamp){
    // Conservative upper bounds are visible before the exact generation.
    // No allocator, mutex or non-lock-free atomic enters the signal path.
    publish_max(r.blocks[page/64],stamp);publish_max(r.newest,stamp);r.pages[page].generation=stamp;
}
static_assert(std::atomic<uint8_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free && std::atomic<Range*>::is_always_lock_free);
std::array<std::atomic<Range*>,4096> ranges{};
std::atomic<unsigned> high_water{0},readers{0};std::atomic<uint64_t> generation{1},faults{0};
std::mutex control;
// Range descriptors are immutable while published. Remove unpublishes, waits
// for signal/read-side users to exit, then frees. Reused slots never mutate a
// descriptor that a signal handler could still be reading.
struct ReadSide {ReadSide(){readers.fetch_add(1);}~ReadSide(){readers.fetch_sub(1);}};
uint64_t now_ns(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
bool bounds(uintptr_t address,size_t size){return size && size<=UINTPTR_MAX-address;}
Range* find(uintptr_t address,size_t size=1){
    if(!bounds(address,size))return nullptr;
    const auto n=high_water.load();
    for(unsigned i=0;i<n;i++)if(auto* r=ranges[i].load();r&&address>=r->base&&address+size<=r->end)return r;
    return nullptr;
}
bool wait_page(Page& page){
    const auto start=now_ns();unsigned spins=0;
    while(page.state.load()==busy){if((++spins&1023)==0){if(now_ns()-start>2000000000)return false;sched_yield();}}
    return true;
}
bool unprotect(Range& r,size_t index,bool verify_mapping=false){
    auto& page=r.pages[index];
    for(;;){
        uint8_t expected=protected_page;
        if(page.state.compare_exchange_strong(expected,busy)){
            publish(r,index,generation.fetch_add(1)+1);
            if(mprotect(reinterpret_cast<void*>(r.base+index*r.page_size),r.page_size,PROT_READ|PROT_WRITE)){
                page.state=protected_page;return false;
            }
            page.unprotected=true;page.state=writable;return true;
        }
        if(expected==writable&&verify_mapping){
            // Another writer may already have handled this fault. Confirm the
            // mapping still exists; a stale registered range after munmap must
            // not make a translation fault retry forever. BUSY excludes a
            // concurrent reprotect while restoring the page permissions.
            if(!page.unprotected.load())return false;
            if(!page.state.compare_exchange_strong(expected,busy))continue;
            const bool ok=mprotect(reinterpret_cast<void*>(r.base+index*r.page_size),r.page_size,PROT_READ|PROT_WRITE)==0;
            page.state=writable;return ok;
        }
        if(expected!=busy)return true;
        if(!wait_page(page))return false;
    }
}
bool overlap(uintptr_t a,size_t size,const Range& r){return a<r.end&&r.base<a+size;}
}
extern "C" int nvn_dirty_add_range(const void* memory,size_t size){
    const auto address=reinterpret_cast<uintptr_t>(memory);const size_t page=sysconf(_SC_PAGESIZE);
    // Never protect neighboring heap/stack memory for a non-aligned allocation.
    if(!bounds(address,size)||address%page||size%page)return 0;
    std::lock_guard lock(control);const auto n=high_water.load();unsigned slot=n;
    for(unsigned i=0;i<n;i++){
        auto* r=ranges[i].load();if(!r){slot=std::min(slot,i);continue;}
        if(overlap(address,size,*r))return r->base==address&&r->end==address+size;
    }
    if(slot==ranges.size())return 0;
    auto* r=new Range(address,size,page);
    // The same address can belong to a new pool without a protection fault.
    // Lifetime changes must invalidate both page stamps and global fast paths.
    const auto stamp=generation.fetch_add(1)+1;
    for(size_t p=0;p<r->count;p++)publish(*r,p,stamp);
    ranges[slot].store(r);if(slot==n)high_water=n+1;return 1;
}
extern "C" void nvn_dirty_remove_range(const void* memory){
    std::lock_guard lock(control);const auto address=reinterpret_cast<uintptr_t>(memory);const auto n=high_water.load();
    for(unsigned i=0;i<n;i++)if(auto* r=ranges[i].load();r&&r->base==address){
        // NVN pool finalization requires that guest writers have finished.
        for(size_t p=0;p<r->count;p++)if(!unprotect(*r,p))return;
        ranges[i]=nullptr;generation.fetch_add(1);
        while(readers.load())sched_yield();
        delete r;return;
    }
}
extern "C" int nvn_dirty_tracked(const void* p,size_t size){ReadSide read;return find(reinterpret_cast<uintptr_t>(p),size)!=nullptr;}
extern "C" int nvn_dirty_protect(const void* memory,size_t size){
    std::lock_guard lock(control);const auto address=reinterpret_cast<uintptr_t>(memory);auto* r=find(address,size);if(!r)return 0;
    const size_t first=(address-r->base)/r->page_size,last=(address+size-1-r->base)/r->page_size;
    for(size_t p=first;p<=last;){
        if(!wait_page(r->pages[p]))return 0;
        uint8_t expected=writable;if(!r->pages[p].state.compare_exchange_strong(expected,busy)){if(expected!=busy)++p;continue;}
        const auto begin=p++;
        while(p<=last){expected=writable;if(!r->pages[p].state.compare_exchange_strong(expected,busy))break;++p;}
        const bool ok=mprotect(reinterpret_cast<void*>(r->base+begin*r->page_size),(p-begin)*r->page_size,PROT_READ)==0;
        for(size_t i=begin;i<p;i++)r->pages[i].state=ok?protected_page:writable;
        if(!ok)return 0;
    }
    return 1;
}
extern "C" uint64_t nvn_dirty_now(){return generation.load();}
extern "C" int nvn_dirty_since(const void* memory,size_t size,uint64_t stamp){
    if(!size)return 0;ReadSide read;auto address=reinterpret_cast<uintptr_t>(memory);auto* r=find(address,size);if(!r)return 1;
    const auto first=(address-r->base)/r->page_size,last=(address+size-1-r->base)/r->page_size;
    const bool summaries=use_summary.load()&&last-first>=4;
    if(summaries&&r->newest.load()<=stamp){query_skipped.fetch_add(last-first+1);return 0;}
    uint64_t reads=0,blocks=0,skipped=0;bool dirty=false;
    for(size_t p=first;p<=last;){
        const size_t end=std::min(last+1,((p/64)+1)*64);
        if(summaries){++blocks;if(r->blocks[p/64].load()<=stamp){skipped+=end-p;p=end;continue;}}
        for(;p<end;++p){++reads;if(r->pages[p].generation.load()>stamp){dirty=true;break;}}
        if(dirty)break;
    }
    query_pages.fetch_add(reads);query_blocks.fetch_add(blocks);query_skipped.fetch_add(skipped);
    if(dirty)return 1;
    return 0;
}
extern "C" void nvn_dirty_use_summary(int on){use_summary=on!=0;}
extern "C" void nvn_dirty_query_stats(uint64_t* pages,uint64_t* blocks,uint64_t* skipped){
    *pages=query_pages.exchange(0);*blocks=query_blocks.exchange(0);*skipped=query_skipped.exchange(0);
}
extern "C" int nvn_dirty_next_range(const void* memory,size_t size,uint64_t stamp,size_t* cursor,size_t* offset,size_t* bytes){
    if(!cursor||!offset||!bytes||*cursor>=size)return 0;
    ReadSide read;const auto address=reinterpret_cast<uintptr_t>(memory);auto* r=find(address,size);
    if(!r){*offset=*cursor;*bytes=size-*cursor;*cursor=size;return 1;}
    const size_t last=(address+size-1-r->base)/r->page_size;
    size_t page=(address+*cursor-r->base)/r->page_size;
    const bool summaries=use_summary.load();
    if(summaries&&r->newest.load()<=stamp){*cursor=size;return 0;}
    while(page<=last){
        if(summaries&&r->blocks[page/64].load()<=stamp){page=std::min(last+1,((page/64)+1)*64);continue;}
        if(r->pages[page].generation.load()>stamp)break;++page;
    }
    if(page>last){*cursor=size;return 0;}
    const auto begin=std::max(address+*cursor,r->base+page*r->page_size);
    do{++page;}while(page<=last&&r->pages[page].generation.load()>stamp);
    const auto end=std::min(address+size,r->base+page*r->page_size);
    *offset=begin-address;*bytes=end-begin;*cursor=end-address;return 1;
}
extern "C" int nvn_dirty_fault(const void* memory){
    ReadSide read;auto address=reinterpret_cast<uintptr_t>(memory);auto* r=find(address);if(!r)return 0;
    const auto index=(address-r->base)/r->page_size;
    if(!unprotect(*r,index,true))return 0;
    faults.fetch_add(1);return 1;
}
extern "C" int nvn_dirty_begin_write(void* memory,size_t size){
    const auto address=reinterpret_cast<uintptr_t>(memory);if(!bounds(address,size))return 0;
    bool found=false;{
        ReadSide read;for(unsigned i=0,n=high_water.load();i<n;i++)if(auto* r=ranges[i].load();r&&overlap(address,size,*r)){found=true;break;}
    }
    if(!found)return 0;
    control.lock();
    for(unsigned i=0,n=high_water.load();i<n;i++)if(auto* r=ranges[i].load();r&&overlap(address,size,*r)){
        const auto first=(std::max(address,r->base)-r->base)/r->page_size,last=(std::min(address+size,r->end)-1-r->base)/r->page_size;
        for(size_t p=first;p<=last;p++)if(!unprotect(*r,p)){control.unlock();return 0;}
    }
    return 1;
}
extern "C" void nvn_dirty_end_write(){control.unlock();}
extern "C" uint64_t nvn_dirty_fault_count(){return faults.load();}
